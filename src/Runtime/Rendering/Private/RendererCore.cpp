#include "RendererCore.h"

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

#include "CacheSlotMeta.h"
#include "ColorData.h"
#include "MeshRef.h"
#include "Scale.h"
#include "TransRot.h"

#include <immintrin.h>
#include "LogicThread.h"
#include "TrinyxEngine.h"
#include "../../Core/Private/ThreadPinning.h"

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static void MultMat4(float* out, const float* A, const float* B)
{
	for (int col = 0; col < 4; ++col)
	{
		for (int row = 0; row < 4; ++row)
		{
			float sum = 0.0f;
			for (int k = 0; k < 4; ++k) sum += A[k * 4 + row] * B[col * 4 + k];
			out[col * 4 + row] = sum;
		}
	}
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

// -----------------------------------------------------------------------
// Initialize / Start / Stop / Join
// -----------------------------------------------------------------------

template <typename Derived>
void RendererCore<Derived>::Initialize(Registry* registry,
									   LogicThread* logic,
									   const EngineConfig* config,
									   VulkanContext* vkCtx,
									   VulkanMemory* vkMem,
									   SDL_Window* window, InputBuffer* vizInput)
{
	RegistryPtr = registry;
	LogicPtr    = logic;
	ConfigPtr   = config;
	VkCtx       = vkCtx;
	VkMem       = vkMem;
	WindowPtr   = window;
	VizInputPtr = vizInput;

	DirtyWordCount = (static_cast<uint32_t>(config->MAX_RENDERABLE_ENTITIES) + 63) / 64;
	DirtySnapshot  = new uint64_t[DirtyWordCount]();
	for (auto& plane : DirtyPlanes) plane = new uint64_t[DirtyWordCount]();

	LOG_INFO("[Renderer] Initialized");
}

template <typename Derived>
void RendererCore<Derived>::Start()
{
	device = VkCtx->GetDevice();

	if (!CreateDepthImage())
	{
		LOG_ERROR("[Renderer] Failed to create depth image");
		return;
	}

	if (!LoadShaders())
	{
		LOG_ERROR("[Renderer] Shader load failed; thread exiting");
		return;
	}

	if (!CreateFrameSync())
	{
		LOG_ERROR("[Renderer] Frame sync creation failed; thread exiting");
		return;
	}

	if (!CreatePipeline())
	{
		LOG_ERROR("[Renderer] Pipeline creation failed; thread exiting");
		return;
	}

	if (!CreateComputePipelines())
	{
		LOG_ERROR("[Renderer] Compute pipeline creation failed; thread exiting");
		return;
	}

	if (!CreateMeshBuffers())
	{
		LOG_ERROR("[Renderer] Mesh buffer upload failed; thread exiting");
		return;
	}

#ifdef TNX_GPU_PICKING
	if (!CreatePickImages())
	{
		LOG_ERROR("[Renderer] Pick image creation failed; thread exiting");
		return;
	}
	if (!LoadPickShaders())
	{
		LOG_ERROR("[Renderer] Pick shader load failed; thread exiting");
		return;
	}
	if (!CreatePickPipeline())
	{
		LOG_ERROR("[Renderer] Pick pipeline creation failed; thread exiting");
		return;
	}
#endif

#if TNX_DEV_METRICS
	SDL_DisplayID displayId     = SDL_GetDisplayForWindow(WindowPtr);
	const SDL_DisplayMode* mode = displayId ? SDL_GetCurrentDisplayMode(displayId) : nullptr;
	if (mode && mode->refresh_rate > 0.0f)
	{
		DisplayRefreshMs = 1000.0 / static_cast<double>(mode->refresh_rate);
		LOG_INFO_F("[Renderer] Display refresh: %.1f Hz (%.2f ms scanout offset)",
				   mode->refresh_rate, DisplayRefreshMs);
	}
	else
	{
		DisplayRefreshMs = 16.667;
		LOG_INFO("[Renderer] Could not query refresh rate, assuming 60 Hz");
	}
#endif

	Self().OnPostStart();

	bIsRunning.store(true, std::memory_order_release);
	Thread = std::thread(&RendererCore::ThreadMain, this);
	TrinyxThreading::PinThread(Thread);
	LOG_INFO("[Renderer] Started");
}

template <typename Derived>
void RendererCore<Derived>::Stop()
{
	bIsRunning.store(false, std::memory_order_release);
	LOG_INFO("[Renderer] Stop requested");
}

template <typename Derived>
void RendererCore<Derived>::Join()
{
	if (Thread.joinable())
	{
		Thread.join();
		LOG_INFO("[Renderer] Joined");
	}

	if (VkCtx && VkCtx->GetDevice() != VK_NULL_HANDLE)
	{
		vkDeviceWaitIdle(VkCtx->GetDevice());
	}

	DestroyShaders();
#ifdef TNX_GPU_PICKING
	DestroyPickShaders();
#endif

	delete[] DirtySnapshot;
	DirtySnapshot = nullptr;
	for (auto& plane : DirtyPlanes)
	{
		delete[] plane;
		plane = nullptr;
	}

	Self().OnShutdown();
}

// -----------------------------------------------------------------------
// ThreadMain / RenderFrame
// -----------------------------------------------------------------------

template <typename Derived>
void RendererCore<Derived>::ThreadMain()
{
	LOG_INFO("[Renderer] Thread running — GPU-driven compute pipeline");

	while (!TrinyxEngine::Get().GetJobsInitialized())
	{
	}

	graphicsQueue     = static_cast<VkQueue>(VkCtx->GetQueues().Graphics);

	while (bIsRunning.load(std::memory_order_acquire))
	{
		uint32_t newTemporalFrame = RegistryPtr->GetTemporalCache()->GetActiveReadFrame();
		uint32_t newVolatileFrame = RegistryPtr->GetVolatileCache()->GetActiveReadFrame();

		if (newVolatileFrame != LastVolatileFrame && newTemporalFrame != LastTemporalFrame)
		{
			LastVolatileFrame = newVolatileFrame;
			LastTemporalFrame = newTemporalFrame;
			WriteToFrameSlab();
		}

		int8_t renderRes = RenderFrame();
		if (renderRes < 0) break;
		else if (renderRes == 0) continue;

		TrackFPS();
	}

	LOG_INFO("[Renderer] Thread exiting");
	bIsRunning.store(false, std::memory_order_release);
}

template <typename Derived>
int RendererCore<Derived>::RenderFrame()
{
	TNX_ZONE_COARSE_NC("Render_Frame", TNX_COLOR_RENDERING)
	FrameSync& frame = Frames[CurrentFrame];
	VkFence fence    = *frame.Fence;

	{
		TNX_ZONE_COARSE_NC("Render_FenceWait", TNX_COLOR_RENDERING)
		VkResult fenceResult = vkWaitForFences(device, 1, &fence, VK_TRUE, 2'000);
		if (fenceResult == VK_TIMEOUT)
		{
			return 0;
		}
		if (fenceResult != VK_SUCCESS)
		{
			LOG_ERROR_F("[Renderer] vkWaitForFences failed: %d", fenceResult);
			return -1;
		}
	}

#ifdef TNX_GPU_PICKING
	// After fence: the readback buffer from this frame slot is safe to read.
#if defined(TNX_GPU_PICKING_FAST)
	// FAST mode: every frame copies a pixel, so every slot has a valid result
	// after its fence completes. Read unconditionally.
	{
		auto* pickData = static_cast<const uint32_t*>(frame.PickReadbackBuffer.MappedPtr);
		PickResult.store(pickData[0], std::memory_order_relaxed);
		bPickResultReady.store(true, std::memory_order_release);
	}
#else
	// On-demand: only read if this slot actually had a pick copy recorded.
	if (PickReadbackFrame == CurrentFrame)
	{
		auto* pickData = static_cast<const uint32_t*>(frame.PickReadbackBuffer.MappedPtr);
		PickResult.store(pickData[0], std::memory_order_relaxed);
		bPickResultReady.store(true, std::memory_order_release);
	}
#endif
#endif

	if (bResizeRequested.exchange(false, std::memory_order_acq_rel))
	{
		OnSwapchainResize();
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
			LOG_ERROR_F("[Renderer] vkAcquireNextImageKHR failed: %d", acquireResult);
			return -1;
		}
	}

	vkResetFences(device, 1, &fence);

	FillGpuFrameData(frame);

	// Hook: editor builds ImGui frame here; gameplay is a no-op.
	Self().OnPreRecord();

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
			LOG_ERROR_F("[Renderer] vkQueuePresentKHR failed: %d", presentResult);
			return -1;
		}
	}

