// skinning.slang — GPU vertex skinning compute pass (M2).
//
// One thread per skeletal entity. Evaluates FK bone chain from animation
// keyframes and writes SkinMatrix[compactIdx × MAX_BONES_PER_ENTITY + b]
// for each bone. The vertex shader reads these matrices for LBS.
//
// Dispatch: vkCmdDispatchIndirect(SkeletalDispatchArgsAddr), numthreads(1,1,1).
// x=skeletalCount is GPU-written by scatter; dispatch runs after sort_instances.

#include "common/GpuFrameData.slang"

struct GpuAnimBoneTrack { uint keyframeOffset; uint keyframeCount; };
struct GpuAnimKeyframe   { float time; float tx, ty, tz, rx, ry, rz, rw; };
struct GpuBoneData       { float inverseBindPose[16]; }; // column-major float4x4

[[vk::push_constant]]
cbuffer PushConstants { uint64_t gFrameDataAddr; }

float4 quatMul(float4 a, float4 b)
{
    return float4(
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    );
}

// Compose base rotation with an additive delta weighted by alpha.
// Delta is authored from identity, so: weighted = nlerp(identity, delta, alpha); result = base * weighted.
float4 applyAdditiveRot(float4 baseQ, float4 addQ, float alpha)
{
    float4 identQ = float4(0.0, 0.0, 0.0, 1.0);
    if (dot(identQ, addQ) < 0.0) addQ = -addQ;
    float4 weighted = normalize(lerp(identQ, addQ, alpha));
    return normalize(quatMul(baseQ, weighted));
}

float4x4 quatToMat(float4 q)
{
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float x2 = x+x, y2 = y+y, z2 = z+z;
    float xx = x*x2, xy = x*y2, xz = x*z2;
    float yy = y*y2, yz = y*z2, zz = z*z2;
    float wx = w*x2, wy = w*y2, wz = w*z2;
    float4x4 m;
    m[0] = float4(1-(yy+zz), xy-wz,    xz+wy,    0);
    m[1] = float4(xy+wz,     1-(xx+zz), yz-wx,   0);
    m[2] = float4(xz-wy,     yz+wx,    1-(xx+yy), 0);
    m[3] = float4(0,         0,         0,         1);
    return m;
}

float4x4 makeTRS(float3 t, float4 q, float3 s)
{
    float4x4 r = quatToMat(normalize(q));
    float4x4 m;
    m[0] = float4(r[0].x * s.x, r[0].y * s.y, r[0].z * s.z, t.x);
    m[1] = float4(r[1].x * s.x, r[1].y * s.y, r[1].z * s.z, t.y);
    m[2] = float4(r[2].x * s.x, r[2].y * s.y, r[2].z * s.z, t.z);
    m[3] = float4(0, 0, 0, 1);
    return m;
}

float4x4 loadMat(float* data)
{
    // Column-major: M[row][col] = data[col*4 + row]
    float4x4 m;
    m[0] = float4(data[0], data[4], data[8],  data[12]);
    m[1] = float4(data[1], data[5], data[9],  data[13]);
    m[2] = float4(data[2], data[6], data[10], data[14]);
    m[3] = float4(data[3], data[7], data[11], data[15]);
    return m;
}

