#include "VulkRender.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

#include <SDL3/SDL.h>
#include "CubeMesh.h"
#include "FieldMeta.h"
#include "GpuFrameData.h"
#include "Logger.h"
#include "Profiler.h"
#include "Registry.h"
#include "TemporalComponentCache.h"

#include "ColorData.h"
#include "TransRot.h"
#include "Scale.h"
#include "TemporalFlags.h"
#include "LogicThread.h"
#include "TrinyxEngine.h"
#include "../../Core/Private/ThreadPinning.h"
// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

// Build a column-major ViewProj matrix for a fixed camera.
// RH projection, depth [0,1], Y-flip for Vulkan NDC (+Y down).
// Camera sits at (0, 0, kCamZ) looking toward the origin.
//
// Column-major layout: vp[col * 4 + row]
//   col 0: (F/aspect,  0,  0,  0)
//   col 1: (0,        -F,  0,  0)   — negative F = Y-flip
//   col 2: (0,         0,  b, -1)   — b = zfar/(znear-zfar), -1 gives w = -z_view
//   col 3: (0,         0,  t, kCamZ) — t = b*(-kCamZ)+c, kCamZ = w contribution from translation
//
// For the cube at world origin with kCamZ = 3:
//   view z = -3  →  w_clip = 3  →  NDC z ≈ 0.97  (well within [0,1])
static void BuildViewProjMatrix(float* vp, float aspect)
{
	constexpr float kFovY  = 3.14159265f * 0.25f; // 45°
	constexpr float kZNear = 0.1f;
	constexpr float kZFar  = 100.0f;
	constexpr float kCamZ  = 3.0f;

	const float F  = 1.0f / tanf(kFovY * 0.5f);
	const float dz = kZNear - kZFar;
	const float b  = kZFar / dz;          // depth scale
	const float c  = kZNear * kZFar / dz; // depth bias (translation contribution)

	vp[0]  = F / aspect;
	vp[1]  = 0.0f;
	vp[2]  = 0.0f;
	vp[3]  = 0.0f; // col 0
	vp[4]  = 0.0f;
	vp[5]  = -F;
	vp[6]  = 0.0f;
	vp[7]  = 0.0f; // col 1  Y-flip
	vp[8]  = 0.0f;
	vp[9]  = 0.0f;
	vp[10] = b;
	vp[11] = -1.0f; // col 2
	vp[12] = 0.0f;
	vp[13] = 0.0f;
	vp[14] = b * (-kCamZ) + c;
	vp[15] = kCamZ; // col 3
}

static std::vector<uint32_t> ReadSPIRV(const char* path)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file.is_open())
	{
		return {};
	}

	const std::streamsize byteSize = file.tellg();
	if (byteSize % 4 != 0)
	{
		return {};
	}

	file.seekg(0);
	std::vector<uint32_t> code(static_cast<size_t>(byteSize) / 4);
	file.read(reinterpret_cast<char*>(code.data()), byteSize);
	return code;
}

void VulkRender::Initialize(Registry* registry,
							LogicThread* logic,
							const EngineConfig* config,
							VulkanContext* vkCtx,
							VulkanMemory* vkMem,
							SDL_Window* window)
{
	RegistryPtr = registry;
	LogicPtr    = logic;
	ConfigPtr   = config;
	VkCtx       = vkCtx;
	VkMem       = vkMem;
	WindowPtr   = window;

	LOG_INFO("[VulkRender] Initialized");
}

void VulkRender::Start()
{
	device = VkCtx->GetDevice();

	// Depth attachment (recreated on swapchain resize).
	if (!CreateDepthImage())
	{
		LOG_ERROR("[VulkRender] Failed to create depth image");
		return;
	}

	if (!LoadShaders())
	{
		LOG_ERROR("[VulkRender] Shader load failed; thread exiting");
		return;
	}

	if (!CreateFrameSync())
	{
		LOG_ERROR("[VulkRender] Frame sync creation failed; thread exiting");
		return;
	}

	if (!CreatePipeline())
	{
		LOG_ERROR("[VulkRender] Pipeline creation failed; thread exiting");
		return;
	}

	if (!CreateComputePipelines())
	{
		LOG_ERROR("[VulkRender] Compute pipeline creation failed; thread exiting");
		return;
	}

	if (!CreateMeshBuffers())
	{
		LOG_ERROR("[VulkRender] Mesh buffer upload failed; thread exiting");
		return;
	}

	bIsRunning.store(true, std::memory_order_release);
	Thread = std::thread(&VulkRender::ThreadMain, this);
	TrinyxThreading::PinThread(Thread);
	LOG_INFO("[VulkRender] Started");
}

void VulkRender::Stop()
{
	bIsRunning.store(false, std::memory_order_release);
	LOG_INFO("[VulkRender] Stop requested");
}

void VulkRender::Join()
{
	if (Thread.joinable())
	{
		Thread.join();
		LOG_INFO("[VulkRender] Joined");
	}

	// GPU must be fully idle before sync objects, images, and pipelines are destroyed.
	if (VkCtx && VkCtx
		->
		GetDevice() != VK_NULL_HANDLE
	)
	{
		vkDeviceWaitIdle(VkCtx->GetDevice());
	}

	DestroyShaders();
}

// -----------------------------------------------------------------------
// CreateFrameSync
// -----------------------------------------------------------------------

