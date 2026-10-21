// D3D12 and DXGI vtable hooks:
//   CreateStateObject, CreateRootSignature        -> RT pipeline cache (rt_cache.cpp)
//   Create{Graphics,Compute}PipelineState,
//   CreatePipelineState                           -> pipeline state reuse (pso_reuse.cpp)
//   BuildRaytracingAccelerationStructure          -> BLAS refit thinning
//   Present, Present1                             -> frame counter
#include "common.h"

#include <wrl/client.h>

#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace errt {

std::atomic<uint64_t> g_frame{1};

static HookSlot s_CreateGPS("Device::CreateGraphicsPipelineState");
static HookSlot s_CreateCPS("Device::CreateComputePipelineState");
static HookSlot s_CreatePipelineState("Device2::CreatePipelineState");
static HookSlot s_CreateStateObject("Device5::CreateStateObject");
static HookSlot s_CreateRootSig("Device::CreateRootSignature");
static HookSlot s_BuildAS("CmdList4::BuildRaytracingAccelerationStructure");
static HookSlot s_Present("SwapChain::Present");
static HookSlot s_Present1("SwapChain1::Present1");

// ---------------------------------------------------------------- patching helpers
// Reads bytes of a loaded module from its file on disk, i.e. before any patching or relocation.
// Returns false if the address is not file-backed.
static bool ReadDiskBytes(const void* addr, void* out, DWORD len, uint64_t* fileImageBase, const uint8_t** modBase) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &mod))
        return false;
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(mod, path, MAX_PATH)) return false;
    const uint8_t* base = (const uint8_t*)mod;
    const uint64_t rva = (uint64_t)((const uint8_t*)addr - base);

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    auto readAt = [&](uint64_t off, void* dst, DWORD n) {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)off;
        DWORD got = 0;
        return SetFilePointerEx(f, li, nullptr, FILE_BEGIN) && ReadFile(f, dst, n, &got, nullptr) && got == n;
    };
    bool ok = false;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 fnt{};
    if (readAt(0, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE && readAt(dos.e_lfanew, &fnt, sizeof(fnt)) &&
        fnt.Signature == IMAGE_NT_SIGNATURE && fnt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        const uint64_t secOff = (uint64_t)dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                                fnt.FileHeader.SizeOfOptionalHeader;
        for (WORD i = 0; i < fnt.FileHeader.NumberOfSections && i < 96; ++i) {
            IMAGE_SECTION_HEADER sh{};
            if (!readAt(secOff + i * sizeof(sh), &sh, sizeof(sh))) break;
            if (rva < sh.VirtualAddress || rva + len > (uint64_t)sh.VirtualAddress + sh.SizeOfRawData) continue;
            ok = readAt(sh.PointerToRawData + (rva - sh.VirtualAddress), out, len);
            break;
        }
    }
    CloseHandle(f);
    if (ok && fileImageBase) *fileImageBase = fnt.OptionalHeader.ImageBase;
    if (ok && modBase) *modBase = base;
    return ok;
}

// The original value of a vtable slot, read from the owning module's file and rebased. Used to reach
// the real function when another overlay has hooked the slot.
static void* DiskVtableEntry(void* const* entry) {
    uint64_t v = 0, fileBase = 0;
    const uint8_t* base = nullptr;
    if (!ReadDiskBytes(entry, &v, 8, &fileBase, &base) || !v) return nullptr;
    const auto* nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    // The file holds addresses for the preferred image base; rebase to the actual load address.
    const uint64_t fn = v - fileBase + (uint64_t)base;
    if (fn < (uint64_t)base || fn >= (uint64_t)base + nt->OptionalHeader.SizeOfImage) return nullptr;
    return (void*)fn;
}