void evalBoneAtTime(GpuAnimBoneTrack track, GpuAnimKeyframe* kfBuf, float t,
                    out float3 outT, out float4 outQ)
{
    if (track.keyframeCount == 0)
    {
        outT = float3(0, 0, 0);
        outQ = float4(0, 0, 0, 1);
        return;
    }

    uint first = track.keyframeOffset;
    uint last  = first + track.keyframeCount - 1u;

    // Clamp to range
    if (t <= kfBuf[first].time)
    {
        GpuAnimKeyframe k = kfBuf[first];
        outT = float3(k.tx, k.ty, k.tz);
        outQ = float4(k.rx, k.ry, k.rz, k.rw);
        return;
    }
    if (t >= kfBuf[last].time)
    {
        GpuAnimKeyframe k = kfBuf[last];
        outT = float3(k.tx, k.ty, k.tz);
        outQ = float4(k.rx, k.ry, k.rz, k.rw);
        return;
    }

    // Linear search — sufficient for M2; binary search in M3.
    uint lo = first;
    for (uint i = first; i < last; ++i)
    {
        if (kfBuf[i+1].time > t) { lo = i; break; }
    }

    GpuAnimKeyframe a = kfBuf[lo];
    GpuAnimKeyframe b = kfBuf[lo + 1u];
    float span = b.time - a.time;
    float alpha = (span > 0.0) ? (t - a.time) / span : 0.0;

    outT = lerp(float3(a.tx, a.ty, a.tz), float3(b.tx, b.ty, b.tz), alpha);

    float4 qa = float4(a.rx, a.ry, a.rz, a.rw);
    float4 qb = float4(b.rx, b.ry, b.rz, b.rw);
    if (dot(qa, qb) < 0.0) qb = -qb;
    outQ = normalize(lerp(qa, qb, alpha));
}

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    GpuFrameData* fd = (GpuFrameData*)gFrameDataAddr;

    uint compactIdx = tid.x;
    // Bounds enforced by vkCmdDispatchIndirect — x is scatter-written skeletalCount.
    uint cacheIdx = ((uint*)fd->SkeletalListAddr)[compactIdx];

    uint  skelID    = ((uint*)fd->CurrFieldAddrs[SlabIdx_SkeletonID])[cacheIdx];
    uint  animID    = ((uint*)fd->CurrFieldAddrs[SlabIdx_BaseAnimID])[cacheIdx];
    float timestamp = ((float*)fd->CurrFieldAddrs[SlabIdx_BaseTimestamp])[cacheIdx];

    if (skelID == 0 || animID == 0) return;

    // Cross-fade fields
    uint  fadeAnimID    = ((uint*)fd->CurrFieldAddrs[SlabIdx_FadeAnimID])[cacheIdx];
    float fadeTimestamp = ((float*)fd->CurrFieldAddrs[SlabIdx_FadeTimestamp])[cacheIdx];
    float fadeAlpha     = ((float*)fd->CurrFieldAddrs[SlabIdx_FadeAlpha])[cacheIdx];
    bool  hasFade       = (fadeAnimID != 0 && fadeAlpha > 0.0);

    // Additive overlay layer fields
    uint  l0AnimID    = ((uint*)fd->CurrFieldAddrs[SlabIdx_Layer0AnimID])[cacheIdx];
    float l0Timestamp = ((float*)fd->CurrFieldAddrs[SlabIdx_Layer0Timestamp])[cacheIdx];
    float l0Alpha     = ((float*)fd->CurrFieldAddrs[SlabIdx_Layer0Alpha])[cacheIdx];
    uint  l0Config    = ((uint*)fd->CurrFieldAddrs[SlabIdx_Layer0Config])[cacheIdx];
    bool  hasL0       = (l0AnimID != 0 && l0Alpha > 0.0 && ((l0Config >> 8) & 1u) != 0);

    uint  l1AnimID    = ((uint*)fd->CurrFieldAddrs[SlabIdx_Layer1AnimID])[cacheIdx];
    float l1Timestamp = ((float*)fd->CurrFieldAddrs[SlabIdx_Layer1Timestamp])[cacheIdx];
    float l1Alpha     = ((float*)fd->CurrFieldAddrs[SlabIdx_Layer1Alpha])[cacheIdx];
    uint  l1Config    = ((uint*)fd->CurrFieldAddrs[SlabIdx_Layer1Config])[cacheIdx];
    bool  hasL1       = (l1AnimID != 0 && l1Alpha > 0.0 && ((l1Config >> 8) & 1u) != 0);

    GpuSkeletonSlotInfo skelSlot = ((GpuSkeletonSlotInfo*)fd->GpuSkeletonSlotAddr)[skelID];
    GpuAnimSlotInfo     animSlot = ((GpuAnimSlotInfo*)fd->GpuAnimSlotAddr)[animID];

    // Optional slots: trackOffset/boneCount default to 0 when the feature is inactive.
    uint fadeTrackOffset = 0; uint fadeBoneCount = 0;
    if (hasFade) {
        GpuAnimSlotInfo s = ((GpuAnimSlotInfo*)fd->GpuAnimSlotAddr)[fadeAnimID];
        fadeTrackOffset = s.trackOffset; fadeBoneCount = s.boneCount;
    }

    uint l0TrackOffset = 0; uint l0BoneCount = 0;
    if (hasL0) {
        GpuAnimSlotInfo s = ((GpuAnimSlotInfo*)fd->GpuAnimSlotAddr)[l0AnimID];
        l0TrackOffset = s.trackOffset; l0BoneCount = s.boneCount;
    }

    uint l1TrackOffset = 0; uint l1BoneCount = 0;
    if (hasL1) {
        GpuAnimSlotInfo s = ((GpuAnimSlotInfo*)fd->GpuAnimSlotAddr)[l1AnimID];
        l1TrackOffset = s.trackOffset; l1BoneCount = s.boneCount;
    }

    uint boneCount = min(min(skelSlot.boneCount, animSlot.boneCount), MAX_BONES_PER_ENTITY);
    if (boneCount == 0) return;

    GpuAnimBoneTrack* tracks  = (GpuAnimBoneTrack*)fd->AnimTrackAddr;
    GpuAnimKeyframe*  kfs     = (GpuAnimKeyframe*)fd->AnimKeyframeAddr;
    GpuBoneData*      bones   = (GpuBoneData*)fd->GpuBoneDataAddr;
    uint*             parents = (uint*)fd->GpuBoneParentAddr;
    float4x4*         outMats = (float4x4*)fd->SkinMatrixAddr;

    float posX  = ((float*)fd->CurrFieldAddrs[SemPosX  - 1u])[cacheIdx];
    float posY  = ((float*)fd->CurrFieldAddrs[SemPosY  - 1u])[cacheIdx];
    float posZ  = ((float*)fd->CurrFieldAddrs[SemPosZ  - 1u])[cacheIdx];
    float rotQx = ((float*)fd->CurrFieldAddrs[SemRotQx - 1u])[cacheIdx];
    float rotQy = ((float*)fd->CurrFieldAddrs[SemRotQy - 1u])[cacheIdx];
    float rotQz = ((float*)fd->CurrFieldAddrs[SemRotQz - 1u])[cacheIdx];
    float rotQw = ((float*)fd->CurrFieldAddrs[SemRotQw - 1u])[cacheIdx];
    float scaX  = ((float*)fd->CurrFieldAddrs[SemScaleX - 1u])[cacheIdx];
    float scaY  = ((float*)fd->CurrFieldAddrs[SemScaleY - 1u])[cacheIdx];
    float scaZ  = ((float*)fd->CurrFieldAddrs[SemScaleZ - 1u])[cacheIdx];

    float4x4 entityWorld = makeTRS(float3(posX, posY, posZ),
                                   float4(rotQx, rotQy, rotQz, rotQw),
                                   float3(scaX, scaY, scaZ));

    // Importer guarantees parentIndex < boneIndex for all non-root bones.
    float4x4 worldBone[MAX_BONES_PER_ENTITY];

    for (uint b = 0; b < boneCount; ++b)
    {
        uint globalBone = skelSlot.boneOffset + b;
        uint parentIdx  = parents[globalBone]; // 0xFFFFFFFF = root

        float3 localT; float4 localQ;
        evalBoneAtTime(tracks[animSlot.trackOffset + b], kfs, timestamp, localT, localQ);

        // Cross-fade: lerp base → fade by fadeAlpha
        if (hasFade && b < fadeBoneCount)
        {
            float3 fadeT; float4 fadeQ;
            evalBoneAtTime(tracks[fadeTrackOffset + b], kfs, fadeTimestamp, fadeT, fadeQ);
            localT = lerp(localT, fadeT, fadeAlpha);
            if (dot(localQ, fadeQ) < 0.0) fadeQ = -fadeQ;
            localQ = normalize(lerp(localQ, fadeQ, fadeAlpha));
        }

        if (hasL0 && b < l0BoneCount)
        {
            float3 addT; float4 addQ;
            evalBoneAtTime(tracks[l0TrackOffset + b], kfs, l0Timestamp, addT, addQ);
            localT += addT * l0Alpha;
            localQ  = applyAdditiveRot(localQ, addQ, l0Alpha);
        }

        if (hasL1 && b < l1BoneCount)
        {
            float3 addT; float4 addQ;
            evalBoneAtTime(tracks[l1TrackOffset + b], kfs, l1Timestamp, addT, addQ);
            localT += addT * l1Alpha;
            localQ  = applyAdditiveRot(localQ, addQ, l1Alpha);
        }

        float4x4 localMat = makeTRS(localT, localQ, float3(1, 1, 1));

        if (parentIdx == 0xFFFFFFFFu || parentIdx >= boneCount)
            worldBone[b] = mul(entityWorld, localMat);
        else
            worldBone[b] = mul(worldBone[parentIdx], localMat);

        float4x4 ibp = loadMat(&bones[globalBone].inverseBindPose[0]);
        outMats[compactIdx * MAX_BONES_PER_ENTITY + b] = mul(worldBone[b], ibp);
    }
}