#if TNX_DEV_METRICS
	if (FrameInputTimestamp[CurrentFrame] != 0)
	{
		uint64_t presentTime = SDL_GetPerformanceCounter();
		double pipelineMs    = static_cast<double>(presentTime - FrameInputTimestamp[CurrentFrame])
			/ static_cast<double>(SDL_GetPerformanceFrequency()) * 1000.0;
		double totalMs = pipelineMs + DisplayRefreshMs;
		LatencyAccumMs += totalMs;
		++LatencySamples;
#if TNX_DEV_METRICS_DETAILED
		LOG_DEBUG_F("[Latency] Pipeline: %.2fms | Scanout: %.2fms | Total: %.2fms",
					pipelineMs, DisplayRefreshMs, totalMs);
#endif
	}
#endif

	CurrentFrame = (CurrentFrame + 1) % kMaxFramesInFlight;

	return 1;
}

// -----------------------------------------------------------------------
// RecordCommandBuffer
// -----------------------------------------------------------------------

template <typename Derived>
void RendererCore<Derived>::RecordCommandBuffer(FrameSync& frame, uint32_t imageIndex)
{
	VkCommandBuffer cmd         = frame.Cmd;
	const VulkanSwapchain& swap = VkCtx->GetSwapchain();
	VkImage swapImg             = static_cast<VkImage>(swap.Images[imageIndex]);
	VkImageView swapView        = *swap.ImageViews[imageIndex];
	VkImageView depthView       = static_cast<VkImageView>(frame.DepthAttachment.View);
	const vk::Extent2D ext      = swap.Extent;

	// --- Pick state for this frame ---
#if defined(TNX_GPU_PICKING_FAST)
	// FAST: always pick, read mouse position every frame.
	// SDL_GetMouseState returns logical (window) coordinates; the pick attachment
	// is at physical pixel resolution. Scale by the DPI ratio.

	// TODO: if we're not in editor and determinism is enabled use VizInput mouse pos instead
	float mx, my;
	SDL_GetMouseState(&mx, &my);
	int logicalW = 0, physicalW = 0;
	SDL_GetWindowSize(WindowPtr, &logicalW, nullptr);
	SDL_GetWindowSizeInPixels(WindowPtr, &physicalW, nullptr);
	const float dpiScale = (logicalW > 0) ? static_cast<float>(physicalW) / static_cast<float>(logicalW) : 1.0f;
	const int32_t pickX  = static_cast<int32_t>(mx * dpiScale);
	const int32_t pickY  = static_cast<int32_t>(my * dpiScale);

	// Debug logging (every 60 frames to avoid spam)
	static uint32_t pickDebugFrameCounter = 0;
	if (++pickDebugFrameCounter >= 60)
	{
		//LOG_INFO_F("[Picking] Mouse: (%.1f, %.1f) logical, (%d, %d) physical, DPI scale: %.2f, extent: %ux%u",
		//		   mx, my, pickX, pickY, dpiScale, ext.width, ext.height);
		pickDebugFrameCounter = 0;
	}
#elif defined(TNX_GPU_PICKING)
	// On-demand: only pick when requested.
	const bool bDoPick = bPickRequested.load(std::memory_order_acquire);
	int32_t pickX      = 0, pickY = 0;
	if (bDoPick) [[unlikely]]
	{
		pickX = PickX.load(std::memory_order_relaxed);
		pickY = PickY.load(std::memory_order_relaxed);
		bPickRequested.store(false, std::memory_order_relaxed);
	}
#endif

	vkResetCommandBuffer(cmd, 0);

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &beginInfo);

	// Barriers: UNDEFINED → attachment optimal
	{
		TNX_ZONE_COARSE_NC("Render_BarrierToRender", TNX_COLOR_RENDERING)

#if defined(TNX_GPU_PICKING_FAST)
		// FAST: always 3 barriers (swap + depth + pick)
		VkImageMemoryBarrier2 barriers[3]{};
		constexpr uint32_t barrierCount = 3;
#elif defined(TNX_GPU_PICKING)
		VkImageMemoryBarrier2 barriers[3]{};
		uint32_t barrierCount = 2;
#else
		VkImageMemoryBarrier2 barriers[2]{};
		constexpr uint32_t barrierCount = 2;
#endif

		barriers[0].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barriers[0].srcStageMask     = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		barriers[0].srcAccessMask    = 0;
		barriers[0].dstStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		barriers[0].dstAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		barriers[0].oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[0].newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barriers[0].image            = swapImg;
		barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

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

#if defined(TNX_GPU_PICKING_FAST)
		// FAST: pick barrier is unconditional
		barriers[2].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barriers[2].srcStageMask     = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		barriers[2].srcAccessMask    = 0;
		barriers[2].dstStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		barriers[2].dstAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		barriers[2].oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[2].newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barriers[2].image            = static_cast<VkImage>(frame.PickAttachment.Image);
		barriers[2].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
#elif defined(TNX_GPU_PICKING)
		if (bDoPick) [[unlikely]]
		{
			barriers[2].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			barriers[2].srcStageMask     = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
			barriers[2].srcAccessMask    = 0;
			barriers[2].dstStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			barriers[2].dstAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
			barriers[2].oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
			barriers[2].newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			barriers[2].image            = static_cast<VkImage>(frame.PickAttachment.Image);
			barriers[2].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			barrierCount                 = 3;
		}
#endif

		VkDependencyInfo dep{};
		dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dep.imageMemoryBarrierCount = barrierCount;
		dep.pImageMemoryBarriers    = barriers;
		vkCmdPipelineBarrier2(cmd, &dep);
	}

	// Compute: predicate → prefix_sum → scatter
	{
		TNX_ZONE_COARSE_NC("Render_Compute", TNX_COLOR_RENDERING)

		const uint64_t gpuDataAddr = frame.GpuData.DeviceAddr;
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

		// Zero CompactCounter and MeshHistogram before compute passes
		vkCmdFillBuffer(cmd, static_cast<VkBuffer>(frame.CompactCounterBuffer.Buffer), 0, sizeof(uint32_t), 0u);
		vkCmdFillBuffer(cmd, static_cast<VkBuffer>(frame.MeshHistogramBuffer.Buffer), 0,
						kMaxMeshSlots * sizeof(uint32_t), 0u);
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

#if defined(TNX_GPU_PICKING_FAST)
		// FAST: always use pick scatter (writes entity cache index)
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ScatterPickPipeline);
#elif defined(TNX_GPU_PICKING)
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
						  bDoPick ? ScatterPickPipeline : ScatterPipeline);
