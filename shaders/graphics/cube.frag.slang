// cube.frag.slang — Fragment shader.
// Receives interpolated color from the vertex shader and outputs it directly.
// Normal-based lighting can be added once camera setup is confirmed working.
//
// When TNX_GPU_PICKING is enabled, outputs to a second R32_UINT attachment
// containing the entity's cache index for click-to-select resolution.

#include "common/GpuFrameData.slang"

struct FSIn
{
    float4 color : COLOR0;
#ifdef TNX_GPU_PICKING
    nointerpolation uint entityCacheIdx : ENTITYID0;
#endif
};

struct FSOut
{
    float4 color     : SV_Target0;
#ifdef TNX_GPU_PICKING
    uint   entityIdx : SV_Target1;
#endif
};

[shader("fragment")]
FSOut main(FSIn i)
{
    FSOut o;
    o.color = float4(i.color.rgb, 1.0f);
#ifdef TNX_GPU_PICKING
    o.entityIdx = i.entityCacheIdx;
#endif
    return o;
}