#include "EditorRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include "CacheSlotMeta.h"
#include "ColorData.h"
#include "EditorContext.h"
#include "GpuFrameData.h"
#include "ImGuizmo.h"
#include "Logger.h"
#include "LogicThread.h"
#include "MeshRef.h"
#include "Registry.h"
#include "Scale.h"
#include "TemporalComponentCache.h"
#include "TransRot.h"
#include "World.h"
#include "WorldViewport.h"

// -----------------------------------------------------------------------
// ImGuiEventQueue — ring buffer for cross-thread SDL event forwarding.
// Defined here to keep SDL_Event out of EditorRenderer.h.
// -----------------------------------------------------------------------
struct ImGuiEventQueue
{
	static constexpr uint32_t Capacity = 64;
	SDL_Event Events[Capacity]{};
	uint32_t Head = 0;
	uint32_t Tail = 0;
	std::mutex Mutex;

	// SDL3 drop.data is only valid during SDL_PollEvent — capture it immediately.
	std::string PendingDropPath;

	void Push(const SDL_Event& e)
	{
		std::lock_guard lock(Mutex);
		if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) PendingDropPath = e.drop.data;
		Events[Head] = e;
		Head         = (Head + 1) % Capacity;
		if (Head == Tail) Tail = (Tail + 1) % Capacity;
	}

	// Drain events: feed to ImGui and collect any dropped file paths.
	// Returns the last dropped file path (empty if none).
	std::string DrainIntoImGui()
	{
		std::string droppedFile;
		std::lock_guard lock(Mutex);
		while (Tail != Head)
		{
			SDL_Event& ev = Events[Tail];
			ImGui_ImplSDL3_ProcessEvent(&ev);
			if (ev.type == SDL_EVENT_DROP_FILE) droppedFile = std::move(PendingDropPath);
			Tail = (Tail + 1) % Capacity;
		}
		return droppedFile;
	}
};

// -----------------------------------------------------------------------
// CRTP hooks
// -----------------------------------------------------------------------

void EditorRenderer::OnPostStart()
{
	if (!InitImGui())
	{
		LOG_ERROR("[EditorRenderer] ImGui initialization failed; editor disabled");
	}
}

void EditorRenderer::OnShutdown()
{
	ShutdownImGui();
}

void EditorRenderer::OnPreRecord()
{
	if (bImGuiInitialized) BuildImGuiFrame();
}

void EditorRenderer::RecordOverlay(VkCommandBuffer cmd)
{
	if (bImGuiInitialized)
	{
		ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
	}
}

// -----------------------------------------------------------------------
// ImGui lifecycle
// -----------------------------------------------------------------------

