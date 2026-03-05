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
#include "LogicThread.h"
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

	if (!CreateMeshBuffers())
	{
		LOG_ERROR("[VulkRender] Mesh buffer upload failed; thread exiting");
		return;
	}

	bIsRunning.store(true, std::memory_order_release);
	Thread = std::thread(&VulkRender::ThreadMain, this);
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
	if (VkCtx&& VkCtx
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

	// GpuFrameData struct + appended SoA instance slots (kGpuOutFieldCount floats for 1 entity).
	constexpr VkDeviceSize kGpuDataSize = sizeof(GpuFrameData) + kGpuOutFieldCount * sizeof(float);

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
	const vk::Extent2D ext = VkCtx->GetSwapchain().Extent;
	const float aspect     = static_cast<float>(ext.width) / static_cast<float>(ext.height);

	auto* fd = static_cast<GpuFrameData*>(frame.GpuData.MappedPtr);
	std::memset(fd, 0, sizeof(GpuFrameData));

	// ---- Camera + projection -------------------------------------------
	BuildViewProjMatrix(fd->ViewProj, aspect);

	// ---- Buffer device addresses ---------------------------------------
	fd->VerticesAddr = VertexBuffer.DeviceAddr;
	// SoA instance data sits immediately after GpuFrameData in the same buffer.
	fd->InstancesAddr = frame.GpuData.DeviceAddr + sizeof(GpuFrameData);

	// ---- Instance layout -----------------------------------------------
	fd->EntityCount    = 1;
	fd->FieldCount     = kGpuOutFieldCount;
	fd->OutFieldStride = 1; // 1 entity → stride 1, inst[k-1] = field k for entity 0
	fd->Alpha          = 1.0f;

	// ---- Hardcoded single instance (SoA: kGpuOutFieldCount floats) -----
	// Slot order matches kSem* constants: inst[kSem - 1] = value for entity 0.
	auto* inst = reinterpret_cast<float*>(
		static_cast<uint8_t*>(frame.GpuData.MappedPtr) + sizeof(GpuFrameData));

	inst[kSemPosX - 1]   = 0.0f; // position
	inst[kSemPosY - 1]   = 0.0f;
	inst[kSemPosZ - 1]   = 0.0f;
	inst[kSemRotX - 1]   = 0.0f; // rotation (radians)
	inst[kSemRotY - 1]   = 0.0f;
	inst[kSemRotZ - 1]   = 0.0f;
	inst[kSemScaleX - 1] = 1.0f; // scale
	inst[kSemScaleY - 1] = 1.0f;
	inst[kSemScaleZ - 1] = 1.0f;
	// Read first entity's color from VolatileSlab frame 0
	float r = 1.0f, g = 0.5f, b = 0.0f, a = 1.0f; // fallback orange
	{
		ComponentCacheBase* cache = RegistryPtr->GetVolatileCache();
		TemporalFrameHeader* hdr  = cache->GetFrameHeader(LastLogicFrame);
		const uint8_t slot        = ComponentFieldRegistry::Get().GetCacheSlotIndex(ColorData<>::StaticTypeID());
		size_t count              = 0;
		const auto* rp            = static_cast<const float*>(cache->GetFieldData(hdr, slot, 0, count));
		if (rp && count > 0)
		{
			const auto* gp = static_cast<const float*>(cache->GetFieldData(hdr, slot, 1, count));
			const auto* bp = static_cast<const float*>(cache->GetFieldData(hdr, slot, 2, count));
			const auto* ap = static_cast<const float*>(cache->GetFieldData(hdr, slot, 3, count));
			r              = rp[0];
			g              = gp ? gp[0] : 0.0f;
			b              = bp ? bp[0] : 0.0f;
			a              = ap ? ap[0] : 1.0f;
		}
	}
	inst[kSemColorR - 1] = r;
	inst[kSemColorG - 1] = g;
	inst[kSemColorB - 1] = b;
	inst[kSemColorA - 1] = a;
}

bool VulkRender::CreatePipeline()
{
	VkFormat colorFmt = static_cast<VkFormat>(VkCtx->GetSwapchain().Format);

	// ---- Push constants: one uint64_t = GpuFrameData BDA ---------------
	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
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
	LOG_INFO("[VulkRender] Thread running — Step 3 draw cube");

	graphicsQueue = static_cast<VkQueue>(VkCtx->GetQueues().Graphics);

	while (bIsRunning.load(std::memory_order_acquire))
	{
		// Process Render deltas
		LastLogicFrame = LogicPtr->GetLastCompletedFrame();

		if (RenderFrame() < 0) break;

		TrackFPS();
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
			std::this_thread::yield();
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

		if (acquireResult == VK_NOT_READY || acquireResult == VK_TIMEOUT) return 0;

		if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
		{
			OnSwapchainResize();
			return 0; // fence still SIGNALED — next check on this slot passes immediately
		}
		else if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
		{
			LOG_ERROR_F("[VulkRender] vkAcquireNextImageKHR failed: %d", acquireResult);
			return -1;
		}
	}

	vkResetFences(device, 1, &fence);

	// Fill per-frame GPU data, then record.
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
	VkImageView depthView       = static_cast<VkImageView>(DepthAttachment.View);
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
		barriers[1].image            = static_cast<VkImage>(DepthAttachment.Image);
		barriers[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};

		VkDependencyInfo dep{};
		dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dep.imageMemoryBarrierCount = 2;
		dep.pImageMemoryBarriers    = barriers;
		vkCmdPipelineBarrier2(cmd, &dep);
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

		// Push the GpuFrameData buffer device address — the vertex shader reads
		// all per-frame data (ViewProj, VerticesAddr, InstancesAddr) through it.
		uint64_t gpuDataAddr = frame.GpuData.DeviceAddr;
		vkCmdPushConstants(cmd, *PipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
						   0, sizeof(uint64_t), &gpuDataAddr);

		// Index buffer bound here; vertex data fetched via BDA (no vertex buffer bind).
		VkBuffer indexBuf = static_cast<VkBuffer>(IndexBuffer.Buffer);
		vkCmdBindIndexBuffer(cmd, indexBuf, 0, VK_INDEX_TYPE_UINT16);

		vkCmdDrawIndexed(cmd, static_cast<uint32_t>(CubeMesh::IndexCount), 1, 0, 0, 0);
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
	DepthAttachment        = VkMem->AllocateImage(
		{ext.width, ext.height},
		depthFormat,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		VK_IMAGE_ASPECT_DEPTH_BIT);

	return DepthAttachment.IsValid();
}