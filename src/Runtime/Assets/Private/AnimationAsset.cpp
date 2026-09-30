#include "AnimationAsset.h"
#include "FixedMath.h"
#include "Logger.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

BoneTransform BoneTransform::Compose(const BoneTransform& parent, const BoneTransform& child)
{
	SimUnit qx = parent.rx, qy = parent.ry, qz = parent.rz, qw = parent.rw;
	SimFloat vx = child.tx * parent.sx;
	SimFloat vy = child.ty * parent.sy;
	SimFloat vz = child.tz * parent.sz;

	BoneTransform r;
#ifdef TNX_DETERMINISM
	int32_t rx, ry, rz;
	RotateVectorFixed(qx.value.value, qy.value.value, qz.value.value, qw.value.value,
		vx.value.value, vy.value.value, vz.value.value, rx, ry, rz);
	r.tx = SimFloat(Fixed32::FromRaw(parent.tx.value.value + rx));
	r.ty = SimFloat(Fixed32::FromRaw(parent.ty.value.value + ry));
	r.tz = SimFloat(Fixed32::FromRaw(parent.tz.value.value + rz));
#else
	const SimFloat tx_ = 2 * (qy * vz - qz * vy);
	const SimFloat ty_ = 2 * (qz * vx - qx * vz);
	const SimFloat tz_ = 2 * (qx * vy - qy * vx);
	r.tx               = parent.tx + vx + qw * tx_ + qy * tz_ - qz * ty_;
	r.ty               = parent.ty + vy + qw * ty_ + qz * tx_ - qx * tz_;
	r.tz               = parent.tz + vz + qw * tz_ + qx * ty_ - qy * tx_;
#endif

	// Quaternion product: parent.r * child.r — all SimUnit * SimUnit → SimUnit
	r.rx = qw * child.rx + qx * child.rw + qy * child.rz - qz * child.ry;
	r.ry = qw * child.ry - qx * child.rz + qy * child.rw + qz * child.rx;
	r.rz = qw * child.rz + qx * child.ry - qy * child.rx + qz * child.rw;
	r.rw = qw * child.rw - qx * child.rx - qy * child.ry - qz * child.rz;

	r.sx = parent.sx * child.sx;
	r.sy = parent.sy * child.sy;
	r.sz = parent.sz * child.sz;

	return r;
}

BoneTransform BoneTransform::NLerp(const BoneTransform& a, const BoneTransform& b, SimFloat t)
{
	BoneTransform r;
	SimFloat it = SimFloat(1) - t;
	r.tx        = it * a.tx + t * b.tx;
	r.ty        = it * a.ty + t * b.ty;
	r.tz        = it * a.tz + t * b.tz;
	QNLerp(a.rx, a.ry, a.rz, a.rw, b.rx, b.ry, b.rz, b.rw, t, r.rx, r.ry, r.rz, r.rw);
	r.sx = it * a.sx + t * b.sx;
	r.sy = it * a.sy + t * b.sy;
	r.sz = it * a.sz + t * b.sz;
	return r;
}

BoneTransform BoneTransform::WeightedAdd(const BoneTransform& acc, const BoneTransform& sample, SimFloat w)
{
	BoneTransform r;
	r.tx = acc.tx + sample.tx * w;
	r.ty = acc.ty + sample.ty * w;
	r.tz = acc.tz + sample.tz * w;
	// Flip sign for shortest-arc before accumulating; Normalize() re-normalizes after all slots.
	// SimFloat * SimUnit → SimUnit (weight scales quat component, argument order matters)
	SimFloat flip = (QDot(acc.rx, acc.ry, acc.rz, acc.rw,
						 sample.rx, sample.ry, sample.rz, sample.rw)
						< SimUnit(0))
						? -w
						: w;
	r.rx          = acc.rx + flip * sample.rx;
	r.ry          = acc.ry + flip * sample.ry;
	r.rz          = acc.rz + flip * sample.rz;
	r.rw          = acc.rw + flip * sample.rw;
	r.sx          = acc.sx + sample.sx * w;
	r.sy          = acc.sy + sample.sy * w;
	r.sz          = acc.sz + sample.sz * w;
	return r;
}