bool VulkRender::CreateFrameSync()
{
	VkCommandPool pool              = VkCtx->GetGraphicsCommandPool();
	const vk::raii::Device& raiiDev = VkCtx->GetRaiiDevice();

	// Allocate all command buffers in one call.
	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool        = pool;
	allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = kMaxFramesInFlight;

	VkCommandBuffer cmds[kMaxFramesInFlight];
	VkResult result = vkAllocateCommandBuffers(device, &allocInfo, cmds);
	if (result != VK_SUCCESS)
	{
		LOG_ERROR_F("[VulkRender] vkAllocateCommandBuffers failed: %d", result);
		return false;
	}

	const vk::SemaphoreCreateInfo semCI{};
	// Pre-signal the fence so the first frame doesn't wait on an unsignalled fence.
	const vk::FenceCreateInfo fenceCI{vk::FenceCreateFlagBits::eSignaled};

	// GpuFrameData struct only — field SoA lives in dedicated FieldSlabs[], scatter output in InstancesBuffer.
	constexpr VkDeviceSize kGpuDataSize = sizeof(GpuFrameData);

	for (int i = 0; i < kMaxFramesInFlight; ++i)
	{
		Frames[i].Cmd      = cmds[i];
		Frames[i].Acquired = raiiDev.createSemaphore(semCI);
		Frames[i].Fence    = raiiDev.createFence(fenceCI);

		// Per-frame GpuData: CPU writes ViewProj + BDAs + instance SoA each frame.
		// PersistentMapped = direct VRAM write on ReBAR; HOST_VISIBLE fallback otherwise.
		Frames[i].GpuData = VkMem->AllocateBuffer(
			kGpuDataSize,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			GpuMemoryDomain::PersistentMapped,
			/*requestDeviceAddress=*/ true);

		if (!Frames[i].GpuData.IsValid())
		{
			LOG_ERROR_F("[VulkRender] GpuData allocation failed for frame slot %d", i);
			return false;
		}

		// Per-frame compute scratch/output buffers.
		// These must be per-frame-slot: two frame slots can be GPU-in-flight simultaneously
		// (fence only guarantees the *same* slot is idle, not the other one).
		// Sharing a single copy would let frame N's scatter overwrite frame N-1's draw args/output.
		const VkDeviceSize kScanSize =
			static_cast<VkDeviceSize>(ConfigPtr->MAX_CACHED_ENTITIES) * sizeof(uint32_t);
		Frames[i].ScanBuffer = VkMem->AllocateBuffer(kScanSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
													 GpuMemoryDomain::DeviceLocal, /*requestDeviceAddress=*/ true);
		if (!Frames[i].ScanBuffer.IsValid())
		{
			LOG_ERROR_F("[VulkRender] ScanBuffer alloc failed (slot %d)", i);
			return false;
		}

		Frames[i].CompactCounterBuffer = VkMem->AllocateBuffer(sizeof(uint32_t),
															   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
															   GpuMemoryDomain::DeviceLocal, /*requestDeviceAddress=*/ true);
		if (!Frames[i].CompactCounterBuffer.IsValid())
		{
			LOG_ERROR_F("[VulkRender] CompactCounterBuffer alloc failed (slot %d)", i);
			return false;
		}

		// DrawArgsBuffer: word 0 (indexCount) set once here; scatter sets word 1 each frame.
		Frames[i].DrawArgsBuffer = VkMem->AllocateBuffer(sizeof(VkDrawIndexedIndirectCommand),
														 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
														 GpuMemoryDomain::PersistentMapped, /*requestDeviceAddress=*/ true);
		if (!Frames[i].DrawArgsBuffer.IsValid())
		{
			LOG_ERROR_F("[VulkRender] DrawArgsBuffer alloc failed (slot %d)", i);
			return false;
		}
		auto* drawArgs = static_cast<uint32_t*>(Frames[i].DrawArgsBuffer.MappedPtr);
		drawArgs[0]    = static_cast<uint32_t>(CubeMesh::IndexCount);
		drawArgs[1]    = 0;
		drawArgs[2]    = 0;
		drawArgs[3]    = 0;
		drawArgs[4]    = 0;

		// InstancesBuffer: compact SoA written by scatter, read by vertex shader.
		// Same layout as a FieldSlab (kGpuOutFieldCount × MAX_CACHED_ENTITIES floats).
		const VkDeviceSize kInstancesSize =
			kGpuOutFieldCount * static_cast<VkDeviceSize>(ConfigPtr->MAX_CACHED_ENTITIES) * sizeof(float);
		Frames[i].InstancesBuffer = VkMem->AllocateBuffer(kInstancesSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
														  GpuMemoryDomain::DeviceLocal, /*requestDeviceAddress=*/ true);
		if (!Frames[i].InstancesBuffer.IsValid())
		{
			LOG_ERROR_F("[VulkRender] InstancesBuffer alloc failed (slot %d)", i);
			return false;
		}
	}

	// 5 field slabs — CPU cycles through them writing raw SoA field data per frame.
	// Each slab: kGpuOutFieldCount × MAX_CACHED_ENTITIES × sizeof(float).
	// Field f base address = slab.DeviceAddr + f * MAX_CACHED_ENTITIES * sizeof(float).
	const VkDeviceSize kFieldSlabSize =
		kGpuOutFieldCount * static_cast<VkDeviceSize>(ConfigPtr->MAX_CACHED_ENTITIES) * sizeof(float);
	for (int i = 0; i < kInstanceBufferCount; ++i)
	{
		FieldSlabs[i] = VkMem->AllocateBuffer(
			kFieldSlabSize,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			GpuMemoryDomain::PersistentMapped,
			/*requestDeviceAddress=*/ true);

		if (!FieldSlabs[i].IsValid())
		{
			LOG_ERROR_F("[VulkRender] FieldSlab allocation failed for slot %d", i);
			return false;
		}
	}

	// One render-finished semaphore per swapchain image (not per frame slot).
	// Avoids reuse while the presentation engine may still hold a reference.
	const uint32_t imageCount = static_cast<uint32_t>(VkCtx->GetSwapchain().Images.size());
	RenderedSems.reserve(imageCount);
	for (uint32_t i = 0; i < imageCount; ++i) RenderedSems.push_back(raiiDev.createSemaphore(semCI));

	LOG_INFO("[VulkRender] Frame sync objects created");
	return true;
}