#else
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ScatterPipeline);
#endif
		vkCmdDispatch(cmd, dispatchX, 1, 1);
		ComputeBarrier(); // scatter → build_draws

		// Pass 4: build_draws — prefix-sum histogram → DrawArgs + MeshWriteIdx base offsets
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, BuildDrawsPipeline);
		vkCmdDispatch(cmd, 1, 1, 1); // single workgroup, 256 threads
		ComputeBarrier();            // build_draws → sort_instances

		// Pass 5: sort_instances — reorder unsorted → sorted by MeshID
#if defined(TNX_GPU_PICKING_FAST)
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, SortPickPipeline);
#elif defined(TNX_GPU_PICKING)
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
						  bDoPick ? SortPickPipeline : SortInstancesPipeline);
#else
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, SortInstancesPipeline);
#endif
		vkCmdDispatch(cmd, dispatchX, 1, 1);

		// Final barrier: sorted instances + draw args → vertex shader + indirect draw
		VkMemoryBarrier2 sortDone{};
		sortDone.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
		sortDone.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
		sortDone.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
		sortDone.dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
			VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
		sortDone.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
			VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
		VkDependencyInfo sortDep{};
		sortDep.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		sortDep.memoryBarrierCount = 1;
		sortDep.pMemoryBarriers    = &sortDone;
		vkCmdPipelineBarrier2(cmd, &sortDep);
	}

	// Begin rendering pass
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

#if defined(TNX_GPU_PICKING_FAST)
		// FAST: always two color attachments
		VkClearValue pickClear{};
		pickClear.color.uint32[0] = UINT32_MAX;

		VkRenderingAttachmentInfo pickAttach{};
		pickAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		pickAttach.imageView   = static_cast<VkImageView>(frame.PickAttachment.View);
		pickAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		pickAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
		pickAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
		pickAttach.clearValue  = pickClear;

		VkRenderingAttachmentInfo colorAttachments[2] = {colorAttach, pickAttach};

		VkRenderingInfo renderingInfo{};
		renderingInfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
		renderingInfo.renderArea           = {{0, 0}, {ext.width, ext.height}};
		renderingInfo.layerCount           = 1;
		renderingInfo.colorAttachmentCount = 2;
		renderingInfo.pColorAttachments    = colorAttachments;
		renderingInfo.pDepthAttachment     = &depthAttach;
#elif defined(TNX_GPU_PICKING)
		VkRenderingAttachmentInfo colorAttachments[2];
		uint32_t colorAttachCount = 1;
		colorAttachments[0]       = colorAttach;

		if (bDoPick) [[unlikely]]
		{
			VkClearValue pickClear{};
			pickClear.color.uint32[0] = UINT32_MAX;

			VkRenderingAttachmentInfo pickAttach{};
			pickAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
			pickAttach.imageView   = static_cast<VkImageView>(frame.PickAttachment.View);
			pickAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			pickAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
			pickAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
			pickAttach.clearValue  = pickClear;

			colorAttachments[1] = pickAttach;
			colorAttachCount    = 2;
		}

		VkRenderingInfo renderingInfo{};
		renderingInfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
		renderingInfo.renderArea           = {{0, 0}, {ext.width, ext.height}};
		renderingInfo.layerCount           = 1;
		renderingInfo.colorAttachmentCount = colorAttachCount;
		renderingInfo.pColorAttachments    = colorAttachments;
		renderingInfo.pDepthAttachment     = &depthAttach;
#else
		VkRenderingInfo renderingInfo{};
		renderingInfo.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
		renderingInfo.renderArea           = {{0, 0}, {ext.width, ext.height}};
		renderingInfo.layerCount           = 1;
		renderingInfo.colorAttachmentCount = 1;
		renderingInfo.pColorAttachments    = &colorAttach;
		renderingInfo.pDepthAttachment     = &depthAttach;
#endif

		vkCmdBeginRendering(cmd, &renderingInfo);
	}

	// Draw
	{
		TNX_ZONE_COARSE_NC("Render_Draw", TNX_COLOR_RENDERING)

		VkViewport viewport{};
		viewport.width    = static_cast<float>(ext.width);
		viewport.height   = static_cast<float>(ext.height);
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		vkCmdSetViewport(cmd, 0, 1, &viewport);

		VkRect2D scissor{};
		scissor.extent = {ext.width, ext.height};
		vkCmdSetScissor(cmd, 0, 1, &scissor);

#if defined(TNX_GPU_PICKING_FAST)
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *PickPipeline);
#elif defined(TNX_GPU_PICKING)
		if (bDoPick) [[unlikely]]
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *PickPipeline);
		else vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *Pipeline);
#else
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *Pipeline);
#endif

		VkBuffer indexBuf = Meshes.GetIndexBufferHandle();
		vkCmdBindIndexBuffer(cmd, indexBuf, 0, VK_INDEX_TYPE_UINT32);

		VkBuffer drawBuf = static_cast<VkBuffer>(frame.DrawArgsBuffer.Buffer);
		vkCmdDrawIndexedIndirect(cmd, drawBuf, 0, Meshes.GetMeshCount(),
								 sizeof(VkDrawIndexedIndirectCommand));
	}

	// End the scene render pass (which may have 2 color attachments for picking).
	vkCmdEndRendering(cmd);

	// Hook: editor renders ImGui overlay in a separate 1-attachment pass.
	// ImGui's pipeline was created with 1 color attachment — it can't run inside
	// the 2-attachment pick render pass.
	{
		VkRenderingAttachmentInfo overlayAttach{};
		overlayAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		overlayAttach.imageView   = swapView;
		overlayAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		overlayAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
		overlayAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

		VkRenderingInfo overlayRI{};
		overlayRI.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
		overlayRI.renderArea           = {{0, 0}, {ext.width, ext.height}};
		overlayRI.layerCount           = 1;
		overlayRI.colorAttachmentCount = 1;
		overlayRI.pColorAttachments    = &overlayAttach;

		vkCmdBeginRendering(cmd, &overlayRI);
		Self().RecordOverlay(cmd);
		vkCmdEndRendering(cmd);
	}

	// Pick readback: transition pick image → transfer src, copy one pixel to staging buffer.
