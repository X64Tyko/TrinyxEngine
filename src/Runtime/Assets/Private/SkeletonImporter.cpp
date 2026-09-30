#include "SkeletonImporter.h"
#include "VertexFormat.h"
#include "Logger.h"

#include "cgltf.h" // implementation in cgltf_impl.cpp

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

// -----------------------------------------------------------------------
// Vertex hashing — mirrors MeshImporter dedup logic
// -----------------------------------------------------------------------

struct VertexHash
{
	size_t operator()(const Vertex& v) const
	{
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
// Accessor helpers
// -----------------------------------------------------------------------

static bool ReadFloat3(const cgltf_accessor* acc, cgltf_size index, float* out)
{
	if (!acc || index >= acc->count) return false;
	return cgltf_accessor_read_float(acc, index, out, 3) != 0;
}

static bool ReadFloat4(const cgltf_accessor* acc, cgltf_size index, float* out)
{
	if (!acc || index >= acc->count) return false;
	return cgltf_accessor_read_float(acc, index, out, 4) != 0;
}

static bool ReadFloat2(const cgltf_accessor* acc, cgltf_size index, float* out)
{
	if (!acc || index >= acc->count) return false;
	return cgltf_accessor_read_float(acc, index, out, 2) != 0;
}

// Read JOINTS_0 attribute — 4 indices stored as uint8 or uint16.
static void ReadJoints(const cgltf_accessor* acc, cgltf_size index, uint8_t out[4])
{
	out[0] = out[1] = out[2] = out[3] = 0;
	if (!acc || index >= acc->count) return;
	cgltf_uint tmp[4] = {};
	cgltf_accessor_read_uint(acc, index, tmp, 4);
	out[0] = static_cast<uint8_t>(tmp[0]);
	out[1] = static_cast<uint8_t>(tmp[1]);
	out[2] = static_cast<uint8_t>(tmp[2]);
	out[3] = static_cast<uint8_t>(tmp[3]);
}

// -----------------------------------------------------------------------
// Transform helpers
// -----------------------------------------------------------------------

static void TransformPos(const float m[16], const float in[3], float out[3])
{
	out[0] = in[0] * m[0] + in[1] * m[4] + in[2] * m[8] + m[12];
	out[1] = in[0] * m[1] + in[1] * m[5] + in[2] * m[9] + m[13];
	out[2] = in[0] * m[2] + in[1] * m[6] + in[2] * m[10] + m[14];
}

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

// Column-major 4x4 multiply: C = A * B.
// flat layout: m[col*4 + row]
static void MatMul16CM(const float A[16], const float B[16], float C[16])
{
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
		{
			float s = 0.f;
			for (int k = 0; k < 4; ++k)
				s += A[k * 4 + row] * B[col * 4 + k];
			C[col * 4 + row] = s;
		}
}

// Invert a column-major 4x4 matrix via Gauss-Jordan. Returns false if singular.
static bool MatInv16CM(const float m[16], float inv[16])
{
	float A[4][8];
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
		{
			A[r][c]     = m[c * 4 + r];
			A[r][c + 4] = (r == c) ? 1.f : 0.f;
		}
	for (int col = 0; col < 4; ++col)
	{
		int pivot = col;
		for (int r = col + 1; r < 4; ++r)
			if (fabsf(A[r][col]) > fabsf(A[pivot][col])) pivot = r;
		if (pivot != col)
			for (int c = 0; c < 8; ++c)
				std::swap(A[col][c], A[pivot][c]);
		float dv = A[col][col];
		if (fabsf(dv) < 1e-12f) return false;
		for (int c = 0; c < 8; ++c)
			A[col][c] /= dv;
		for (int r = 0; r < 4; ++r)
		{
			if (r == col) continue;
			float f = A[r][col];
			for (int c = 0; c < 8; ++c)
				A[r][c] -= f * A[col][c];
		}
	}
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			inv[c * 4 + r] = A[r][c + 4];
	return true;
}

// -----------------------------------------------------------------------
// Pass 1 — Mesh geometry + skin weights
// -----------------------------------------------------------------------

static bool ImportSkeletalPrimitive(
	const cgltf_primitive& prim,
	[[maybe_unused]] const cgltf_skin* skin,
	const float worldMatrix[16],
	std::vector<Vertex>& rawVerts,
	std::vector<SkinWeights>& rawWeights,
	std::vector<uint32_t>& rawIndices,
	float aabbMin[3], float aabbMax[3])
{
	if (prim.type != cgltf_primitive_type_triangles) return false;

	const cgltf_accessor* posAcc     = nullptr;
	const cgltf_accessor* normAcc    = nullptr;
	const cgltf_accessor* uvAcc      = nullptr;
	const cgltf_accessor* tanAcc     = nullptr;
	const cgltf_accessor* jointsAcc  = nullptr;
	const cgltf_accessor* weightsAcc = nullptr;

	for (cgltf_size i = 0; i < prim.attributes_count; ++i)
	{
		const auto& attr = prim.attributes[i];
		switch (attr.type)
		{
			case cgltf_attribute_type_position: posAcc = attr.data; break;
			case cgltf_attribute_type_normal: normAcc = attr.data; break;
			case cgltf_attribute_type_texcoord:
				if (attr.index == 0) uvAcc = attr.data;
				break;
			case cgltf_attribute_type_tangent:
				if (attr.index == 0) tanAcc = attr.data;
				break;
			case cgltf_attribute_type_joints:
				if (attr.index == 0) jointsAcc = attr.data;
				break;
			case cgltf_attribute_type_weights:
				if (attr.index == 0) weightsAcc = attr.data;
				break;
			default: break;
		}
	}

	if (!posAcc) return false;

	const uint32_t baseVertex  = static_cast<uint32_t>(rawVerts.size());
	const cgltf_size vertCount = posAcc->count;

	rawVerts.resize(rawVerts.size() + vertCount);
	rawWeights.resize(rawWeights.size() + vertCount);

	for (cgltf_size i = 0; i < vertCount; ++i)
	{
		Vertex& vert        = rawVerts[baseVertex + i];
		SkinWeights& weight = rawWeights[baseVertex + i];
		std::memset(&vert, 0, sizeof(Vertex));

		float pos[3] = {};
		ReadFloat3(posAcc, i, pos);
		float wPos[3];
		TransformPos(worldMatrix, pos, wPos);
		vert.px = wPos[0];
		vert.py = wPos[1];
		vert.pz = wPos[2];

		for (int a = 0; a < 3; ++a)
		{
			aabbMin[a] = std::min(aabbMin[a], wPos[a]);
			aabbMax[a] = std::max(aabbMax[a], wPos[a]);
		}

		float norm[3] = { 0.f, 1.f, 0.f };
		if (normAcc) ReadFloat3(normAcc, i, norm);
		float wNorm[3];
		TransformDir(worldMatrix, norm, wNorm);
		vert.n_oct16x2 = OctEncode(wNorm[0], wNorm[1], wNorm[2]);

		float uv[2] = {};
		if (uvAcc) ReadFloat2(uvAcc, i, uv);
		vert.u = uv[0];
		vert.v = uv[1];

		if (tanAcc)
		{
			float tan4[4] = { 1.f, 0.f, 0.f, 1.f };
			ReadFloat4(tanAcc, i, tan4);
			float wTan[3];
			TransformDir(worldMatrix, tan4, wTan);
			vert.t_oct16x2 = OctEncode(wTan[0], wTan[1], wTan[2]);
			vert.flags     = (tan4[3] < 0.f) ? 1 : 0;
		}

		// Skin weights
		float rawW[4] = { 1.f, 0.f, 0.f, 0.f };
		if (weightsAcc) ReadFloat4(weightsAcc, i, rawW);

		uint8_t joints[4] = {};
		if (jointsAcc) ReadJoints(jointsAcc, i, joints);

		// Convert float weights to UNORM8 — normalize to sum 255
		float total = rawW[0] + rawW[1] + rawW[2] + rawW[3];
		if (total < 1e-6f) total = 1.f;
		uint32_t sumW = 0;
		for (int j = 0; j < 4; ++j)
		{
			weight.boneWeights[j] = static_cast<uint8_t>(rawW[j] / total * 255.f + 0.5f);
			sumW += weight.boneWeights[j];
			weight.boneIndices[j] = joints[j];
		}
		// Fix rounding: adjust largest weight so sum == 255
		if (sumW != 255)
		{
			uint8_t* maxW = std::max_element(weight.boneWeights, weight.boneWeights + 4);
			*maxW         = static_cast<uint8_t>(*maxW + (255 - sumW));
		}
	}

	// Indices
	if (prim.indices)
	{
		const cgltf_size idxStart = rawIndices.size();
		rawIndices.resize(idxStart + prim.indices->count);
		for (cgltf_size i = 0; i < prim.indices->count; ++i)
			rawIndices[idxStart + i] = baseVertex + static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i));
	}
	else
	{
		const cgltf_size idxStart = rawIndices.size();
		rawIndices.resize(idxStart + vertCount);
		for (cgltf_size i = 0; i < vertCount; ++i)
			rawIndices[idxStart + i] = baseVertex + static_cast<uint32_t>(i);
	}

