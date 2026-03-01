// cube.vert.slang — Vertex shader.
//
// All data arrives via buffer device address (BDA) — no vertex-attribute
// bindings are used.  A single uint64_t push constant carries the address of
// GpuFrameData, from which the shader obtains:
//   - VerticesAddr   — packed Vertex[24]  (float4 pos + float4 normal, 32 B each)
//   - InstancesAddr  — SoA compact instances written by scatter.slang
//   - ViewProj       — column-major view-projection matrix
//
// SoA read for instance instIdx:
//   field k = semantic - 1;  value = InstancesAddr[ k * OutFieldStride + instIdx ]

#include "GpuFrameData.slang"

[[vk::push_constant]]
cbuffer PushConstants { uint64_t gFrameDataAddr; }

// 32-byte vertex matching CubeMesh::Vertex in CubeMesh.h.
struct Vertex
{
    float4 position;  // xyz + w=1
    float4 normal;    // xyz + w=0
};

struct VSOut
{
    float4 pos   : SV_Position;
    float4 color : COLOR0;
};

// Build a TRS model matrix from translation t, Euler-XYZ rotation r (radians),
// and per-axis scale s.  Returns a row-major float4x4 (Slang M[row][col]).
//
// Rotation: R = Rz * Ry * Rx (intrinsic XYZ order)
//   M[row][col] = R[row,col] * scale[col]  (for cols 0..2)
//   M[row][3]   = translation[row]          (col 3)
float4x4 buildModel(float3 t, float3 r, float3 s)
{
    float cx = cos(r.x), sx = sin(r.x);
    float cy = cos(r.y), sy = sin(r.y);
    float cz = cos(r.z), sz = sin(r.z);

    float4x4 m;

    // Row 0
    m[0][0] =  cy * cz                    * s.x;
    m[0][1] = (cz * sx * sy - cx * sz)    * s.y;
    m[0][2] = (cx * cz * sy + sx * sz)    * s.z;
    m[0][3] =  t.x;

    // Row 1
    m[1][0] =  cy * sz                    * s.x;
    m[1][1] = (cx * cz + sx * sy * sz)    * s.y;
    m[1][2] = (cx * sy * sz - cz * sx)    * s.z;
    m[1][3] =  t.y;

    // Row 2
    m[2][0] = -sy                          * s.x;
    m[2][1] =  cy * sx                     * s.y;
    m[2][2] =  cx * cy                     * s.z;
    m[2][3] =  t.z;

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
VSOut main(uint vertIdx : SV_VertexID, uint instIdx : SV_InstanceID)
{
    GpuFrameData* fd = (GpuFrameData*)gFrameDataAddr;

    // --- Mesh vertex (BDA) ---
    Vertex v = ((Vertex*)fd->VerticesAddr)[vertIdx];

    // --- Per-instance SoA data ---
    // Layout: slot k for instance i  =  InstancesAddr[ k * OutFieldStride + i ]
    float* inst   = (float*)fd->InstancesAddr;
    uint   stride = fd->OutFieldStride;

    float3 pos   = float3(inst[(kSemPosX   - 1u) * stride + instIdx],
                          inst[(kSemPosY   - 1u) * stride + instIdx],
                          inst[(kSemPosZ   - 1u) * stride + instIdx]);

    float3 rot   = float3(inst[(kSemRotX   - 1u) * stride + instIdx],
                          inst[(kSemRotY   - 1u) * stride + instIdx],
                          inst[(kSemRotZ   - 1u) * stride + instIdx]);

    float3 scale = float3(inst[(kSemScaleX - 1u) * stride + instIdx],
                          inst[(kSemScaleY - 1u) * stride + instIdx],
                          inst[(kSemScaleZ - 1u) * stride + instIdx]);

    float4 color = float4(inst[(kSemColorR - 1u) * stride + instIdx],
                          inst[(kSemColorG - 1u) * stride + instIdx],
                          inst[(kSemColorB - 1u) * stride + instIdx],
                          inst[(kSemColorA - 1u) * stride + instIdx]);

    // --- Transform ---
    float4x4 model = buildModel(pos, rot, scale);
    float4x4 vp    = loadViewProj(fd);

    float4 worldPos = mul(model, v.position);
    float4 clipPos  = mul(vp, worldPos);

    VSOut o;
    o.pos   = clipPos;
    o.color = color;
    return o;
}