bool EditorRenderer::InitImGui()
{
	VkDescriptorPoolSize poolSizes[] = {
		{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16},
	};

	VkDescriptorPoolCreateInfo poolCI{};
	poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	poolCI.maxSets       = 16;
	poolCI.poolSizeCount = 1;
	poolCI.pPoolSizes    = poolSizes;

	if (vkCreateDescriptorPool(Device, &poolCI, nullptr, &ImGuiDescriptorPool) != VK_SUCCESS)
	{
		LOG_ERROR("[EditorRenderer] Failed to create ImGui descriptor pool");
		return false;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& io    = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

	ImGui::StyleColorsDark();

	int logicalW = 0, physicalW = 0;
	SDL_GetWindowSize(WindowPtr, &logicalW, nullptr);
	SDL_GetWindowSizeInPixels(WindowPtr, &physicalW, nullptr);
	float dpiScale = (logicalW > 0) ? static_cast<float>(physicalW) / static_cast<float>(logicalW) : 1.0f;

	if (dpiScale > 1.01f)
	{
		ImGui::GetStyle().ScaleAllSizes(dpiScale);
		ImFontConfig fontCfg;
		fontCfg.SizePixels  = 13.0f * dpiScale;
		fontCfg.OversampleH = 2;
		fontCfg.OversampleV = 2;
		io.Fonts->AddFontDefault(&fontCfg);
	}

	ImGui_ImplSDL3_InitForVulkan(WindowPtr);

	const VulkanSwapchain& swap = VkCtx->GetSwapchain();
	VkFormat colorFormat        = static_cast<VkFormat>(swap.Format);

	ImGui_ImplVulkan_InitInfo initInfo{};
	initInfo.ApiVersion          = VK_API_VERSION_1_4;
	initInfo.Instance            = VkCtx->GetInstance();
	initInfo.PhysicalDevice      = VkCtx->GetPhysicalDevice();
	initInfo.Device              = Device;
	initInfo.QueueFamily         = VkCtx->GetQueues().GraphicsFamily;
	initInfo.Queue               = static_cast<VkQueue>(VkCtx->GetQueues().Graphics);
	initInfo.DescriptorPool      = ImGuiDescriptorPool;
	initInfo.MinImageCount       = static_cast<uint32_t>(swap.Images.size());
	initInfo.ImageCount          = static_cast<uint32_t>(swap.Images.size());
	initInfo.UseDynamicRendering = true;
	initInfo.MinAllocationSize   = 1024 * 1024;

	initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
	initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;
	initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat   = DepthFormat;

	if (!ImGui_ImplVulkan_Init(&initInfo))
	{
		LOG_ERROR("[EditorRenderer] ImGui_ImplVulkan_Init failed");
		return false;
	}

	EventQueue = new ImGuiEventQueue();

	Editor = new EditorContext();
	Editor->Initialize(EnginePtr, LogicPtr, &Meshes);

	bImGuiInitialized = true;
	LOG_INFO("[EditorRenderer] ImGui initialized (dynamic rendering, docking enabled)");
	return true;
}

void EditorRenderer::ShutdownImGui()
{
	if (!bImGuiInitialized) return;

	vkDeviceWaitIdle(Device);

	delete Editor;
	Editor = nullptr;
	delete EventQueue;
	EventQueue = nullptr;

	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();

	if (ImGuiDescriptorPool != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorPool(Device, ImGuiDescriptorPool, nullptr);
		ImGuiDescriptorPool = VK_NULL_HANDLE;
	}

	bImGuiInitialized = false;
	LOG_INFO("[EditorRenderer] ImGui shut down");
}

void EditorRenderer::PushImGuiEvent(const SDL_Event& event)
{
	if (EventQueue) EventQueue->Push(event);
}

void EditorRenderer::DrainImGuiEvents()
{
	if (!EventQueue) return;
	std::string dropped = EventQueue->DrainIntoImGui();
	if (!dropped.empty() && Editor) Editor->HandleDroppedFile(dropped);
}

void EditorRenderer::BuildImGuiFrame()
{
	DrainImGuiEvents();

	// Process deferred PIE stop before opening a new ImGui frame.
	// This runs one frame after bPIEStopRequested was set, ensuring the
	// frame that emitted ImGui::Image() calls for the viewport textures has
	// been fully recorded and submitted. StopPIE calls WaitForGPU() so the
	// descriptor sets are only freed once the GPU is no longer referencing them.
	if (Editor && Editor->bPIEStopRequested)
	{
		Editor->bPIEStopRequested = false;
		Editor->StopPIE();
	}

	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
	ImGuizmo::BeginFrame();

	if (Editor) Editor->BuildFrame();

	ImGui::Render();
}

// -----------------------------------------------------------------------
// Multi-viewport management (PIE)
// -----------------------------------------------------------------------

void EditorRenderer::AddViewport(WorldViewport* vp)
{
	ActiveViewports.push_back(vp);
	LOG_INFO_F("[EditorRenderer] Added viewport %p (world %p), %u active",
			   static_cast<void*>(vp), static_cast<void*>(vp->TargetWorld),
			   static_cast<uint32_t>(ActiveViewports.size()));
}

void EditorRenderer::RemoveViewport(WorldViewport* vp)
{
	auto it = std::find(ActiveViewports.begin(), ActiveViewports.end(), vp);
	if (it != ActiveViewports.end())
	{
		ActiveViewports.erase(it);
		LOG_INFO_F("[EditorRenderer] Removed viewport %p, %u remaining",
				   static_cast<void*>(vp),
				   static_cast<uint32_t>(ActiveViewports.size()));
	}
}

void EditorRenderer::AllocateViewportResources(WorldViewport* vp, uint32_t width, uint32_t height)
{
	vp->Width  = width;
	vp->Height = height;

	// Offscreen color target — must match the pipeline's color attachment format (swapchain format).
	VkFormat swapchainFmt = static_cast<VkFormat>(VkCtx->GetSwapchain().Format);
	vp->ColorTarget       = VkMem->AllocateImage(
		{width, height},
		swapchainFmt,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		VK_IMAGE_ASPECT_COLOR_BIT);

	// Offscreen depth target
	vp->DepthTarget = VkMem->AllocateImage(
		{width, height},
		DepthFormat,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);

	// Per-world field slabs (same layout as main renderer's)
	const VkDeviceSize slabSize = static_cast<VkDeviceSize>(ConfigPtr->MAX_CACHED_ENTITIES)
		* sizeof(float) * GpuOutFieldCount;
	for (auto& slab : vp->FieldSlabs)
	{
		slab = VkMem->AllocateBuffer(
			slabSize,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			GpuMemoryDomain::PersistentMapped,
			true);
	}

	// Per-viewport GpuFrameData buffers — one per FrameSync slot to avoid
	// overwriting slot N's data while the GPU is still executing slot N-1.
	for (auto& gd : vp->GpuData)
	{
		gd = VkMem->AllocateBuffer(
			sizeof(GpuFrameData),
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
			GpuMemoryDomain::PersistentMapped,
			true);
	}

	// Dirty tracking
	vp->AllocateDirtyPlanes(DirtyWordCount);

	// Register offscreen color target as ImGui texture for compositing
	VkSampler sampler = VK_NULL_HANDLE;
	VkSamplerCreateInfo samplerCI{};
	samplerCI.sType     = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerCI.magFilter = VK_FILTER_LINEAR;
	samplerCI.minFilter = VK_FILTER_LINEAR;
	vkCreateSampler(Device, &samplerCI, nullptr, &sampler);

	vp->ImGuiTexture = ImGui_ImplVulkan_AddTexture(
		sampler,
		static_cast<VkImageView>(vp->ColorTarget.View),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	LOG_INFO_F("[EditorRenderer] Allocated viewport resources %ux%u", width, height);
}

void EditorRenderer::FreeViewportResources(WorldViewport* vp)
{
	// Remove ImGui texture registration
	if (vp->ImGuiTexture != VK_NULL_HANDLE)
	{
		ImGui_ImplVulkan_RemoveTexture(vp->ImGuiTexture);
		vp->ImGuiTexture = VK_NULL_HANDLE;
	}

	// Free dirty tracking
	vp->FreeDirtyPlanes();

	// Free field slabs and GpuData
	for (auto& slab : vp->FieldSlabs) slab.Free();
	for (auto& gd : vp->GpuData) gd.Free();

	// Free offscreen images
	vp->ColorTarget.Free();
	vp->DepthTarget.Free();

	vp->Width  = 0;
	vp->Height = 0;

	LOG_INFO("[EditorRenderer] Freed viewport resources");
}

// -----------------------------------------------------------------------
// CRTP hooks — viewport slab updates + frame recording
// -----------------------------------------------------------------------

void EditorRenderer::UpdateViewportSlabs()
{
	for (WorldViewport* vp : ActiveViewports)
	{
		if (!vp->bActive || !vp->TargetWorld) continue;

		Registry* reg                 = vp->TargetWorld->GetRegistry();
		ComponentCacheBase* temporal  = reg->GetTemporalCache();
		ComponentCacheBase* volatile_ = reg->GetVolatileCache();
		uint32_t newTemporal          = temporal->GetActiveReadFrame();
		uint32_t newVolatile          = volatile_->GetActiveReadFrame();

		if (newVolatile != vp->LastVolatileFrame && newTemporal != vp->LastTemporalFrame)
		{
			vp->LastVolatileFrame = newVolatile;
			vp->LastTemporalFrame = newTemporal;
			WriteToViewportSlab(vp);
		}
	}
}

void EditorRenderer::RecordFrame(FrameSync& frame, uint32_t imageIndex)
{
	if (ActiveViewports.empty())
	{
		RecordCommandBuffer(frame, imageIndex);
		return;
	}

	RecordPIEFrame(frame, imageIndex);
}

// -----------------------------------------------------------------------
// Viewport slab writing — mirrors RendererCore::WriteToFrameSlab but
// reads from the viewport's world and writes to the viewport's slabs.
// -----------------------------------------------------------------------

void EditorRenderer::WriteToViewportSlab(WorldViewport* vp)
{
	uint32_t nextSlab = vp->CurrentFieldSlab;
	do
	{
		nextSlab = (nextSlab + 1) % kViewportSlabCount;
	} while (nextSlab == vp->GPUActiveFrame || nextSlab == vp->GPUPrevFrame);

	Registry* reg                 = vp->TargetWorld->GetRegistry();
	ComponentCacheBase* temporalC = reg->GetTemporalCache();
	ComponentCacheBase* volatileC = reg->GetVolatileCache();

	if (!volatileC->TryLockFrameForRead(vp->LastVolatileFrame)) return;
#ifdef TNX_ENABLE_ROLLBACK
	if (!temporalC->TryLockFrameForRead(vp->LastTemporalFrame))
	{
		volatileC->UnlockFrameRead(vp->LastVolatileFrame);
		return;
	}
#endif

	vp->PrevFieldSlab    = vp->CurrentFieldSlab;
	vp->CurrentFieldSlab = nextSlab;

	TemporalFrameHeader* temporalHdr = temporalC->GetFrameHeader(vp->LastTemporalFrame);
	TemporalFrameHeader* volatileHdr = volatileC->GetFrameHeader(vp->LastVolatileFrame);

	const VkDeviceSize fieldStride = static_cast<VkDeviceSize>(ConfigPtr->MAX_CACHED_ENTITIES) * sizeof(float);
	uint8_t* slabPtr               = static_cast<uint8_t*>(vp->FieldSlabs[nextSlab].MappedPtr);

	const ComponentTypeID transformSlot = TransRot<>::StaticTemporalIndex();
	const ComponentTypeID scaleSlot     = Scale<>::StaticTemporalIndex();
	const ComponentTypeID colorSlot     = ColorData<>::StaticTemporalIndex();
	const ComponentTypeID flagsSlot     = CacheSlotMeta<>::StaticTemporalIndex();
	const ComponentTypeID meshRefSlot   = MeshRef<>::StaticTemporalIndex();

	struct FD
	{
		ComponentCacheBase* cache;
		TemporalFrameHeader* hdr;
		ComponentTypeID slot;
		size_t fi;
	};
	const FD fieldDescs[GpuOutFieldCount] = {
		{temporalC, temporalHdr, flagsSlot, 0},
		{temporalC, temporalHdr, transformSlot, 0}, {temporalC, temporalHdr, transformSlot, 1},
		{temporalC, temporalHdr, transformSlot, 2}, {temporalC, temporalHdr, transformSlot, 3},
		{temporalC, temporalHdr, transformSlot, 4}, {temporalC, temporalHdr, transformSlot, 5},
		{temporalC, temporalHdr, transformSlot, 6},
		{volatileC, volatileHdr, scaleSlot, 0}, {volatileC, volatileHdr, scaleSlot, 1},
		{volatileC, volatileHdr, scaleSlot, 2},
		{volatileC, volatileHdr, colorSlot, 0}, {volatileC, volatileHdr, colorSlot, 1},
		{volatileC, volatileHdr, colorSlot, 2}, {volatileC, volatileHdr, colorSlot, 3},
		{volatileC, volatileHdr, meshRefSlot, 0},
	};

	// Full copy — viewport slabs are always fully rewritten (no dirty tracking optimization yet)
	for (uint32_t f = 0; f < GpuOutFieldCount; ++f)
	{
		const FD& fd    = fieldDescs[f];
		const void* src = fd.cache->GetFieldData(fd.hdr, fd.slot, fd.fi);
		uint8_t* dst    = slabPtr + static_cast<size_t>(f) * static_cast<size_t>(fieldStride);
		if (src) std::memcpy(dst, src, static_cast<size_t>(fieldStride));
		else std::memset(dst, 0, static_cast<size_t>(fieldStride));
	}

	vp->bHasSlabData = true;

	reg->RenderAck.store(temporalHdr->FrameNumber, std::memory_order_release);
	reg->RenderHasAcked = true;

	volatileC->UnlockFrameRead(vp->LastVolatileFrame);
#ifdef TNX_ENABLE_ROLLBACK
	temporalC->UnlockFrameRead(vp->LastTemporalFrame);
#endif
}

// -----------------------------------------------------------------------
// Fill GpuFrameData for a viewport — uses viewport's slab addresses and
// its world's camera, but the FrameSync's scratch buffer addresses.
// -----------------------------------------------------------------------

static void MultMat4(float* out, const float* A, const float* B)
{
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
		{
			float sum = 0.0f;
			for (int k = 0; k < 4; ++k) sum += A[k * 4 + row] * B[col * 4 + k];
			out[col * 4 + row] = sum;
		}
}

void EditorRenderer::FillGpuFrameDataForViewport(WorldViewport* vp, FrameSync& frame)
{
	auto* data = static_cast<GpuFrameData*>(vp->GpuData[CurrentFrame].MappedPtr);
	std::memset(data, 0, sizeof(GpuFrameData));

	// ViewProj from viewport's world — use the same read frame that WriteToViewportSlab captured
	Registry* reg            = vp->TargetWorld->GetRegistry();
	ComponentCacheBase* tc   = reg->GetTemporalCache();
	TemporalFrameHeader* hdr = tc->GetFrameHeader(vp->LastTemporalFrame);
	MultMat4(data->ViewProj, hdr->ProjectionMatrix.m, hdr->ViewMatrix.m);

	// Scratch buffers from shared FrameSync
	data->VerticesAddr          = Meshes.GetVertexBufferAddr();
	data->InstancesAddr         = frame.InstancesBuffer.DeviceAddr;
	data->ScanAddr              = frame.ScanBuffer.DeviceAddr;
	data->CompactCounterAddr    = frame.CompactCounterBuffer.DeviceAddr;
	data->DrawArgsAddr          = frame.DrawArgsBuffer.DeviceAddr;
	data->UnsortedInstancesAddr = frame.UnsortedInstancesBuffer.DeviceAddr;
	data->MeshHistogramAddr     = frame.MeshHistogramBuffer.DeviceAddr;
	data->MeshWriteIdxAddr      = frame.MeshWriteIdxBuffer.DeviceAddr;
	data->MeshTableAddr         = Meshes.GetMeshTableAddr();
	data->MeshCount             = Meshes.GetMeshCount();

	LogicThread* logic   = vp->TargetWorld->GetLogicThread();
	data->Alpha          = logic ? static_cast<float>(std::clamp(logic->GetFixedAlpha(), 0.0, 1.0)) : 1.0f;
	data->EntityCount    = static_cast<uint32_t>(ConfigPtr->MAX_CACHED_ENTITIES);
	data->OutFieldStride = static_cast<uint32_t>(ConfigPtr->MAX_CACHED_ENTITIES);
	data->FieldCount     = GpuOutFieldCount;

	// Field addresses from viewport's slabs
	const uint32_t Fields[GpuOutFieldCount] = {
		SemFlags,
		SemPosX, SemPosY, SemPosZ,
		SemRotQx, SemRotQy, SemRotQz, SemRotQw,
		SemScaleX, SemScaleY, SemScaleZ,
		SemColorR, SemColorG, SemColorB, SemColorA,
		SemMeshID,
	};

	const VkDeviceSize fieldStride = static_cast<VkDeviceSize>(ConfigPtr->MAX_CACHED_ENTITIES) * sizeof(float);
	const uint64_t currBase        = vp->FieldSlabs[vp->CurrentFieldSlab].DeviceAddr;
	const uint64_t prevBase        = vp->FieldSlabs[vp->PrevFieldSlab].DeviceAddr;

	for (uint32_t f = 0; f < GpuOutFieldCount; ++f)
	{
		data->CurrFieldAddrs[f]   = currBase + static_cast<uint64_t>(f) * fieldStride;
		data->PrevFieldAddrs[f]   = prevBase + static_cast<uint64_t>(f) * fieldStride;
		data->FieldSemantics[f]   = Fields[f];
		data->FieldElementSize[f] = sizeof(float);
	}

	vp->GPUActiveFrame = vp->CurrentFieldSlab;
	vp->GPUPrevFrame   = vp->PrevFieldSlab;
}

// -----------------------------------------------------------------------
// RecordViewportScenePass — compute dispatches + scene draw to a
// viewport's offscreen color + depth targets.
// -----------------------------------------------------------------------

void EditorRenderer::RecordViewportScenePass(VkCommandBuffer cmd, FrameSync& frame, WorldViewport* vp)
{
	const VkExtent2D ext = {vp->Width, vp->Height};

	// Barriers: offscreen color + depth to attachment optimal
	{
		VkImageMemoryBarrier2 barriers[2]{};

		barriers[0].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barriers[0].srcStageMask     = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		barriers[0].srcAccessMask    = 0;
		barriers[0].dstStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		barriers[0].dstAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		barriers[0].oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[0].newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barriers[0].image            = static_cast<VkImage>(vp->ColorTarget.Image);
		barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

		barriers[1].sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barriers[1].srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		barriers[1].srcAccessMask = 0;
		barriers[1].dstStageMask  = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
		barriers[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		barriers[1].oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[1].newLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		barriers[1].image            = static_cast<VkImage>(vp->DepthTarget.Image);
		barriers[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};

		VkDependencyInfo dep{};
		dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dep.imageMemoryBarrierCount = 2;
		dep.pImageMemoryBarriers    = barriers;
		vkCmdPipelineBarrier2(cmd, &dep);
	}

	// Compute dispatches — same pipelines, viewport's GpuData address
	{
		const uint64_t gpuDataAddr = vp->GpuData[CurrentFrame].DeviceAddr;
		const uint32_t entityCount = static_cast<uint32_t>(ConfigPtr->MAX_CACHED_ENTITIES);
		const uint32_t dispatchX   = (entityCount + 63u) / 64u;

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

		// Zero CompactCounter and MeshHistogram
		vkCmdFillBuffer(cmd, static_cast<VkBuffer>(frame.CompactCounterBuffer.Buffer), 0, sizeof(uint32_t), 0u);
		vkCmdFillBuffer(cmd, static_cast<VkBuffer>(frame.MeshHistogramBuffer.Buffer), 0,
						MaxMeshSlots * sizeof(uint32_t), 0u);
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

		vkCmdPushConstants(cmd, *PipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
						   0, sizeof(uint64_t), &gpuDataAddr);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, PredicatePipeline);
		vkCmdDispatch(cmd, dispatchX, 1, 1);
		ComputeBarrier();

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, PrefixSumPipeline);
		vkCmdDispatch(cmd, dispatchX, 1, 1);
		ComputeBarrier();

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ScatterPipeline);
		vkCmdDispatch(cmd, dispatchX, 1, 1);
		ComputeBarrier();

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, BuildDrawsPipeline);
		vkCmdDispatch(cmd, 1, 1, 1);
		ComputeBarrier();

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, SortInstancesPipeline);
		vkCmdDispatch(cmd, dispatchX, 1, 1);

		// Final barrier: sorted instances + draw args → vertex shader + indirect draw
		VkMemoryBarrier2 sortDone{};
		sortDone.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
		sortDone.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
		sortDone.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
		sortDone.dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
		sortDone.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
		VkDependencyInfo sortDep{};
		sortDep.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		sortDep.memoryBarrierCount = 1;
		sortDep.pMemoryBarriers    = &sortDone;
		vkCmdPipelineBarrier2(cmd, &sortDep);
	}

	// Scene render pass to offscreen targets
	{
		VkClearValue colorClear{};
		colorClear.color.float32[0] = 0.4f;
		colorClear.color.float32[1] = 0.0f;
		colorClear.color.float32[2] = 0.6f;
		colorClear.color.float32[3] = 1.0f;

		VkClearValue depthClear{};
		depthClear.depthStencil = {1.0f, 0};

		VkRenderingAttachmentInfo colorAttach{};
		colorAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		colorAttach.imageView   = static_cast<VkImageView>(vp->ColorTarget.View);
		colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttach.clearValue  = colorClear;

		VkRenderingAttachmentInfo depthAttach{};
		depthAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		depthAttach.imageView   = static_cast<VkImageView>(vp->DepthTarget.View);
		depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttach.clearValue  = depthClear;

		VkRenderingInfo ri{};
		ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
		ri.renderArea           = {{0, 0}, ext};
		ri.layerCount           = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments    = &colorAttach;
		ri.pDepthAttachment     = &depthAttach;

		vkCmdBeginRendering(cmd, &ri);

		VkViewport viewport{};
		viewport.width    = static_cast<float>(ext.width);
		viewport.height   = static_cast<float>(ext.height);
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		vkCmdSetViewport(cmd, 0, 1, &viewport);

		VkRect2D scissor{};
		scissor.extent = ext;
		vkCmdSetScissor(cmd, 0, 1, &scissor);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *Pipeline);

		VkBuffer indexBuf = Meshes.GetIndexBufferHandle();
		vkCmdBindIndexBuffer(cmd, indexBuf, 0, VK_INDEX_TYPE_UINT32);

		VkBuffer drawBuf = static_cast<VkBuffer>(frame.DrawArgsBuffer.Buffer);
		vkCmdDrawIndexedIndirect(cmd, drawBuf, 0, Meshes.GetMeshCount(),
								 sizeof(VkDrawIndexedIndirectCommand));

		vkCmdEndRendering(cmd);
	}

	// Barrier: offscreen color → SHADER_READ_ONLY for ImGui sampling
	{
		VkImageMemoryBarrier2 barrier{};
		barrier.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barrier.srcStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		barrier.srcAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstStageMask     = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
		barrier.dstAccessMask    = VK_ACCESS_2_SHADER_READ_BIT;
		barrier.oldLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.image            = static_cast<VkImage>(vp->ColorTarget.Image);
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

		VkDependencyInfo dep{};
		dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dep.imageMemoryBarrierCount = 1;
		dep.pImageMemoryBarriers    = &barrier;
		vkCmdPipelineBarrier2(cmd, &dep);
	}
}