	return true;
}

// -----------------------------------------------------------------------
// Pass 2 — Skeleton
// -----------------------------------------------------------------------

static bool ImportSkeleton(const cgltf_skin& skin, [[maybe_unused]] const cgltf_data& data,
	const float meshWorldMatrix[16], SkeletonAsset& outSkeleton)
{
	const cgltf_size boneCount = skin.joints_count;
	outSkeleton.boneCount      = static_cast<uint32_t>(boneCount);
	outSkeleton.bones.resize(boneCount);

	// Build joint-node to index map
	std::unordered_map<const cgltf_node*, uint32_t> jointIndex;
	jointIndex.reserve(boneCount);
	for (cgltf_size i = 0; i < boneCount; ++i)
		jointIndex[skin.joints[i]] = static_cast<uint32_t>(i);

	// The GLTF IBP transforms vertices from mesh-node-local space to joint-local space.
	// Our importer bakes the mesh node's world transform into vertex positions
	// (via cgltf_node_transform_world), so vertices are in GLTF world space, not mesh-local.
	// Correcting: IBP_stored = IBP_gltf * inv(meshWorldMatrix) makes the GPU formula
	// worldBone * IBP_stored * v_world produce the correct result.
	float invMeshWorld[16];
	const bool meshIsIdentity = !MatInv16CM(meshWorldMatrix, invMeshWorld);
	if (meshIsIdentity)
	{
		// Singular (degenerate) mesh matrix — treat as identity; IBPs are used as-is.
		std::memset(invMeshWorld, 0, sizeof(invMeshWorld));
		invMeshWorld[0] = invMeshWorld[5] = invMeshWorld[10] = invMeshWorld[15] = 1.f;
	}

	for (cgltf_size i = 0; i < boneCount; ++i)
	{
		BoneInfo& bone          = outSkeleton.bones[i];
		const cgltf_node* joint = skin.joints[i];

		// Parent index
		bone.parentIndex = 0xFFFFFFFF;
		if (joint->parent)
		{
			auto it = jointIndex.find(joint->parent);
			if (it != jointIndex.end()) bone.parentIndex = it->second;
		}

		// Inverse bind pose — corrected for the mesh node's world transform.
		if (skin.inverse_bind_matrices && i < skin.inverse_bind_matrices->count)
		{
			float ibp_gltf[16];
			cgltf_accessor_read_float(skin.inverse_bind_matrices, i, ibp_gltf, 16);
			MatMul16CM(ibp_gltf, invMeshWorld, bone.inverseBindPose);
		}
		else
		{
			// No IBP provided — use inv(meshWorldMatrix) so the formula still cancels correctly.
			std::memcpy(bone.inverseBindPose, invMeshWorld, sizeof(float) * 16);
		}

		// Name
		const char* nameStr = joint->name ? joint->name : "";
		bone.name           = TnxName(nameStr);
	}

	// Sockets are authored in the editor (M3). Leave socketCount = 0 for M1.
	outSkeleton.socketCount = 0;

	LOG_ENG_INFO_F("[SkeletonImporter] Skeleton '%s': %zu bones",
		skin.name ? skin.name : "(unnamed)", boneCount);
	return true;
}