#if defined(TNX_GPU_PICKING_FAST)
	{
		int32_t px = (pickX >= 0 && pickX < static_cast<int32_t>(ext.width)) ? pickX : 0;
		int32_t py = (pickY >= 0 && pickY < static_cast<int32_t>(ext.height)) ? pickY : 0;

		VkImageMemoryBarrier2 pickToTransfer{};
		pickToTransfer.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		pickToTransfer.srcStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		pickToTransfer.srcAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		pickToTransfer.dstStageMask     = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		pickToTransfer.dstAccessMask    = VK_ACCESS_2_TRANSFER_READ_BIT;
		pickToTransfer.oldLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		pickToTransfer.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		pickToTransfer.image            = static_cast<VkImage>(frame.PickAttachment.Image);
		pickToTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

		VkDependencyInfo pickDep{};
		pickDep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		pickDep.imageMemoryBarrierCount = 1;
		pickDep.pImageMemoryBarriers    = &pickToTransfer;
		vkCmdPipelineBarrier2(cmd, &pickDep);

		VkBufferImageCopy2 copyRegion{};
		copyRegion.sType            = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
		copyRegion.bufferOffset     = 0;
		copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copyRegion.imageOffset      = {px, py, 0};
		copyRegion.imageExtent      = {1, 1, 1};

		VkCopyImageToBufferInfo2 copyInfo{};
		copyInfo.sType          = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2;
		copyInfo.srcImage       = static_cast<VkImage>(frame.PickAttachment.Image);
		copyInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		copyInfo.dstBuffer      = static_cast<VkBuffer>(frame.PickReadbackBuffer.Buffer);
		copyInfo.regionCount    = 1;
		copyInfo.pRegions       = &copyRegion;

		vkCmdCopyImageToBuffer2(cmd, &copyInfo);
		PickReadbackFrame = CurrentFrame;
	}
#elif defined(TNX_GPU_PICKING)
	if (bDoPick) [[unlikely]]
	{
		int32_t px = (pickX >= 0 && pickX < static_cast<int32_t>(ext.width)) ? pickX : 0;
		int32_t py = (pickY >= 0 && pickY < static_cast<int32_t>(ext.height)) ? pickY : 0;

		VkImageMemoryBarrier2 pickToTransfer{};
		pickToTransfer.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		pickToTransfer.srcStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		pickToTransfer.srcAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		pickToTransfer.dstStageMask     = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		pickToTransfer.dstAccessMask    = VK_ACCESS_2_TRANSFER_READ_BIT;
		pickToTransfer.oldLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		pickToTransfer.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		pickToTransfer.image            = static_cast<VkImage>(frame.PickAttachment.Image);
		pickToTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

		VkDependencyInfo pickDep{};
		pickDep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		pickDep.imageMemoryBarrierCount = 1;
		pickDep.pImageMemoryBarriers    = &pickToTransfer;
		vkCmdPipelineBarrier2(cmd, &pickDep);

		VkBufferImageCopy2 copyRegion{};
		copyRegion.sType            = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
		copyRegion.bufferOffset     = 0;
		copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copyRegion.imageOffset      = {px, py, 0};
		copyRegion.imageExtent      = {1, 1, 1};

		VkCopyImageToBufferInfo2 copyInfo{};
		copyInfo.sType          = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2;
		copyInfo.srcImage       = static_cast<VkImage>(frame.PickAttachment.Image);
		copyInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		copyInfo.dstBuffer      = static_cast<VkBuffer>(frame.PickReadbackBuffer.Buffer);
		copyInfo.regionCount    = 1;
		copyInfo.pRegions       = &copyRegion;

		vkCmdCopyImageToBuffer2(cmd, &copyInfo);
		PickReadbackFrame = CurrentFrame;
	}
#endif // TNX_GPU_PICKING

	// Barrier: color attachment → present
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

// -----------------------------------------------------------------------
// CreateFrameSync
// -----------------------------------------------------------------------

template <typename Derived>
bool RendererCore<Derived>::CreateFrameSync()
{
	VkCommandPool pool              = VkCtx->GetGraphicsCommandPool();
	const vk::raii::Device& raiiDev = VkCtx->GetRaiiDevice();

	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool        = pool;
	allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = kMaxFramesInFlight;

	VkCommandBuffer cmds[kMaxFramesInFlight];
	VkResult result = vkAllocateCommandBuffers(device, &allocInfo, cmds);
	if (result != VK_SUCCESS)
	{
		LOG_ERROR_F("[Renderer] vkAllocateCommandBuffers failed: %d", result);
		return false;
	}

	const vk::SemaphoreCreateInfo semCI{};
	const vk::FenceCreateInfo fenceCI{vk::FenceCreateFlagBits::eSignaled};

	constexpr VkDeviceSize kGpuDataSize = sizeof(GpuFrameData);

	for (int i = 0; i < kMaxFramesInFlight; ++i)
	{
		Frames[i].Cmd      = cmds[i];
		Frames[i].Acquired = raiiDev.createSemaphore(semCI);
		Frames[i].Fence    = raiiDev.createFence(fenceCI);

		Frames[i].GpuData = VkMem->AllocateBuffer(
			kGpuDataSize,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
			GpuMemoryDomain::PersistentMapped,
			/*requestDeviceAddress=*/ true);

		if (!Frames[i].GpuData.IsValid())
		{
			LOG_ERROR_F("[Renderer] GpuData allocation failed for frame slot %d", i);
			return false;
		}

		const VkDeviceSize kScanSize =
			static_cast<VkDeviceSize>(ConfigPtr->MAX_CACHED_ENTITIES) * sizeof(uint32_t);
		Frames[i].ScanBuffer = VkMem->AllocateBuffer(kScanSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
													 GpuMemoryDomain::DeviceLocal, /*requestDeviceAddress=*/ true);
		if (!Frames[i].ScanBuffer.IsValid())
		{
			LOG_ERROR_F("[Renderer] ScanBuffer alloc failed (slot %d)", i);
			return false;
		}

		Frames[i].CompactCounterBuffer = VkMem->AllocateBuffer(sizeof(uint32_t),
															   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
															   GpuMemoryDomain::DeviceLocal, /*requestDeviceAddress=*/ true);
		if (!Frames[i].CompactCounterBuffer.IsValid())
		{
			LOG_ERROR_F("[Renderer] CompactCounterBuffer alloc failed (slot %d)", i);
			return false;
		}

		// DrawArgs: one VkDrawIndexedIndirectCommand per mesh slot (256 max × 20 bytes = 5120 bytes)
		constexpr VkDeviceSize kDrawArgsSize = kMaxMeshSlots * sizeof(VkDrawIndexedIndirectCommand);
		Frames[i].DrawArgsBuffer             = VkMem->AllocateBuffer(kDrawArgsSize,
														 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
														 GpuMemoryDomain::PersistentMapped, /*requestDeviceAddress=*/ true);
		if (!Frames[i].DrawArgsBuffer.IsValid())
		{
			LOG_ERROR_F("[Renderer] DrawArgsBuffer alloc failed (slot %d)", i);
			return false;
		}
		std::memset(Frames[i].DrawArgsBuffer.MappedPtr, 0, kDrawArgsSize);

#ifdef TNX_GPU_PICKING
		constexpr uint32_t kInstanceFieldCount = kGpuOutFieldCount + 1; // +1 for entity cache index
#else
		constexpr uint32_t kInstanceFieldCount = kGpuOutFieldCount;
#endif
		const VkDeviceSize kInstancesSize =
			kInstanceFieldCount * static_cast<VkDeviceSize>(ConfigPtr->MAX_CACHED_ENTITIES) * sizeof(float);
		Frames[i].InstancesBuffer = VkMem->AllocateBuffer(kInstancesSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
														  GpuMemoryDomain::DeviceLocal, /*requestDeviceAddress=*/ true);
		if (!Frames[i].InstancesBuffer.IsValid())
		{
			LOG_ERROR_F("[Renderer] InstancesBuffer alloc failed (slot %d)", i);
			return false;
		}

		// Unsorted instances buffer (scatter output, pre-sort) — same size as sorted
		Frames[i].UnsortedInstancesBuffer = VkMem->AllocateBuffer(kInstancesSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
																  GpuMemoryDomain::DeviceLocal, /*requestDeviceAddress=*/ true);
		if (!Frames[i].UnsortedInstancesBuffer.IsValid())
		{
			LOG_ERROR_F("[Renderer] UnsortedInstancesBuffer alloc failed (slot %d)", i);
			return false;
		}

		// Mesh histogram + write index buffers (256 uint32 each = 1 KB)
		constexpr VkDeviceSize kHistSize = kMaxMeshSlots * sizeof(uint32_t);
		Frames[i].MeshHistogramBuffer    = VkMem->AllocateBuffer(kHistSize,
															  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
															  GpuMemoryDomain::DeviceLocal, /*requestDeviceAddress=*/ true);
		if (!Frames[i].MeshHistogramBuffer.IsValid())
		{
			LOG_ERROR_F("[Renderer] MeshHistogramBuffer alloc failed (slot %d)", i);
			return false;
		}

		Frames[i].MeshWriteIdxBuffer = VkMem->AllocateBuffer(kHistSize,
															 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
															 GpuMemoryDomain::DeviceLocal, /*requestDeviceAddress=*/ true);
		if (!Frames[i].MeshWriteIdxBuffer.IsValid())
		{
			LOG_ERROR_F("[Renderer] MeshWriteIdxBuffer alloc failed (slot %d)", i);
			return false;
		}
	}

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
			LOG_ERROR_F("[Renderer] FieldSlab allocation failed for slot %d", i);
			return false;
		}
	}

	const uint32_t imageCount = static_cast<uint32_t>(VkCtx->GetSwapchain().Images.size());
	RenderedSems.reserve(imageCount);
	for (uint32_t i = 0; i < imageCount; ++i) RenderedSems.push_back(raiiDev.createSemaphore(semCI));

	LOG_INFO("[Renderer] Frame sync objects created");
	return true;
}

