#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "vtidx.h"

namespace errt {

inline constexpr const char* kVersion = "1.0.0";

// ---------------------------------------------------------------- time
int64_t Now();
double TicksToMs(int64_t ticks);
double Seconds();

// ---------------------------------------------------------------- config / paths
struct Config {
    bool rtCache = true;     // prebuilt RT pipeline containing every known material (rt_cache.cpp)
    int rtCacheMax = 2000;   // maximum number of collections in the prebuilt pipeline
    bool rtScan = false;     // add every RT material found in process memory to the cache
    int rtScanSeconds = 30;
    bool refitThinning = true;  // skip a BLAS build or refit repeated within refitFrames frames
    int refitFrames = 2;
    bool psoReuse = true;    // create repeated pipelines from the stored driver binary (pso_reuse.cpp)
    int psoReuseMinMs = 2;  // minimum compile time for a binary to be kept
};
extern Config g_cfg;
extern std::wstring g_outDir;  // ...\errt_fix\ (trailing backslash)

std::string Narrow(const wchar_t* w);

// ---------------------------------------------------------------- logging (errt_fix\events.log)
void Log(const char* fmt, ...);
// Written with raw Win32 and flushed immediately, so the message survives a crash that follows.
void RawAlert(const char* fmt, ...);
bool InOurModule(const void* p);

// ---------------------------------------------------------------- vtable hooks
struct HookSlot {
    explicit HookSlot(const char* nm) : name(nm) {}
    const char* name;
    void* vt[4] = {};
    void* orig[4] = {};
    // The untouched function from the vtable owner's file on disk. Differs from orig when another
    // overlay hooked the slot first.
    void* real[4] = {};
    // Entry into the real function that skips another overlay's inline patch (see CleanEntryStub).
    std::atomic<void*> clean{nullptr};
    std::atomic<int> n{0};
    std::atomic<uint8_t> warnedSelf{0};
    std::atomic<uint8_t> warnedFallback{0};
    std::atomic<uint8_t> warnedLoop{0};
};

// Returns the original function for the object's vtable. Guards against an original that points
// back into this module, which would recurse until the stack overflows.
inline void* SafeOrig(HookSlot& s, const void* self) {
    void* vt = *reinterpret_cast<void* const*>(self);
    const int n = s.n.load(std::memory_order_acquire);
    void* o = nullptr;
    bool matched = false;
    for (int i = 0; i < n; ++i)
        if (s.vt[i] == vt) {
            o = s.orig[i];
            matched = true;
            break;
        }
    if (!matched) {
        if (s.warnedFallback.exchange(1) == 0)
            RawAlert("UNKNOWN VTABLE for %s (vtable %p) - using another recorded original", s.name, vt);
        o = s.orig[0];
    }
    if (InOurModule(o)) {
        if (s.warnedSelf.exchange(1) == 0)
            RawAlert("SELF-RECURSION: %s recorded its original as %p, which is inside errt_fix itself", s.name, o);
        for (int i = 0; i < n; ++i)
            if (!InOurModule(s.orig[i])) return s.orig[i];
    }
    return o;
}

template <class F>
inline F OrigOf(HookSlot& s, const void* self) {
    return reinterpret_cast<F>(SafeOrig(s, self));
}

bool InstallHooks();
extern std::atomic<uint64_t> g_frame;  // incremented at Present

// ---------------------------------------------------------------- rt_cache.cpp
using RtCreateFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device5*, const D3D12_STATE_OBJECT_DESC*, REFIID, void**);
void RtCacheOnRootSignature(ID3D12RootSignature* rs, const void* blob, size_t len);
void RtCacheOnCollection(const D3D12_STATE_OBJECT_DESC* d, ID3D12StateObject* so);
// Returns the prebuilt pipeline when it covers the request (*handled = true).
HRESULT RtCacheCreate(ID3D12Device5* dev, const D3D12_STATE_OBJECT_DESC* d, REFIID riid, void** pp, RtCreateFn create,
                      bool* handled);
void RtScanStart();
void RtCacheLogSummary();
// Hash of the blob a root signature was created from, recorded by the CreateRootSignature hook.
bool RootSignatureKey(ID3D12RootSignature* rs, uint64_t* key);

// ---------------------------------------------------------------- pso_reuse.cpp
// Content key of a pipeline description, or 0 if it cannot be compared. For a stream, *cachedInner
// receives the offset of an existing CACHED_PSO subobject's data, or SIZE_MAX.
uint64_t PsoKeyGraphics(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d);
uint64_t PsoKeyCompute(const D3D12_COMPUTE_PIPELINE_STATE_DESC* d);
uint64_t PsoKeyStream(const D3D12_PIPELINE_STATE_STREAM_DESC* d, size_t* cachedInner);
// Returns the stored driver binary for the key, or null if the pipeline must be compiled. Every call
// must be followed by PsoReuseEnd (fromScratch = no binary was returned).
std::shared_ptr<const std::vector<uint8_t>> PsoReuseBegin(uint64_t key);
void PsoReuseEnd(uint64_t key, bool fromScratch, HRESULT hr, void* obj, double ms, bool usedBlob, bool rejected,
                 const char* kind);
void PsoReuseLogSummary();

}  // namespace errt