static bool ReadCode(const void* p, uint8_t* out, int n) {
    __try {
        for (int i = 0; i < n; ++i) out[i] = ((const uint8_t*)p)[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Length of one x64 instruction, for the handful of forms that open MSVC function prologues.
// Returns 0 for anything else, and for RIP-relative operands (they would break once copied).
static int PrologueInsnLen(const uint8_t* p) {
    int i = 0;
    bool opsize = false, rexW = false;
    if (p[i] == 0x66) { opsize = true; ++i; }
    if ((p[i] & 0xF0) == 0x40) { rexW = (p[i] & 0x08) != 0; ++i; }
    const uint8_t op = p[i++];
    auto modrm = [&](int imm) -> int {
        const uint8_t m = p[i++];
        const int mod = m >> 6, rm = m & 7;
        if (mod != 3 && rm == 4) {
            const uint8_t sib = p[i++];
            if (mod == 0 && (sib & 7) == 5) i += 4;
        } else if (mod == 0 && rm == 5) {
            return 0;  // RIP-relative
        }
        if (mod == 1) i += 1;
        else if (mod == 2) i += 4;
        return i + imm;
    };
    if (op >= 0x50 && op <= 0x5F) return i;  // push / pop reg
    if (op == 0x90) return i;                // nop
    switch (op) {
        case 0x01: case 0x03: case 0x29: case 0x2B: case 0x31: case 0x33: case 0x39: case 0x3B:
        case 0x85: case 0x89: case 0x8B: case 0x8D:
            return modrm(0);
        case 0x83: case 0xC6: return modrm(1);
        case 0x81: case 0xC7: return modrm(opsize ? 2 : 4);
        case 0x0F: {
            const uint8_t op2 = p[i++];
            if (op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29 || op2 == 0x1F) return modrm(0);
            return 0;
        }
        default:
            if (op >= 0xB8 && op <= 0xBF) return i + (rexW ? 8 : 4);
            return 0;
    }
}

// A callable copy of the entry of fn as shipped on disk. When another overlay overwrote the first
// bytes of fn with a jump into itself, this runs the original instructions and continues inside fn
// past the patch, so the call reaches the real implementation without passing through that overlay.
// Returns fn itself when it is not patched, nullptr when it cannot be done safely.
static void* CleanEntryStub(void* fn, std::string* why) {
    uint8_t disk[32], cur[32];
    if (!ReadDiskBytes(fn, disk, sizeof(disk), nullptr, nullptr)) { *why = "not file-backed"; return nullptr; }
    if (!ReadCode(fn, cur, sizeof(cur))) { *why = "unreadable"; return nullptr; }
    int patched = 0;
    for (int i = 0; i < 24; ++i)
        if (disk[i] != cur[i]) patched = i + 1;
    if (!patched) { *why = "not patched"; return fn; }
    int len = 0;
    while (len < patched) {
        const int l = PrologueInsnLen(disk + len);
        if (!l) { *why = "unsupported instruction in prologue"; return nullptr; }
        len += l;
    }
    if (len > 24) { *why = "patch too long"; return nullptr; }
    uint8_t* stub = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!stub) { *why = "VirtualAlloc failed"; return nullptr; }
    memcpy(stub, disk, len);
    const uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]
    memcpy(stub + len, jmp, 6);
    const uint64_t back = (uint64_t)fn + len;
    memcpy(stub + len + 6, &back, 8);
    DWORD old = 0;
    VirtualProtect(stub, 64, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), stub, 64);
    char b[64];
    snprintf(b, sizeof(b), "%d patched bytes, %d copied", patched, len);
    *why = b;
    return stub;
}

bool PatchSlot(void** vt, int idx, void* hook, HookSlot& s) {
    if (!vt || idx <= 2) return false;
    void** entry = vt + idx;
    if (*entry == hook) return true;
    const int n = s.n.load();
    for (int i = 0; i < n; ++i)
        if (s.vt[i] == vt) return true;
    if (n >= 4) return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(entry, &mbi, sizeof(mbi))) return false;
    const DWORD execMask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const DWORD want = (mbi.Protect & execMask) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    DWORD old = 0;
    if (!VirtualProtect(entry, sizeof(void*), want, &old)) return false;

    // Recording a hook of this module as the original would cause infinite recursion.
    if (InOurModule(*entry)) {
        RawAlert("REFUSING to patch %s: slot %d already points into errt_fix (%p)", s.name, idx, *entry);
        VirtualProtect(entry, sizeof(void*), old, &old);
        return false;
    }
    s.vt[n] = vt;
    s.orig[n] = *entry;
    s.real[n] = DiskVtableEntry(entry);
    s.n.store(n + 1, std::memory_order_release);
    InterlockedExchangePointer(entry, hook);
    VirtualProtect(entry, sizeof(void*), old, &old);
    return true;
}

