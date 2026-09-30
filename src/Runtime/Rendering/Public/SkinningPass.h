#pragma once
#include <cstdint>
#include "VulkanMemory.h"
#include "VulkanInclude.h"

// -----------------------------------------------------------------------
// SkinningPass — GPU vertex skinning compute pass.
//
// Owns four GPU buffers:
//   SkinMatrixBuffer           — device-local float4x4 array, written by the
//                                skinning compute shader, read by vertex shader.
//                                Layout: [compactIdx × MAX_BONES_PER_ENTITY + boneIdx]
//   SkeletalListBuffer          — device-local uint array, GPU-written by scatter.
//                                SkeletalList[compactIdx] = cacheIdx of the Nth skeletal entity.
//   SkeletalIdxByEntityBuffer   — device-local uint array, GPU-written by scatter.
//                                SkeletalIdxByEntity[cacheIdx] = compact skeletal slot, or UINT32_MAX.
//                                Cleared to UINT32_MAX each frame via vkCmdFillBuffer before scatter.
//   SkeletalDispatchArgsBuffer  — persistent-mapped VkDispatchIndirectCommand {x,y=1,z=1}.
//                                x is zeroed each frame; scatter atomically increments it.
//                                y and z are initialized to 1 and never changed.
//
// Skinning runs AFTER sort_instances via vkCmdDispatchIndirect.
// -----------------------------------------------------------------------

static constexpr uint32_t MaxSkeletalInstances = 512;
static constexpr uint32_t MaxBonesPerEntity    = 128;

class SkinningPass
{
public:
	bool Initialize(VkDevice device, VkPipelineLayout pipelineLayout,
		VulkanMemory* vkMem, uint32_t maxCachedEntities);

	void Destroy(VkDevice device);

	// Indirect dispatch: scatter writes SkeletalDispatchArgsBuffer.x; skinning runs after sort.
	void Dispatch(VkCommandBuffer cmd, uint64_t fdAddr);

	uint64_t GetSkinMatrixAddr() const { return SkinMatrixBuffer.DeviceAddr; }
	uint64_t GetSkeletalListAddr() const { return SkeletalListBuffer.DeviceAddr; }
	uint64_t GetSkeletalIdxByEntityAddr() const { return SkeletalIdxByEntityBuffer.DeviceAddr; }
	uint64_t GetSkeletalDispatchArgsAddr() const { return SkeletalDispatchArgsBuffer.DeviceAddr; }
	VkBuffer GetSkeletalIdxByEntityBuffer() const { return static_cast<VkBuffer>(SkeletalIdxByEntityBuffer.Buffer); }
	VkBuffer GetSkeletalDispatchBuffer() const { return static_cast<VkBuffer>(SkeletalDispatchArgsBuffer.Buffer); }
	bool IsValid() const { return Pipeline != VK_NULL_HANDLE; }

private:
	VulkanBuffer SkinMatrixBuffer;           // device-local, written by skinning compute
	VulkanBuffer SkeletalListBuffer;         // device-local, GPU-written by scatter
	VulkanBuffer SkeletalIdxByEntityBuffer;  // device-local + TRANSFER_DST, GPU-written by scatter
	VulkanBuffer SkeletalDispatchArgsBuffer; // persistent-mapped + TRANSFER_DST, x zeroed per frame
	VkPipeline Pipeline         = VK_NULL_HANDLE;
	VkPipelineLayout PipeLayout = VK_NULL_HANDLE;
};
