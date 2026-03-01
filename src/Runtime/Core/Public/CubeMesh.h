#pragma once
#include <cstdint>

// -----------------------------------------------------------------------
// CubeMesh — static cube geometry in the engine's canonical vertex format.
//
// Runtime vertex format (32 bytes, 16-byte aligned):
//   vec4 position  (xyz + w=1)
//   vec4 normal    (xyz + w=0)
//
// This is the binary format the offline mesh importer targets.  Any source
// asset (GLTF, FBX, OBJ) is pre-processed into this layout at import time,
// not at runtime.  The GPU reads vertices via buffer device address
// (BDA) — no vertex-attribute bindings are used in the pipeline.
//
// Indices are uint16 (sufficient for the cube; the offline importer may
// emit uint32 for larger meshes).
// -----------------------------------------------------------------------
namespace CubeMesh
{
	// 32 bytes, 16-byte aligned.  Matches the GLSL 'Vertex' buffer_reference
	// layout in cube.vert.
	struct alignas(16) Vertex
	{
		float px, py, pz, pw; // position (w = 1.0)
		float nx, ny, nz, nw; // normal   (w = 0.0)
	};

	// 24 unique vertices — 4 per face — so each face can have its own
	// flat normal without sharing vertices across faces.
	// Winding: CCW front-face (Vulkan default).
	constexpr Vertex Vertices[] = {
		// +Z front face  (normal  0, 0, 1)
		{-0.5f, -0.5f, 0.5f, 1, 0, 0, 1, 0}, //  0
		{0.5f, -0.5f, 0.5f, 1, 0, 0, 1, 0},  //  1
		{0.5f, 0.5f, 0.5f, 1, 0, 0, 1, 0},   //  2
		{-0.5f, 0.5f, 0.5f, 1, 0, 0, 1, 0},  //  3
		// -Z back face   (normal  0, 0,-1)
		{0.5f, -0.5f, -0.5f, 1, 0, 0, -1, 0},  //  4
		{-0.5f, -0.5f, -0.5f, 1, 0, 0, -1, 0}, //  5
		{-0.5f, 0.5f, -0.5f, 1, 0, 0, -1, 0},  //  6
		{0.5f, 0.5f, -0.5f, 1, 0, 0, -1, 0},   //  7
		// +X right face  (normal  1, 0, 0)
		{0.5f, -0.5f, 0.5f, 1, 1, 0, 0, 0},  //  8
		{0.5f, -0.5f, -0.5f, 1, 1, 0, 0, 0}, //  9
		{0.5f, 0.5f, -0.5f, 1, 1, 0, 0, 0},  // 10
		{0.5f, 0.5f, 0.5f, 1, 1, 0, 0, 0},   // 11
		// -X left face   (normal -1, 0, 0)
		{-0.5f, -0.5f, -0.5f, 1, -1, 0, 0, 0}, // 12
		{-0.5f, -0.5f, 0.5f, 1, -1, 0, 0, 0},  // 13
		{-0.5f, 0.5f, 0.5f, 1, -1, 0, 0, 0},   // 14
		{-0.5f, 0.5f, -0.5f, 1, -1, 0, 0, 0},  // 15
		// +Y top face    (normal  0, 1, 0)
		{-0.5f, 0.5f, 0.5f, 1, 0, 1, 0, 0},  // 16
		{0.5f, 0.5f, 0.5f, 1, 0, 1, 0, 0},   // 17
		{0.5f, 0.5f, -0.5f, 1, 0, 1, 0, 0},  // 18
		{-0.5f, 0.5f, -0.5f, 1, 0, 1, 0, 0}, // 19
		// -Y bottom face (normal  0,-1, 0)
		{-0.5f, -0.5f, -0.5f, 1, 0, -1, 0, 0}, // 20
		{0.5f, -0.5f, -0.5f, 1, 0, -1, 0, 0},  // 21
		{0.5f, -0.5f, 0.5f, 1, 0, -1, 0, 0},   // 22
		{-0.5f, -0.5f, 0.5f, 1, 0, -1, 0, 0},  // 23
	};

	// Two triangles per face × 6 faces = 12 triangles = 36 indices.
	constexpr uint16_t Indices[] = {
		0, 1, 2, 2, 3, 0,       // +Z front
		4, 5, 6, 6, 7, 4,       // -Z back
		8, 9, 10, 10, 11, 8,    // +X right
		12, 13, 14, 14, 15, 12, // -X left
		16, 17, 18, 18, 19, 16, // +Y top
		20, 21, 22, 22, 23, 20, // -Y bottom
	};

	constexpr size_t VertexCount = sizeof(Vertices) / sizeof(Vertex);
	constexpr size_t IndexCount  = sizeof(Indices) / sizeof(uint16_t);
}