// ---------------------------------------------------------------- pipelines (PSO reuse)
static void SlowCompile(const char* kind, int64_t ticks) {
    const double ms = TicksToMs(ticks);
    if (ms >= 20.0) Log("slow pipeline compile: %s %.1f ms (thread %lu)", kind, ms, GetCurrentThreadId());
}

static size_t AlignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

using PFN_CreateGPS = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
static HRESULT STDMETHODCALLTYPE Hk_CreateGPS(ID3D12Device* self, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d,
                                              REFIID riid, void** pp) {
    const auto orig = OrigOf<PFN_CreateGPS>(s_CreateGPS, self);
    const uint64_t key = (g_cfg.psoReuse && pp) ? PsoKeyGraphics(d) : 0;
    const auto blob = key ? PsoReuseBegin(key) : nullptr;
    bool usedBlob = false, rejected = false;
    const int64_t t0 = Now();
    HRESULT hr;
    if (blob) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC c = *d;
        c.CachedPSO = {blob->data(), blob->size()};
        hr = orig(self, &c, riid, pp);
        usedBlob = SUCCEEDED(hr);
        if (!usedBlob) {
            rejected = true;
            hr = orig(self, d, riid, pp);
        }
    } else {
        hr = orig(self, d, riid, pp);
    }
    const int64_t dt = Now() - t0;
    if (key) PsoReuseEnd(key, !blob, hr, SUCCEEDED(hr) ? *pp : nullptr, TicksToMs(dt), usedBlob, rejected, "GFX");
    SlowCompile("GFX", dt);
    return hr;
}

using PFN_CreateCPS = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
static HRESULT STDMETHODCALLTYPE Hk_CreateCPS(ID3D12Device* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* d,
                                              REFIID riid, void** pp) {
    const auto orig = OrigOf<PFN_CreateCPS>(s_CreateCPS, self);
    const uint64_t key = (g_cfg.psoReuse && pp) ? PsoKeyCompute(d) : 0;
    const auto blob = key ? PsoReuseBegin(key) : nullptr;
    bool usedBlob = false, rejected = false;
    const int64_t t0 = Now();
    HRESULT hr;
    if (blob) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC c = *d;
        c.CachedPSO = {blob->data(), blob->size()};
        hr = orig(self, &c, riid, pp);
        usedBlob = SUCCEEDED(hr);
        if (!usedBlob) {
            rejected = true;
            hr = orig(self, d, riid, pp);
        }
    } else {
        hr = orig(self, d, riid, pp);
    }
    const int64_t dt = Now() - t0;
    if (key) PsoReuseEnd(key, !blob, hr, SUCCEEDED(hr) ? *pp : nullptr, TicksToMs(dt), usedBlob, rejected, "CS");
    SlowCompile("CS", dt);
    return hr;
}

// A copy of the game's stream whose CACHED_PSO points at blob: the existing subobject is patched, or one
// is appended (d3dx12 layout: the type, then the data at pointer alignment).
static void WithCachedBlob(const D3D12_PIPELINE_STATE_STREAM_DESC* d, size_t cachedInner, const void* blob, size_t len,
                           std::vector<uint64_t>* buf, D3D12_PIPELINE_STATE_STREAM_DESC* out) {
    const size_t size = d->SizeInBytes;
    const size_t append = cachedInner == SIZE_MAX ? AlignUp(size, sizeof(void*)) : size;
    const size_t total = cachedInner == SIZE_MAX ? append + 8 + sizeof(D3D12_CACHED_PIPELINE_STATE) : size;
    buf->assign((total + 7) / 8, 0);
    uint8_t* p = (uint8_t*)buf->data();
    memcpy(p, d->pPipelineStateSubobjectStream, size);
    const D3D12_CACHED_PIPELINE_STATE cp{blob, len};
    if (cachedInner == SIZE_MAX) {
        *(UINT*)(p + append) = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO;
        memcpy(p + append + 8, &cp, sizeof(cp));
    } else {
        memcpy(p + cachedInner, &cp, sizeof(cp));
    }
    out->SizeInBytes = total;
    out->pPipelineStateSubobjectStream = p;
}