BoneTransform BoneTransform::Normalize(const BoneTransform& acc, SimFloat totalWeight)
{
	if (totalWeight <= SimFloat(0)) return BoneTransform::Identity();
	SimFloat inv = SimFloat(1) / totalWeight;
	BoneTransform r;
	r.tx = acc.tx * inv;
	r.ty = acc.ty * inv;
	r.tz = acc.tz * inv;
	r.sx = acc.sx * inv;
	r.sy = acc.sy * inv;
	r.sz = acc.sz * inv;
	// Normalize quaternion (all SimUnit arithmetic)
	SimUnit len = Sqrt(acc.rx * acc.rx + acc.ry * acc.ry + acc.rz * acc.rz + acc.rw * acc.rw);
	if (len > SimUnit(0))
	{
		r.rx = acc.rx / len;
		r.ry = acc.ry / len;
		r.rz = acc.rz / len;
		r.rw = acc.rw / len;
	}
	else
	{
		r.rx = SimUnit{};
		r.ry = SimUnit{};
		r.rz = SimUnit{};
		r.rw = SimUnit(1);
	}
	return r;
}

BoneTransform BoneTransform::ApplyAdditive(const BoneTransform& base, const BoneTransform& delta, SimFloat alpha)
{
	BoneTransform r = base;

	// Translation: offset added directly
	r.tx = base.tx + delta.tx * alpha;
	r.ty = base.ty + delta.ty * alpha;
	r.tz = base.tz + delta.tz * alpha;

	// Rotation: compose base * nlerp(identity, delta, alpha)
	// SimFloat * SimUnit → SimUnit (weight scales quat component)
	SimUnit dx = alpha * delta.rx;
	SimUnit dy = alpha * delta.ry;
	SimUnit dz = alpha * delta.rz;
	SimUnit dw = SimUnit(1) + alpha * (delta.rw - SimUnit(1));
	// Ensure shortest-arc from identity
	if (dw < SimUnit(0))
	{
		dx = -dx;
		dy = -dy;
		dz = -dz;
		dw = -dw;
	}
	SimUnit len = Sqrt(dx * dx + dy * dy + dz * dz + dw * dw);
	if (len > SimUnit(0))
	{
		dx /= len;
		dy /= len;
		dz /= len;
		dw /= len;
	}
	else
	{
		dx = SimUnit{};
		dy = SimUnit{};
		dz = SimUnit{};
		dw = SimUnit(1);
	}
	// quatMul(base.r, weighted_delta) — all SimUnit
	SimUnit bx = base.rx, by = base.ry, bz = base.rz, bw = base.rw;
	r.rx         = bw * dx + bx * dw + by * dz - bz * dy;
	r.ry         = bw * dy - bx * dz + by * dw + bz * dx;
	r.rz         = bw * dz + bx * dy - by * dx + bz * dw;
	r.rw         = bw * dw - bx * dx - by * dy - bz * dz;
	SimUnit rlen = Sqrt(r.rx * r.rx + r.ry * r.ry + r.rz * r.rz + r.rw * r.rw);
	if (rlen > SimUnit(0))
	{
		r.rx /= rlen;
		r.ry /= rlen;
		r.rz /= rlen;
		r.rw /= rlen;
	}

	// Scale: offset from identity (delta.s - 1) added
	r.sx = base.sx + (delta.sx - SimFloat(1)) * alpha;
	r.sy = base.sy + (delta.sy - SimFloat(1)) * alpha;
	r.sz = base.sz + (delta.sz - SimFloat(1)) * alpha;

	return r;
}