// -----------------------------------------------------------------------
// RecordPIEFrame — render each viewport to offscreen, then composite
// all viewports via ImGui onto the swapchain.
// -----------------------------------------------------------------------

void EditorRenderer::RecordPIEFrame(FrameSync& frame, uint32_t imageIndex)
{
	VkCommandBuffer cmd         = frame.Cmd;
	const VulkanSwapchain& swap = VkCtx->GetSwapchain();
	VkImage swapImg             = static_cast<VkImage>(swap.Images[imageIndex]);
	VkImageView swapView        = *swap.ImageViews[imageIndex];
	const vk::Extent2D ext      = swap.Extent;

	vkResetCommandBuffer(cmd, 0);

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &beginInfo);

	// Render each viewport's world to its offscreen target.
	// Viewports share scratch buffers (ScanBuffer, CompactCounter, DrawArgs, Instances, etc.)
	// so we need a full barrier between each viewport's scene pass to ensure the previous
	// viewport's draws finish reading scratch data before the next viewport's compute overwrites it.
	bool needScratchBarrier = false;
	for (WorldViewport* vp : ActiveViewports)
	{
		if (!vp->bActive || !vp->TargetWorld || !vp->bHasSlabData) continue;

		if (needScratchBarrier)
		{
			VkMemoryBarrier2 mb{};
			mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
			mb.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
			mb.srcAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
			mb.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
			VkDependencyInfo dep{};
			dep.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			dep.memoryBarrierCount = 1;
			dep.pMemoryBarriers    = &mb;
			vkCmdPipelineBarrier2(cmd, &dep);
		}

		FillGpuFrameDataForViewport(vp, frame);
		RecordViewportScenePass(cmd, frame, vp);
		needScratchBarrier = true;
	}

	// Transition skipped viewport color targets from UNDEFINED → SHADER_READ_ONLY
	// so ImGui can safely sample them in the composite pass.
	for (WorldViewport* vp : ActiveViewports)
	{
		if (!vp->bActive || !vp->TargetWorld) continue;
		if (vp->bHasSlabData) continue; // Already transitioned by RecordViewportScenePass

		VkImageMemoryBarrier2 barrier{};
		barrier.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barrier.srcStageMask     = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		barrier.srcAccessMask    = 0;
		barrier.dstStageMask     = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
		barrier.dstAccessMask    = VK_ACCESS_2_SHADER_READ_BIT;
		barrier.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.image            = static_cast<VkImage>(vp->ColorTarget.Image);
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

		VkDependencyInfo dep{};
		dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dep.imageMemoryBarrierCount = 1;
		dep.pImageMemoryBarriers    = &barrier;
		vkCmdPipelineBarrier2(cmd, &dep);
	}

	// Barrier: swapchain → color attachment for ImGui composite
	{
		VkImageMemoryBarrier2 barrier{};
		barrier.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barrier.srcStageMask     = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		barrier.srcAccessMask    = 0;
		barrier.dstStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		barrier.dstAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.image            = swapImg;
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

		VkDependencyInfo dep{};
		dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dep.imageMemoryBarrierCount = 1;
		dep.pImageMemoryBarriers    = &barrier;
		vkCmdPipelineBarrier2(cmd, &dep);
	}

	// ImGui composite pass onto swapchain (clears, then draws all panels)
	{
		VkClearValue clearColor{};
		clearColor.color.float32[0] = 0.1f;
		clearColor.color.float32[1] = 0.1f;
		clearColor.color.float32[2] = 0.1f;
		clearColor.color.float32[3] = 1.0f;

		VkRenderingAttachmentInfo colorAttach{};
		colorAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		colorAttach.imageView   = swapView;
		colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttach.clearValue  = clearColor;

		VkRenderingInfo ri{};
		ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
		ri.renderArea           = {{0, 0}, {ext.width, ext.height}};
		ri.layerCount           = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments    = &colorAttach;

		vkCmdBeginRendering(cmd, &ri);
		RecordOverlay(cmd);
		vkCmdEndRendering(cmd);
	}

	// Barrier: swapchain → present
	{
		VkImageMemoryBarrier2 barrier{};
		barrier.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barrier.srcStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		barrier.srcAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstStageMask     = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
		barrier.dstAccessMask    = 0;
		barrier.oldLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.newLayout        = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		barrier.image            = swapImg;
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

		VkDependencyInfo dep{};
		dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dep.imageMemoryBarrierCount = 1;
		dep.pImageMemoryBarriers    = &barrier;
		vkCmdPipelineBarrier2(cmd, &dep);
	}

	vkEndCommandBuffer(cmd);
}