using PFN_CreatePipelineState = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
static HRESULT STDMETHODCALLTYPE Hk_CreatePipelineState(ID3D12Device2* self, const D3D12_PIPELINE_STATE_STREAM_DESC* d,
                                                        REFIID riid, void** pp) {
    const auto orig = OrigOf<PFN_CreatePipelineState>(s_CreatePipelineState, self);
    size_t cachedInner = SIZE_MAX;
    const uint64_t key = (g_cfg.psoReuse && pp) ? PsoKeyStream(d, &cachedInner) : 0;
    const auto blob = key ? PsoReuseBegin(key) : nullptr;
    bool usedBlob = false, rejected = false;
    const int64_t t0 = Now();
    HRESULT hr;
    if (blob) {
        D3D12_PIPELINE_STATE_STREAM_DESC c{};
        std::vector<uint64_t> buf;  // 8-byte aligned copy of the stream
        WithCachedBlob(d, cachedInner, blob->data(), blob->size(), &buf, &c);
        hr = orig(self, &c, riid, pp);
        usedBlob = SUCCEEDED(hr);
        if (!usedBlob) {
            rejected = true;
            hr = orig(self, d, riid, pp);
        }
    } else {
        hr = orig(self, d, riid, pp);
    }
    const int64_t dt = Now() - t0;
    if (key) PsoReuseEnd(key, !blob, hr, SUCCEEDED(hr) ? *pp : nullptr, TicksToMs(dt), usedBlob, rejected, "STREAM");
    SlowCompile("STREAM", dt);
    return hr;
}

// ---------------------------------------------------------------- ray tracing pipelines (RT cache)
using PFN_CreateStateObject = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device5*, const D3D12_STATE_OBJECT_DESC*, REFIID, void**);
static HRESULT STDMETHODCALLTYPE Hk_CreateStateObject(ID3D12Device5* self, const D3D12_STATE_OBJECT_DESC* d,
                                                      REFIID riid, void** pp) {
    const int64_t t0 = Now();
    const auto create = OrigOf<PFN_CreateStateObject>(s_CreateStateObject, self);
    HRESULT hr = E_FAIL;
    bool handled = false;
    if (g_cfg.rtCache && d && d->Type == D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE)
        hr = RtCacheCreate(self, d, riid, pp, create, &handled);
    if (!handled) hr = create(self, d, riid, pp);
    if (g_cfg.rtCache && SUCCEEDED(hr) && pp && *pp && d && d->Type == D3D12_STATE_OBJECT_TYPE_COLLECTION) {
        ComPtr<ID3D12StateObject> so;
        if (SUCCEEDED(((IUnknown*)*pp)->QueryInterface(IID_PPV_ARGS(&so)))) RtCacheOnCollection(d, so.Get());
    }
    SlowCompile("RT state object", Now() - t0);
    return hr;
}

using PFN_CreateRootSig = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
static HRESULT STDMETHODCALLTYPE Hk_CreateRootSig(ID3D12Device* self, UINT mask, const void* blob, SIZE_T len,
                                                  REFIID riid, void** pp) {
    const HRESULT hr = OrigOf<PFN_CreateRootSig>(s_CreateRootSig, self)(self, mask, blob, len, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) {
        ComPtr<ID3D12RootSignature> rs;
        if (SUCCEEDED(((IUnknown*)*pp)->QueryInterface(IID_PPV_ARGS(&rs)))) RtCacheOnRootSignature(rs.Get(), blob, len);
    }
    return hr;
}

// ---------------------------------------------------------------- BLAS refit thinning
// The game refits its wind-animated foliage BLASes every frame. A BLAS that was built or refit within
// the last refit_frames frames is skipped; the previous acceleration structure remains valid, so ray
// traced geometry lags by at most one frame. Never skipped: the first build at an address, builds that
// emit postbuild info, refits into a different destination buffer, and TLAS builds.
struct BlasTrack {
    uint64_t lastFrame = 0;
};
static std::mutex s_blasM;
static std::unordered_map<uint64_t, BlasTrack> s_blas;
static std::atomic<uint64_t> s_blasSkipped{0}, s_blasDone{0};