static BoneTransform EvaluateTrack(const std::vector<AnimKeyframe>& keyframes,
	const AnimBoneTrack& track, float timestamp)
{
	if (track.keyframeCount == 0) return BoneTransform::Identity();

	const AnimKeyframe* keys = keyframes.data() + track.keyframeOffset;

	if (track.keyframeCount == 1 || timestamp <= keys[0].time)
	{
		const AnimKeyframe& k = keys[0];
		BoneTransform r;
		r.tx = SimFloat(k.tx);
		r.ty = SimFloat(k.ty);
		r.tz = SimFloat(k.tz);
		r.rx = SimUnit(k.rx);
		r.ry = SimUnit(k.ry);
		r.rz = SimUnit(k.rz);
		r.rw = SimUnit(k.rw);
		return r;
	}

	const uint32_t last = track.keyframeCount - 1;
	if (timestamp >= keys[last].time)
	{
		const AnimKeyframe& k = keys[last];
		BoneTransform r;
		r.tx = SimFloat(k.tx);
		r.ty = SimFloat(k.ty);
		r.tz = SimFloat(k.tz);
		r.rx = SimUnit(k.rx);
		r.ry = SimUnit(k.ry);
		r.rz = SimUnit(k.rz);
		r.rw = SimUnit(k.rw);
		return r;
	}

	// Binary search for the segment
	uint32_t lo = 0, hi = last;
	while (lo + 1 < hi)
	{
		uint32_t mid = (lo + hi) / 2;
		if (keys[mid].time <= timestamp)
			lo = mid;
		else
			hi = mid;
	}

	const AnimKeyframe& a = keys[lo];
	const AnimKeyframe& b = keys[hi];
	float span            = b.time - a.time;
	float t               = (span > 1e-6f) ? (timestamp - a.time) / span : 0.f;

	BoneTransform r;
	r.tx = SimFloat(a.tx + (b.tx - a.tx) * t);
	r.ty = SimFloat(a.ty + (b.ty - a.ty) * t);
	r.tz = SimFloat(a.tz + (b.tz - a.tz) * t);
	QNLerp(SimUnit(a.rx), SimUnit(a.ry), SimUnit(a.rz), SimUnit(a.rw),
		SimUnit(b.rx), SimUnit(b.ry), SimUnit(b.rz), SimUnit(b.rw),
		SimFloat(t), r.rx, r.ry, r.rz, r.rw);
	r.sx = SimFloat(1.f);
	r.sy = SimFloat(1.f);
	r.sz = SimFloat(1.f);
	return r;
}

BoneTransform AnimationAsset::EvaluateBone(uint32_t boneIndex, float timestamp) const
{
	if (boneIndex >= boneCount) return BoneTransform::Identity();
	return EvaluateTrack(keyframes, boneTracks[boneIndex], timestamp);
}

BoneTransform AnimationAsset::EvaluateRootMotionDelta(float fromTime, float toTime) const
{
	// M2: returns the delta transform between two timestamps on the root motion track.
	if (rootMotionTrack.empty()) return BoneTransform::Identity();
	BoneTransform from = EvaluateTrack(rootMotionTrack, { 0, static_cast<uint32_t>(rootMotionTrack.size()) }, fromTime);
	BoneTransform to   = EvaluateTrack(rootMotionTrack, { 0, static_cast<uint32_t>(rootMotionTrack.size()) }, toTime);
	// Delta: to.pos - from.pos in world space (simplified — ignores rotation delta for M2 stub)
	BoneTransform delta = BoneTransform::Identity();
	delta.tx            = to.tx - from.tx;
	delta.ty            = to.ty - from.ty;
	delta.tz            = to.tz - from.tz;
	return delta;
}

// Defined here to resolve the forward declaration of SkeletonAsset in AnimTypes.h.

#include "SkeletonAsset.h"

ChainWalkResult BoneCacheLocal::FindNearestCachedAncestor(uint32_t targetBoneIndex,
	const SkeletonAsset& skeleton) const
{
	// Build chain from root to targetBone
	uint32_t fullChain[256];
	uint32_t chainLen = 0;
	skeleton.GetChainFromRoot(targetBoneIndex, fullChain, chainLen);

	// Walk from targetBone upward to find the nearest cached ancestor
	// fullChain[0] == root, fullChain[chainLen-1] == targetBone
	ChainWalkResult result;
	result.ancestorTransform = BoneTransform::Identity();
	result.remainingBones    = 0;

	int32_t firstUncachedIdx = 0; // index into fullChain of first bone we still need to evaluate

	for (int32_t i = static_cast<int32_t>(chainLen) - 1; i >= 0; --i)
	{
		const BoneTransform* cached = Find(fullChain[i]);
		if (cached)
		{
			result.ancestorTransform = *cached;
			firstUncachedIdx         = i + 1;
			break;
		}
	}

	// Remaining = bones from firstUncachedIdx to targetBone (inclusive)
	result.remainingBones = chainLen - static_cast<uint32_t>(firstUncachedIdx);
	for (uint32_t i = 0; i < result.remainingBones; ++i)
		result.chain[i] = fullChain[firstUncachedIdx + i];

	return result;
}