// -----------------------------------------------------------------------
// LoadShaders / DestroyShaders
// -----------------------------------------------------------------------

bool VulkRender::LoadShaders()
{
	auto vert = ReadSPIRV(TNX_SHADER_DIR "/cube.vert.spv");
	auto frag = ReadSPIRV(TNX_SHADER_DIR "/cube.frag.spv");

	if (vert.empty() || frag.empty())
	{
		LOG_ERROR_F("[VulkRender] Failed to read SPIR-V from %s", TNX_SHADER_DIR);
		return false;
	}

	auto createModule = [&](const std::vector<uint32_t>& code) -> VkShaderModule
	{
		VkShaderModuleCreateInfo ci{};
		ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		ci.codeSize = code.size() * sizeof(uint32_t);
		ci.pCode    = code.data();

		VkShaderModule mod = VK_NULL_HANDLE;
		vkCreateShaderModule(device, &ci, nullptr, &mod);
		return mod;
	};

	VertShader = createModule(vert);
	FragShader = createModule(frag);

	if (VertShader == VK_NULL_HANDLE || FragShader == VK_NULL_HANDLE)
	{
		LOG_ERROR("[VulkRender] Failed to create shader modules");
		return false;
	}

	LOG_INFO("[VulkRender] Shaders loaded (vert + frag)");
	return true;
}

void VulkRender::DestroyShaders()
{
	if (!VkCtx) return;

	if (PredicatePipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(device, PredicatePipeline, nullptr);
		PredicatePipeline = VK_NULL_HANDLE;
	}
	if (PrefixSumPipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(device, PrefixSumPipeline, nullptr);
		PrefixSumPipeline = VK_NULL_HANDLE;
	}
	if (ScatterPipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(device, ScatterPipeline, nullptr);
		ScatterPipeline = VK_NULL_HANDLE;
	}
	if (VertShader != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(device, VertShader, nullptr);
		VertShader = VK_NULL_HANDLE;
	}
	if (FragShader != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(device, FragShader, nullptr);
		FragShader = VK_NULL_HANDLE;
	}
}

bool VulkRender::CreateComputePipelines()
{
	const char* paths[3] = {
		TNX_SHADER_DIR "/predicate.spv",
		TNX_SHADER_DIR "/prefix_sum.spv",
		TNX_SHADER_DIR "/scatter.spv",
	};
	VkPipeline* targets[3] = {&PredicatePipeline, &PrefixSumPipeline, &ScatterPipeline};

	for (int i = 0; i < 3; ++i)
	{
		auto code = ReadSPIRV(paths[i]);
		if (code.empty())
		{
			LOG_ERROR_F("[VulkRender] Failed to read compute SPIR-V: %s", paths[i]);
			return false;
		}

		VkShaderModuleCreateInfo modCI{};
		modCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		modCI.codeSize = code.size() * sizeof(uint32_t);
		modCI.pCode    = code.data();

		VkShaderModule mod = VK_NULL_HANDLE;
		if (vkCreateShaderModule(device, &modCI, nullptr, &mod) != VK_SUCCESS)
		{
			LOG_ERROR_F("[VulkRender] vkCreateShaderModule failed for %s", paths[i]);
			return false;
		}

		VkPipelineShaderStageCreateInfo stageCI{};
		stageCI.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stageCI.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
		stageCI.module = mod;
		stageCI.pName  = "main";

		VkComputePipelineCreateInfo pipeCI{};
		pipeCI.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		pipeCI.stage  = stageCI;
		pipeCI.layout = *PipelineLayout;

		VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeCI, nullptr, targets[i]);
		vkDestroyShaderModule(device, mod, nullptr); // module no longer needed after pipeline creation

		if (result != VK_SUCCESS)
		{
			LOG_ERROR_F("[VulkRender] vkCreateComputePipelines failed for %s: %d", paths[i], result);
			return false;
		}
	}

	LOG_INFO("[VulkRender] Compute pipelines created (predicate + prefix_sum + scatter)");
	return true;
}

bool VulkRender::CreateMeshBuffers()
{
	// Vertex buffer: read by the vertex shader via BDA — no vertex-attribute binding.
	// PersistentMapped (ReBAR) gives direct CPU→VRAM writes without a staging pass.
	VertexBuffer = VkMem->AllocateBuffer(
		sizeof(CubeMesh::Vertices),
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		GpuMemoryDomain::PersistentMapped,
		/*requestDeviceAddress=*/ true);

	if (!VertexBuffer.IsValid())
	{
		LOG_ERROR("[VulkRender] Vertex buffer allocation failed");
		return false;
	}
	std::memcpy(VertexBuffer.MappedPtr, CubeMesh::Vertices, sizeof(CubeMesh::Vertices));

	// Index buffer: bound via vkCmdBindIndexBuffer, not accessed via BDA.
	IndexBuffer = VkMem->AllocateBuffer(
		sizeof(CubeMesh::Indices),
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
		GpuMemoryDomain::PersistentMapped,
		/*requestDeviceAddress=*/ false);

	if (!IndexBuffer.IsValid())
	{
		LOG_ERROR("[VulkRender] Index buffer allocation failed");
		return false;
	}
	std::memcpy(IndexBuffer.MappedPtr, CubeMesh::Indices, sizeof(CubeMesh::Indices));

	LOG_INFO_F("[VulkRender] Mesh buffers uploaded (%zu verts, %zu indices)",
			   CubeMesh::VertexCount, CubeMesh::IndexCount);
	return true;
}