// -----------------------------------------------------------------------
// Pass 3 — Animations
// -----------------------------------------------------------------------

static void ImportAnimation(const cgltf_animation& anim,
	const cgltf_skin& skin,
	uint32_t boneCount,
	AnimationAsset& outAnim,
	TnxName& outName)
{
	outName           = TnxName(anim.name ? anim.name : "anim");
	outAnim.boneCount = boneCount;
	outAnim.boneTracks.resize(boneCount, { 0, 0 });
	outAnim.duration = 0.f;

	// Build joint → bone index map from skin
	std::unordered_map<const cgltf_node*, uint32_t> jointIndex;
	jointIndex.reserve(boneCount);
	for (cgltf_size i = 0; i < skin.joints_count; ++i)
		jointIndex[skin.joints[i]] = static_cast<uint32_t>(i);

	// Temporary per-bone keyframe accumulation
	struct BoneKeys
	{
		std::vector<AnimKeyframe> keys;
		bool hasTranslation = false;
	};
	std::vector<BoneKeys> perBone(boneCount);

	for (cgltf_size c = 0; c < anim.channels_count; ++c)
	{
		const cgltf_animation_channel& chan = anim.channels[c];
		if (!chan.target_node) continue;

		auto it = jointIndex.find(chan.target_node);
		if (it == jointIndex.end()) continue;

		uint32_t boneIdx = it->second;
		if (chan.target_path != cgltf_animation_path_type_translation && chan.target_path != cgltf_animation_path_type_rotation)
			continue; // scale ignored in M1

		const cgltf_animation_sampler& sampler = *chan.sampler;
		if (!sampler.input || !sampler.output) continue;

		const cgltf_size keyCount         = sampler.input->count;
		std::vector<AnimKeyframe>& boneKf = perBone[boneIdx].keys;
		if (boneKf.size() < keyCount) boneKf.resize(keyCount);

		for (cgltf_size k = 0; k < keyCount; ++k)
		{
			float t = 0.f;
			cgltf_accessor_read_float(sampler.input, k, &t, 1);
			boneKf[k].time   = t;
			outAnim.duration = std::max(outAnim.duration, t);

			if (chan.target_path == cgltf_animation_path_type_translation)
			{
				float v[3] = {};
				cgltf_accessor_read_float(sampler.output, k, v, 3);
				boneKf[k].tx                    = v[0];
				boneKf[k].ty                    = v[1];
				boneKf[k].tz                    = v[2];
				perBone[boneIdx].hasTranslation = true;
			}
			else // rotation
			{
				float v[4] = { 0.f, 0.f, 0.f, 1.f };
				cgltf_accessor_read_float(sampler.output, k, v, 4);
				boneKf[k].rx = v[0];
				boneKf[k].ry = v[1];
				boneKf[k].rz = v[2];
				boneKf[k].rw = v[3];
			}
		}
	}

	// Flatten into contiguous keyframes array.
	// GLTF spec: a joint without a translation channel holds its bind-pose local translation
	// for the entire animation. Bones without any channel get a single static keyframe so the
	// shader never collapses them to origin (keyframeCount==0 path returns (0,0,0)).
	uint32_t nextOffset = 0;
	for (uint32_t b = 0; b < boneCount; ++b)
	{
		std::vector<AnimKeyframe>& keys = perBone[b].keys;

		const cgltf_node* joint = skin.joints[b];
		float bindT[3]          = { 0.f, 0.f, 0.f };
		if (joint->has_translation)
		{
			bindT[0] = joint->translation[0];
			bindT[1] = joint->translation[1];
			bindT[2] = joint->translation[2];
		}

		if (keys.empty())
		{
			// No animation channels — emit one static keyframe at the bind-pose local TRS.
			AnimKeyframe kf{};
			kf.time = 0.f;
			kf.tx   = bindT[0];
			kf.ty   = bindT[1];
			kf.tz   = bindT[2];
			kf.rw   = 1.f; // identity quaternion
			keys.push_back(kf);
		}
		else
		{
			for (auto& k : keys)
			{
				if (k.rw == 0.f) k.rw = 1.f; // no rotation channel → identity quat
				if (!perBone[b].hasTranslation)
				{
					k.tx = bindT[0];
					k.ty = bindT[1];
					k.tz = bindT[2];
				}
			}
		}

		outAnim.boneTracks[b] = { nextOffset, static_cast<uint32_t>(keys.size()) };
		for (const AnimKeyframe& k : keys)
			outAnim.keyframes.push_back(k);
		nextOffset += static_cast<uint32_t>(keys.size());
	}

	LOG_ENG_INFO_F("[SkeletonImporter] Animation '%s': %.2fs, %u bones, %zu keyframes",
		outName.GetStr(), outAnim.duration, boneCount, outAnim.keyframes.size());
}

