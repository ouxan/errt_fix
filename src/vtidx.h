#pragma once

typedef struct VtIndices {
    int dev_CreateGraphicsPipelineState;
    int dev_CreateComputePipelineState;
    int dev_CreatePipelineState;
    int dev_CreateStateObject;
    int dev_CreateRootSignature;

    int cl_BuildRaytracingAccelerationStructure;

    int sc_Present;
    int sc_Present1;
} VtIndices;

#ifdef __cplusplus
extern "C" {
#endif
extern const VtIndices g_vt;
#ifdef __cplusplus
}
#endif
