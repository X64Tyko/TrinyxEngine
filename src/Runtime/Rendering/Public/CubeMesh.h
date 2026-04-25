#pragma once
#include <cstdint>

#include "VertexFormat.h"

// -----------------------------------------------------------------------
// CubeMesh — static cube geometry in the engine's canonical vertex format.
//
// Runtime vertex format (32 bytes, 16-byte aligned):
//   float3 position + oct16x2 normal + float2 UV + oct16x2 tangent
//   + uint16 mask + uint8 flags + uint8 pad
//
// This is the binary format the offline mesh importer targets.  Any source
// asset (GLTF, FBX, OBJ) is pre-processed into this layout at import time,
// not at runtime.  The GPU reads vertices via buffer device address
// (BDA) — no vertex-attribute bindings are used in the pipeline.
//
// Indices are uint32 for consistency with the mega-buffer pipeline.
//
// NOTE: CubeMesh is retained for backward compatibility and tests.
// The actual cube geometry used by the renderer is registered via
// MeshManager::RegisterBuiltinCube(), which uses the same data.
// -----------------------------------------------------------------------
namespace CubeMesh
{
	inline Vertex MakeVert(float px, float py, float pz, float nx, float ny, float nz)
	{
		Vertex v{};
		v.px        = px;
		v.py        = py;
		v.pz        = pz;
		v.n_oct16x2 = OctEncode(nx, ny, nz);
		v.u         = 0.0f;
		v.v         = 0.0f;
		v.t_oct16x2 = OctEncode(0.0f, 0.0f, 1.0f); // default tangent +Z
		v.mask      = 0;
		v.flags     = 0;
		v.pad       = 0;
		return v;
	}

	// 24 unique vertices — 4 per face — so each face can have its own
	// flat normal without sharing vertices across faces.
	// Winding: CCW front-face (Vulkan default).
	inline const Vertex Vertices[] = {
		// +Z front face  (normal  0, 0, 1)
		MakeVert(-0.5f, -0.5f, 0.5f, 0, 0, 1),
		MakeVert(0.5f, -0.5f, 0.5f, 0, 0, 1),
		MakeVert(0.5f, 0.5f, 0.5f, 0, 0, 1),
		MakeVert(-0.5f, 0.5f, 0.5f, 0, 0, 1),
		// -Z back face   (normal  0, 0,-1)
		MakeVert(0.5f, -0.5f, -0.5f, 0, 0, -1),
		MakeVert(-0.5f, -0.5f, -0.5f, 0, 0, -1),
		MakeVert(-0.5f, 0.5f, -0.5f, 0, 0, -1),
		MakeVert(0.5f, 0.5f, -0.5f, 0, 0, -1),
		// +X right face  (normal  1, 0, 0)
		MakeVert(0.5f, -0.5f, 0.5f, 1, 0, 0),
		MakeVert(0.5f, -0.5f, -0.5f, 1, 0, 0),
		MakeVert(0.5f, 0.5f, -0.5f, 1, 0, 0),
		MakeVert(0.5f, 0.5f, 0.5f, 1, 0, 0),
		// -X left face   (normal -1, 0, 0)
		MakeVert(-0.5f, -0.5f, -0.5f, -1, 0, 0),
		MakeVert(-0.5f, -0.5f, 0.5f, -1, 0, 0),
		MakeVert(-0.5f, 0.5f, 0.5f, -1, 0, 0),
		MakeVert(-0.5f, 0.5f, -0.5f, -1, 0, 0),
		// +Y top face    (normal  0, 1, 0)
		MakeVert(-0.5f, 0.5f, 0.5f, 0, 1, 0),
		MakeVert(0.5f, 0.5f, 0.5f, 0, 1, 0),
		MakeVert(0.5f, 0.5f, -0.5f, 0, 1, 0),
		MakeVert(-0.5f, 0.5f, -0.5f, 0, 1, 0),
		// -Y bottom face (normal  0,-1, 0)
		MakeVert(-0.5f, -0.5f, -0.5f, 0, -1, 0),
		MakeVert(0.5f, -0.5f, -0.5f, 0, -1, 0),
		MakeVert(0.5f, -0.5f, 0.5f, 0, -1, 0),
		MakeVert(-0.5f, -0.5f, 0.5f, 0, -1, 0),
	};

	// Two triangles per face × 6 faces = 12 triangles = 36 indices.
	// uint32 for consistency with the mega-buffer pipeline.
	constexpr uint32_t Indices[] = {
		0, 1, 2, 2, 3, 0,       // +Z front
		4, 5, 6, 6, 7, 4,       // -Z back
		8, 9, 10, 10, 11, 8,    // +X right
		12, 13, 14, 14, 15, 12, // -X left
		16, 17, 18, 18, 19, 16, // +Y top
		20, 21, 22, 22, 23, 20, // -Y bottom
	};

	constexpr size_t VertexCount = sizeof(Vertices) / sizeof(Vertex);
	constexpr size_t IndexCount  = sizeof(Indices) / sizeof(uint32_t);
}