// cube.vert.slang — Vertex shader.
//
// All data arrives via buffer device address (BDA) — no vertex-attribute
// bindings are used.  A single uint64_t push constant carries the address of
// GpuFrameData, from which the shader obtains:
//   - VerticesAddr   — packed Vertex[N]  (32 bytes each, oct-encoded normals)
//   - InstancesAddr  — SoA compact instances written by scatter.slang
//   - ViewProj       — column-major view-projection matrix
//
// Rotation is stored as quaternion (x,y,z,w). The vertex shader normalizes
// the quaternion to correct for linear interpolation drift from scatter.slang.
//
// SoA read for instance instIdx:
//   field k = semantic - 1;  value = InstancesAddr[ k * OutFieldStride + instIdx ]

#include "common/GpuFrameData.slang"

[[vk::push_constant]]
cbuffer PushConstants { uint64_t gFrameDataAddr; }

// 32-byte vertex matching Vertex in VertexFormat.h.
// float3 pos (12B) + oct normal uint32 (4B) + float2 UV (8B) +
// oct tangent uint32 (4B) + uint16 mask (2B) + uint8 flags (1B) + uint8 pad (1B)
struct Vertex
{
    float3   pos;           // 12 bytes
    uint     n_oct16x2;     //  4 bytes — octahedral-encoded normal
    float2   uv;            //  8 bytes
    uint     t_oct16x2;     //  4 bytes — octahedral-encoded tangent direction
    uint     mask_flags;    //  4 bytes — mask(16) + flags(8) + pad(8)
};

struct VSOut
{
    float4 pos      : SV_Position;
    float4 color    : COLOR0;
    float3 worldNrm : NORMAL0;
#ifdef TNX_GPU_PICKING
    nointerpolation uint entityCacheIdx : ENTITYID0;
#endif
};

// Decode octahedral-encoded normal from packed uint32 (2× snorm16).
float3 octDecode(uint packed)
{
    float2 e = float2(
        float(int(packed << 16) >> 16) / 32767.0,
        float(int(packed) >> 16) / 32767.0
    );
    float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
    {
        float2 s = float2(n.x >= 0 ? 1 : -1, n.y >= 0 ? 1 : -1);
        n.xy = (1.0 - abs(n.yx)) * s;
    }
    return normalize(n);
}

// Build a TRS model matrix from translation t, unit quaternion q (x,y,z,w),
// and per-axis scale s.  Returns a row-major float4x4 (Slang M[row][col]).
//
// Quaternion is normalized before use to handle interpolation drift.
// Rotation matrix R from q, then M[row][col] = R[row,col] * scale[col].
float4x4 buildModel(float3 t, float4 q, float3 s)
{
    // Normalize to handle lerp drift from scatter pass
    float invLen = rsqrt(dot(q, q));
    q *= invLen;

    float x = q.x, y = q.y, z = q.z, w = q.w;
    float x2 = x + x, y2 = y + y, z2 = z + z;
    float xx = x * x2, xy = x * y2, xz = x * z2;
    float yy = y * y2, yz = y * z2, zz = z * z2;
    float wx = w * x2, wy = w * y2, wz = w * z2;

    float4x4 m;

    // Row 0
    m[0][0] = (1.0f - (yy + zz)) * s.x;
    m[0][1] = (xy - wz)          * s.y;
    m[0][2] = (xz + wy)          * s.z;
    m[0][3] = t.x;

    // Row 1
    m[1][0] = (xy + wz)          * s.x;
    m[1][1] = (1.0f - (xx + zz)) * s.y;
    m[1][2] = (yz - wx)          * s.z;
    m[1][3] = t.y;

    // Row 2
    m[2][0] = (xz - wy)          * s.x;
    m[2][1] = (yz + wx)          * s.y;
    m[2][2] = (1.0f - (xx + yy)) * s.z;
    m[2][3] = t.z;

    // Row 3
    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = 0.0f;
    m[3][3] = 1.0f;

    return m;
}

