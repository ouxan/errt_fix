/* Compiled as C so the SDK's C vtable structs give exact method slots instead of hand-counted indices. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <stddef.h>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include "vtidx.h"

#define IDX(iface, method) ((int)(offsetof(iface##Vtbl, method) / sizeof(void*)))

const VtIndices g_vt = {
    IDX(ID3D12Device8, CreateGraphicsPipelineState),
    IDX(ID3D12Device8, CreateComputePipelineState),
    IDX(ID3D12Device8, CreatePipelineState),
    IDX(ID3D12Device8, CreateStateObject),
    IDX(ID3D12Device8, CreateRootSignature),

    IDX(ID3D12GraphicsCommandList4, BuildRaytracingAccelerationStructure),

    IDX(IDXGISwapChain1, Present),
    IDX(IDXGISwapChain1, Present1),
};