static bool ParseBuildImpl(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* d, bool* tlas, bool* update) {
    *tlas = d->Inputs.Type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    *update = (d->Inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE) != 0;
    return true;
}
static bool ParseBuild(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* d, bool* tlas, bool* update) {
    __try {
        return ParseBuildImpl(d, tlas, update);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SkipBuild(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* d, bool hasPostbuild) {
    bool tlas = false, update = false;
    if (!d || !ParseBuild(d, &tlas, &update) || tlas) return false;
    const uint64_t dest = d->DestAccelerationStructureData;
    const bool inPlace = !d->SourceAccelerationStructureData || d->SourceAccelerationStructureData == dest;
    const uint64_t F = g_frame.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(s_blasM);
    if (s_blas.size() > 400000) s_blas.clear();
    auto it = s_blas.find(dest);
    const bool seen = it != s_blas.end();
    const bool skip = seen && (!update || inPlace) && !hasPostbuild &&
                      it->second.lastFrame + (uint64_t)g_cfg.refitFrames > F;
    if (!skip) s_blas[dest].lastFrame = F;
    return skip;
}

using PFN_BuildAS = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*,
                                             const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC*, UINT,
                                             const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC*);
static void STDMETHODCALLTYPE Hk_BuildAS(ID3D12GraphicsCommandList4* self,
                                         const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC* d, UINT n,
                                         const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pb) {
    if (SkipBuild(d, n > 0 && pb != nullptr)) {
        if (s_blasSkipped.fetch_add(1, std::memory_order_relaxed) == 0)
            Log("refit thinning active: first repeated BLAS build skipped");
        return;
    }
    s_blasDone.fetch_add(1, std::memory_order_relaxed);
    OrigOf<PFN_BuildAS>(s_BuildAS, self)(self, d, n, pb);
}

// ---------------------------------------------------------------- Present (frame counter)
static thread_local int t_inPresent = 0;
static thread_local int t_presentDepth = 0;
// Set once another overlay's hook re-entered this hook. From then on the real DXGI
// function is called directly; the other overlay still runs once, outside of this hook.
static std::atomic<int> s_presentBypass{0};

static void* RealOf(HookSlot& s, const void* self) {
    void* vt = *reinterpret_cast<void* const*>(self);
    const int n = s.n.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i)
        if (s.vt[i] == vt) return s.real[i];
    return n ? s.real[0] : nullptr;
}

// Overlays that also hook Present (Steam, RTSS, Discord, Game Bar) can route the call back into this
// hook. Re-entry means a cycle: the real DXGI function is then called through a stub that skips the
// overlay's inline patch. If that is not possible, the nested call is dropped. Returns the function to
// call, or nullptr to return S_OK.
static void* PresentTarget(HookSlot& slot, const void* self) {
    if (t_presentDepth > 0) {
        if (s_presentBypass.load() == 0) {
            void* real = RealOf(slot, self);
            std::string why = "real function unknown";
            void* clean = real ? CleanEntryStub(real, &why) : nullptr;
            Log("%s: another overlay re-enters this hook; calling the real function directly (%s)",
                slot.name, why.c_str());
            if (clean) {
                slot.clean.store(clean);
                s_presentBypass.store(1);
                return clean;
            }
            s_presentBypass.store(2);
        }
        if (t_presentDepth == 1)
            if (void* clean = slot.clean.load()) return clean;
        if (slot.warnedLoop.exchange(1) == 0)
            RawAlert("PRESENT HOOK LOOP on %s persists: dropping the nested call (frames may not reach the screen)",
                     slot.name);
        return nullptr;
    }
    if (s_presentBypass.load(std::memory_order_relaxed) == 1)
        if (void* clean = slot.clean.load(std::memory_order_relaxed)) return clean;
    return SafeOrig(slot, self);
}

static double s_lastSummary = 0;
static void OnFrame() {
    g_frame.fetch_add(1, std::memory_order_release);
    const double now = Seconds();
    if (now - s_lastSummary > 300.0) {
        s_lastSummary = now;
        RtCacheLogSummary();
        PsoReuseLogSummary();
        if (g_cfg.refitThinning)
            Log("refit thinning: %llu BLAS builds skipped, %llu done", (unsigned long long)s_blasSkipped.load(),
                (unsigned long long)s_blasDone.load());
    }
}

