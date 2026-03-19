#pragma once
#include <string>

struct MeshAsset;

// -----------------------------------------------------------------------
// MeshImporter — glTF → MeshAsset → .tnxmesh
//
// Parses a .gltf or .glb file with cgltf, extracts the first mesh
// primitive, converts to the engine vertex format (oct-encoded normals,
// tangent frame, deduplication), and writes a .tnxmesh binary.
//
// If tangents are absent, they are generated from normal + UV gradient.
// If UVs are absent, they default to (0,0).
// -----------------------------------------------------------------------

/// Import a glTF file and write the result as .tnxmesh binary.
/// Returns true on success.
bool ImportGLTF(const std::string& srcPath, const std::string& outPath);

/// Import a glTF file into a MeshAsset in memory (no file write).
/// Returns true on success.
bool ImportGLTFToAsset(const std::string& srcPath, MeshAsset& outAsset);