// -----------------------------------------------------------------------
// Shaders / Pipelines / Mesh
// -----------------------------------------------------------------------

template <typename Derived>
bool RendererCore<Derived>::LoadShaders()
{
	auto vert = ReadSPIRV(TNX_SHADER_DIR "/graphics/cube.vert.spv");
	auto frag = ReadSPIRV(TNX_SHADER_DIR "/graphics/cube.frag.spv");

	if (vert.empty() || frag.empty())
	{
		LOG_ERROR_F("[Renderer] Failed to read SPIR-V from %s", TNX_SHADER_DIR);
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
		LOG_ERROR("[Renderer] Failed to create shader modules");
		return false;
	}

	LOG_INFO("[Renderer] Shaders loaded (vert + frag)");
	return true;
}

template <typename Derived>
void RendererCore<Derived>::DestroyShaders()
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
	if (BuildDrawsPipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(device, BuildDrawsPipeline, nullptr);
		BuildDrawsPipeline = VK_NULL_HANDLE;
	}
	if (SortInstancesPipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(device, SortInstancesPipeline, nullptr);
		SortInstancesPipeline = VK_NULL_HANDLE;
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

template <typename Derived>
bool RendererCore<Derived>::CreateComputePipelines()
{
	const char* paths[5] = {
		TNX_SHADER_DIR "/compute/predicate.spv",
		TNX_SHADER_DIR "/compute/prefix_sum.spv",
		TNX_SHADER_DIR "/compute/scatter.spv",
		TNX_SHADER_DIR "/compute/build_draws.spv",
		TNX_SHADER_DIR "/compute/sort_instances.spv",
	};
	VkPipeline* targets[5] = {
		&PredicatePipeline, &PrefixSumPipeline, &ScatterPipeline,
		&BuildDrawsPipeline, &SortInstancesPipeline
	};

	for (int i = 0; i < 5; ++i)
	{
		auto code = ReadSPIRV(paths[i]);
		if (code.empty())
		{
			LOG_ERROR_F("[Renderer] Failed to read compute SPIR-V: %s", paths[i]);
			return false;
		}

		VkShaderModuleCreateInfo modCI{};
		modCI.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		modCI.codeSize = code.size() * sizeof(uint32_t);
		modCI.pCode    = code.data();

		VkShaderModule mod = VK_NULL_HANDLE;
		if (vkCreateShaderModule(device, &modCI, nullptr, &mod) != VK_SUCCESS)
		{
			LOG_ERROR_F("[Renderer] vkCreateShaderModule failed for %s", paths[i]);
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
		vkDestroyShaderModule(device, mod, nullptr);

		if (result != VK_SUCCESS)
		{
			LOG_ERROR_F("[Renderer] vkCreateComputePipelines failed for %s: %d", paths[i], result);
			return false;
		}
	}

	LOG_INFO("[Renderer] Compute pipelines created (predicate + prefix_sum + scatter + build_draws + sort_instances)");
	return true;
}

template <typename Derived>
bool RendererCore<Derived>::CreateMeshBuffers()
{
	if (!Meshes.Initialize(VkMem))
	{
		LOG_ERROR("[Renderer] MeshManager initialization failed");
		return false;
	}

	uint32_t cubeSlot = Meshes.RegisterBuiltinCube();
	if (cubeSlot == UINT32_MAX)
	{
		LOG_ERROR("[Renderer] Failed to register built-in cube mesh");
		return false;
	}

	LOG_INFO_F("[Renderer] MeshManager ready — cube at slot %u", cubeSlot);
	return true;
}

// -----------------------------------------------------------------------
// FillGpuFrameData / WriteToFrameSlab
// -----------------------------------------------------------------------

template <typename Derived>
void RendererCore<Derived>::FillGpuFrameData(FrameSync& frame)
{
	TNX_ZONE_NC("Fill GPU", TNX_COLOR_RENDERING)

	auto* FrameData = static_cast<GpuFrameData*>(frame.GpuData.MappedPtr);
	std::memset(FrameData, 0, sizeof(GpuFrameData));

	ComponentCacheBase* tc   = RegistryPtr->GetTemporalCache();
	TemporalFrameHeader* hdr = tc->GetFrameHeader();
	MultMat4(FrameData->ViewProj, hdr->ProjectionMatrix.m, hdr->ViewMatrix.m);

	FrameData->VerticesAddr          = Meshes.GetVertexBufferAddr();
	FrameData->InstancesAddr         = frame.InstancesBuffer.DeviceAddr;
	FrameData->ScanAddr              = frame.ScanBuffer.DeviceAddr;
	FrameData->CompactCounterAddr    = frame.CompactCounterBuffer.DeviceAddr;
	FrameData->DrawArgsAddr          = frame.DrawArgsBuffer.DeviceAddr;
	FrameData->Alpha                 = std::clamp(LogicPtr->GetFixedAlpha(), 0.0, 1.0);
	FrameData->EntityCount           = static_cast<uint32_t>(ConfigPtr->MAX_CACHED_ENTITIES);
	FrameData->OutFieldStride        = static_cast<uint32_t>(ConfigPtr->MAX_CACHED_ENTITIES);
	FrameData->FieldCount            = kGpuOutFieldCount;
	FrameData->UnsortedInstancesAddr = frame.UnsortedInstancesBuffer.DeviceAddr;
	FrameData->MeshHistogramAddr     = frame.MeshHistogramBuffer.DeviceAddr;
	FrameData->MeshWriteIdxAddr      = frame.MeshWriteIdxBuffer.DeviceAddr;
	FrameData->MeshTableAddr         = Meshes.GetMeshTableAddr();
	FrameData->MeshCount             = Meshes.GetMeshCount();

	const uint32_t kFields[kGpuOutFieldCount] = {
		kSemFlags,
		kSemPosX, kSemPosY, kSemPosZ,
		kSemRotQx, kSemRotQy, kSemRotQz, kSemRotQw,
		kSemScaleX, kSemScaleY, kSemScaleZ,
		kSemColorR, kSemColorG, kSemColorB, kSemColorA,
		kSemMeshID,
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

#if TNX_DEV_METRICS
	FrameInputTimestamp[CurrentFrame] = hdr->InputTimestamp;
#endif
}

template <typename Derived>
void RendererCore<Derived>::WriteToFrameSlab()
{
	uint32_t nextSlab = CurrentFieldSlab;
	do
	{
		nextSlab = (nextSlab + 1) % kInstanceBufferCount;
	} while (nextSlab == GPUActiveFrame || nextSlab == GPUPrevFrame);

	const VkDeviceSize fieldStride = static_cast<VkDeviceSize>(ConfigPtr->MAX_CACHED_ENTITIES) * sizeof(float);
	uint8_t* slabPtr               = static_cast<uint8_t*>(FieldSlabs[nextSlab].MappedPtr);

	ComponentCacheBase* temporalCache = RegistryPtr->GetTemporalCache();
	ComponentCacheBase* volatileCache = RegistryPtr->GetVolatileCache();

	if (!volatileCache->TryLockFrameForRead(LastVolatileFrame)
#ifdef TNX_ENABLE_ROLLBACK
		|| !temporalCache->TryLockFrameForRead(LastTemporalFrame)
#endif
	)
	{
		LOG_ERROR("[Renderer] Failed to lock frame for read");
		return;
	}

	TNX_ZONE_NC("Write Frame Slab", TNX_COLOR_RENDERING)
	PrevFieldSlab    = CurrentFieldSlab;
	CurrentFieldSlab = nextSlab;

	TemporalFrameHeader* temporalHdr = temporalCache->GetFrameHeader(LastTemporalFrame);
	TemporalFrameHeader* volatileHdr = volatileCache->GetFrameHeader(LastVolatileFrame);

	const ComponentTypeID transformSlot = TransRot<>::StaticTemporalIndex();
	const ComponentTypeID scaleSlot     = Scale<>::StaticTemporalIndex();
	const ComponentTypeID colorSlot     = ColorData<>::StaticTemporalIndex();
	const ComponentTypeID flagsSlot     = CacheSlotMeta<>::StaticTemporalIndex();
	const ComponentTypeID meshRefSlot   = MeshRef<>::StaticTemporalIndex();

	struct FieldDescription
	{
		ComponentCacheBase* cache;
		TemporalFrameHeader* hdr;
		ComponentTypeID slot;
		size_t fi;
		uint32_t sem;
	};
	const FieldDescription fieldDescs[kGpuOutFieldCount] = {
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
		{volatileCache, volatileHdr, meshRefSlot, 0, kSemMeshID},
	};

	// ── Step 1: Scan slab Flags for dirty bit (bit 30) → build current dirty set ──
	const auto* flagsSrc = static_cast<const int32_t*>(
		temporalCache->GetFieldData(temporalHdr, flagsSlot, 0));

	if (flagsSrc)
	{
		constexpr int32_t dirtyBit = static_cast<int32_t>(TemporalFlagBits::Dirty);
		const uint32_t entityCount = (ConfigPtr->MAX_RENDERABLE_ENTITIES + 7) & ~7u; // round up to SIMD width

		// Scan flags → build DirtySnapshot, then OR into all planes
		std::memset(DirtySnapshot, 0, DirtyWordCount * sizeof(uint64_t));
		for (uint32_t i = 0; i < entityCount; i += 8)
		{
			// Load 8 flags, test bit 30, pack to bitmask
			__m256i flags = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&flagsSrc[i]));
			__m256i test  = _mm256_and_si256(flags, _mm256_set1_epi32(dirtyBit));
			int mask      = _mm256_movemask_ps(_mm256_castsi256_ps(
				_mm256_cmpeq_epi32(test, _mm256_set1_epi32(dirtyBit))));

			if (mask)
			{
				DirtySnapshot[i / 64] |= static_cast<uint64_t>(mask) << (i % 64);
			}
		}

		// Fan out: OR snapshot into ALL 5 GPU slab bitplanes
		for (uint32_t s = 0; s < kInstanceBufferCount; ++s)
		{
			for (uint32_t w = 0; w < DirtyWordCount; ++w)
			{
				DirtyPlanes[s][w] |= DirtySnapshot[w];
			}
		}
	}

	// ── Step 2: Upload dirty entities for this slab, or full copy on first write ──
	const bool fullCopy = FirstSlabWrite[nextSlab];
	uint64_t* plane     = DirtyPlanes[nextSlab];

	TrinyxJobs::JobCounter GPUTransferCounter;

	if (fullCopy)
	{
		// First time writing to this slab — full copy, all fields
		for (uint32_t f = 0; f < kGpuOutFieldCount; ++f)
		{
			const FieldDescription& fd = fieldDescs[f];
			const void* src            = fd.cache->GetFieldData(fd.hdr, fd.slot, fd.fi);
			uint8_t* dst               = slabPtr + static_cast<size_t>(f) * static_cast<size_t>(fieldStride);
			TrinyxJobs::Dispatch([src, dst, fieldStride](uint32_t)
			{
				if (src) std::memcpy(dst, src, static_cast<size_t>(fieldStride));
				else std::memset(dst, 0, static_cast<size_t>(fieldStride));
			}, &GPUTransferCounter, TrinyxJobs::Queue::Render);
		}
		FirstSlabWrite[nextSlab] = false;
	}
	else
	{
		// Selective upload: one job per field, each scatters only dirty entities.
		// All jobs read the same bitplane (immutable until cleared after wait).
		for (uint32_t f = 0; f < kGpuOutFieldCount; ++f)
		{
			const FieldDescription& fd = fieldDescs[f];
			const auto* src            = static_cast<const uint8_t*>(fd.cache->GetFieldData(fd.hdr, fd.slot, fd.fi));
			uint8_t* dst               = slabPtr + static_cast<size_t>(f) * static_cast<size_t>(fieldStride);

			if (!src) continue;

			const uint64_t* dirtyPlane = plane;
			const uint32_t wordCount   = DirtyWordCount;

			TrinyxJobs::Dispatch([src, dst, dirtyPlane, wordCount](uint32_t)
			{
				for (uint32_t w = 0; w < wordCount; ++w)
				{
					uint64_t bits = dirtyPlane[w];
					while (bits)
					{
						uint32_t bit = __builtin_ctzll(bits);
						uint32_t idx = w * 64 + bit;
						std::memcpy(dst + idx * sizeof(float), src + idx * sizeof(float), sizeof(float));
						bits &= bits - 1;
					}
				}
			}, &GPUTransferCounter, TrinyxJobs::Queue::Render);
		}
	}

	TrinyxJobs::WaitForCounter(&GPUTransferCounter, TrinyxJobs::Queue::Render);

	// ── Step 3: Clear this slab's dirty plane — it's now up to date ──
	std::memset(plane, 0, DirtyWordCount * sizeof(uint64_t));

	// ── Step 4: Publish RenderAck so logic knows we consumed this frame ──
	RegistryPtr->RenderAck.store(temporalHdr->FrameNumber, std::memory_order_release);
	RegistryPtr->RenderHasAcked = true;

	volatileCache->UnlockFrameRead(LastVolatileFrame);
#ifdef TNX_ENABLE_ROLLBACK
	temporalCache->UnlockFrameRead(LastTemporalFrame);
#endif
}

// -----------------------------------------------------------------------
// CreatePipeline / CreateDepthImage / OnSwapchainResize / TrackFPS
// -----------------------------------------------------------------------

template <typename Derived>
bool RendererCore<Derived>::CreatePipeline()
{
	VkFormat colorFmt = static_cast<VkFormat>(VkCtx->GetSwapchain().Format);

	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset     = 0;
	pushRange.size       = sizeof(uint64_t);

	VkPipelineLayoutCreateInfo layoutCI{};
	layoutCI.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutCI.pushConstantRangeCount = 1;
	layoutCI.pPushConstantRanges    = &pushRange;

	VkPipelineLayout rawLayout = VK_NULL_HANDLE;
	if (vkCreatePipelineLayout(device, &layoutCI, nullptr, &rawLayout) != VK_SUCCESS)
	{
		LOG_ERROR("[Renderer] vkCreatePipelineLayout failed");
		return false;
	}
	PipelineLayout = vk::raii::PipelineLayout(VkCtx->GetRaiiDevice(), rawLayout);

	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = VertShader;
	stages[0].pName  = "main";
	stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = FragShader;
	stages[1].pName  = "main";

	VkPipelineVertexInputStateCreateInfo vertexInput{};
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount  = 1;

	VkPipelineRasterizationStateCreateInfo raster{};
	raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode    = VK_CULL_MODE_BACK_BIT;
	raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth   = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample{};
	multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable  = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

	VkPipelineColorBlendAttachmentState blendAttach{};
	blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	VkPipelineColorBlendStateCreateInfo colorBlend{};
	colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlend.attachmentCount = 1;
	colorBlend.pAttachments    = &blendAttach;

	const VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

	VkPipelineDynamicStateCreateInfo dynState{};
	dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynState.dynamicStateCount = 2;
	dynState.pDynamicStates    = dynStates;

	VkPipelineRenderingCreateInfo renderingCI{};
	renderingCI.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	renderingCI.colorAttachmentCount    = 1;
	renderingCI.pColorAttachmentFormats = &colorFmt;
	renderingCI.depthAttachmentFormat   = DepthFormat;

	VkGraphicsPipelineCreateInfo pipelineCI{};
	pipelineCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineCI.pNext               = &renderingCI;
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
		LOG_ERROR("[Renderer] vkCreateGraphicsPipelines failed");
		return false;
	}
	Pipeline = vk::raii::Pipeline(VkCtx->GetRaiiDevice(), rawPipeline);

	LOG_INFO("[Renderer] Graphics pipeline created");
	return true;
}