void VulkRender::FillGpuFrameData(FrameSync& frame)
{
	TNX_ZONE_NC("Fill GPU", TNX_COLOR_RENDERING)

	auto* FrameData = static_cast<GpuFrameData*>(frame.GpuData.MappedPtr);
	std::memset(FrameData, 0, sizeof(GpuFrameData));

	const vk::Extent2D ext = VkCtx->GetSwapchain().Extent;
	BuildViewProjMatrix(FrameData->ViewProj, static_cast<float>(ext.width) / static_cast<float>(ext.height));

	// ---- Per-frame BDAs (compute scratch buffers, one copy per frame slot) ---
	FrameData->VerticesAddr       = VertexBuffer.DeviceAddr;
	FrameData->InstancesAddr      = frame.InstancesBuffer.DeviceAddr;
	FrameData->ScanAddr           = frame.ScanBuffer.DeviceAddr;
	FrameData->CompactCounterAddr = frame.CompactCounterBuffer.DeviceAddr;
	FrameData->DrawArgsAddr       = frame.DrawArgsBuffer.DeviceAddr;
	FrameData->Alpha              = LogicPtr->GetFixedAlpha();
	FrameData->EntityCount        = static_cast<uint32_t>(ConfigPtr->MAX_CACHED_ENTITIES);
	FrameData->OutFieldStride     = static_cast<uint32_t>(ConfigPtr->MAX_CACHED_ENTITIES);
	FrameData->FieldCount         = kGpuOutFieldCount;

	const uint32_t kFields[kGpuOutFieldCount] = {
		kSemFlags, // always index 0 — shaders read CurrFieldAddrs[0] for flags
		kSemPosX, kSemPosY, kSemPosZ,
		kSemRotQx, kSemRotQy, kSemRotQz, kSemRotQw,
		kSemScaleX, kSemScaleY, kSemScaleZ,
		kSemColorR, kSemColorG, kSemColorB, kSemColorA,
	};

	const VkDeviceSize fieldStride = static_cast<VkDeviceSize>(ConfigPtr->MAX_CACHED_ENTITIES) * sizeof(float);
	const uint64_t slabBase        = FieldSlabs[CurrentFieldSlab].DeviceAddr;
	const uint64_t prevSlabBase    = FieldSlabs[PrevFieldSlab].DeviceAddr;

	for (uint32_t f = 0; f < kGpuOutFieldCount; ++f)
	{
		FrameData->CurrFieldAddrs[f]   = slabBase + static_cast<uint64_t>(f) * fieldStride;
		FrameData->PrevFieldAddrs[f]   = prevSlabBase + static_cast<uint64_t>(f) * fieldStride;
		FrameData->FieldSemantics[f]   = kFields[f];
		FrameData->FieldElementSize[f] = sizeof(float);
	}

	GPUActiveFrame = CurrentFieldSlab;
	GPUPrevFrame   = PrevFieldSlab;
}

