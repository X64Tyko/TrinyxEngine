#include "SkinningPass.h"
#include "Logger.h"
#include "Profiler.h"

#include <cstdio>
#include <cstring>
#include <vector>

static std::vector<uint32_t> ReadSPIRV_Skin(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f) return {};
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || (size % 4) != 0)
	{
		fclose(f);
		return {};
	}
	std::vector<uint32_t> code(static_cast<size_t>(size) / 4);
	fread(code.data(), 1, static_cast<size_t>(size), f);
	fclose(f);
	return code;
}

bool SkinningPass::Initialize(VkDevice device, VkPipelineLayout pipelineLayout,
	VulkanMemory* vkMem, uint32_t maxCachedEntities)
{
	// SkinMatrix: device-local (written by compute, read by vertex shader).
	SkinMatrixBuffer = vkMem->AllocateBuffer(
		static_cast<VkDeviceSize>(MaxSkeletalInstances) * MaxBonesPerEntity * sizeof(float) * 16,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		GpuMemoryDomain::DeviceLocal,
		/*requestDeviceAddress=*/true);

	if (!SkinMatrixBuffer.IsValid())
	{
		LOG_ENG_ERROR("[SkinningPass] SkinMatrixBuffer allocation failed");
		return false;
	}

	// SkeletalList: device-local, GPU-written by scatter (compact list of cacheIdx per skeletal entity).
	SkeletalListBuffer = vkMem->AllocateBuffer(
		static_cast<VkDeviceSize>(MaxSkeletalInstances) * sizeof(uint32_t),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		GpuMemoryDomain::DeviceLocal,
		/*requestDeviceAddress=*/true);

	if (!SkeletalListBuffer.IsValid())
	{
		LOG_ENG_ERROR("[SkinningPass] SkeletalListBuffer allocation failed");
		return false;
	}

	// SkeletalIdxByEntity: device-local + TRANSFER_DST. cacheIdx → compact skeletal idx.
	// Cleared to UINT32_MAX each frame by vkCmdFillBuffer before scatter.
	SkeletalIdxByEntityBuffer = vkMem->AllocateBuffer(
		static_cast<VkDeviceSize>(maxCachedEntities) * sizeof(uint32_t),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		GpuMemoryDomain::DeviceLocal,
		/*requestDeviceAddress=*/true);

	if (!SkeletalIdxByEntityBuffer.IsValid())
	{
		LOG_ENG_ERROR("[SkinningPass] SkeletalIdxByEntityBuffer allocation failed");
		return false;
	}

	// SkeletalDispatchArgs: persistent-mapped + TRANSFER_DST. {x=0,y=1,z=1}.
	// x is zeroed each frame by vkCmdFillBuffer before scatter; scatter atomically increments it.
	// y=1 and z=1 are set here and never changed.
	SkeletalDispatchArgsBuffer = vkMem->AllocateBuffer(
		sizeof(uint32_t) * 3,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		GpuMemoryDomain::PersistentMapped,
		/*requestDeviceAddress=*/true);

	if (!SkeletalDispatchArgsBuffer.IsValid())
	{
		LOG_ENG_ERROR("[SkinningPass] SkeletalDispatchArgsBuffer allocation failed");
		return false;
	}
	auto* args = static_cast<uint32_t*>(SkeletalDispatchArgsBuffer.MappedPtr);
	args[0]    = 0; // x = skeletalCount, zeroed each frame
	args[1]    = 1; // y = 1
	args[2]    = 1; // z = 1

	auto code = ReadSPIRV_Skin(TNX_SHADER_DIR "/compute/skinning.spv");
	if (code.empty())
	{
		LOG_ENG_WARN("[SkinningPass] skinning.spv not found — GPU skinning disabled (M1 mode)");
		return true;
	}

	VkShaderModuleCreateInfo modCI{};
	modCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	modCI.codeSize = code.size() * sizeof(uint32_t);
	modCI.pCode    = code.data();

	VkShaderModule mod = VK_NULL_HANDLE;
	if (vkCreateShaderModule(device, &modCI, nullptr, &mod) != VK_SUCCESS)
	{
		LOG_ENG_ERROR("[SkinningPass] vkCreateShaderModule failed");
		return true;
	}

	VkPipelineShaderStageCreateInfo stageCI{};
	stageCI.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageCI.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	stageCI.module = mod;
	stageCI.pName  = "main";

	VkComputePipelineCreateInfo pipeCI{};
	pipeCI.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipeCI.stage  = stageCI;
	pipeCI.layout = pipelineLayout;

	const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &Pipeline);
	vkDestroyShaderModule(device, mod, nullptr);

	if (result != VK_SUCCESS)
	{
		LOG_ENG_WARN_F("[SkinningPass] vkCreateComputePipelines failed (%d) — GPU skinning disabled", result);
		Pipeline = VK_NULL_HANDLE;
		return true;
	}

	PipeLayout = pipelineLayout;

	LOG_ENG_INFO_F("[SkinningPass] Initialized (SkinMatrix: %.1f MB, SkeletalList: %u slots, pipeline ready)",
		static_cast<float>(MaxSkeletalInstances * MaxBonesPerEntity * sizeof(float) * 16) / (1024.f * 1024.f),
		MaxSkeletalInstances);
	return true;
}

void SkinningPass::Destroy(VkDevice device)
{
	if (Pipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(device, Pipeline, nullptr);
		Pipeline = VK_NULL_HANDLE;
	}
	SkinMatrixBuffer           = {};
	SkeletalListBuffer         = {};
	SkeletalIdxByEntityBuffer  = {};
	SkeletalDispatchArgsBuffer = {};
}

void SkinningPass::Dispatch(VkCommandBuffer cmd, uint64_t fdAddr)
{
	TNX_ZONE_MEDIUM_NC("SkinningDispatch", TNX_COLOR_RENDERING)

	if (Pipeline == VK_NULL_HANDLE) return;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, Pipeline);
	vkCmdPushConstants(cmd, PipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
		0, sizeof(uint64_t), &fdAddr);
	vkCmdDispatchIndirect(cmd, static_cast<VkBuffer>(SkeletalDispatchArgsBuffer.Buffer), 0);
}
