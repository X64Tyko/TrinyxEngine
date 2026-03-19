#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "MeshImporter.h"
#include "MeshAsset.h"
#include "VertexFormat.h"
#include "Logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

// -----------------------------------------------------------------------
// Vertex hashing for deduplication
// -----------------------------------------------------------------------

struct VertexHash
{
	size_t operator()(const Vertex& v) const
	{
		// FNV-1a over the raw bytes
		const auto* bytes = reinterpret_cast<const uint8_t*>(&v);
		size_t hash       = 14695981039346656037ULL;
		for (size_t i = 0; i < sizeof(Vertex); ++i)
		{
			hash ^= bytes[i];
			hash *= 1099511628211ULL;
		}
		return hash;
	}
};

struct VertexEqual
{
	bool operator()(const Vertex& a, const Vertex& b) const
	{
		return std::memcmp(&a, &b, sizeof(Vertex)) == 0;
	}
};

// -----------------------------------------------------------------------
// Tangent generation from normal + UV gradient (Lengyel's method, simplified)
// Runs on the final deduped mesh so shared vertices accumulate correctly.
// -----------------------------------------------------------------------

static void GenerateTangents(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
{
	const size_t vertCount = vertices.size();
	std::vector<float> tan1(vertCount * 3, 0.0f);

	for (size_t i = 0; i + 2 < indices.size(); i += 3)
	{
		const Vertex& v0 = vertices[indices[i]];
		const Vertex& v1 = vertices[indices[i + 1]];
		const Vertex& v2 = vertices[indices[i + 2]];

		float dx1 = v1.px - v0.px, dy1 = v1.py - v0.py, dz1 = v1.pz - v0.pz;
		float dx2 = v2.px - v0.px, dy2 = v2.py - v0.py, dz2 = v2.pz - v0.pz;
		float du1 = v1.u - v0.u, dv1   = v1.v - v0.v;
		float du2 = v2.u - v0.u, dv2   = v2.v - v0.v;

		float r = du1 * dv2 - du2 * dv1;
		if (fabsf(r) < 1e-12f) r = 1.0f;
		r = 1.0f / r;

		float tx = (dv2 * dx1 - dv1 * dx2) * r;
		float ty = (dv2 * dy1 - dv1 * dy2) * r;
		float tz = (dv2 * dz1 - dv1 * dz2) * r;

		for (int j = 0; j < 3; ++j)
		{
			uint32_t idx      = indices[i + j];
			tan1[idx * 3]     += tx;
			tan1[idx * 3 + 1] += ty;
			tan1[idx * 3 + 2] += tz;
		}
	}

	for (size_t i = 0; i < vertCount; ++i)
	{
		float nx, ny, nz;
		OctDecode(vertices[i].n_oct16x2, nx, ny, nz);

		float tx = tan1[i * 3], ty = tan1[i * 3 + 1], tz = tan1[i * 3 + 2];

		// Gram-Schmidt orthogonalize
		float dot = nx * tx + ny * ty + nz * tz;
		tx        -= nx * dot;
		ty        -= ny * dot;
		tz        -= nz * dot;

		float len = sqrtf(tx * tx + ty * ty + tz * tz);
		if (len > 1e-6f)
		{
			tx /= len;
			ty /= len;
			tz /= len;
		}
		else
		{
			tx = 1.0f;
			ty = 0.0f;
			tz = 0.0f;
		} // fallback

		vertices[i].t_oct16x2 = OctEncode(tx, ty, tz);
		vertices[i].flags     = (vertices[i].flags & ~1u) | 0u; // handedness = 0 → positive
	}
}

// -----------------------------------------------------------------------
// cgltf accessor helpers
// -----------------------------------------------------------------------

static size_t AccessorCount(const cgltf_accessor* acc)
{
	return acc ? acc->count : 0;
}

static bool ReadFloat3(const cgltf_accessor* acc, size_t index, float* out)
{
	if (!acc || index >= acc->count) return false;
	return cgltf_accessor_read_float(acc, index, out, 3);
}

static bool ReadFloat2(const cgltf_accessor* acc, size_t index, float* out)
{
	if (!acc || index >= acc->count) return false;
	return cgltf_accessor_read_float(acc, index, out, 2);
}

static bool ReadFloat4(const cgltf_accessor* acc, size_t index, float* out)
{
	if (!acc || index >= acc->count) return false;
	return cgltf_accessor_read_float(acc, index, out, 4);
}

// -----------------------------------------------------------------------
// Import a single primitive into the accumulation buffers
// -----------------------------------------------------------------------

// Transform a direction vector (normal/tangent) by the upper-3x3 of a
// column-major 4x4 matrix.  The result is normalized.
static void TransformDir(const float m[16], const float in[3], float out[3])
{
	float x   = in[0] * m[0] + in[1] * m[4] + in[2] * m[8];
	float y   = in[0] * m[1] + in[1] * m[5] + in[2] * m[9];
	float z   = in[0] * m[2] + in[1] * m[6] + in[2] * m[10];
	float len = sqrtf(x * x + y * y + z * z);
	if (len > 1e-6f)
	{
		x /= len;
		y /= len;
		z /= len;
	}
	out[0] = x;
	out[1] = y;
	out[2] = z;
}

// Transform a position by a column-major 4x4 matrix (w=1 homogeneous).
static void TransformPos(const float m[16], const float in[3], float out[3])
{
	out[0] = in[0] * m[0] + in[1] * m[4] + in[2] * m[8] + m[12];
	out[1] = in[0] * m[1] + in[1] * m[5] + in[2] * m[9] + m[13];
	out[2] = in[0] * m[2] + in[1] * m[6] + in[2] * m[10] + m[14];
}

static bool ImportPrimitive(
	const cgltf_primitive& prim,
	const float worldMatrix[16],
	std::vector<Vertex>& rawVerts,
	std::vector<uint32_t>& rawIndices,
	float aabbMin[3], float aabbMax[3],
	bool& anyMissingTangents)
{
	if (prim.type != cgltf_primitive_type_triangles) return false;

	// Find accessors — filter by attribute index for UV/tangent
	const cgltf_accessor* posAcc  = nullptr;
	const cgltf_accessor* normAcc = nullptr;
	const cgltf_accessor* uvAcc   = nullptr;
	const cgltf_accessor* tanAcc  = nullptr;

	for (cgltf_size i = 0; i < prim.attributes_count; ++i)
	{
		const auto& attr = prim.attributes[i];
		switch (attr.type)
		{
			case cgltf_attribute_type_position: posAcc = attr.data;
				break;
			case cgltf_attribute_type_normal: normAcc = attr.data;
				break;
			case cgltf_attribute_type_texcoord: if (attr.index == 0) uvAcc = attr.data; // TEXCOORD_0 only
				break;
			case cgltf_attribute_type_tangent: if (attr.index == 0) tanAcc = attr.data;
				break;
			default: break;
		}
	}

	if (!posAcc) return false;
	if (!tanAcc) anyMissingTangents = true;

	const uint32_t baseVertex = static_cast<uint32_t>(rawVerts.size());
	const size_t vertCount    = AccessorCount(posAcc);

	// Append vertices
	size_t vertStart = rawVerts.size();
	rawVerts.resize(vertStart + vertCount);

	for (size_t i = 0; i < vertCount; ++i)
	{
		Vertex& vert = rawVerts[vertStart + i];
		std::memset(&vert, 0, sizeof(Vertex));

		float pos[3] = {};
		ReadFloat3(posAcc, i, pos);
		float worldPos[3];
		TransformPos(worldMatrix, pos, worldPos);
		vert.px = worldPos[0];
		vert.py = worldPos[1];
		vert.pz = worldPos[2];

		for (int a = 0; a < 3; ++a)
		{
			aabbMin[a] = std::min(aabbMin[a], worldPos[a]);
			aabbMax[a] = std::max(aabbMax[a], worldPos[a]);
		}

		float norm[3] = {0.0f, 1.0f, 0.0f};
		if (normAcc) ReadFloat3(normAcc, i, norm);
		float worldNorm[3];
		TransformDir(worldMatrix, norm, worldNorm);
		vert.n_oct16x2 = OctEncode(worldNorm[0], worldNorm[1], worldNorm[2]);

		float uv[2] = {};
		if (uvAcc) ReadFloat2(uvAcc, i, uv);
		vert.u = uv[0];
		vert.v = uv[1];

		if (tanAcc)
		{
			float tan4[4] = {1.0f, 0.0f, 0.0f, 1.0f};
			ReadFloat4(tanAcc, i, tan4);
			float worldTan[3];
			TransformDir(worldMatrix, tan4, worldTan);
			vert.t_oct16x2 = OctEncode(worldTan[0], worldTan[1], worldTan[2]);
			vert.flags     = (tan4[3] < 0.0f) ? 1 : 0;
		}
	}

	// Append indices (offset by baseVertex)
	if (prim.indices)
	{
		size_t idxStart = rawIndices.size();
		rawIndices.resize(idxStart + prim.indices->count);
		for (cgltf_size i = 0; i < prim.indices->count; ++i)
			rawIndices[idxStart + i] = baseVertex
				+ static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i));
	}
	else
	{
		size_t idxStart = rawIndices.size();
		rawIndices.resize(idxStart + vertCount);
		for (size_t i = 0; i < vertCount; ++i) rawIndices[idxStart + i] = baseVertex + static_cast<uint32_t>(i);
	}

	return true;
}