using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
static HRESULT STDMETHODCALLTYPE Hk_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
    auto f = reinterpret_cast<PFN_Present>(PresentTarget(s_Present, self));
    if (!f) return S_OK;
    ++t_presentDepth;
    if (!t_inPresent && !(flags & DXGI_PRESENT_TEST)) OnFrame();
    t_inPresent++;
    const HRESULT hr = f(self, sync, flags);
    t_inPresent--;
    --t_presentDepth;
    return hr;
}

using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static HRESULT STDMETHODCALLTYPE Hk_Present1(IDXGISwapChain1* self, UINT sync, UINT flags,
                                             const DXGI_PRESENT_PARAMETERS* params) {
    auto f = reinterpret_cast<PFN_Present1>(PresentTarget(s_Present1, self));
    if (!f) return S_OK;
    ++t_presentDepth;
    if (!t_inPresent && !(flags & DXGI_PRESENT_TEST)) OnFrame();
    t_inPresent++;
    const HRESULT hr = f(self, sync, flags, params);
    t_inPresent--;
    --t_presentDepth;
    return hr;
}

// ---------------------------------------------------------------- install
static LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

// Vtables are shared by all objects of a class, so patching them on temporary objects created here
// also covers the game's objects.
bool InstallHooks() {
    ComPtr<ID3D12Device> dev;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr)) {
        Log("D3D12CreateDevice (dummy) failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    ComPtr<ID3D12CommandQueue> q;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (alloc) dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));

    ComPtr<IDXGISwapChain1> sc;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"errt_fix_Dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"errt_fix", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (hwnd && q) {
        ComPtr<IDXGIFactory2> fac;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) {
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = 64;
            sd.Height = 64;
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            hr = fac->CreateSwapChainForHwnd(q.Get(), hwnd, &sd, nullptr, nullptr, &sc);
            if (FAILED(hr)) Log("dummy swapchain failed hr=0x%08lX", (unsigned long)hr);
        }
    }

    int ok = 0, fail = 0;
    auto P = [&](void* obj, int idx, void* hook, HookSlot& s) {
        if (!obj) return;
        if (PatchSlot(*(void***)obj, idx, hook, s)) ++ok;
        else {
            ++fail;
            Log("patch failed: %s (slot %d)", s.name, idx);
        }
    };
    if (g_cfg.psoReuse) {
        P(dev.Get(), g_vt.dev_CreateGraphicsPipelineState, (void*)&Hk_CreateGPS, s_CreateGPS);
        P(dev.Get(), g_vt.dev_CreateComputePipelineState, (void*)&Hk_CreateCPS, s_CreateCPS);
        ComPtr<ID3D12Device2> d2;
        if (SUCCEEDED(dev.As(&d2))) P(d2.Get(), g_vt.dev_CreatePipelineState, (void*)&Hk_CreatePipelineState, s_CreatePipelineState);
    }
    if (g_cfg.psoReuse || g_cfg.rtCache)
        P(dev.Get(), g_vt.dev_CreateRootSignature, (void*)&Hk_CreateRootSig, s_CreateRootSig);
    if (g_cfg.rtCache) {
        ComPtr<ID3D12Device5> d5;
        if (SUCCEEDED(dev.As(&d5))) P(d5.Get(), g_vt.dev_CreateStateObject, (void*)&Hk_CreateStateObject, s_CreateStateObject);
        else Log("ID3D12Device5 not available: no ray tracing, RT cache unused");
    }
    if (g_cfg.refitThinning && list) {
        ComPtr<ID3D12GraphicsCommandList4> l4;
        if (SUCCEEDED(list.As(&l4)))
            P(l4.Get(), g_vt.cl_BuildRaytracingAccelerationStructure, (void*)&Hk_BuildAS, s_BuildAS);
    }
    if (list) list->Close();
    if (sc) {
        P(sc.Get(), g_vt.sc_Present, (void*)&Hk_Present, s_Present);
        P(sc.Get(), g_vt.sc_Present1, (void*)&Hk_Present1, s_Present1);
    }
    Log("patched %d vtable slots, %d failed", ok, fail);

    sc.Reset();
    if (hwnd) DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return s_Present.n.load() > 0;
}

}  // namespace errt