void VulkRender::WriteToFrameSlab()
{
	// ---- Advance field slab slot ---------------------------------------
	uint32_t nextSlab = CurrentFieldSlab;
	do
	{
		nextSlab = (nextSlab + 1) % kInstanceBufferCount;
	} while (nextSlab == GPUActiveFrame || nextSlab == GPUPrevFrame);

	const VkDeviceSize fieldStride = static_cast<VkDeviceSize>(ConfigPtr->MAX_CACHED_ENTITIES) * sizeof(float);
	uint8_t* slabPtr               = static_cast<uint8_t*>(FieldSlabs[nextSlab].MappedPtr);

	// Try to lock a cache frame for reading BEFORE committing to this render tick.
	// Must happen before vkAcquireNextImageKHR so we can bail cleanly (fence still signaled).
	ComponentCacheBase* temporalCache = RegistryPtr->GetTemporalCache();
	ComponentCacheBase* volatileCache = RegistryPtr->GetVolatileCache();

	if (!volatileCache->TryLockFrameForRead(LastVolatileFrame)
#ifdef TNX_ENABLE_ROLLBACK
		|| !temporalCache->TryLockFrameForRead(LastTemporalFrame)
#endif
	)
	{
		LOG_ERROR("[VulkRender] Failed to lock frame for read");
		return;
	}

	TNX_ZONE_NC("Write Frame Slab", TNX_COLOR_RENDERING)
	PrevFieldSlab    = CurrentFieldSlab;
	CurrentFieldSlab = nextSlab;

	TemporalFrameHeader* temporalHdr = temporalCache->GetFrameHeader(LastTemporalFrame);
	TemporalFrameHeader* volatileHdr = volatileCache->GetFrameHeader(LastVolatileFrame);

	assert(temporalHdr->FrameNumber == volatileHdr->FrameNumber);

	const ComponentFieldRegistry& CFR = ComponentFieldRegistry::Get();
	const uint8_t transformSlot       = CFR.GetCacheSlotIndex(TransRot<>::StaticTypeID());
	const uint8_t scaleSlot           = CFR.GetCacheSlotIndex(Scale<>::StaticTypeID());
	const uint8_t colorSlot           = CFR.GetCacheSlotIndex(ColorData<>::StaticTypeID());
	const uint8_t flagsSlot           = CFR.GetCacheSlotIndex(TemporalFlags<>::StaticTypeID());

	// ---- Copy MAX_CACHED_ENTITIES floats per field into current slab ---
	// Field table: { cache, hdr, compSlot, fieldIndex, semantic }
	struct FieldDescription
	{
		ComponentCacheBase* cache;
		TemporalFrameHeader* hdr;
		uint8_t slot;
		size_t fi;
		uint32_t sem;
	};
	const FieldDescription kFields[kGpuOutFieldCount] = {
		{temporalCache, temporalHdr, flagsSlot, 0, kSemFlags},
		{temporalCache, temporalHdr, transformSlot, 0, kSemPosX},
		{temporalCache, temporalHdr, transformSlot, 1, kSemPosY},
		{temporalCache, temporalHdr, transformSlot, 2, kSemPosZ},
		{temporalCache, temporalHdr, transformSlot, 3, kSemRotQx},
		{temporalCache, temporalHdr, transformSlot, 4, kSemRotQy},
		{temporalCache, temporalHdr, transformSlot, 5, kSemRotQz},
		{temporalCache, temporalHdr, transformSlot, 6, kSemRotQw},
		{volatileCache, volatileHdr, scaleSlot, 0, kSemScaleX},
		{volatileCache, volatileHdr, scaleSlot, 1, kSemScaleY},
		{volatileCache, volatileHdr, scaleSlot, 2, kSemScaleZ},
		{volatileCache, volatileHdr, colorSlot, 0, kSemColorR},
		{volatileCache, volatileHdr, colorSlot, 1, kSemColorG},
		{volatileCache, volatileHdr, colorSlot, 2, kSemColorB},
		{volatileCache, volatileHdr, colorSlot, 3, kSemColorA},
	};

	TrinyxJobs::JobCounter GPUTransferCounter;

	for (uint32_t f = 0; f < kGpuOutFieldCount; ++f)
	{
		const FieldDescription& fieldDesc = kFields[f];
		const void* src                   = fieldDesc.cache->GetFieldData(fieldDesc.hdr, fieldDesc.slot, fieldDesc.fi);
		uint8_t* dst                      = slabPtr + static_cast<size_t>(f) * static_cast<size_t>(fieldStride);
		TrinyxJobs::Dispatch([src, dst, fieldStride](uint32_t)
		{
			if (src) std::memcpy(dst, src, static_cast<size_t>(fieldStride));
			else std::memset(dst, 0, static_cast<size_t>(fieldStride));
		}, &GPUTransferCounter, TrinyxJobs::Queue::Render);
	}

	TrinyxJobs::WaitForCounter(&GPUTransferCounter, TrinyxJobs::Queue::Render);

	volatileCache->UnlockFrameRead(LastVolatileFrame);
#ifdef TNX_ENABLE_ROLLBACK
	temporalCache->UnlockFrameRead(LastTemporalFrame);
#endif
}

bool VulkRender::CreatePipeline()
{
	VkFormat colorFmt = static_cast<VkFormat>(VkCtx->GetSwapchain().Format);

	// ---- Push constants: one uint64_t = GpuFrameData BDA ---------------
	// Shared by vertex shader and all 3 compute shaders — one layout for all pipelines.
	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset     = 0;
	pushRange.size       = sizeof(uint64_t);

	// ---- Pipeline layout ------------------------------------------------
	VkPipelineLayoutCreateInfo layoutCI{};
	layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutCI.pushConstantRangeCount = 1;
	layoutCI.pPushConstantRanges    = &pushRange;

	VkPipelineLayout rawLayout = VK_NULL_HANDLE;
	if (vkCreatePipelineLayout(device, &layoutCI, nullptr, &rawLayout) != VK_SUCCESS)
	{
		LOG_ERROR("[VulkRender] vkCreatePipelineLayout failed");
		return false;
	}
	PipelineLayout = vk::raii::PipelineLayout(VkCtx->GetRaiiDevice(), rawLayout);

	// ---- Shader stages --------------------------------------------------
	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = VertShader;
	stages[0].pName  = "main";
	stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = FragShader;
	stages[1].pName  = "main";

	// ---- Vertex input: empty — vertices read via BDA in vertex shader ---
	VkPipelineVertexInputStateCreateInfo vertexInput{};
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	// ---- Input assembly: indexed triangle list --------------------------
	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	// ---- Viewport / scissor: fully dynamic ------------------------------
	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount  = 1;

	// ---- Rasterization: fill, back-face cull, CCW front -----------------
	VkPipelineRasterizationStateCreateInfo raster{};
	raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode    = VK_CULL_MODE_BACK_BIT;
	raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth   = 1.0f;

	// ---- Multisample: 1x ------------------------------------------------
	VkPipelineMultisampleStateCreateInfo multisample{};
	multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	// ---- Depth / stencil: depth test + write, LESS ----------------------
	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable  = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

	// ---- Color blend: write all channels, blending off ------------------
	VkPipelineColorBlendAttachmentState blendAttach{};
	blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	VkPipelineColorBlendStateCreateInfo colorBlend{};
	colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlend.attachmentCount = 1;
	colorBlend.pAttachments    = &blendAttach;

	// ---- Dynamic states: viewport + scissor set each frame --------------
	const VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

	VkPipelineDynamicStateCreateInfo dynState{};
	dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynState.dynamicStateCount = 2;
	dynState.pDynamicStates    = dynStates;

	// ---- Dynamic rendering: color + depth formats, no render pass -------
	VkPipelineRenderingCreateInfo renderingCI{};
	renderingCI.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	renderingCI.colorAttachmentCount    = 1;
	renderingCI.pColorAttachmentFormats = &colorFmt;
	renderingCI.depthAttachmentFormat   = DepthFormat;

	// ---- Assemble -------------------------------------------------------
	VkGraphicsPipelineCreateInfo pipelineCI{};
	pipelineCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineCI.pNext               = &renderingCI; // no renderPass — dynamic rendering
	pipelineCI.stageCount          = 2;
	pipelineCI.pStages             = stages;
	pipelineCI.pVertexInputState   = &vertexInput;
	pipelineCI.pInputAssemblyState = &inputAssembly;
	pipelineCI.pViewportState      = &viewportState;
	pipelineCI.pRasterizationState = &raster;
	pipelineCI.pMultisampleState   = &multisample;
	pipelineCI.pDepthStencilState  = &depthStencil;
	pipelineCI.pColorBlendState    = &colorBlend;
	pipelineCI.pDynamicState       = &dynState;
	pipelineCI.layout              = *PipelineLayout;

	VkPipeline rawPipeline = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineCI, nullptr, &rawPipeline) != VK_SUCCESS)
	{
		LOG_ERROR("[VulkRender] vkCreateGraphicsPipelines failed");
		return false;
	}
	Pipeline = vk::raii::Pipeline(VkCtx->GetRaiiDevice(), rawPipeline);

	LOG_INFO("[VulkRender] Graphics pipeline created");
	return true;
}