// -----------------------------------------------------------------------
// ImportSkeletalGLTF — main entry point
// -----------------------------------------------------------------------

bool ImportSkeletalGLTF(const std::string& srcPath, SkeletalImportResult& outResult)
{
	cgltf_options options{};
	cgltf_data* data = nullptr;

	cgltf_result result = cgltf_parse_file(&options, srcPath.c_str(), &data);
	if (result != cgltf_result_success)
	{
		LOG_ENG_ERROR_F("[SkeletonImporter] Failed to parse '%s' (cgltf error %d)", srcPath.c_str(), result);
		return false;
	}

	result = cgltf_load_buffers(&options, data, srcPath.c_str());
	if (result != cgltf_result_success)
	{
		LOG_ENG_ERROR_F("[SkeletonImporter] Failed to load buffers for '%s'", srcPath.c_str());
		cgltf_free(data);
		return false;
	}

	if (data->skins_count == 0)
	{
		LOG_ENG_ERROR_F("[SkeletonImporter] No skins found in '%s' — use MeshImporter for static meshes", srcPath.c_str());
		cgltf_free(data);
		return false;
	}

	// Use the first skin — multi-skin support is an M3 concern
	const cgltf_skin& skin = data->skins[0];

	// -----------------------------------------------------------------------
	// Pass 1 — Mesh geometry + skin weights
	// -----------------------------------------------------------------------

	std::vector<Vertex> rawVerts;
	std::vector<SkinWeights> rawWeights;
	std::vector<uint32_t> rawIndices;
	float aabbMin[3] = { 1e30f, 1e30f, 1e30f };
	float aabbMax[3] = { -1e30f, -1e30f, -1e30f };

	// Capture the mesh node's world matrix — used in Pass 2 to correct IBPs.
	// All skin mesh nodes should share the same skin, so any one of them gives the right matrix.
	float meshWorldMatrix[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	bool meshWorldMatrixSet   = false;

	for (cgltf_size ni = 0; ni < data->nodes_count; ++ni)
	{
		const cgltf_node* node = &data->nodes[ni];
		if (!node->mesh || node->skin != &skin) continue;

		float worldMatrix[16];
		cgltf_node_transform_world(node, worldMatrix);

		if (!meshWorldMatrixSet)
		{
			std::memcpy(meshWorldMatrix, worldMatrix, sizeof(meshWorldMatrix));
			meshWorldMatrixSet = true;
		}

		for (cgltf_size pi = 0; pi < node->mesh->primitives_count; ++pi)
		{
			if (!ImportSkeletalPrimitive(node->mesh->primitives[pi], &skin, worldMatrix,
					rawVerts, rawWeights, rawIndices,
					aabbMin, aabbMax))
			{
				LOG_ENG_WARN_F("[SkeletonImporter] Skipped non-triangle primitive %zu in node '%s'",
					pi, node->name ? node->name : "(unnamed)");
			}
		}
	}

	if (rawVerts.empty())
	{
		LOG_ENG_ERROR_F("[SkeletonImporter] No valid primitives found in '%s'", srcPath.c_str());
		cgltf_free(data);
		return false;
	}

	// Deduplicate — geometry only; skin weights tracked in parallel
	std::unordered_map<Vertex, uint32_t, VertexHash, VertexEqual> vertMap;
	vertMap.reserve(rawVerts.size());
	std::vector<Vertex> dedupVerts;
	std::vector<SkinWeights> dedupWeights;
	std::vector<uint32_t> dedupIndices;
	dedupVerts.reserve(rawVerts.size());
	dedupWeights.reserve(rawVerts.size());
	dedupIndices.reserve(rawIndices.size());

	for (uint32_t idx : rawIndices)
	{
		const Vertex& v     = rawVerts[idx];
		auto [it, inserted] = vertMap.emplace(v, static_cast<uint32_t>(dedupVerts.size()));
		if (inserted)
		{
			dedupVerts.push_back(v);
			dedupWeights.push_back(rawWeights[idx]);
		}
		dedupIndices.push_back(it->second);
	}

	outResult.mesh.Vertices = std::move(dedupVerts);
	outResult.mesh.Indices  = std::move(dedupIndices);
	outResult.mesh.Skin     = std::move(dedupWeights);
	std::memcpy(outResult.mesh.AABBMin, aabbMin, sizeof(float) * 3);
	std::memcpy(outResult.mesh.AABBMax, aabbMax, sizeof(float) * 3);

	LOG_ENG_INFO_F("[SkeletonImporter] Mesh: %zu verts, %zu indices (deduped from %zu)",
		outResult.mesh.Vertices.size(), outResult.mesh.Indices.size(), rawVerts.size());

	// -----------------------------------------------------------------------
	// Pass 2 — Skeleton
	// -----------------------------------------------------------------------

	if (!ImportSkeleton(skin, *data, meshWorldMatrix, outResult.skeleton))
	{
		LOG_ENG_ERROR_F("[SkeletonImporter] Skeleton import failed for '%s'", srcPath.c_str());
		cgltf_free(data);
		return false;
	}

	// -----------------------------------------------------------------------
	// Pass 3 — Animations
	// -----------------------------------------------------------------------

	outResult.animations.resize(data->animations_count);
	outResult.animNames.resize(data->animations_count);

	for (cgltf_size a = 0; a < data->animations_count; ++a)
	{
		ImportAnimation(data->animations[a], skin, outResult.skeleton.boneCount,
			outResult.animations[a], outResult.animNames[a]);
	}

	LOG_ENG_INFO_F("[SkeletonImporter] '%s': %zu animations imported",
		srcPath.c_str(), data->animations_count);

	cgltf_free(data);
	return true;
}