// -----------------------------------------------------------------------
// ImportGLTFToAsset
//
// Walks the scene graph and bakes each node's world transform into the
// mesh vertices.  Multi-part models (helmet, body, armor, etc.) are
// merged into a single MeshAsset with correct spatial relationships.
//
// Unreferenced meshes (not attached to any node) are imported at identity
// as a fallback — some exporters store bind-pose geometry this way.
// -----------------------------------------------------------------------

bool ImportGLTFToAsset(const std::string& srcPath, MeshAsset& outAsset)
{
	cgltf_options options{};
	cgltf_data* data = nullptr;

	cgltf_result result = cgltf_parse_file(&options, srcPath.c_str(), &data);
	if (result != cgltf_result_success)
	{
		LOG_ERROR_F("[MeshImporter] Failed to parse '%s' (cgltf error %d)", srcPath.c_str(), result);
		return false;
	}

	result = cgltf_load_buffers(&options, data, srcPath.c_str());
	if (result != cgltf_result_success)
	{
		LOG_ERROR_F("[MeshImporter] Failed to load buffers for '%s'", srcPath.c_str());
		cgltf_free(data);
		return false;
	}

	if (data->meshes_count == 0)
	{
		LOG_ERROR_F("[MeshImporter] No meshes found in '%s'", srcPath.c_str());
		cgltf_free(data);
		return false;
	}

	std::vector<Vertex> rawVerts;
	std::vector<uint32_t> rawIndices;
	float aabbMin[3]        = {1e30f, 1e30f, 1e30f};
	float aabbMax[3]        = {-1e30f, -1e30f, -1e30f};
	bool anyMissingTangents = false;
	size_t totalPrimitives  = 0;

	// Case-insensitive substring check
	auto ContainsCI = [](const char* haystack, const char* needle) -> bool
	{
		if (!haystack || !needle) return false;
		for (const char* h = haystack; *h; ++h)
		{
			const char* a = h;
			const char* b = needle;
			while (*a && *b && ((*a | 32) == (*b | 32)))
			{
				++a;
				++b;
			}
			if (!*b) return true;
		}
		return false;
	};

	auto ShouldSkipNode = [&](const cgltf_node* node) -> bool
	{
		if (node->camera || node->light) return true;
		const char* name = node->name ? node->name : "";
		return ContainsCI(name, "armature") || ContainsCI(name, "skeleton")
			|| ContainsCI(name, "gizmo") || ContainsCI(name, "helper")
			|| ContainsCI(name, "ctrl") || ContainsCI(name, "control")
			|| ContainsCI(name, "locator") || ContainsCI(name, "dummy")
			|| ContainsCI(name, "floor") || ContainsCI(name, "ground")
			|| ContainsCI(name, "UCX_") || ContainsCI(name, "collision");
	};

	auto ShouldSkipMeshName = [&](const char* name) -> bool
	{
		return ContainsCI(name, "armature") || ContainsCI(name, "skeleton")
			|| ContainsCI(name, "gizmo") || ContainsCI(name, "helper")
			|| ContainsCI(name, "ctrl") || ContainsCI(name, "control")
			|| ContainsCI(name, "locator") || ContainsCI(name, "dummy")
			|| ContainsCI(name, "floor") || ContainsCI(name, "ground")
			|| ContainsCI(name, "UCX_") || ContainsCI(name, "collision");
	};

	// Track which meshes are referenced by nodes so we can catch unreferenced ones
	std::vector<bool> meshImported(data->meshes_count, false);

	// Walk scene graph — bake each node's world transform into its mesh vertices
	for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
	{
		const cgltf_node* node = &data->nodes[ni];
		if (!node->mesh) continue;

		const char* nodeName = node->name ? node->name : "(unnamed)";

		if (ShouldSkipNode(node))
		{
			LOG_INFO_F("[MeshImporter] Skipping node '%s' (filter)", nodeName);
			continue;
		}

		const cgltf_mesh* mesh = node->mesh;
		const char* meshName   = mesh->name ? mesh->name : "(unnamed)";

		if (ShouldSkipMeshName(meshName))
		{
			LOG_INFO_F("[MeshImporter] Skipping mesh '%s' from node '%s' (name filter)", meshName, nodeName);
			continue;
		}

		// Get world transform for this node — bakes scale, rotation, translation
		float worldMatrix[16];
		cgltf_node_transform_world(node, worldMatrix);

		size_t meshVertsBefore = rawVerts.size();

		for (cgltf_size pi = 0; pi < mesh->primitives_count; ++pi)
		{
			if (ImportPrimitive(mesh->primitives[pi], worldMatrix, rawVerts, rawIndices,
								aabbMin, aabbMax, anyMissingTangents))
			{
				++totalPrimitives;
			}
			else
			{
				LOG_WARN_F("[MeshImporter] Skipped primitive %zu in mesh '%s' (non-triangle or no positions)",
						   pi, meshName);
			}
		}

		// Mark this mesh as imported
		cgltf_size meshIdx = static_cast<cgltf_size>(mesh - data->meshes);
		if (meshIdx < data->meshes_count) meshImported[meshIdx] = true;

		LOG_INFO_F("[MeshImporter] Node '%s' mesh '%s': %zu primitives, %zu verts",
				   nodeName, meshName, mesh->primitives_count, rawVerts.size() - meshVertsBefore);
	}

	// Import unreferenced meshes at identity (some exporters store bind-pose
	// geometry as meshes not attached to any node)
	const float identity[16] = {
		1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1
	};
	for (cgltf_size mi = 0; mi < data->meshes_count; ++mi)
	{
		if (meshImported[mi]) continue;

		const cgltf_mesh& mesh = data->meshes[mi];
		const char* meshName   = mesh.name ? mesh.name : "(unnamed)";

		if (ShouldSkipMeshName(meshName))
		{
			LOG_INFO_F("[MeshImporter] Skipping unreferenced mesh %zu '%s' (name filter)", mi, meshName);
			continue;
		}

		size_t meshVertsBefore = rawVerts.size();

		for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi)
		{
			if (ImportPrimitive(mesh.primitives[pi], identity, rawVerts, rawIndices,
								aabbMin, aabbMax, anyMissingTangents))
			{
				++totalPrimitives;
			}
		}

		LOG_INFO_F("[MeshImporter] Unreferenced mesh %zu '%s': %zu verts (identity transform)",
				   mi, meshName, rawVerts.size() - meshVertsBefore);
	}

	if (rawVerts.empty())
	{
		LOG_ERROR_F("[MeshImporter] No valid primitives found in '%s'", srcPath.c_str());
		cgltf_free(data);
		return false;
	}

	const size_t totalRawVerts = rawVerts.size();

	// Zero tangent fields before dedup so they don't affect vertex identity
	// when tangents will be regenerated.
	if (anyMissingTangents)
	{
		for (auto& v : rawVerts)
		{
			v.t_oct16x2 = 0;
			v.flags     &= ~1u;
		}
	}

	// Deduplicate vertices
	std::unordered_map<Vertex, uint32_t, VertexHash, VertexEqual> vertMap;
	vertMap.reserve(totalRawVerts);
	std::vector<Vertex> dedupVerts;
	dedupVerts.reserve(totalRawVerts);
	std::vector<uint32_t> dedupIndices;
	dedupIndices.reserve(rawIndices.size());

	for (uint32_t idx : rawIndices)
	{
		const Vertex& v     = rawVerts[idx];
		auto [it, inserted] = vertMap.emplace(v, static_cast<uint32_t>(dedupVerts.size()));
		if (inserted) dedupVerts.push_back(v);
		dedupIndices.push_back(it->second);
	}

	// Generate tangents on the final deduped mesh
	if (anyMissingTangents) GenerateTangents(dedupVerts, dedupIndices);

	outAsset.Vertices = std::move(dedupVerts);
	outAsset.Indices  = std::move(dedupIndices);
	std::memcpy(outAsset.AABBMin, aabbMin, sizeof(float) * 3);
	std::memcpy(outAsset.AABBMax, aabbMax, sizeof(float) * 3);

	LOG_INFO_F("[MeshImporter] Imported '%s': %zu meshes, %zu primitives, %zu verts, %zu indices (deduped from %zu)",
			   srcPath.c_str(), data->meshes_count, totalPrimitives,
			   outAsset.Vertices.size(), outAsset.Indices.size(), totalRawVerts);
	LOG_INFO_F("[MeshImporter] AABB: min(%.3f, %.3f, %.3f) max(%.3f, %.3f, %.3f) size(%.3f, %.3f, %.3f)",
			   aabbMin[0], aabbMin[1], aabbMin[2],
			   aabbMax[0], aabbMax[1], aabbMax[2],
			   aabbMax[0]-aabbMin[0], aabbMax[1]-aabbMin[1], aabbMax[2]-aabbMin[2]);

	cgltf_free(data);
	return true;
}

// -----------------------------------------------------------------------
// ImportGLTF — convenience wrapper: import + save
// -----------------------------------------------------------------------

bool ImportGLTF(const std::string& srcPath, const std::string& outPath)
{
	MeshAsset asset;
	if (!ImportGLTFToAsset(srcPath, asset)) return false;
	return SaveMeshAsset(asset, outPath);
}