template <typename Derived>
bool RendererCore<Derived>::CreateDepthImage()
{
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

template <typename Derived>
void RendererCore<Derived>::OnSwapchainResize()
{
	vkDeviceWaitIdle(device);
	VkCtx->RecreateSwapchain(WindowPtr);
	CreateDepthImage();
#ifdef TNX_GPU_PICKING
	CreatePickImages();
#endif
}

template <typename Derived>
void RendererCore<Derived>::TrackFPS()
{
	RenderFrameCount++;
	const double now = SDL_GetPerformanceCounter() /
		static_cast<double>(SDL_GetPerformanceFrequency());
	RenderFpsTimer     += now - RenderLastFPSCheck;
	RenderLastFPSCheck = now;

	if (RenderFpsTimer >= 1.0) [[unlikely]]
	{
#if TNX_DEV_METRICS
		double avgLatencyMs = (LatencySamples > 0) ? (LatencyAccumMs / LatencySamples) : 0.0;
		LOG_DEBUG_F("Render FPS: %d | Frame: %.2fms | Input→Photon: %.2fms",
					static_cast<int>(RenderFrameCount / RenderFpsTimer),
					(RenderFpsTimer / RenderFrameCount) * 1000.0,
					avgLatencyMs);
		LatencyAccumMs = 0.0;
		LatencySamples = 0;
#else
		LOG_DEBUG_F("Render FPS: %d | Frame: %.2fms",
					static_cast<int>(RenderFrameCount / RenderFpsTimer),
					(RenderFpsTimer / RenderFrameCount) * 1000.0);
#endif
		RenderFrameCount = 0;
		RenderFpsTimer   = 0.0;
	}
}

// -----------------------------------------------------------------------
// GPU Picking
// -----------------------------------------------------------------------

#ifdef TNX_GPU_PICKING

template <typename Derived>
void RendererCore<Derived>::RequestPick(int32_t x, int32_t y)
{
	PickX.store(x, std::memory_order_relaxed);
	PickY.store(y, std::memory_order_relaxed);
	bPickRequested.store(true, std::memory_order_release);
	bPickResultReady.store(false, std::memory_order_relaxed);
}

template <typename Derived>
bool RendererCore<Derived>::ConsumePickResult(uint32_t& outCacheIdx)
{
#ifdef TNX_GPU_PICKING_FAST
	outCacheIdx = PickResult.load(std::memory_order_relaxed);
	return true;
#endif
	if (!bPickResultReady.load(std::memory_order_acquire)) return false;
	outCacheIdx = PickResult.load(std::memory_order_relaxed);
	bPickResultReady.store(false, std::memory_order_relaxed);
	return true;
}

template <typename Derived>
bool RendererCore<Derived>::CreatePickImages()
{
	const vk::Extent2D ext = VkCtx->GetSwapchain().Extent;
	for (int i = 0; i < kMaxFramesInFlight; ++i)
	{
		Frames[i].PickAttachment = VkMem->AllocateImage(
			{ext.width, ext.height},
			VK_FORMAT_R32_UINT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			VK_IMAGE_ASPECT_COLOR_BIT);
		if (!Frames[i].PickAttachment.IsValid())
		{
			LOG_ERROR_F("[Renderer] PickAttachment alloc failed (slot %d)", i);
			return false;
		}

		Frames[i].PickReadbackBuffer = VkMem->AllocateBuffer(
			sizeof(uint32_t),
			VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			GpuMemoryDomain::Staging);
		if (!Frames[i].PickReadbackBuffer.IsValid())
		{
			LOG_ERROR_F("[Renderer] PickReadbackBuffer alloc failed (slot %d)", i);
			return false;
		}
	}
	return true;
}

template <typename Derived>
bool RendererCore<Derived>::LoadPickShaders()
{
	auto vert     = ReadSPIRV(TNX_SHADER_DIR "/graphics/cube.vert_pick.spv");
	auto frag     = ReadSPIRV(TNX_SHADER_DIR "/graphics/cube.frag_pick.spv");
	auto scatter  = ReadSPIRV(TNX_SHADER_DIR "/compute/scatter_pick.spv");
	auto sortInst = ReadSPIRV(TNX_SHADER_DIR "/compute/sort_instances_pick.spv");

	if (vert.empty() || frag.empty() || scatter.empty() || sortInst.empty())
	{
		LOG_ERROR("[Renderer] Failed to read pick SPIR-V shaders");
		return false;
	}

	auto createModule = [&](const std::vector<uint32_t>& code) -> VkShaderModule
	{
		VkShaderModuleCreateInfo ci{};
		ci.sType           = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		ci.codeSize        = code.size() * sizeof(uint32_t);
		ci.pCode           = code.data();
		VkShaderModule mod = VK_NULL_HANDLE;
		vkCreateShaderModule(device, &ci, nullptr, &mod);
		return mod;
	};

	PickVertShader = createModule(vert);
	PickFragShader = createModule(frag);

	if (PickVertShader == VK_NULL_HANDLE || PickFragShader == VK_NULL_HANDLE)
	{
		LOG_ERROR("[Renderer] Failed to create pick shader modules");
		return false;
	}

	// Scatter pick compute pipeline
	VkShaderModule scatterMod = createModule(scatter);
	if (scatterMod == VK_NULL_HANDLE)
	{
		LOG_ERROR("[Renderer] Failed to create scatter_pick shader module");
		return false;
	}

	VkPipelineShaderStageCreateInfo stageCI{};
	stageCI.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageCI.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	stageCI.module = scatterMod;
	stageCI.pName  = "main";

	VkComputePipelineCreateInfo pipeCI{};
	pipeCI.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipeCI.stage  = stageCI;
	pipeCI.layout = *PipelineLayout; // same push constant layout

	VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &ScatterPickPipeline);
	vkDestroyShaderModule(device, scatterMod, nullptr);

	if (result != VK_SUCCESS)
	{
		LOG_ERROR_F("[Renderer] ScatterPickPipeline creation failed: %d", result);
		return false;
	}

	// Sort instances pick compute pipeline
	VkShaderModule sortMod = createModule(sortInst);
	if (sortMod == VK_NULL_HANDLE)
	{
		LOG_ERROR("[Renderer] Failed to create sort_instances_pick shader module");
		return false;
	}

	stageCI.module = sortMod;
	pipeCI.stage   = stageCI;
	result         = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeCI, nullptr, &SortPickPipeline);
	vkDestroyShaderModule(device, sortMod, nullptr);

	if (result != VK_SUCCESS)
	{
		LOG_ERROR_F("[Renderer] SortPickPipeline creation failed: %d", result);
		return false;
	}

	LOG_INFO("[Renderer] Pick shaders loaded (vert + frag + scatter_pick + sort_pick)");
	return true;
}