void VulkRender::ThreadMain()
{
	LOG_INFO("[VulkRender] Thread running — Step 4 GPU-driven compute pipeline");

	while (!TrinyxEngine::Get().GetJobsInitialized())
	{
	}

	graphicsQueue     = static_cast<VkQueue>(VkCtx->GetQueues().Graphics);
	LastVolatileFrame = LogicPtr->GetLastCompletedFrame();
	while (LastVolatileFrame < 1) { LastVolatileFrame = LogicPtr->GetLastCompletedFrame(); } // wait for valid data

	while (bIsRunning.load(std::memory_order_acquire))
	{
		// Process Render deltas
		uint32_t newTemporalFrame = RegistryPtr->GetTemporalCache()->GetActiveReadFrame();
		uint32_t newVolatileFrame = RegistryPtr->GetVolatileCache()->GetActiveReadFrame();
		if (newVolatileFrame != LastVolatileFrame && newTemporalFrame != LastTemporalFrame) // make sure both have updated
		{
			LastVolatileFrame = newVolatileFrame;
			LastTemporalFrame = newTemporalFrame;
			// Write frame data
			WriteToFrameSlab();
		}
		
		int8_t renderRes = RenderFrame();
		if (renderRes < 0) break;
		else if (renderRes == 0) continue; // This stops us counting FPS from failing to render.

		TrackFPS();

		// Update alpha value?
	}

	LOG_INFO("[VulkRender] Thread exiting");
	bIsRunning.store(false, std::memory_order_release);
}

int VulkRender::RenderFrame()
{
	TNX_ZONE_COARSE_NC("Render_Frame", TNX_COLOR_RENDERING)
	FrameSync& frame = Frames[CurrentFrame];
	VkFence fence    = *frame.Fence;

	{
		TNX_ZONE_COARSE_NC("Render_FenceCheck", TNX_COLOR_RENDERING)
		if (vkGetFenceStatus(device, fence) == VK_NOT_READY)
		{
			return 0;
		}
	}

	uint32_t imageIndex = 0;
	{
		TNX_ZONE_COARSE_NC("Render_Acquire", TNX_COLOR_RENDERING)
		VkResult acquireResult = vkAcquireNextImageKHR(
			device,
			*VkCtx->GetSwapchain().Handle,
			0,
			*frame.Acquired,
			VK_NULL_HANDLE,
			&imageIndex);

		if (acquireResult == VK_NOT_READY || acquireResult == VK_TIMEOUT)
		{
			return 0;
		}

		if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
		{
			OnSwapchainResize();
			return 0;
		}
		else if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
		{
			LOG_ERROR_F("[VulkRender] vkAcquireNextImageKHR failed: %d", acquireResult);
			return -1;
		}
	}

	vkResetFences(device, 1, &fence);

	FillGpuFrameData(frame);

	{
		TNX_ZONE_COARSE_NC("Render_Record", TNX_COLOR_RENDERING)
		RecordCommandBuffer(frame, imageIndex);
	}

	// Submit (sync2 path)
	VkSemaphoreSubmitInfo waitSemInfo{};
	waitSemInfo.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	waitSemInfo.semaphore = *frame.Acquired;
	waitSemInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

	VkSemaphoreSubmitInfo signalSemInfo{};
	signalSemInfo.sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	signalSemInfo.semaphore = *RenderedSems[imageIndex];
	signalSemInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

	VkCommandBufferSubmitInfo cmdInfo{};
	cmdInfo.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
	cmdInfo.commandBuffer = frame.Cmd;

	VkSubmitInfo2 submitInfo{};
	submitInfo.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
	submitInfo.waitSemaphoreInfoCount   = 1;
	submitInfo.pWaitSemaphoreInfos      = &waitSemInfo;
	submitInfo.commandBufferInfoCount   = 1;
	submitInfo.pCommandBufferInfos      = &cmdInfo;
	submitInfo.signalSemaphoreInfoCount = 1;
	submitInfo.pSignalSemaphoreInfos    = &signalSemInfo;

	{
		TNX_ZONE_COARSE_NC("Render_Submit", TNX_COLOR_RENDERING)
		vkQueueSubmit2(graphicsQueue, 1, &submitInfo, fence);
	}

	// Present
	VkSwapchainKHR swapchain = *VkCtx->GetSwapchain().Handle;

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores    = &signalSemInfo.semaphore;
	presentInfo.swapchainCount     = 1;
	presentInfo.pSwapchains        = &swapchain;
	presentInfo.pImageIndices      = &imageIndex;

	{
		TNX_ZONE_COARSE_NC("Render_Present", TNX_COLOR_RENDERING)
		VkResult presentResult = vkQueuePresentKHR(graphicsQueue, &presentInfo);
		if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
		{
			OnSwapchainResize();
		}
		else if (presentResult != VK_SUCCESS)
		{
			LOG_ERROR_F("[VulkRender] vkQueuePresentKHR failed: %d", presentResult);
			return -1;
		}
	}

	CurrentFrame = (CurrentFrame + 1) % kMaxFramesInFlight;

	return 1;
}

