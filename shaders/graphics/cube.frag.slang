// cube.frag.slang — Fragment shader.
// Simple directional + ambient lighting using interpolated world-space normals.
//
// When TNX_GPU_PICKING is enabled, outputs to a second R32_UINT attachment
// containing the entity's cache index for click-to-select resolution.

#include "common/GpuFrameData.slang"

struct FSIn
{
    float4 color    : COLOR0;
    float3 worldNrm : NORMAL0;
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
    // Hardcoded sun direction (top-right-forward, normalized)
    static const float3 lightDir = normalize(float3(0.4, 0.7, -0.5));
    static const float  ambient  = 0.15;

    float3 N   = normalize(i.worldNrm);
    float  NdL = saturate(dot(N, lightDir));

    // Half-Lambert wrap for softer shadows
    float  diffuse = NdL * 0.7 + 0.3;
    float3 lit     = i.color.rgb * max(diffuse, ambient);

    FSOut o;
    o.color = float4(lit, 1.0f);
#ifdef TNX_GPU_PICKING
    o.entityIdx = i.entityCacheIdx;
#endif
    return o;
}