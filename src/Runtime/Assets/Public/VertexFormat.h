#pragma once
#include <cmath>
#include <cstdint>

// -----------------------------------------------------------------------
// Vertex — engine canonical vertex format (32 bytes, 16-byte aligned).
//
// All meshes (imported or procedural) use this layout. The GPU reads
// vertices via buffer device address (BDA) — no vertex-attribute bindings.
//
// Normals and tangent directions are octahedral-encoded into a single
// uint32 each (2× snorm16), giving better angular precision than
// 10-10-10-2 at the same 4-byte cost.
//
// Material convention (locked for pipeline alignment):
//   - Metal/rough PBR workflow
//   - Normal maps: tangent-space
//   - Texture packing: ORM (Occlusion-Roughness-Metallic)
//   - BaseColor: sRGB
// -----------------------------------------------------------------------

struct alignas(16) Vertex
{
	float px, py, pz;   // 12B — position
	uint32_t n_oct16x2; //  4B — normal  (oct-encoded 2× snorm16)
	float u, v;         //  8B — primary UV
	uint32_t t_oct16x2; //  4B — tangent dir (oct-encoded 2× snorm16)
	uint16_t mask;      //  2B — generic mask (blend weight / AO / curvature)
	uint8_t flags;      //  1B — bit0 = tangent handedness sign
	uint8_t pad;        //  1B — pad to 32
};

static_assert(sizeof(Vertex) == 32, "Vertex must be exactly 32 bytes");
static_assert(alignof(Vertex) == 16, "Vertex must be 16-byte aligned");

// -----------------------------------------------------------------------
// Octahedral encoding — maps unit sphere ↔ [-1,1]² octahedron
// Stored as two snorm16 packed into a uint32.
// -----------------------------------------------------------------------

inline uint32_t OctEncode(float nx, float ny, float nz)
{
	float absSum = fabsf(nx) + fabsf(ny) + fabsf(nz);
	float ox     = nx / absSum;
	float oy     = ny / absSum;
	if (nz < 0.0f)
	{
		float tmpX = (1.0f - fabsf(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
		float tmpY = (1.0f - fabsf(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
		ox         = tmpX;
		oy         = tmpY;
	}
	auto sx = static_cast<int16_t>(ox * 32767.0f);
	auto sy = static_cast<int16_t>(oy * 32767.0f);
	return static_cast<uint32_t>(static_cast<uint16_t>(sx))
		| (static_cast<uint32_t>(static_cast<uint16_t>(sy)) << 16);
}

inline void OctDecode(uint32_t packed, float& nx, float& ny, float& nz)
{
	auto sx  = static_cast<int16_t>(packed & 0xFFFF);
	auto sy  = static_cast<int16_t>(packed >> 16);
	float ex = static_cast<float>(sx) / 32767.0f;
	float ey = static_cast<float>(sy) / 32767.0f;
	nz       = 1.0f - fabsf(ex) - fabsf(ey);
	if (nz < 0.0f)
	{
		float tmpX = (1.0f - fabsf(ey)) * (ex >= 0.0f ? 1.0f : -1.0f);
		float tmpY = (1.0f - fabsf(ex)) * (ey >= 0.0f ? 1.0f : -1.0f);
		ex         = tmpX;
		ey         = tmpY;
	}
	nx        = ex;
	ny        = ey;
	float len = sqrtf(nx * nx + ny * ny + nz * nz);
	if (len > 0.0f)
	{
		nx /= len;
		ny /= len;
		nz /= len;
	}
}