void VulkRender::RecordCommandBuffer(FrameSync& frame, uint32_t imageIndex)
{
	VkCommandBuffer cmd         = frame.Cmd;
	const VulkanSwapchain& swap = VkCtx->GetSwapchain();
	VkImage swapImg             = static_cast<VkImage>(swap.Images[imageIndex]);
	VkImageView swapView        = *swap.ImageViews[imageIndex];
	VkImageView depthView       = static_cast<VkImageView>(frame.DepthAttachment.View);
	const vk::Extent2D ext      = swap.Extent;

	vkResetCommandBuffer(cmd, 0);

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &beginInfo);

	// Both images start as UNDEFINED each frame so the driver can discard
	// previous contents (clear handles initialization).
	{
		TNX_ZONE_COARSE_NC("Render_BarrierToRender", TNX_COLOR_RENDERING)

		VkImageMemoryBarrier2 barriers[2]{};

		// Swapchain color image: UNDEFINED → COLOR_ATTACHMENT_OPTIMAL
		barriers[0].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barriers[0].srcStageMask     = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		barriers[0].srcAccessMask    = 0;
		barriers[0].dstStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		barriers[0].dstAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		barriers[0].oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[0].newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barriers[0].image            = swapImg;
		barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

		// Depth image: UNDEFINED → DEPTH_STENCIL_ATTACHMENT_OPTIMAL
		barriers[1].sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barriers[1].srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		barriers[1].srcAccessMask = 0;
		barriers[1].dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
		barriers[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		barriers[1].oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[1].newLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		barriers[1].image            = static_cast<VkImage>(frame.DepthAttachment.Image);
		barriers[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};

		VkDependencyInfo dep{};
		dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dep.imageMemoryBarrierCount = 2;
		dep.pImageMemoryBarriers    = barriers;
		vkCmdPipelineBarrier2(cmd, &dep);
	}

	// ---- Compute: predicate → prefix_sum → scatter --------------------
	// All 3 compute shaders read GpuFrameData via the same push constant BDA.
	// PipelineLayout covers both VERTEX and COMPUTE stages — push once for all.
	{
		TNX_ZONE_COARSE_NC("Render_Compute", TNX_COLOR_RENDERING)

		const uint64_t gpuDataAddr = frame.GpuData.DeviceAddr;
		const uint32_t entityCount = static_cast<uint32_t>(ConfigPtr->MAX_CACHED_ENTITIES);
		const uint32_t dispatchX   = (entityCount + 63u) / 64u;

		// Helper: global compute→compute memory barrier (all BDA storage accesses).
		auto ComputeBarrier = [&]()
		{
			VkMemoryBarrier2 mb{};
			mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
			mb.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
			mb.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
			VkDependencyInfo d{};
			d.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			d.memoryBarrierCount = 1;
			d.pMemoryBarriers    = &mb;
			vkCmdPipelineBarrier2(cmd, &d);
		};

		// Zero CompactCounterBuffer so prefix_sum accumulates from 0 each frame.
		vkCmdFillBuffer(cmd, static_cast<VkBuffer>(frame.CompactCounterBuffer.Buffer), 0, sizeof(uint32_t), 0u);
		{
			VkMemoryBarrier2 mb{};
			mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
			mb.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			mb.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
			VkDependencyInfo d{};
			d.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			d.memoryBarrierCount = 1;
			d.pMemoryBarriers    = &mb;
			vkCmdPipelineBarrier2(cmd, &d);
		}

		// Push GpuFrameData BDA once — covers both compute and vertex stages (same PipelineLayout).
		// This single push is reused by all 3 compute dispatches and the subsequent draw.
		vkCmdPushConstants(cmd, *PipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
						   0, sizeof(uint64_t), &gpuDataAddr);

		// Pass 1: predicate — reads Flags slab, writes ScanAddr (0/1 per entity).
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, PredicatePipeline);
		vkCmdDispatch(cmd, dispatchX, 1, 1);
		ComputeBarrier();

		// Pass 2: prefix_sum — reads ScanAddr, overwrites with exclusive-scan indices.
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, PrefixSumPipeline);
		vkCmdDispatch(cmd, dispatchX, 1, 1);
		ComputeBarrier();

		// Pass 3: scatter — lerps fields, writes compact output to InstancesAddr;
		//                    also sets DrawArgsAddr.instanceCount from CompactCounterAddr.
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ScatterPipeline);
		vkCmdDispatch(cmd, dispatchX, 1, 1);

		// Scatter output → vertex read + indirect draw read.
		VkMemoryBarrier2 scatterDone{};
		scatterDone.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
		scatterDone.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
		scatterDone.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
		scatterDone.dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
			VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
		scatterDone.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
			VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
		VkDependencyInfo scatterDep{};
		scatterDep.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		scatterDep.memoryBarrierCount = 1;
		scatterDep.pMemoryBarriers    = &scatterDone;
		vkCmdPipelineBarrier2(cmd, &scatterDep);
	}

	// loadOp CLEAR handles background (purple) and depth (1.0) in one step.
	{
		TNX_ZONE_COARSE_NC("Render_BeginPass", TNX_COLOR_RENDERING)

		VkClearValue colorClear{};
		colorClear.color.float32[0] = 0.4f;
		colorClear.color.float32[1] = 0.0f;
		colorClear.color.float32[2] = 0.6f;
		colorClear.color.float32[3] = 1.0f;

		VkClearValue depthClear{};
		depthClear.depthStencil = {1.0f, 0};

		VkRenderingAttachmentInfo colorAttach{};
		colorAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		colorAttach.imageView   = swapView;
		colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttach.clearValue  = colorClear;

		VkRenderingAttachmentInfo depthAttach{};
		depthAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		depthAttach.imageView   = depthView;
		depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttach.clearValue  = depthClear;

		VkRenderingInfo renderingInfo{};
		renderingInfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
		renderingInfo.renderArea           = {{0, 0}, {ext.width, ext.height}};
		renderingInfo.layerCount           = 1;
		renderingInfo.colorAttachmentCount = 1;
		renderingInfo.pColorAttachments    = &colorAttach;
		renderingInfo.pDepthAttachment     = &depthAttach;

		vkCmdBeginRendering(cmd, &renderingInfo);
	}

	{
		TNX_ZONE_COARSE_NC("Render_Draw", TNX_COLOR_RENDERING)

		// Dynamic viewport + scissor (required since they were declared dynamic in pipeline).
		VkViewport viewport{};
		viewport.width    = static_cast<float>(ext.width);
		viewport.height   = static_cast<float>(ext.height);
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		vkCmdSetViewport(cmd, 0, 1, &viewport);

		VkRect2D scissor{};
		scissor.extent = {ext.width, ext.height};
		vkCmdSetScissor(cmd, 0, 1, &scissor);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *Pipeline);

		// GpuFrameData BDA was already pushed with VERTEX|COMPUTE flags during the compute section.
		// Push constants persist across pipeline binds; no re-push needed here.

		// Index buffer bound here; vertex data fetched via BDA (no vertex buffer bind).
		VkBuffer indexBuf = static_cast<VkBuffer>(IndexBuffer.Buffer);
		vkCmdBindIndexBuffer(cmd, indexBuf, 0, VK_INDEX_TYPE_UINT16);

		// DrawIndexedIndirect: instanceCount was written by scatter into this frame's DrawArgsBuffer[1].
		VkBuffer drawBuf = static_cast<VkBuffer>(frame.DrawArgsBuffer.Buffer);
		vkCmdDrawIndexedIndirect(cmd, drawBuf, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
	}

	vkCmdEndRendering(cmd);

	{
		TNX_ZONE_COARSE_NC("Render_BarrierToPresent", TNX_COLOR_RENDERING)

		VkImageMemoryBarrier2 barrierToPresent{};
		barrierToPresent.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barrierToPresent.srcStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		barrierToPresent.srcAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		barrierToPresent.dstStageMask     = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
		barrierToPresent.dstAccessMask    = 0;
		barrierToPresent.oldLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrierToPresent.newLayout        = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		barrierToPresent.image            = swapImg;
		barrierToPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

		VkDependencyInfo dep{};
		dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dep.imageMemoryBarrierCount = 1;
		dep.pImageMemoryBarriers    = &barrierToPresent;
		vkCmdPipelineBarrier2(cmd, &dep);
	}

	vkEndCommandBuffer(cmd);
}

