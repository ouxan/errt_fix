// Ray tracing pipeline cache.
//
// Elden Ring relinks its entire RAYTRACING_PIPELINE whenever a new material collection is created.
// Each relink runs on the render thread and costs tens of milliseconds. This module removes those
// relinks:
//
//  1. Every collection the game creates is serialized to rt_pipeline_cache.bin (DXIL libraries, hit
//     groups, configs and root signature blobs), together with the pipeline base it was used with.
//  2. When the game creates a pipeline, a background thread recreates every known collection and
//     links a single pipeline that contains all of them.
//  3. When the game later requests a pipeline whose collections are all part of that pipeline, the
//     prebuilt object is returned. Shader identifiers are looked up by export name, and the names are
//     identical, so the game's shader tables are unaffected.
//
// A collection that is not in the cache is linked normally and recorded for the next session.
#include "common.h"

#include <wrl/client.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace errt {
namespace {

using Bytes = std::vector<uint8_t>;

uint64_t Fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

struct Writer {
    Bytes b;
    void raw(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void wstr(LPCWSTR s) {
        if (!s) {
            u32(0xFFFFFFFFu);
            return;
        }
        const uint32_t n = (uint32_t)wcslen(s);
        u32(n);
        raw(s, n * sizeof(wchar_t));
    }
};

struct Reader {
    const uint8_t* p;
    size_t n, i = 0;
    bool ok = true;
    const uint8_t* raw(size_t len) {
        if (!ok || len > n - i) {
            ok = false;
            return nullptr;
        }
        const uint8_t* r = p + i;
        i += len;
        return r;
    }
    uint32_t u32() {
        const uint8_t* r = raw(4);
        uint32_t v = 0;
        if (r) memcpy(&v, r, 4);
        return v;
    }
    uint64_t u64() {
        const uint8_t* r = raw(8);
        uint64_t v = 0;
        if (r) memcpy(&v, r, 8);
        return v;
    }
    // Returns nullptr for a null string; otherwise a pointer into `store`.
    LPCWSTR wstr(std::deque<std::wstring>& store) {
        const uint32_t len = u32();
        if (len == 0xFFFFFFFFu || !ok) return nullptr;
        if (len > 4096) {
            ok = false;
            return nullptr;
        }
        const uint8_t* r = raw(len * sizeof(wchar_t));
        if (!r) return nullptr;
        store.emplace_back((const wchar_t*)r, len);
        return store.back().c_str();
    }
};

// ---------------------------------------------------------------- root signatures
std::mutex s_rsM;
std::unordered_map<ID3D12RootSignature*, uint64_t> s_rsKey;  // live objects -> blob hash
std::unordered_map<uint64_t, Bytes> s_rsBlob;                 // blob hash -> serialized root signature
std::unordered_map<uint64_t, ID3D12RootSignature*> s_rsObj;   // blob hash -> referenced root signature

// ---------------------------------------------------------------- store
std::mutex s_m;
bool s_loaded = false;
FILE* s_file = nullptr;
std::unordered_map<uint64_t, Bytes> s_colls;                       // collection key -> serialized desc
std::unordered_map<uint64_t, std::vector<uint64_t>> s_baseColls;   // base key -> collections used with it
std::unordered_set<uint64_t> s_rsPersisted;
std::unordered_map<ID3D12StateObject*, uint64_t> s_collOf;         // game collection object -> key

struct Super {
    uint64_t base = 0;
    std::unordered_set<uint64_t> keys;
    ID3D12StateObject* so = nullptr;
};
Super s_super;

// Background link worker
std::condition_variable s_cv;
bool s_workerUp = false, s_want = false, s_building = false;
uint64_t s_wantBase = 0;
Bytes s_wantBaseSer;
ID3D12Device5* s_dev = nullptr;
RtCreateFn s_create = nullptr;
// Last pipeline base the game requested, so the memory scan can schedule a link by itself.
ID3D12Device5* s_lastDev = nullptr;
RtCreateFn s_lastCreate = nullptr;
uint64_t s_lastBase = 0;
Bytes s_lastBaseSer;
std::unordered_set<uint64_t> s_synthKeys;  // collections produced by synthesis rather than by the game
bool s_disabled = false;                   // set when a background link fails; no further links this session

struct Stats {
    uint64_t requests = 0, hits = 0, missUnknown = 0, missNotReady = 0;
    uint64_t collsLoaded = 0, builds = 0, buildFailed = 0;
} s_st;

// Cache file: a sequence of records, each {magic, kind, key, key2, length, data}. Unknown kinds are
// skipped when loading.
//   REC_COLL   key = hash of data, data = serialized collection
//   REC_USE    key = pipeline base, key2 = collection used with it
//   REC_RS     key = hash of data, data = root signature blob
//   REC_SYNTH  key = collection produced by synthesis
enum : uint32_t { REC_COLL = 1, REC_USE = 2, REC_RS = 3, REC_SYNTH = 4, REC_MAGIC = 0x31435452 };  // "RTC1"

void Append(uint32_t kind, uint64_t key, uint64_t key2, const Bytes* data) {
    if (!s_file) return;
    const uint32_t len = data ? (uint32_t)data->size() : 0;
    const uint32_t magic = REC_MAGIC;
    fwrite(&magic, 4, 1, s_file);
    fwrite(&kind, 4, 1, s_file);
    fwrite(&key, 8, 1, s_file);
    fwrite(&key2, 8, 1, s_file);
    fwrite(&len, 4, 1, s_file);
    if (len) fwrite(data->data(), 1, len, s_file);
    fflush(s_file);
}

void LoadLocked() {
    if (s_loaded) return;
    s_loaded = true;
    const std::wstring path = g_outDir + L"rt_pipeline_cache.bin";
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        for (;;) {
            uint32_t magic = 0, kind = 0, len = 0;
            uint64_t key = 0, key2 = 0;
            if (fread(&magic, 4, 1, f) != 1 || magic != REC_MAGIC) break;
            if (fread(&kind, 4, 1, f) != 1 || fread(&key, 8, 1, f) != 1 || fread(&key2, 8, 1, f) != 1 ||
                fread(&len, 4, 1, f) != 1 || len > (64u << 20))
                break;
            Bytes data(len);
            if (len && fread(data.data(), 1, len, f) != len) break;
            if (kind == REC_COLL) {
                if (Fnv(data.data(), data.size()) == key && s_colls.emplace(key, std::move(data)).second) s_st.collsLoaded++;
            } else if (kind == REC_USE) {
                auto& v = s_baseColls[key];
                if (std::find(v.begin(), v.end(), key2) == v.end()) v.push_back(key2);
            } else if (kind == REC_SYNTH) {
                s_synthKeys.insert(key);
            } else if (kind == REC_RS) {
                std::lock_guard<std::mutex> lk(s_rsM);
                if (Fnv(data.data(), data.size()) == key) s_rsBlob.emplace(key, std::move(data));
                s_rsPersisted.insert(key);
            }
        }
        fclose(f);
    }
    s_file = _wfopen(path.c_str(), L"ab");
    Log("RT CACHE: loaded %llu collections, %zu pipeline bases from %s", (unsigned long long)s_st.collsLoaded,
        s_baseColls.size(), Narrow(path.c_str()).c_str());
}

// ---------------------------------------------------------------- serialization
// Root signatures are written as the hash of their blob, so the same layout created again in a later
// session (a different object) maps to the same record.
bool RsKeyOf(ID3D12RootSignature* rs, uint64_t* key) {
    std::lock_guard<std::mutex> lk(s_rsM);
    auto it = s_rsKey.find(rs);
    if (it == s_rsKey.end()) return false;
    *key = it->second;
    if (!s_rsObj.count(it->second)) {
        rs->AddRef();  // keep the game's object so rebuilt pipelines use the identical root signature
        s_rsObj[it->second] = rs;
    }
    return true;
}

bool SerializeImpl(const D3D12_STATE_OBJECT_DESC* d, bool skipCollections, Writer* w, std::vector<uint64_t>* rsUsed) {
    const UINT n = d->NumSubobjects;
    std::vector<int> idx(n, -1);
    uint32_t count = 0;
    for (UINT i = 0; i < n; ++i) {
        if (d->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION) {
            if (!skipCollections) return false;
            continue;
        }
        idx[i] = (int)count++;
    }
    w->u32((uint32_t)d->Type);
    w->u32(count);
    for (UINT i = 0; i < n; ++i) {
        const D3D12_STATE_SUBOBJECT& s = d->pSubobjects[i];
        if (s.Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION) continue;
        if (!s.pDesc) return false;
        w->u32((uint32_t)s.Type);
        switch (s.Type) {
            case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
                w->u32((uint32_t)((const D3D12_STATE_OBJECT_CONFIG*)s.pDesc)->Flags);
                break;
            case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
            case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE: {
                ID3D12RootSignature* rs = *(ID3D12RootSignature* const*)s.pDesc;
                uint64_t key = 0;
                if (!rs || !RsKeyOf(rs, &key)) return false;
                w->u64(key);
                rsUsed->push_back(key);
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK: w->u32(((const D3D12_NODE_MASK*)s.pDesc)->NodeMask); break;
            case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY: {
                const auto* l = (const D3D12_DXIL_LIBRARY_DESC*)s.pDesc;
                if (!l->DXILLibrary.pShaderBytecode || l->DXILLibrary.BytecodeLength > (16u << 20)) return false;
                w->u32((uint32_t)l->DXILLibrary.BytecodeLength);
                w->raw(l->DXILLibrary.pShaderBytecode, l->DXILLibrary.BytecodeLength);
                w->u32(l->NumExports);
                for (UINT k = 0; k < l->NumExports; ++k) {
                    w->wstr(l->pExports[k].Name);
                    w->wstr(l->pExports[k].ExportToRename);
                    w->u32((uint32_t)l->pExports[k].Flags);
                }
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION: {
                const auto* a = (const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*)s.pDesc;
                const ptrdiff_t t = a->pSubobjectToAssociate - d->pSubobjects;
                if (t < 0 || (UINT)t >= n || idx[t] < 0) return false;
                w->u32((uint32_t)idx[t]);
                w->u32(a->NumExports);
                for (UINT k = 0; k < a->NumExports; ++k) w->wstr(a->pExports[k]);
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION: {
                const auto* a = (const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION*)s.pDesc;
                w->wstr(a->SubobjectToAssociate);
                w->u32(a->NumExports);
                for (UINT k = 0; k < a->NumExports; ++k) w->wstr(a->pExports[k]);
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG: w->raw(s.pDesc, sizeof(D3D12_RAYTRACING_SHADER_CONFIG)); break;
            case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG: w->raw(s.pDesc, sizeof(D3D12_RAYTRACING_PIPELINE_CONFIG)); break;
            case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1: w->raw(s.pDesc, sizeof(D3D12_RAYTRACING_PIPELINE_CONFIG1)); break;
            case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP: {
                const auto* g = (const D3D12_HIT_GROUP_DESC*)s.pDesc;
                w->wstr(g->HitGroupExport);
                w->u32((uint32_t)g->Type);
                w->wstr(g->AnyHitShaderImport);
                w->wstr(g->ClosestHitShaderImport);
                w->wstr(g->IntersectionShaderImport);
                break;
            }
            default: return false;
        }
    }
    return true;
}

bool Serialize(const D3D12_STATE_OBJECT_DESC* d, bool skipCollections, Writer* w, std::vector<uint64_t>* rsUsed) {
    __try {
        return SerializeImpl(d, skipCollections, w, rsUsed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A desc rebuilt from its serialized form, owning every pointer it hands to D3D12.
struct Built {
    std::vector<D3D12_STATE_SUBOBJECT> subs;
    std::deque<Bytes> pods;
    std::deque<std::wstring> strs;
    std::vector<ComPtr<ID3D12RootSignature>> rs;
    template <class T>
    T* Alloc(size_t count = 1) {
        pods.emplace_back(sizeof(T) * std::max<size_t>(count, 1));
        return (T*)pods.back().data();
    }
};

ID3D12RootSignature* ResolveRs(ID3D12Device5* dev, uint64_t key, Built* out) {
    Bytes blob;
    {
        std::lock_guard<std::mutex> lk(s_rsM);
        auto it = s_rsObj.find(key);
        if (it != s_rsObj.end()) {
            out->rs.emplace_back(it->second);
            return it->second;
        }
        auto b = s_rsBlob.find(key);
        if (b == s_rsBlob.end()) return nullptr;
        blob = b->second;
    }
    ComPtr<ID3D12RootSignature> rs;
    if (FAILED(dev->CreateRootSignature(0, blob.data(), blob.size(), IID_PPV_ARGS(&rs)))) return nullptr;
    out->rs.push_back(rs);
    return rs.Get();
}

// extra: number of subobjects the caller appends afterwards. The array must not reallocate, because
// associations point into it.
bool Deserialize(const Bytes& data, ID3D12Device5* dev, size_t extra, Built* out, D3D12_STATE_OBJECT_TYPE* type) {
    Reader r{data.data(), data.size()};
    *type = (D3D12_STATE_OBJECT_TYPE)r.u32();
    const uint32_t count = r.u32();
    if (!r.ok || count > 4096) return false;
    out->subs.reserve(count + extra);
    out->subs.resize(count);
    for (uint32_t i = 0; i < count && r.ok; ++i) {
        const auto type = (D3D12_STATE_SUBOBJECT_TYPE)r.u32();
        D3D12_STATE_SUBOBJECT& s = out->subs[i];
        s.Type = type;
        switch (type) {
            case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG: {
                auto* c = out->Alloc<D3D12_STATE_OBJECT_CONFIG>();
                c->Flags = (D3D12_STATE_OBJECT_FLAGS)r.u32();
                s.pDesc = c;
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
            case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE: {
                ID3D12RootSignature* rs = ResolveRs(dev, r.u64(), out);
                if (!rs) return false;
                auto* p = out->Alloc<ID3D12RootSignature*>();
                *p = rs;
                s.pDesc = p;
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK: {
                auto* m = out->Alloc<D3D12_NODE_MASK>();
                m->NodeMask = r.u32();
                s.pDesc = m;
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY: {
                auto* l = out->Alloc<D3D12_DXIL_LIBRARY_DESC>();
                const uint32_t len = r.u32();
                const uint8_t* code = r.raw(len);
                if (!code) return false;
                out->pods.emplace_back(code, code + len);
                l->DXILLibrary = {out->pods.back().data(), len};
                l->NumExports = r.u32();
                if (l->NumExports > 65536) return false;
                auto* ex = out->Alloc<D3D12_EXPORT_DESC>(l->NumExports);
                for (UINT k = 0; k < l->NumExports && r.ok; ++k) {
                    ex[k].Name = r.wstr(out->strs);
                    ex[k].ExportToRename = r.wstr(out->strs);
                    ex[k].Flags = (D3D12_EXPORT_FLAGS)r.u32();
                }
                l->pExports = l->NumExports ? ex : nullptr;
                s.pDesc = l;
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION: {
                auto* a = out->Alloc<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION>();
                const uint32_t t = r.u32();
                if (t >= count) return false;
                a->pSubobjectToAssociate = &out->subs[t];
                a->NumExports = r.u32();
                if (a->NumExports > 65536) return false;
                auto* names = out->Alloc<LPCWSTR>(a->NumExports);
                for (UINT k = 0; k < a->NumExports && r.ok; ++k) names[k] = r.wstr(out->strs);
                a->pExports = a->NumExports ? names : nullptr;
                s.pDesc = a;
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION: {
                auto* a = out->Alloc<D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION>();
                a->SubobjectToAssociate = r.wstr(out->strs);
                a->NumExports = r.u32();
                if (a->NumExports > 65536) return false;
                auto* names = out->Alloc<LPCWSTR>(a->NumExports);
                for (UINT k = 0; k < a->NumExports && r.ok; ++k) names[k] = r.wstr(out->strs);
                a->pExports = a->NumExports ? names : nullptr;
                s.pDesc = a;
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG: {
                auto* c = out->Alloc<D3D12_RAYTRACING_SHADER_CONFIG>();
                const uint8_t* p = r.raw(sizeof(*c));
                if (!p) return false;
                memcpy(c, p, sizeof(*c));
                s.pDesc = c;
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG: {
                auto* c = out->Alloc<D3D12_RAYTRACING_PIPELINE_CONFIG>();
                const uint8_t* p = r.raw(sizeof(*c));
                if (!p) return false;
                memcpy(c, p, sizeof(*c));
                s.pDesc = c;
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1: {
                auto* c = out->Alloc<D3D12_RAYTRACING_PIPELINE_CONFIG1>();
                const uint8_t* p = r.raw(sizeof(*c));
                if (!p) return false;
                memcpy(c, p, sizeof(*c));
                s.pDesc = c;
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP: {
                auto* g = out->Alloc<D3D12_HIT_GROUP_DESC>();
                g->HitGroupExport = r.wstr(out->strs);
                g->Type = (D3D12_HIT_GROUP_TYPE)r.u32();
                g->AnyHitShaderImport = r.wstr(out->strs);
                g->ClosestHitShaderImport = r.wstr(out->strs);
                g->IntersectionShaderImport = r.wstr(out->strs);
                s.pDesc = g;
                break;
            }
            default: return false;
        }
    }
    return r.ok && r.i == r.n;
}

void PersistRsLocked(const std::vector<uint64_t>& keys) {
    for (uint64_t k : keys) {
        if (s_rsPersisted.count(k)) continue;
        Bytes blob;
        {
            std::lock_guard<std::mutex> lk(s_rsM);
            auto it = s_rsBlob.find(k);
            if (it == s_rsBlob.end()) continue;
            blob = it->second;
        }
        Append(REC_RS, k, 0, &blob);
        s_rsPersisted.insert(k);
    }
}

// ---------------------------------------------------------------- background link
void BuildSuper(ID3D12Device5* dev, RtCreateFn create, uint64_t baseKey, const Bytes& baseSer,
                std::vector<std::pair<uint64_t, Bytes>> colls) {
    const int64_t t0 = Now();
    Built base;
    D3D12_STATE_OBJECT_TYPE type{};
    if (!Deserialize(baseSer, dev, colls.size(), &base, &type)) {
        std::lock_guard<std::mutex> lk(s_m);
        s_st.buildFailed++;
        Log("RT CACHE: could not rebuild the pipeline base");
        return;
    }
    std::vector<ComPtr<ID3D12StateObject>> objs;
    std::vector<D3D12_EXISTING_COLLECTION_DESC> ecd;
    ecd.reserve(colls.size());
    std::unordered_set<uint64_t> keys;
    uint64_t collFailed = 0;
    for (auto& c : colls) {
        Built cb;
        D3D12_STATE_OBJECT_TYPE ct{};
        ComPtr<ID3D12StateObject> so;
        if (!Deserialize(c.second, dev, 0, &cb, &ct) || ct != D3D12_STATE_OBJECT_TYPE_COLLECTION) {
            ++collFailed;
            continue;
        }
        D3D12_STATE_OBJECT_DESC cd{ct, (UINT)cb.subs.size(), cb.subs.data()};
        if (FAILED(create(dev, &cd, IID_PPV_ARGS(&so)))) {
            ++collFailed;
            continue;
        }
        objs.push_back(so);
        ecd.push_back({so.Get(), 0, nullptr});
        base.subs.push_back({D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &ecd.back()});
        keys.insert(c.first);
    }
    D3D12_STATE_OBJECT_DESC pd{type, (UINT)base.subs.size(), base.subs.data()};
    ID3D12StateObject* so = nullptr;
    const HRESULT hr = create(dev, &pd, IID_PPV_ARGS(&so));
    const double ms = TicksToMs(Now() - t0);
    std::lock_guard<std::mutex> lk(s_m);
    if (FAILED(hr) || !so) {
        s_st.buildFailed++;
        s_disabled = true;
        Log("RT CACHE: background link of %zu collections failed (hr=0x%08lX, device removed reason 0x%08lX); "
            "cache disabled for this session",
            keys.size(), (unsigned long)hr, (unsigned long)dev->GetDeviceRemovedReason());
        return;
    }
    if (s_super.so) s_super.so->Release();
    s_super.so = so;
    s_super.base = baseKey;
    s_super.keys = std::move(keys);
    s_st.builds++;
    Log("RT CACHE: prebuilt pipeline ready with %zu collections in %.1f ms (%llu could not be recreated)",
        s_super.keys.size(), ms, (unsigned long long)collFailed);
}

// Collections the prebuilt pipeline should contain: those the game created first, then synthesized
// ones, up to rt_cache_max.
std::vector<uint64_t> SelectLocked(uint64_t baseKey) {
    std::vector<uint64_t> out;
    auto it = s_baseColls.find(baseKey);
    if (it == s_baseColls.end()) return out;
    const size_t cap = (size_t)std::max(1, g_cfg.rtCacheMax);
    for (int pass = 0; pass < 2 && out.size() < cap; ++pass)
        for (uint64_t k : it->second) {
            if (out.size() >= cap) break;
            if ((s_synthKeys.count(k) != 0) != (pass == 1) || !s_colls.count(k)) continue;
            out.push_back(k);
        }
    return out;
}

void WorkerLoop() {
    std::unique_lock<std::mutex> lk(s_m);
    for (;;) {
        s_cv.wait(lk, [] { return s_want; });
        s_want = false;
        s_building = true;
        const uint64_t baseKey = s_wantBase;
        const Bytes baseSer = s_wantBaseSer;
        std::vector<std::pair<uint64_t, Bytes>> colls;
        // A request queued while the previous link was running may already be covered by it.
        const std::vector<uint64_t> want = SelectLocked(baseKey);
        bool covered = s_super.so && s_super.base == baseKey;
        for (size_t i = 0; covered && i < want.size(); ++i) covered = s_super.keys.count(want[i]) != 0;
        if (covered) {
            s_building = false;
            continue;
        }
        for (uint64_t k : want) colls.emplace_back(k, s_colls[k]);
        ID3D12Device5* dev = s_dev;
        RtCreateFn create = s_create;
        lk.unlock();
        if (!colls.empty()) BuildSuper(dev, create, baseKey, baseSer, std::move(colls));
        lk.lock();
        s_building = false;
    }
}

// Schedules a link when the base changed or known collections are missing from the prebuilt pipeline.
void MaybeScheduleLocked(ID3D12Device5* dev, RtCreateFn create, uint64_t baseKey, const Bytes& baseSer) {
    if (s_disabled) return;
    const std::vector<uint64_t> want = SelectLocked(baseKey);
    if (want.empty()) return;
    bool covered = s_super.so && s_super.base == baseKey;
    for (size_t i = 0; covered && i < want.size(); ++i) covered = s_super.keys.count(want[i]) != 0;
    if (covered) return;
    if (!s_dev) {
        dev->AddRef();
        s_dev = dev;
    }
    s_create = create;
    s_wantBase = baseKey;
    s_wantBaseSer = baseSer;
    s_want = true;
    if (!s_workerUp) {
        s_workerUp = true;
        std::thread(WorkerLoop).detach();
    }
    s_cv.notify_one();
}

}  // namespace

void RtCacheOnRootSignature(ID3D12RootSignature* rs, const void* blob, size_t len) {
    if ((!g_cfg.rtCache && !g_cfg.psoReuse) || !rs || !blob || !len) return;
    const uint64_t key = Fnv(blob, len);
    std::lock_guard<std::mutex> lk(s_rsM);
    s_rsKey[rs] = key;
    if (g_cfg.rtCache && !s_rsBlob.count(key))
        s_rsBlob.emplace(key, Bytes((const uint8_t*)blob, (const uint8_t*)blob + len));
}

bool RootSignatureKey(ID3D12RootSignature* rs, uint64_t* key) {
    std::lock_guard<std::mutex> lk(s_rsM);
    auto it = s_rsKey.find(rs);
    if (it == s_rsKey.end()) return false;
    *key = it->second;
    return true;
}

void RtCacheOnCollection(const D3D12_STATE_OBJECT_DESC* d, ID3D12StateObject* so) {
    if (!g_cfg.rtCache || !d || !so || d->Type != D3D12_STATE_OBJECT_TYPE_COLLECTION) return;
    Writer w;
    std::vector<uint64_t> rs;
    const bool ok = Serialize(d, false, &w, &rs);
    std::lock_guard<std::mutex> lk(s_m);
    LoadLocked();
    if (!ok) {
        s_collOf.erase(so);
        return;
    }
    const uint64_t key = Fnv(w.b.data(), w.b.size());
    s_collOf[so] = key;
    if (!s_colls.count(key)) {
        PersistRsLocked(rs);
        Append(REC_COLL, key, 0, &w.b);
        s_colls.emplace(key, std::move(w.b));
        // Named in the log so that gaps in the cache are visible.
        std::string name = "?";
        for (UINT i = 0; i < d->NumSubobjects; ++i)
            if (d->pSubobjects[i].Type == D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP && d->pSubobjects[i].pDesc) {
                name = Narrow(((const D3D12_HIT_GROUP_DESC*)d->pSubobjects[i].pDesc)->HitGroupExport);
                break;
            }
        Log("RT CACHE: new material recorded (not previously in the cache): %s", name.c_str());
    }
}

HRESULT RtCacheCreate(ID3D12Device5* dev, const D3D12_STATE_OBJECT_DESC* d, REFIID riid, void** pp, RtCreateFn create,
                      bool* handled) {
    *handled = false;
    if (!g_cfg.rtCache || !d || d->Type != D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE || !pp) return E_FAIL;
    const int64_t t0 = Now();
    Writer w;
    std::vector<uint64_t> rs;
    const bool ok = Serialize(d, true, &w, &rs);
    std::lock_guard<std::mutex> lk(s_m);
    LoadLocked();
    s_st.requests++;
    if (!ok) return E_FAIL;
    const uint64_t baseKey = Fnv(w.b.data(), w.b.size());
    PersistRsLocked(rs);
    if (!s_lastDev) {
        dev->AddRef();
        s_lastDev = dev;
    }
    s_lastCreate = create;
    s_lastBase = baseKey;
    s_lastBaseSer = w.b;

    // Map the game's collection objects to cache keys.
    std::vector<uint64_t> keys;
    bool allKnown = true;
    for (UINT i = 0; i < d->NumSubobjects; ++i) {
        const D3D12_STATE_SUBOBJECT& s = d->pSubobjects[i];
        if (s.Type != D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION) continue;
        const auto* c = (const D3D12_EXISTING_COLLECTION_DESC*)s.pDesc;
        auto it = c ? s_collOf.find(c->pExistingCollection) : s_collOf.end();
        // Export subsets or renames are not reproduced by the prebuilt pipeline.
        if (it == s_collOf.end() || c->NumExports) {
            allKnown = false;
            continue;
        }
        keys.push_back(it->second);
        auto& v = s_baseColls[baseKey];
        if (std::find(v.begin(), v.end(), it->second) == v.end()) {
            v.push_back(it->second);
            Append(REC_USE, baseKey, it->second, nullptr);
        }
    }

    bool covered = allKnown && s_super.so && s_super.base == baseKey;
    for (size_t i = 0; covered && i < keys.size(); ++i) covered = s_super.keys.count(keys[i]) != 0;
    if (covered && SUCCEEDED(s_super.so->QueryInterface(riid, pp))) {
        *handled = true;
        s_st.hits++;
        Log("RT CACHE: pipeline with %zu collections served from the prebuilt pipeline (%zu) in %.2f ms", keys.size(),
            s_super.keys.size(), TicksToMs(Now() - t0));
        return S_OK;
    }
    if (!allKnown) s_st.missUnknown++;
    else s_st.missNotReady++;
    Log("RT CACHE: pipeline with %zu collections linked by the game (%s)", keys.size(),
        !allKnown      ? "uses a collection that is not in the cache"
        : !s_super.so ? "prebuilt pipeline not ready yet"
                       : "uses a material missing from the prebuilt pipeline");
    MaybeScheduleLocked(dev, create, baseKey, w.b);
    return E_FAIL;
}

// ---------------------------------------------------------------- shader discovery and synthesis
// Every material collection the game creates follows one template: two DXIL libraries
// <MAT>_RayTracing_[closest]_[AO].cpo and <MAT>_RayTracing_[any]_[AO].cpo, exported as
// <MAT>_RayTracing_[ClosestHit]_[AO] and <MAT>_RayTracing_[AnyHit]_[AO], two hit groups, and fixed
// configs and root signatures. The game keeps the DXIL of all its materials in memory, so collections
// for materials it has not used yet can be synthesized and included in the prebuilt pipeline.
// Synthesis is trusted only after it has reproduced collections created by the game byte for byte.
namespace {

struct FoundMat {
    Bytes closest, any;
    bool conflict = false;
};

std::mutex s_scanM;
bool s_scanUp = false;
int s_scanRuns = 0;
uint64_t s_synthMismatch = 0;  // contradictions over all scans; any contradiction disables synthesis

// Offset of the next "DXBC" container header at 4-byte alignment, or SIZE_MAX. Guarded, because the
// game can free a region while it is being scanned.
size_t FindDxbc(const uint8_t* base, size_t len, size_t start, bool* fault) {
    __try {
        for (size_t i = start; i + 32 <= len; i += 4)
            if (*(const uint32_t*)(base + i) == 0x43425844u) return i;
        return SIZE_MAX;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *fault = true;
        return SIZE_MAX;
    }
}

bool ReadMem(const void* p, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}

// Total size and debug name (ILDN part) of the container at p.
bool ContainerInfo(const uint8_t* p, size_t avail, uint32_t* total, std::string* name) {
    uint8_t hdr[32 + 64 * 4];
    if (avail < 32 || !ReadMem(p, hdr, 32)) return false;
    uint32_t parts = 0;
    memcpy(total, hdr + 24, 4);
    memcpy(&parts, hdr + 28, 4);
    if (*total < 32 || *total > (4u << 20) || *total > avail || parts == 0 || parts > 64 || 32 + parts * 4 > *total)
        return false;
    if (!ReadMem(p + 32, hdr + 32, parts * 4)) return false;
    for (uint32_t i = 0; i < parts; ++i) {
        uint32_t off = 0;
        memcpy(&off, hdr + 32 + i * 4, 4);
        if (off + 12 > *total) return false;
        uint8_t ph[12];
        if (!ReadMem(p + off, ph, 12)) return false;
        if (memcmp(ph, "ILDN", 4) != 0) continue;
        uint32_t psize = 0;
        uint16_t nlen = 0;
        memcpy(&psize, ph + 4, 4);
        memcpy(&nlen, ph + 10, 2);
        if (off + 8 + psize > *total || 4u + nlen > psize || nlen == 0 || nlen > 1024) return false;
        std::string s(nlen, '\0');
        if (!ReadMem(p + off + 12, &s[0], nlen)) return false;
        s.resize(strnlen(s.c_str(), nlen));
        *name = s;
        return true;
    }
    return false;
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

void ScanMemory(std::map<std::string, FoundMat>& found, uint64_t* bytes, uint64_t* containers, uint64_t* faults) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const char* kinds[2] = {"_raytracing_[closest]_[ao]", "_raytracing_[any]_[ao]"};
    for (uint8_t* p = (uint8_t*)si.lpMinimumApplicationAddress; p < (uint8_t*)si.lpMaximumApplicationAddress;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
        const DWORD prot = mbi.Protect & 0xFF;
        const bool readable = prot == PAGE_READWRITE || prot == PAGE_READONLY || prot == PAGE_WRITECOPY;
        // Write-combined and uncached pages map GPU heaps. They hold no shader containers and are
        // very slow to read.
        const bool slow = (mbi.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE)) != 0;
        if (mbi.State == MEM_COMMIT && readable && !slow && !(mbi.Protect & PAGE_GUARD) && mbi.Type != MEM_IMAGE) {
            const uint8_t* base = (const uint8_t*)mbi.BaseAddress;
            const size_t len = mbi.RegionSize;
            *bytes += len;
            size_t i = 0;
            for (;;) {
                bool fault = false;
                i = FindDxbc(base, len, i, &fault);
                if (fault) ++*faults;
                if (i == SIZE_MAX) break;
                uint32_t total = 0;
                std::string name;
                if (!ContainerInfo(base + i, len - i, &total, &name)) {
                    i += 4;
                    continue;
                }
                ++*containers;
                const std::string low = Lower(name);
                const size_t slash = name.find_last_of("\\/");
                const std::string file = slash == std::string::npos ? name : name.substr(slash + 1);
                const std::string lowFile = Lower(file);
                for (int k = 0; k < 2; ++k) {
                    const size_t at = lowFile.find(kinds[k]);
                    if (at == std::string::npos || at == 0) continue;
                    Bytes code(total);
                    if (!ReadMem(base + i, code.data(), total)) break;
                    FoundMat& m = found[file.substr(0, at)];
                    Bytes& slot = k ? m.any : m.closest;
                    if (slot.empty()) slot = std::move(code);
                    else if (slot != code) m.conflict = true;
                }
                i += (total + 3) & ~3u;
            }
        }
        p = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
    }
}

struct Template {
    uint8_t shcfg[8], plcfg[4];
    uint64_t grs = 0, lrs = 0;
    uint32_t assocTarget = 0;
};

// Extracts the fixed parts of a recorded material collection and verifies its layout.
bool ParseTemplate(const Bytes& rec, Template* t) {
    static const uint32_t kShape[9] = {9, 10, 1, 5, 5, 11, 11, 2, 7};
    std::deque<std::wstring> scratch;
    Reader r{rec.data(), rec.size()};
    if (r.u32() != (uint32_t)D3D12_STATE_OBJECT_TYPE_COLLECTION || r.u32() != 9) return false;
    for (int i = 0; i < 9 && r.ok; ++i) {
        if (r.u32() != kShape[i]) return false;
        switch (kShape[i]) {
            case 9: {
                const uint8_t* p = r.raw(8);
                if (p) memcpy(t->shcfg, p, 8);
                break;
            }
            case 10: {
                const uint8_t* p = r.raw(4);
                if (p) memcpy(t->plcfg, p, 4);
                break;
            }
            case 1: t->grs = r.u64(); break;
            case 2: t->lrs = r.u64(); break;
            case 5: {
                r.raw(r.u32());
                const uint32_t ne = r.u32();
                for (uint32_t k = 0; k < ne && r.ok; ++k) {
                    r.wstr(scratch);
                    r.wstr(scratch);
                    r.u32();
                }
                break;
            }
            case 11:
                r.wstr(scratch);
                r.u32();
                r.wstr(scratch);
                r.wstr(scratch);
                r.wstr(scratch);
                break;
            case 7: {
                t->assocTarget = r.u32();
                const uint32_t ne = r.u32();
                for (uint32_t k = 0; k < ne && r.ok; ++k) r.wstr(scratch);
                break;
            }
        }
    }
    return r.ok && r.i == r.n;
}

Bytes Synthesize(const Template& t, const std::string& mat, const FoundMat& f) {
    const std::wstring m(mat.begin(), mat.end());
    const std::wstring ch = m + L"_RayTracing_[ClosestHit]_[AO]", ah = m + L"_RayTracing_[AnyHit]_[AO]";
    const std::wstring hg = L"HitGroup_" + m + L"_[AO]", hga = hg + L"_[A]";
    Writer w;
    w.u32((uint32_t)D3D12_STATE_OBJECT_TYPE_COLLECTION);
    w.u32(9);
    w.u32(9), w.raw(t.shcfg, 8);
    w.u32(10), w.raw(t.plcfg, 4);
    w.u32(1), w.u64(t.grs);
    w.u32(5), w.u32((uint32_t)f.closest.size()), w.raw(f.closest.data(), f.closest.size());
    w.u32(1), w.wstr(ch.c_str()), w.wstr(L"ClosestHit"), w.u32(0);
    w.u32(5), w.u32((uint32_t)f.any.size()), w.raw(f.any.data(), f.any.size());
    w.u32(1), w.wstr(ah.c_str()), w.wstr(L"AnyHit"), w.u32(0);
    w.u32(11), w.wstr(hg.c_str()), w.u32(0), w.wstr(nullptr), w.wstr(ch.c_str()), w.wstr(nullptr);
    w.u32(11), w.wstr(hga.c_str()), w.u32(0), w.wstr(ah.c_str()), w.wstr(ch.c_str()), w.wstr(nullptr);
    w.u32(2), w.u64(t.lrs);
    w.u32(7), w.u32(t.assocTarget), w.u32(2), w.wstr(hg.c_str()), w.wstr(hga.c_str());
    return w.b;
}

bool HasMaterial(const Bytes& rec, const std::string& mat) {
    const std::wstring hg = L"HitGroup_" + std::wstring(mat.begin(), mat.end()) + L"_[AO]";
    const char* needle = (const char*)hg.data();
    const size_t nlen = hg.size() * sizeof(wchar_t);
    return std::search(rec.begin(), rec.end(), needle, needle + nlen) != rec.end();
}

void ScanOnce() {
    const int64_t t0 = Now();
    std::map<std::string, FoundMat> found;
    uint64_t bytes = 0, containers = 0, faults = 0;
    ScanMemory(found, &bytes, &containers, &faults);
    const double scanMs = TicksToMs(Now() - t0);

    size_t pairs = 0;
    uint64_t verified = 0, mismatch = 0, added = 0, skipped = 0;
    std::vector<std::string> addedNames, mismatchNames;
    {
        std::lock_guard<std::mutex> lk(s_m);
        LoadLocked();
        // Take the template from a collection created by the game.
        Template t{};
        bool haveT = false;
        uint64_t tKey = 0;
        for (auto& c : s_colls)
            if (!s_synthKeys.count(c.first) && ParseTemplate(c.second, &t)) {
                haveT = true;
                tKey = c.first;
                break;
            }
        std::vector<std::pair<std::string, Bytes>> candidates;
        if (haveT) {
            for (auto& f : found) {
                if (f.second.conflict || f.second.closest.empty() || f.second.any.empty()) continue;
                ++pairs;
                Bytes rec = Synthesize(t, f.first, f.second);
                const uint64_t key = Fnv(rec.data(), rec.size());
                if (s_colls.count(key)) {
                    if (!s_synthKeys.count(key)) ++verified;
                    continue;
                }
                // The game created this material differently: the template does not apply.
                bool recorded = false;
                for (auto& c : s_colls)
                    if (!s_synthKeys.count(c.first) && HasMaterial(c.second, f.first)) recorded = true;
                if (recorded) {
                    ++mismatch;
                    mismatchNames.push_back(f.first);
                    continue;
                }
                candidates.emplace_back(f.first, std::move(rec));
            }
        }
        // Trust synthesis only after it reproduced recorded collections and never contradicted one.
        const bool trusted = verified >= 3 && mismatch == 0 && s_synthMismatch == 0;
        if (trusted) {
            std::vector<uint64_t> bases;
            for (auto& b : s_baseColls)
                if (std::find(b.second.begin(), b.second.end(), tKey) != b.second.end()) bases.push_back(b.first);
            for (auto& c : candidates) {
                const uint64_t key = Fnv(c.second.data(), c.second.size());
                Append(REC_COLL, key, 0, &c.second);
                Append(REC_SYNTH, key, 0, nullptr);
                s_colls.emplace(key, std::move(c.second));
                s_synthKeys.insert(key);
                for (uint64_t b : bases) {
                    s_baseColls[b].push_back(key);
                    Append(REC_USE, b, key, nullptr);
                }
                ++added;
                addedNames.push_back(c.first);
            }
            if (added && s_lastDev) MaybeScheduleLocked(s_lastDev, s_lastCreate, s_lastBase, s_lastBaseSer);
        } else {
            skipped = candidates.size();
        }
    }
    std::lock_guard<std::mutex> lk(s_scanM);
    s_scanRuns++;
    s_synthMismatch += mismatch;
    Log("RT SCAN #%d: %.0f MB in %.0f ms (%llu read faults), %llu shader containers, %zu RT materials in memory "
        "(%zu complete); synthesis reproduced %llu recorded collections, contradicted %llu; added %llu materials, "
        "held back %llu",
        s_scanRuns, (double)bytes / (1024.0 * 1024.0), scanMs, (unsigned long long)faults,
        (unsigned long long)containers, found.size(), pairs, (unsigned long long)verified,
        (unsigned long long)mismatch, (unsigned long long)added, (unsigned long long)skipped);
    std::string list;
    for (auto& n : addedNames) list += n + " ";
    if (!list.empty()) Log("RT SCAN added: %s", list.c_str());
    list.clear();
    for (auto& n : mismatchNames) list += n + " ";
    if (!list.empty()) Log("RT SCAN template mismatch: %s", list.c_str());
}

}  // namespace

void RtScanStart() {
    if (!g_cfg.rtScan || !g_cfg.rtCache) return;
    std::lock_guard<std::mutex> lk(s_scanM);
    if (s_scanUp) return;
    s_scanUp = true;
    std::thread([] {
        Sleep(20000);
        for (;;) {
            ScanOnce();
            Sleep((DWORD)std::max(5, g_cfg.rtScanSeconds) * 1000);
        }
    }).detach();
}

void RtCacheLogSummary() {
    if (!g_cfg.rtCache) return;
    std::lock_guard<std::mutex> lk(s_m);
    Log("RT CACHE: %zu known collections; pipeline requests %llu, served from the prebuilt pipeline %llu, "
        "missing material or not ready %llu, unknown collection %llu; background links %llu, failed %llu",
        s_colls.size(), (unsigned long long)s_st.requests, (unsigned long long)s_st.hits,
        (unsigned long long)s_st.missNotReady, (unsigned long long)s_st.missUnknown, (unsigned long long)s_st.builds,
        (unsigned long long)s_st.buildFailed);
}

}  // namespace errt