template <typename Derived>
bool RendererCore<Derived>::CreatePickPipeline()
{
	VkFormat colorFmt = static_cast<VkFormat>(VkCtx->GetSwapchain().Format);
	VkFormat pickFmt  = VK_FORMAT_R32_UINT;

	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = PickVertShader;
	stages[0].pName  = "main";
	stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = PickFragShader;
	stages[1].pName  = "main";

	VkPipelineVertexInputStateCreateInfo vertexInput{};
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount  = 1;

	VkPipelineRasterizationStateCreateInfo raster{};
	raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode    = VK_CULL_MODE_BACK_BIT;
	raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth   = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample{};
	multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable  = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

	// Two color blend attachments: color (normal) + pick (no blend, uint)
	VkPipelineColorBlendAttachmentState blendAttachments[2]{};
	blendAttachments[0].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	blendAttachments[1].colorWriteMask = VK_COLOR_COMPONENT_R_BIT; // R32_UINT — only R channel

	VkPipelineColorBlendStateCreateInfo colorBlend{};
	colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlend.attachmentCount = 2;
	colorBlend.pAttachments    = blendAttachments;

	VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynState{};
	dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynState.dynamicStateCount = 2;
	dynState.pDynamicStates    = dynStates;

	VkFormat colorFormats[2] = {colorFmt, pickFmt};
	VkPipelineRenderingCreateInfo renderingCI{};
	renderingCI.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	renderingCI.colorAttachmentCount    = 2;
	renderingCI.pColorAttachmentFormats = colorFormats;
	renderingCI.depthAttachmentFormat   = DepthFormat;

	VkGraphicsPipelineCreateInfo pipelineCI{};
	pipelineCI.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineCI.pNext               = &renderingCI;
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
	pipelineCI.layout              = *PipelineLayout; // same push constant layout

	VkPipeline rawPipeline = VK_NULL_HANDLE;
	VkResult result        = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineCI, nullptr, &rawPipeline);
	if (result != VK_SUCCESS)
	{
		LOG_ERROR_F("[Renderer] Pick pipeline creation failed: %d", result);
		return false;
	}

	PickPipeline = vk::raii::Pipeline(VkCtx->GetRaiiDevice(), rawPipeline);
	LOG_INFO("[Renderer] Pick graphics pipeline created");
	return true;
}

template <typename Derived>
void RendererCore<Derived>::DestroyPickShaders()
{
	if (ScatterPickPipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(device, ScatterPickPipeline, nullptr);
		ScatterPickPipeline = VK_NULL_HANDLE;
	}
	if (SortPickPipeline != VK_NULL_HANDLE)
	{
		vkDestroyPipeline(device, SortPickPipeline, nullptr);
		SortPickPipeline = VK_NULL_HANDLE;
	}
	if (PickVertShader != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(device, PickVertShader, nullptr);
		PickVertShader = VK_NULL_HANDLE;
	}
	if (PickFragShader != VK_NULL_HANDLE)
	{
		vkDestroyShaderModule(device, PickFragShader, nullptr);
		PickFragShader = VK_NULL_HANDLE;
	}
}

#endif // TNX_GPU_PICKING

// -----------------------------------------------------------------------
// Explicit template instantiations
// -----------------------------------------------------------------------

#if TNX_ENABLE_EDITOR
#include "EditorRenderer.h"
template class RendererCore<EditorRenderer>;
#else
#include "GameplayRenderer.h"
template class RendererCore<GameplayRenderer>;
#endif