void VulkRender::OnSwapchainResize()
{
	VkCtx->RecreateSwapchain(WindowPtr);
	CreateDepthImage();
}

void VulkRender::TrackFPS()
{
	RenderFrameCount++;
	const double now = SDL_GetPerformanceCounter() /
		static_cast<double>(SDL_GetPerformanceFrequency());
	RenderFpsTimer     += now - RenderLastFPSCheck;
	RenderLastFPSCheck = now;

	if (RenderFpsTimer >= 1.0) [[unlikely]]
	{
		LOG_DEBUG_F("Render FPS: %d | Frame: %.2fms",
					static_cast<int>(RenderFrameCount / RenderFpsTimer),
					(RenderFpsTimer / RenderFrameCount) * 1000.0);
		RenderFrameCount = 0;
		RenderFpsTimer   = 0.0;
	}
}

bool VulkRender::CreateDepthImage()
{
	// Select a supported depth format (prefer packed depth+stencil).
	const std::vector<VkFormat> candidates{
		VK_FORMAT_D32_SFLOAT_S8_UINT,
		VK_FORMAT_D24_UNORM_S8_UINT,
	};

	VkFormat depthFormat = VK_FORMAT_UNDEFINED;
	for (VkFormat fmt : candidates)
	{
		VkFormatProperties2 props{};
		props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
		vkGetPhysicalDeviceFormatProperties2(VkCtx->GetPhysicalDevice(), fmt, &props);
		if (props.formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
		{
			depthFormat = fmt;
			break;
		}
	}
	assert(depthFormat != VK_FORMAT_UNDEFINED);
	DepthFormat = depthFormat;

	const vk::Extent2D ext = VkCtx->GetSwapchain().Extent;
	for (int i = 0; i < kMaxFramesInFlight; ++i)
	{
		Frames[i].DepthAttachment = VkMem->AllocateImage(
			{ext.width, ext.height},
			depthFormat,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			VK_IMAGE_ASPECT_DEPTH_BIT);

		if (!Frames[i].DepthAttachment.IsValid()) return false;
	}

	return true;
}
