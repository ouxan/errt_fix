// Pipeline state reuse.
//
// The game occasionally creates the same graphics or compute pipeline more than once, and the driver
// compiles it again each time. After a pipeline is compiled, its driver binary (GetCachedBlob) is kept
// in memory. An identical later request is created with that binary as CachedPSO, so the driver loads
// it instead of compiling. The game still receives a new, separate object for every request; returning
// one object for two requests is not safe with this game. If the driver rejects the binary, the request
// is created exactly as issued.
//
// Two descriptions are identical when everything that affects the pipeline matches: shader bytecode,
// every state field (compared field by field, so struct padding is irrelevant), input layout and
// stream output semantics, and the root signature by the hash of its serialized blob. Descriptions
// that cannot be fully read, and requests that already carry a cached blob, are not matched.
//
// When two threads request the same new pipeline concurrently, the second waits up to 2 s for the
// first and then uses its binary.
#include "common.h"

#include <chrono>
#include <condition_variable>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

namespace errt {
namespace {

constexpr size_t kMaxBytes = 256u << 20;  // memory for kept binaries; least recently used are dropped

struct Hasher {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    void mix(uint64_t v) {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h *= 0xFF51AFD7ED558CCDull;
        h ^= h >> 33;
    }
    void bytes(const void* p, size_t n) {
        mix(n);
        const uint8_t* b = (const uint8_t*)p;
        size_t i = 0;
        for (; i + 8 <= n; i += 8) {
            uint64_t v;
            memcpy(&v, b + i, 8);
            mix(v);
        }
        uint64_t tail = 0;
        memcpy(&tail, b + i, n - i);
        mix(tail);
    }
    void str(const char* s) {
        if (!s) return mix(0xFFFFFFFFull);
        bytes(s, strlen(s));
    }
};

bool RootSig(Hasher& h, ID3D12RootSignature* rs) {
    if (!rs) {
        h.mix(0);  // root signature embedded in the shaders
        return true;
    }
    uint64_t k = 0;
    if (!RootSignatureKey(rs, &k)) return false;
    h.mix(k);
    return true;
}

void Shader(Hasher& h, const D3D12_SHADER_BYTECODE& s) {
    if (!s.pShaderBytecode || !s.BytecodeLength) return h.mix(0);
    h.bytes(s.pShaderBytecode, s.BytecodeLength);
}

void Blend(Hasher& h, const D3D12_BLEND_DESC& b) {
    h.mix(b.AlphaToCoverageEnable);
    h.mix(b.IndependentBlendEnable);
    for (const auto& r : b.RenderTarget) {
        h.mix(r.BlendEnable);
        h.mix(r.LogicOpEnable);
        h.mix(r.SrcBlend);
        h.mix(r.DestBlend);
        h.mix(r.BlendOp);
        h.mix(r.SrcBlendAlpha);
        h.mix(r.DestBlendAlpha);
        h.mix(r.BlendOpAlpha);
        h.mix(r.LogicOp);
        h.mix(r.RenderTargetWriteMask);
    }
}

void Raster(Hasher& h, const D3D12_RASTERIZER_DESC& r) {
    h.mix(r.FillMode);
    h.mix(r.CullMode);
    h.mix(r.FrontCounterClockwise);
    h.mix((uint32_t)r.DepthBias);
    uint32_t f;
    memcpy(&f, &r.DepthBiasClamp, 4);
    h.mix(f);
    memcpy(&f, &r.SlopeScaledDepthBias, 4);
    h.mix(f);
    h.mix(r.DepthClipEnable);
    h.mix(r.MultisampleEnable);
    h.mix(r.AntialiasedLineEnable);
    h.mix(r.ForcedSampleCount);
    h.mix(r.ConservativeRaster);
}

void StencilOp(Hasher& h, const D3D12_DEPTH_STENCILOP_DESC& o) {
    h.mix(o.StencilFailOp);
    h.mix(o.StencilDepthFailOp);
    h.mix(o.StencilPassOp);
    h.mix(o.StencilFunc);
}

void Depth(Hasher& h, const D3D12_DEPTH_STENCIL_DESC& d) {
    h.mix(d.DepthEnable);
    h.mix(d.DepthWriteMask);
    h.mix(d.DepthFunc);
    h.mix(d.StencilEnable);
    h.mix(d.StencilReadMask);
    h.mix(d.StencilWriteMask);
    StencilOp(h, d.FrontFace);
    StencilOp(h, d.BackFace);
}

void Depth1(Hasher& h, const D3D12_DEPTH_STENCIL_DESC1& d) {
    h.mix(d.DepthEnable);
    h.mix(d.DepthWriteMask);
    h.mix(d.DepthFunc);
    h.mix(d.StencilEnable);
    h.mix(d.StencilReadMask);
    h.mix(d.StencilWriteMask);
    StencilOp(h, d.FrontFace);
    StencilOp(h, d.BackFace);
    h.mix(d.DepthBoundsTestEnable);
}

void InputLayout(Hasher& h, const D3D12_INPUT_LAYOUT_DESC& il) {
    h.mix(il.NumElements);
    for (UINT i = 0; il.pInputElementDescs && i < il.NumElements; ++i) {
        const auto& e = il.pInputElementDescs[i];
        h.str(e.SemanticName);
        h.mix(e.SemanticIndex);
        h.mix(e.Format);
        h.mix(e.InputSlot);
        h.mix(e.AlignedByteOffset);
        h.mix(e.InputSlotClass);
        h.mix(e.InstanceDataStepRate);
    }
}

void StreamOutput(Hasher& h, const D3D12_STREAM_OUTPUT_DESC& so) {
    h.mix(so.NumEntries);
    for (UINT i = 0; so.pSODeclaration && i < so.NumEntries; ++i) {
        const auto& e = so.pSODeclaration[i];
        h.mix(e.Stream);
        h.str(e.SemanticName);
        h.mix(e.SemanticIndex);
        h.mix(e.StartComponent);
        h.mix(e.ComponentCount);
        h.mix(e.OutputSlot);
    }
    h.mix(so.NumStrides);
    for (UINT i = 0; so.pBufferStrides && i < so.NumStrides; ++i) h.mix(so.pBufferStrides[i]);
    h.mix(so.RasterizedStream);
}

void Formats(Hasher& h, UINT n, const DXGI_FORMAT* f) {
    h.mix(n);
    for (UINT i = 0; i < n && i < 8; ++i) h.mix(f[i]);
}

bool GraphicsImpl(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, uint64_t* key) {
    Hasher h;
    h.mix(1);
    if (!RootSig(h, d->pRootSignature)) return false;
    Shader(h, d->VS);
    Shader(h, d->PS);
    Shader(h, d->DS);
    Shader(h, d->HS);
    Shader(h, d->GS);
    StreamOutput(h, d->StreamOutput);
    Blend(h, d->BlendState);
    h.mix(d->SampleMask);
    Raster(h, d->RasterizerState);
    Depth(h, d->DepthStencilState);
    InputLayout(h, d->InputLayout);
    h.mix(d->IBStripCutValue);
    h.mix(d->PrimitiveTopologyType);
    Formats(h, d->NumRenderTargets, d->RTVFormats);
    h.mix(d->DSVFormat);
    h.mix(d->SampleDesc.Count);
    h.mix(d->SampleDesc.Quality);
    h.mix(d->NodeMask);
    h.mix(d->Flags);
    *key = h.h;
    return true;
}

bool ComputeImpl(const D3D12_COMPUTE_PIPELINE_STATE_DESC* d, uint64_t* key) {
    Hasher h;
    h.mix(2);
    if (!RootSig(h, d->pRootSignature)) return false;
    Shader(h, d->CS);
    h.mix(d->NodeMask);
    h.mix(d->Flags);
    *key = h.h;
    return true;
}

size_t AlignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

struct StreamKey {
    uint64_t key = 0;
    size_t cachedInner = SIZE_MAX;  // offset of the CACHED_PSO subobject's data, if present
    bool ownBlob = false;           // the request already carries a cached blob
};

// Walks a pipeline state stream laid out like d3dx12 (each subobject aligned to pointer size).

bool StreamImpl(const D3D12_PIPELINE_STATE_STREAM_DESC* d, StreamKey* out) {
    const uint8_t* base = (const uint8_t*)d->pPipelineStateSubobjectStream;
    const size_t size = d->SizeInBytes;
    if (!base || !size) return false;
    Hasher h;
    h.mix(3);
    size_t off = 0;
    while (off + sizeof(UINT) <= size) {
        const UINT type = *(const UINT*)(base + off);
        size_t ia = 0, is = 0;
#define SUB(T)       \
    ia = alignof(T); \
    is = sizeof(T);  \
    break
        switch (type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: SUB(void*);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: SUB(D3D12_SHADER_BYTECODE);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: SUB(D3D12_STREAM_OUTPUT_DESC);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: SUB(D3D12_BLEND_DESC);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: SUB(UINT);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: SUB(D3D12_RASTERIZER_DESC);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: SUB(D3D12_DEPTH_STENCIL_DESC);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: SUB(D3D12_INPUT_LAYOUT_DESC);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: SUB(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: SUB(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: SUB(D3D12_RT_FORMAT_ARRAY);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: SUB(DXGI_FORMAT);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: SUB(DXGI_SAMPLE_DESC);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: SUB(UINT);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: SUB(D3D12_CACHED_PIPELINE_STATE);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: SUB(D3D12_PIPELINE_STATE_FLAGS);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: SUB(D3D12_DEPTH_STENCIL_DESC1);
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: SUB(D3D12_VIEW_INSTANCING_DESC);
            default: return false;  // unknown subobject: the description cannot be compared
        }
#undef SUB
        const size_t innerOff = AlignUp(sizeof(UINT), ia);
        if (off + innerOff + is > size) return false;
        const uint8_t* p = base + off + innerOff;
        if (type != D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO) h.mix(type);
        switch (type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
                if (!RootSig(h, *(ID3D12RootSignature* const*)p)) return false;
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: Shader(h, *(const D3D12_SHADER_BYTECODE*)p); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: StreamOutput(h, *(const D3D12_STREAM_OUTPUT_DESC*)p); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: Blend(h, *(const D3D12_BLEND_DESC*)p); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: Raster(h, *(const D3D12_RASTERIZER_DESC*)p); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: Depth(h, *(const D3D12_DEPTH_STENCIL_DESC*)p); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: Depth1(h, *(const D3D12_DEPTH_STENCIL_DESC1*)p); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: InputLayout(h, *(const D3D12_INPUT_LAYOUT_DESC*)p); break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
                const auto& f = *(const D3D12_RT_FORMAT_ARRAY*)p;
                Formats(h, f.NumRenderTargets, f.RTFormats);
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: {
                const auto& s = *(const DXGI_SAMPLE_DESC*)p;
                h.mix(s.Count);
                h.mix(s.Quality);
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
                const auto& v = *(const D3D12_VIEW_INSTANCING_DESC*)p;
                h.mix(v.ViewInstanceCount);
                for (UINT i = 0; v.pViewInstanceLocations && i < v.ViewInstanceCount; ++i) {
                    h.mix(v.pViewInstanceLocations[i].ViewportArrayIndex);
                    h.mix(v.pViewInstanceLocations[i].RenderTargetArrayIndex);
                }
                h.mix(v.Flags);
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:  // a creation hint, not part of the key
                out->cachedInner = off + innerOff;
                out->ownBlob = ((const D3D12_CACHED_PIPELINE_STATE*)p)->CachedBlobSizeInBytes != 0;
                break;
            default: {
                // Plain 4-byte values: SAMPLE_MASK, IB_STRIP_CUT_VALUE, PRIMITIVE_TOPOLOGY,
                // DEPTH_STENCIL_FORMAT, NODE_MASK, FLAGS.
                h.mix(*(const UINT*)p);
                break;
            }
        }
        off = AlignUp(off + innerOff + is, sizeof(void*));
    }
    out->key = h.h;
    return true;
}

// Descriptions point into game memory; an invalid pointer must not crash the process.
template <class D, class O>
bool Guarded(bool (*fn)(const D*, O*), const D* d, O* out) {
    __try {
        return fn(d, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using Blob = std::vector<uint8_t>;
struct Entry {
    std::shared_ptr<const Blob> blob;
    double firstMs = 0;  // duration of the original compile
    std::list<uint64_t>::iterator lru;
};

std::mutex s_m;
std::condition_variable s_cv;
std::unordered_map<uint64_t, Entry> s_done;
std::list<uint64_t> s_lru;                    // most recently used first
size_t s_bytes = 0;
std::unordered_map<uint64_t, int> s_inFlight;  // key -> threads currently compiling it
struct Stats {
    uint64_t requests = 0, compiled = 0, reused = 0, rejected = 0;
    double savedMs = 0;
} s_st;

}  // namespace

uint64_t PsoKeyGraphics(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d) {
    uint64_t k = 0;
    if (!d) return 0;
    if (d->CachedPSO.CachedBlobSizeInBytes) return 0;
    return Guarded(&GraphicsImpl, d, &k) ? (k ? k : 1) : 0;
}
uint64_t PsoKeyCompute(const D3D12_COMPUTE_PIPELINE_STATE_DESC* d) {
    uint64_t k = 0;
    if (!d) return 0;
    if (d->CachedPSO.CachedBlobSizeInBytes) return 0;
    return Guarded(&ComputeImpl, d, &k) ? (k ? k : 1) : 0;
}
uint64_t PsoKeyStream(const D3D12_PIPELINE_STATE_STREAM_DESC* d, size_t* cachedInner) {
    StreamKey sk;
    if (!d || !Guarded(&StreamImpl, d, &sk)) return 0;
    if (sk.ownBlob) return 0;
    *cachedInner = sk.cachedInner;
    return sk.key ? sk.key : 1;
}

std::shared_ptr<const std::vector<uint8_t>> PsoReuseBegin(uint64_t key) {
    std::unique_lock<std::mutex> lk(s_m);
    s_st.requests++;
    if (!key) return nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        auto it = s_done.find(key);
        if (it != s_done.end()) {
            s_lru.erase(it->second.lru);
            s_lru.push_front(key);
            it->second.lru = s_lru.begin();
            return it->second.blob;
        }
        auto f = s_inFlight.find(key);
        if (f == s_inFlight.end() || f->second <= 0 || std::chrono::steady_clock::now() >= deadline) break;
        s_cv.wait_until(lk, deadline);
    }
    s_inFlight[key]++;
    return nullptr;
}

void PsoReuseEnd(uint64_t key, bool fromScratch, HRESULT hr, void* obj, double ms, bool usedBlob, bool rejected,
                 const char* kind) {
    if (!key) return;
    // Fetch the driver binary before taking the lock.
    std::shared_ptr<Blob> blob;
    if (fromScratch && SUCCEEDED(hr) && obj && ms >= (double)g_cfg.psoReuseMinMs) {
        ID3D12PipelineState* pso = nullptr;
        if (SUCCEEDED(((IUnknown*)obj)->QueryInterface(IID_PPV_ARGS(&pso)))) {
            ID3DBlob* b = nullptr;
            if (SUCCEEDED(pso->GetCachedBlob(&b)) && b && b->GetBufferSize()) {
                const uint8_t* p = (const uint8_t*)b->GetBufferPointer();
                blob = std::make_shared<Blob>(p, p + b->GetBufferSize());
            }
            if (b) b->Release();
            pso->Release();
        }
    }
    double firstMs = 0;
    bool logReuse = false;
    {
        std::lock_guard<std::mutex> lk(s_m);
        if (fromScratch) {
            auto f = s_inFlight.find(key);
            if (f != s_inFlight.end() && --f->second <= 0) s_inFlight.erase(f);
            if (SUCCEEDED(hr)) s_st.compiled++;
        }
        if (rejected) {
            s_st.rejected++;
            auto it = s_done.find(key);  // the driver rejected the binary; discard it
            if (it != s_done.end()) {
                s_bytes -= it->second.blob->size();
                s_lru.erase(it->second.lru);
                s_done.erase(it);
            }
        }
        if (usedBlob) {
            auto it = s_done.find(key);
            firstMs = it != s_done.end() ? it->second.firstMs : 0;
            s_st.reused++;
            if (firstMs > ms) s_st.savedMs += firstMs - ms;
            logReuse = s_st.reused <= 200;
        }
        if (blob && !s_done.count(key)) {
            s_lru.push_front(key);
            s_bytes += blob->size();
            s_done[key] = Entry{blob, ms, s_lru.begin()};
            while (s_bytes > kMaxBytes && s_lru.size() > 1) {
                const uint64_t old = s_lru.back();
                s_lru.pop_back();
                auto it = s_done.find(old);
                if (it != s_done.end()) {
                    s_bytes -= it->second.blob->size();
                    s_done.erase(it);
                }
            }
        }
    }
    s_cv.notify_all();
    if (logReuse)
        Log("PSO REUSE: repeated %s pipeline created from the stored driver binary in %.1f ms (original compile "
            "%.1f ms)",
            kind, ms, firstMs);
    if (rejected) Log("PSO REUSE: driver rejected a stored %s binary; pipeline compiled normally", kind);
}

void PsoReuseLogSummary() {
    std::lock_guard<std::mutex> lk(s_m);
    if (!s_st.requests) return;
    Log("PSO REUSE: %llu requests, %llu compiled, %llu created from a stored binary (about %.0f ms saved), "
        "%llu rejected by the driver",
        (unsigned long long)s_st.requests, (unsigned long long)s_st.compiled, (unsigned long long)s_st.reused,
        s_st.savedMs, (unsigned long long)s_st.rejected);
}

}  // namespace errt