// Load the C++ column-major float[16] from GpuFrameData into a Slang row-major
// float4x4.  C++ stores m[col * 4 + row]; Slang accesses M[row][col].
float4x4 loadViewProj(GpuFrameData* fd)
{
    float4x4 vp;
    vp[0] = float4(fd->ViewProj[0],  fd->ViewProj[4],  fd->ViewProj[8],  fd->ViewProj[12]);
    vp[1] = float4(fd->ViewProj[1],  fd->ViewProj[5],  fd->ViewProj[9],  fd->ViewProj[13]);
    vp[2] = float4(fd->ViewProj[2],  fd->ViewProj[6],  fd->ViewProj[10], fd->ViewProj[14]);
    vp[3] = float4(fd->ViewProj[3],  fd->ViewProj[7],  fd->ViewProj[11], fd->ViewProj[15]);
    return vp;
}

[shader("vertex")]
VSOut main(uint vertIdx : SV_VertexID, uint baseVert : SV_StartVertexLocation,
           uint instIdx : SV_VulkanInstanceID)
{
    GpuFrameData* fd = (GpuFrameData*)gFrameDataAddr;

    // --- Mesh vertex (BDA) ---
    // Slang's SV_VertexID = gl_VertexIndex - BaseVertex (DX12 convention).
    // Since we index the mega-buffer via BDA, we need the full gl_VertexIndex
    // which includes VertexOffset from the indirect draw command.
    Vertex vtx = ((Vertex*)fd->VerticesAddr)[vertIdx + baseVert];

    // Decode oct-encoded normal (unused in current color-only shader,
    // but read here for future GBuffer / lighting passes).
    float3 normal = octDecode(vtx.n_oct16x2);

    // --- Per-instance SoA data ---
    // Layout: slot k for instance i  =  InstancesAddr[ k * OutFieldStride + i ]
    float* inst   = (float*)fd->InstancesAddr;
    uint   stride = fd->OutFieldStride;

    float3 pos   = float3(inst[(kSemPosX   - 1u) * stride + instIdx],
                          inst[(kSemPosY   - 1u) * stride + instIdx],
                          inst[(kSemPosZ   - 1u) * stride + instIdx]);

    float4 rotQ  = float4(inst[(kSemRotQx  - 1u) * stride + instIdx],
                          inst[(kSemRotQy  - 1u) * stride + instIdx],
                          inst[(kSemRotQz  - 1u) * stride + instIdx],
                          inst[(kSemRotQw  - 1u) * stride + instIdx]);

    float3 scale = float3(inst[(kSemScaleX - 1u) * stride + instIdx],
                          inst[(kSemScaleY - 1u) * stride + instIdx],
                          inst[(kSemScaleZ - 1u) * stride + instIdx]);

    float4 color = float4(inst[(kSemColorR - 1u) * stride + instIdx],
                          inst[(kSemColorG - 1u) * stride + instIdx],
                          inst[(kSemColorB - 1u) * stride + instIdx],
                          inst[(kSemColorA - 1u) * stride + instIdx]);

    // --- Transform ---
    float4x4 model = buildModel(pos, rotQ, scale);
    float4x4 vp    = loadViewProj(fd);

    float4 worldPos = mul(model, float4(vtx.pos, 1.0));
    float4 clipPos  = mul(vp, worldPos);

    // Rotate normal by the instance quaternion (no scale — normals are unit vectors)
    float3 worldNormal = normal + 2.0 * cross(rotQ.xyz, cross(rotQ.xyz, normal) + rotQ.w * normal);

    VSOut o;
    o.pos      = clipPos;
    o.color    = color;
    o.worldNrm = worldNormal;
#ifdef TNX_GPU_PICKING
    // Read cache index written by scatter as asfloat(uint) — reinterpret back.
    o.entityCacheIdx = asuint(inst[(kSemEntityCacheIdx - 1u) * stride + instIdx]);
#endif
    return o;
}