bool SaveAnimationAsset(const AnimationAsset& asset, const std::string& path)
{
	std::ofstream file(path, std::ios::binary);
	if (!file.is_open())
	{
		LOG_ENG_ERROR_F("[AnimationAsset] Failed to open '%s' for writing", path.c_str());
		return false;
	}

	TnxAnimHeader header;
	header.BoneCount     = asset.boneCount;
	header.KeyframeCount = static_cast<uint32_t>(asset.keyframes.size());
	header.Duration      = asset.duration;
	header.NotifyCount   = static_cast<uint32_t>(asset.notifies.size());
	header.HasRootMotion = asset.hasRootMotion ? 1 : 0;
	file.write(reinterpret_cast<const char*>(&header), sizeof(header));

	// Bone tracks
	for (const AnimBoneTrack& t : asset.boneTracks)
	{
		AnimBoneTrackDisk d{ t.keyframeOffset, t.keyframeCount };
		file.write(reinterpret_cast<const char*>(&d), sizeof(d));
	}

	// Keyframes
	for (const AnimKeyframe& k : asset.keyframes)
	{
		AnimKeyframeDisk d{ k.time, k.tx, k.ty, k.tz, k.rx, k.ry, k.rz, k.rw };
		file.write(reinterpret_cast<const char*>(&d), sizeof(d));
	}

	// Notifies (M2+)
	for (const AnimNotifyDef& n : asset.notifies)
	{
		AnimNotifyDisk d;
		d.idHash      = n.id;
		d.triggerTime = n.triggerTime;
		d.duration    = n.duration;
		d.nameHash    = n.name.Value;
		std::memset(d.nameStr, 0, sizeof(d.nameStr));
#ifndef TNX_STRIP_NAMES
		std::strncpy(d.nameStr, n.name.GetStr(), sizeof(d.nameStr) - 1);
#endif
		file.write(reinterpret_cast<const char*>(&d), sizeof(d));
	}

	if (!file.good())
	{
		LOG_ENG_ERROR_F("[AnimationAsset] Write error for '%s'", path.c_str());
		return false;
	}

	LOG_ENG_INFO_F("[AnimationAsset] Saved '%s' (%.2fs, %u bones, %u keyframes)",
		path.c_str(), asset.duration, asset.boneCount,
		static_cast<uint32_t>(asset.keyframes.size()));
	return true;
}

bool LoadAnimationAsset(AnimationAsset& outAsset, const std::string& path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
	{
		LOG_ENG_ERROR_F("[AnimationAsset] Failed to open '%s' for reading", path.c_str());
		return false;
	}

	TnxAnimHeader header;
	file.read(reinterpret_cast<char*>(&header), sizeof(header));

	if (header.Magic != TnxAnimMagic)
	{
		LOG_ENG_ERROR_F("[AnimationAsset] Invalid magic in '%s'", path.c_str());
		return false;
	}
	if (header.Version != TnxAnimVersion)
	{
		LOG_ENG_ERROR_F("[AnimationAsset] Unsupported version %u in '%s'", header.Version, path.c_str());
		return false;
	}

	outAsset.duration      = header.Duration;
	outAsset.boneCount     = header.BoneCount;
	outAsset.hasRootMotion = header.HasRootMotion != 0;
	outAsset.boneTracks.resize(header.BoneCount);
	outAsset.keyframes.resize(header.KeyframeCount);
	outAsset.notifies.resize(header.NotifyCount);

	for (AnimBoneTrack& t : outAsset.boneTracks)
	{
		AnimBoneTrackDisk d;
		file.read(reinterpret_cast<char*>(&d), sizeof(d));
		t = { d.keyframeOffset, d.keyframeCount };
	}

	for (AnimKeyframe& k : outAsset.keyframes)
	{
		AnimKeyframeDisk d;
		file.read(reinterpret_cast<char*>(&d), sizeof(d));
		k = { d.time, d.tx, d.ty, d.tz, d.rx, d.ry, d.rz, d.rw };
	}

	for (AnimNotifyDef& n : outAsset.notifies)
	{
		AnimNotifyDisk d;
		file.read(reinterpret_cast<char*>(&d), sizeof(d));
		n.id          = d.idHash;
		n.triggerTime = d.triggerTime;
		n.duration    = d.duration;
		n.name        = TnxName(d.nameHash, d.nameStr);
	}

	if (!file.good())
	{
		LOG_ENG_ERROR_F("[AnimationAsset] Read error for '%s'", path.c_str());
		outAsset = {};
		return false;
	}

	LOG_ENG_INFO_F("[AnimationAsset] Loaded '%s' (%.2fs, %u bones, %u keyframes, %u notifies)",
		path.c_str(), header.Duration, header.BoneCount,
		header.KeyframeCount, header.NotifyCount);
	return true;
}
