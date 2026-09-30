#include "EditorRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "TnxStyle.h"

#include "AnimationManager.h"
#include "MeshManager.h"
#include "CacheSlotMeta.h"
#include "CAnimBase.h"
#include "CAnimLayer.h"
#include "CColor.h"
#include "CSkeletonRef.h"
#include "CVisualTransform.h"
#include "EditorContext.h"
#include "GpuFrameData.h"
#include "ImGuizmo.h"
#include "Logger.h"
#include "LogicThreadBase.h"
#include "CMeshRef.h"
#include "Registry.h"
#include "CScale.h"
#include "SkeletonManager.h"
#include "TemporalComponentCache.h"
#include "TrinyxEngine.h"
#include "CTransform.h"
#include "VulkanDebug.h"
#include "World.h"
#include "WorldViewport.h"

// -----------------------------------------------------------------------
// ImGuiEventQueue — ring buffer for cross-thread SDL event forwarding.
// Defined here to keep SDL_Event out of EditorRenderer.h.
// -----------------------------------------------------------------------
struct ImGuiEventQueue
{
	static constexpr uint32_t Capacity = 1024;
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
	std::string DrainIntoImGui(float& relX, float& relY, float& wheel)
	{
		std::string droppedFile;
		std::lock_guard lock(Mutex);
		while (Tail != Head)
		{
			SDL_Event& ev = Events[Tail];
			ImGui_ImplSDL3_ProcessEvent(&ev);
			// Relative motion survives relative mouse mode, where absolute positions stop moving.
			if (ev.type == SDL_EVENT_MOUSE_MOTION)
			{
				relX += ev.motion.xrel;
				relY += ev.motion.yrel;
			}
			else if (ev.type == SDL_EVENT_MOUSE_WHEEL)
				wheel += ev.wheel.y;
			if (ev.type == SDL_EVENT_DROP_FILE) droppedFile = std::move(PendingDropPath);
			Tail = (Tail + 1) % Capacity;
		}
		return droppedFile;
	}
};

// -----------------------------------------------------------------------
// CRTP hooks
// -----------------------------------------------------------------------

// -----------------------------------------------------------------------
// Viewport gradient pre-pass
// -----------------------------------------------------------------------

static std::vector<uint32_t> ReadGradientSPIRV(const char* path)
{
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f.is_open()) return {};
	const auto sz = static_cast<size_t>(f.tellg());
	f.seekg(0);
	std::vector<uint32_t> buf(sz / sizeof(uint32_t));
	f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
	return buf;
}

bool EditorRenderer::LoadGradientShaders()
{
	auto vert = ReadGradientSPIRV(TNX_SHADER_DIR "/graphics/viewport_gradient.vert.spv");
	auto frag = ReadGradientSPIRV(TNX_SHADER_DIR "/graphics/viewport_gradient.frag.spv");
	if (vert.empty() || frag.empty())
	{
		LOG_ENG_ERROR("[EditorRenderer] Failed to read gradient shader SPIR-V");
		return false;
	}

	auto makeModule = [&](const std::vector<uint32_t>& code, VkShaderModule& out) -> bool
	{
		VkShaderModuleCreateInfo ci{};
		ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		ci.codeSize = code.size() * sizeof(uint32_t);
		ci.pCode    = code.data();
		return vkCreateShaderModule(Device, &ci, nullptr, &out) == VK_SUCCESS;
	};

	if (!makeModule(vert, GradientVertShader) || !makeModule(frag, GradientFragShader))
	{
		LOG_ENG_ERROR("[EditorRenderer] Failed to create gradient shader modules");
		return false;
	}
	return true;
}

bool EditorRenderer::CreateGradientPipeline()
{
	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = GradientVertShader;
	stages[0].pName  = "main";
	stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = GradientFragShader;
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
	raster.cullMode    = VK_CULL_MODE_NONE;
	raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth   = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample{};
	multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

	VkPipelineColorBlendAttachmentState blendAttach{};
	blendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	VkPipelineColorBlendStateCreateInfo colorBlend{};
	colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlend.attachmentCount = 1;
	colorBlend.pAttachments    = &blendAttach;

	VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynState{};
	dynState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynState.dynamicStateCount = 2;
	dynState.pDynamicStates    = dynStates;

	VkFormat colorFmt = static_cast<VkFormat>(VkCtx->GetSwapchain().Format);
	VkPipelineRenderingCreateInfo renderingCI{};
	renderingCI.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	renderingCI.colorAttachmentCount    = 1;
	renderingCI.pColorAttachmentFormats = &colorFmt;
	renderingCI.depthAttachmentFormat   = VK_FORMAT_UNDEFINED;

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
	if (vkCreateGraphicsPipelines(Device, VK_NULL_HANDLE, 1, &pipelineCI, nullptr, &rawPipeline) != VK_SUCCESS)
	{
		LOG_ENG_ERROR("[EditorRenderer] Failed to create gradient pipeline");
		return false;
	}
	GradientPipeline = vk::raii::Pipeline(VkCtx->GetRaiiDevice(), rawPipeline);
	return true;
}

void EditorRenderer::DestroyGradientShaders()
{
	if (GradientVertShader)
	{
		vkDestroyShaderModule(Device, GradientVertShader, nullptr);
		GradientVertShader = VK_NULL_HANDLE;
	}
	if (GradientFragShader)
	{
		vkDestroyShaderModule(Device, GradientFragShader, nullptr);
		GradientFragShader = VK_NULL_HANDLE;
	}
}

void EditorRenderer::OnPostStart()
{
	if (!InitImGui())
	{
		LOG_ENG_ERROR("[EditorRenderer] ImGui initialization failed; editor disabled");
	}

	if (!LoadGradientShaders() || !CreateGradientPipeline())
		LOG_ENG_WARN("[EditorRenderer] Gradient pre-pass unavailable; falling back to flat clear");

	DestroyGradientShaders();
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
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 },
	};

	VkDescriptorPoolCreateInfo poolCI{};
	poolCI.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	poolCI.maxSets       = 16;
	poolCI.poolSizeCount = 1;
	poolCI.pPoolSizes    = poolSizes;

	if (vkCreateDescriptorPool(Device, &poolCI, nullptr, &ImGuiDescriptorPool) != VK_SUCCESS)
	{
		LOG_ENG_ERROR("[EditorRenderer] Failed to create ImGui descriptor pool");
		return false;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

	int logicalW = 0, physicalW = 0;
	SDL_GetWindowSize(WindowPtr, &logicalW, nullptr);
	SDL_GetWindowSizeInPixels(WindowPtr, &physicalW, nullptr);
	const float dpiScale = (logicalW > 0) ? static_cast<float>(physicalW) / static_cast<float>(logicalW) : 1.0f;

	// Locate engine root by walking up from the project dir (same anchor as config loading).
	const char* projectDir = (EnginePtr && EnginePtr->GetConfig()->ProjectDir[0] != '\0')
								 ? EnginePtr->GetConfig()->ProjectDir
								 : nullptr;
	std::string engineRoot;
	{
		namespace fs    = std::filesystem;
		fs::path search = projectDir ? fs::path(projectDir) : fs::current_path();
		for (int d = 0; d < 10; ++d)
		{
			if (fs::exists(search / "TrinyxDefaults.ini"))
			{
				engineRoot = search.string();
				break;
			}
			fs::path parent = search.parent_path();
			if (parent == search) break;
			search = parent;
		}
	}

	// Font search: {projectDir}/assets/fonts → {engineRoot}/Trinyx/assets/fonts → assets/fonts
	TnxStyle::LoadFonts(projectDir, engineRoot.c_str(), dpiScale);

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
		LOG_ENG_ERROR("[EditorRenderer] ImGui_ImplVulkan_Init failed");
		return false;
	}

	// Apply theme after both backends are up — this wins over any backend-side resets.
	TnxStyle::Apply();

	// Viewport fixup: multi-viewport platform windows need zero rounding and an
	// opaque background so they blend correctly outside the main SDL window.
	ImGuiStyle& style                 = ImGui::GetStyle();
	style.WindowRounding              = 0.0f;
	style.Colors[ImGuiCol_WindowBg].w = 1.0f;

	if (dpiScale > 1.01f)
		style.ScaleAllSizes(dpiScale);

	EventQueue = new ImGuiEventQueue();

	Editor = new EditorContext();
	Editor->Initialize(EnginePtr, LogicPtr);

	// Allocate the persistent editor viewport for the main world.
	// Use a modest initial resolution — the panel will resize it on the first frame.
	EditorViewport.TargetWorld = EnginePtr->GetDefaultWorld();
	AllocateViewportResources(&EditorViewport, 1280, 720);
	ActiveViewports.insert(ActiveViewports.begin(), &EditorViewport);

	bImGuiInitialized = true;
	LOG_ENG_INFO("[EditorRenderer] ImGui initialized (dynamic rendering, docking + multi-viewport enabled)");
	return true;
}

void EditorRenderer::ShutdownImGui()
{
	if (!bImGuiInitialized) return;

	vkDeviceWaitIdle(Device);

	// Remove the editor viewport from the active list and free its GPU resources
	// before tearing down ImGui (FreeViewportResources calls ImGui_ImplVulkan_RemoveTexture).
	auto it = std::find(ActiveViewports.begin(), ActiveViewports.end(), &EditorViewport);
	if (it != ActiveViewports.end()) ActiveViewports.erase(it);
	FreeViewportResources(&EditorViewport);

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
	LOG_ENG_INFO("[EditorRenderer] ImGui shut down");
}

void EditorRenderer::PushImGuiEvent(const SDL_Event& event)
{
	if (EventQueue) EventQueue->Push(event);
}

void EditorRenderer::DrainImGuiEvents()
{
	if (!EventQueue) return;
	MouseRelX           = 0.0f;
	MouseRelY           = 0.0f;
	MouseWheel          = 0.0f;
	std::string dropped = EventQueue->DrainIntoImGui(MouseRelX, MouseRelY, MouseWheel);
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

	const ImGuiIO& io = ImGui::GetIO();
	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
	{
		ImGui::UpdatePlatformWindows();
		ImGui::RenderPlatformWindowsDefault();
	}
}

// -----------------------------------------------------------------------
// Editor viewport resize & texture access
// -----------------------------------------------------------------------

void EditorRenderer::ResizeEditorViewport(uint32_t width, uint32_t height)
{
	if (width == EditorViewport.Width && height == EditorViewport.Height) return;
	if (width == 0 || height == 0) return;

	vkDeviceWaitIdle(Device);
	FreeViewportResources(&EditorViewport);
	AllocateViewportResources(&EditorViewport, width, height);
}

void EditorRenderer::ResizeViewport(WorldViewport* vp, uint32_t width, uint32_t height)
{
	if (!vp || (width == vp->Width && height == vp->Height)) return;
	if (width == 0 || height == 0) return;

	vkDeviceWaitIdle(Device);
	FreeViewportResources(vp);
	AllocateViewportResources(vp, width, height);
}

VkDescriptorSet EditorRenderer::GetEditorViewportTexture() const
{
	return EditorViewport.ImGuiTexture;
}

// -----------------------------------------------------------------------
// Multi-viewport management (PIE)
// -----------------------------------------------------------------------

void EditorRenderer::AddViewport(WorldViewport* vp)
{
	ActiveViewports.push_back(vp);
	LOG_ENG_INFO_F("[EditorRenderer] Added viewport %p (world %p), %u active",
		static_cast<void*>(vp), static_cast<void*>(vp->TargetWorld),
		static_cast<uint32_t>(ActiveViewports.size()));
}

void EditorRenderer::RemoveViewport(WorldViewport* vp)
{
	auto it = std::find(ActiveViewports.begin(), ActiveViewports.end(), vp);
	if (it != ActiveViewports.end())
	{
		ActiveViewports.erase(it);
		LOG_ENG_INFO_F("[EditorRenderer] Removed viewport %p, %u remaining",
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
		{ width, height },
		swapchainFmt,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		VK_IMAGE_ASPECT_COLOR_BIT);

	// Offscreen depth target
	vp->DepthTarget = VkMem->AllocateImage(
		{ width, height },
		DepthFormat,
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);

#ifdef TNX_GPU_PICKING
	// Pick attachment — R32_UINT, one pixel readback per click
	vp->PickTarget = VkMem->AllocateImage(
		{ width, height },
		VK_FORMAT_R32_UINT,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		VK_IMAGE_ASPECT_COLOR_BIT);
#endif

	// Per-world field slabs — same layout as main renderer, all 35 fields (render + anim).
	const VkDeviceSize slabSize = static_cast<VkDeviceSize>(ConfigPtr->MAX_CACHED_ENTITIES)
								  * sizeof(float) * GpuTotalFieldCount;
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
	VkSamplerCreateInfo samplerCI{};
	samplerCI.sType     = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerCI.magFilter = VK_FILTER_LINEAR;
	samplerCI.minFilter = VK_FILTER_LINEAR;
	vkCreateSampler(Device, &samplerCI, nullptr, &vp->ImGuiSampler);

	vp->ImGuiTexture = ImGui_ImplVulkan_AddTexture(
		vp->ImGuiSampler,
		static_cast<VkImageView>(vp->ColorTarget.View),
		VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	LOG_ENG_INFO_F("[EditorRenderer] Allocated viewport resources %ux%u", width, height);
}

void EditorRenderer::FreeViewportResources(WorldViewport* vp)
{
	// Drain any CPU upload jobs writing into this viewport's slab buffers before freeing them.
	// vkDeviceWaitIdle drains GPU work but does NOT drain CPU job threads.
	FlushViewportSlabUpload(vp);

	// Remove ImGui texture registration
	if (vp->ImGuiTexture != VK_NULL_HANDLE)
	{
		ImGui_ImplVulkan_RemoveTexture(vp->ImGuiTexture);
		vp->ImGuiTexture = VK_NULL_HANDLE;
	}

	// Destroy the sampler that was paired with the ImGui texture
	if (vp->ImGuiSampler != VK_NULL_HANDLE)
	{
		vkDestroySampler(Device, vp->ImGuiSampler, nullptr);
		vp->ImGuiSampler = VK_NULL_HANDLE;
	}

	// Free dirty tracking
	vp->FreeDirtyPlanes();

	// Free field slabs and GpuData
	for (auto& slab : vp->FieldSlabs)
		slab.Free();
	for (auto& gd : vp->GpuData)
		gd.Free();

	// Free offscreen images
	vp->ColorTarget.Free();
	vp->DepthTarget.Free();
#ifdef TNX_GPU_PICKING
	vp->PickTarget.Free();
#endif

	vp->Width  = 0;
	vp->Height = 0;

	LOG_ENG_INFO("[EditorRenderer] Freed viewport resources");
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
			vp->PrevVolatileFrame = vp->LastVolatileFrame;
			vp->PrevTemporalFrame = vp->LastTemporalFrame;
			vp->LastVolatileFrame = newVolatile;
			vp->LastTemporalFrame = newTemporal;
			WriteToViewportSlab(vp);
		}
	}
}

void EditorRenderer::RecordFrame(FrameSync& frame, uint32_t imageIndex)
{
	RecordPIEFrame(frame, imageIndex);
}

// -----------------------------------------------------------------------
// Viewport slab writing — mirrors RendererCore::WriteToFrameSlab but
// reads from the viewport's world and writes to the viewport's slabs.
// -----------------------------------------------------------------------

void EditorRenderer::WriteToViewportSlab(WorldViewport* vp)
{
	// Safety flush: drain any previous upload before overwriting VpUploadFields / reusing counter.
	FlushViewportSlabUpload(vp);

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

	auto resolveVPField = [&](const GpuSlabFieldDesc& desc)
		-> std::pair<ComponentCacheBase*, TemporalFrameHeader*>
	{
		if (desc.tier == GpuSlabTier::Temporal) return { temporalC, temporalHdr };
		return { volatileC, volatileHdr };
	};

	auto flushImmediate = [&]()
	{
		vp->bHasSlabData       = true;
		vp->bSlabUploadPending = false;
		std::memset(vp->DirtyPlanes[nextSlab], 0, vp->DirtyWordCount * sizeof(uint64_t));
		reg->RenderAck.store(temporalHdr->FrameNumber, std::memory_order_release);
		reg->RenderHasAcked = true;
		volatileC->UnlockFrameRead(vp->LastVolatileFrame);
#ifdef TNX_ENABLE_ROLLBACK
		temporalC->UnlockFrameRead(vp->LastTemporalFrame);
#endif
	};

	// ── Step 1: Scan flags → build dirty snapshot, count dirty entities ──
	uint32_t dirtyEntityCount = 0;
	const auto* flagsSrc      = static_cast<const int32_t*>(
		temporalC->GetFieldData(temporalHdr, static_cast<uint8_t>(SlabFieldDescs[0].slot), 0));

	if (flagsSrc)
	{
		constexpr int32_t dirtyBit = static_cast<int32_t>(TemporalFlagBits::Dirty);
		const uint32_t entityCount = (ConfigPtr->MAX_RENDERABLE_ENTITIES + 7) & ~7u;

		std::memset(vp->DirtySnapshot, 0, vp->DirtyWordCount * sizeof(uint64_t));
		for (uint32_t i = 0; i < entityCount; i += 8)
		{
			__m256i flags = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&flagsSrc[i]));
			__m256i test  = _mm256_and_si256(flags, _mm256_set1_epi32(dirtyBit));
			int mask      = _mm256_movemask_ps(_mm256_castsi256_ps(
				_mm256_cmpeq_epi32(test, _mm256_set1_epi32(dirtyBit))));
			if (mask)
			{
				vp->DirtySnapshot[i / 64] |= static_cast<uint64_t>(mask) << (i % 64);
				dirtyEntityCount += TNX_POPCOUNT32(mask);
			}
		}

		for (uint32_t s = 0; s < kViewportSlabCount; ++s)
			for (uint32_t w = 0; w < vp->DirtyWordCount; ++w)
				vp->DirtyPlanes[s][w] |= vp->DirtySnapshot[w];
	}

	// ── Step 2: Upload ──
	const bool fullCopy = vp->FirstSlabWrite[nextSlab];
	uint64_t* plane     = vp->DirtyPlanes[nextSlab];

	if (fullCopy) [[unlikely]]
	{
		TrinyxJobs::JobCounter counter;
		for (uint32_t f = 0; f < GpuTotalFieldCount; ++f)
		{
			const GpuSlabFieldDesc& desc = SlabFieldDescs[f];
			if (desc.kind == GpuSlabKind::EntityIndex) continue;
			auto [cache, hdr] = resolveVPField(desc);
			const void* src   = cache->GetFieldData(hdr, static_cast<uint8_t>(desc.slot), desc.fi);
			uint8_t* dst      = slabPtr + static_cast<size_t>(f) * static_cast<size_t>(fieldStride);
			const bool isSF   = (desc.kind == GpuSlabKind::SimFloat);
			TrinyxJobs::DispatchNamed("VP_Slab_FullCopy", [src, dst, fieldStride, isSF](uint32_t)
			{
				if (!isSF)
				{
					if (src)
						std::memcpy(dst, src, static_cast<size_t>(fieldStride));
					else
						std::memset(dst, 0, static_cast<size_t>(fieldStride));
				}
				else if constexpr (std::is_same_v<SimFloat, SimFloatImpl<float>>)
				{
					if (src)
						std::memcpy(dst, src, static_cast<size_t>(fieldStride));
					else
						std::memset(dst, 0, static_cast<size_t>(fieldStride));
				}
				else
				{
					if (src)
					{
						const Fixed32* fsrc = static_cast<const Fixed32*>(src);
						float* fdst         = reinterpret_cast<float*>(dst);
						const size_t count  = static_cast<size_t>(fieldStride) / sizeof(float);
						for (size_t i = 0; i < count; ++i)
							fdst[i] = fsrc[i].ToFloat();
					}
					else
						std::memset(dst, 0, static_cast<size_t>(fieldStride));
				}
			}, &counter, TrinyxJobs::Queue::Render);
		}
		vp->FirstSlabWrite[nextSlab] = false;
		TrinyxJobs::WaitForCounter(&counter, TrinyxJobs::Queue::Render);
		flushImmediate();
		return;
	}

	const auto inlineThreshold    = static_cast<uint32_t>(ConfigPtr->GetSlabUploadInlineThreshold());
	const auto singleJobThreshold = static_cast<uint32_t>(ConfigPtr->GetSlabUploadSingleJobThreshold());

	if (dirtyEntityCount < inlineThreshold)
	{
		for (uint32_t f = 0; f < GpuTotalFieldCount; ++f)
		{
			const GpuSlabFieldDesc& desc = SlabFieldDescs[f];
			if (desc.kind == GpuSlabKind::EntityIndex) continue;
			auto [cache, hdr] = resolveVPField(desc);
			const auto* src   = static_cast<const uint8_t*>(
				cache->GetFieldData(hdr, static_cast<uint8_t>(desc.slot), desc.fi));
			uint8_t* dst = slabPtr + static_cast<size_t>(f) * static_cast<size_t>(fieldStride);
			if (!src) continue;

			if (desc.kind == GpuSlabKind::SimFloat)
			{
				const SimFloat* ssrc = static_cast<const SimFloat*>(static_cast<const void*>(src));
				float* fdst          = reinterpret_cast<float*>(dst);
				for (uint32_t w = 0; w < vp->DirtyWordCount; ++w)
				{
					uint64_t bits = plane[w];
					while (bits)
					{
						const uint32_t idx = w * 64 + TNX_CTZ64(bits);
						fdst[idx]          = ssrc[idx].ToFloat();
						bits &= bits - 1;
					}
				}
			}
			else
			{
				const uint32_t* usrc = reinterpret_cast<const uint32_t*>(src);
				uint32_t* udst       = reinterpret_cast<uint32_t*>(dst);
				for (uint32_t w = 0; w < vp->DirtyWordCount; ++w)
				{
					uint64_t bits = plane[w];
					while (bits)
					{
						const uint32_t idx = w * 64 + TNX_CTZ64(bits);
						udst[idx]          = usrc[idx];
						bits &= bits - 1;
					}
				}
			}
		}

		flushImmediate();
		return;
	}

	for (uint32_t f = 0; f < GpuTotalFieldCount; ++f)
	{
		const GpuSlabFieldDesc& desc = SlabFieldDescs[f];
		if (desc.kind == GpuSlabKind::EntityIndex)
		{
			vp->VpUploadFields[f].src = nullptr;
			continue;
		}
		auto [cache, hdr]         = resolveVPField(desc);
		vp->VpUploadFields[f].src = static_cast<const uint8_t*>(
			cache->GetFieldData(hdr, static_cast<uint8_t>(desc.slot), desc.fi));
		vp->VpUploadFields[f].dst  = slabPtr + static_cast<size_t>(f) * static_cast<size_t>(fieldStride);
		vp->VpUploadFields[f].kind = desc.kind;
	}

	const uint64_t* capturedPlane = plane;
	const uint32_t capturedWords  = vp->DirtyWordCount;

	if (dirtyEntityCount < singleJobThreshold)
	{
		TrinyxJobs::DispatchNamed("VP_Slab_SingleJob", [vp, capturedPlane, capturedWords](uint32_t)
		{
			for (uint32_t f = 0; f < GpuTotalFieldCount; ++f)
			{
				const auto& info = vp->VpUploadFields[f];
				if (!info.src) continue;
				if (info.kind == GpuSlabKind::SimFloat)
				{
					const SimFloat* ssrc = static_cast<const SimFloat*>(static_cast<const void*>(info.src));
					float* fdst          = reinterpret_cast<float*>(info.dst);
					for (uint32_t w = 0; w < capturedWords; ++w)
					{
						uint64_t bits = capturedPlane[w];
						while (bits)
						{
							const uint32_t idx = w * 64 + TNX_CTZ64(bits);
							fdst[idx]          = ssrc[idx].ToFloat();
							bits &= bits - 1;
						}
					}
				}
				else
				{
					const uint32_t* usrc = reinterpret_cast<const uint32_t*>(info.src);
					uint32_t* udst       = reinterpret_cast<uint32_t*>(info.dst);
					for (uint32_t w = 0; w < capturedWords; ++w)
					{
						uint64_t bits = capturedPlane[w];
						while (bits)
						{
							const uint32_t idx = w * 64 + TNX_CTZ64(bits);
							udst[idx]          = usrc[idx];
							bits &= bits - 1;
						}
					}
				}
			}
		}, &vp->SlabUploadCounter, TrinyxJobs::Queue::Render);
	}
	else
	{
		for (uint32_t f = 0; f < GpuTotalFieldCount; ++f)
		{
			const SlabFieldUploadInfo info = vp->VpUploadFields[f];
			if (!info.src) continue;
			TrinyxJobs::DispatchNamed("VP_Slab_FieldUpload", [info, capturedPlane, capturedWords](uint32_t)
			{
				if (info.kind == GpuSlabKind::SimFloat)
				{
					const SimFloat* ssrc = static_cast<const SimFloat*>(static_cast<const void*>(info.src));
					float* fdst          = reinterpret_cast<float*>(info.dst);
					for (uint32_t w = 0; w < capturedWords; ++w)
					{
						uint64_t bits = capturedPlane[w];
						while (bits)
						{
							const uint32_t idx = w * 64 + TNX_CTZ64(bits);
							fdst[idx]          = ssrc[idx].ToFloat();
							bits &= bits - 1;
						}
					}
				}
				else
				{
					const uint32_t* usrc = reinterpret_cast<const uint32_t*>(info.src);
					uint32_t* udst       = reinterpret_cast<uint32_t*>(info.dst);
					for (uint32_t w = 0; w < capturedWords; ++w)
					{
						uint64_t bits = capturedPlane[w];
						while (bits)
						{
							const uint32_t idx = w * 64 + TNX_CTZ64(bits);
							udst[idx]          = usrc[idx];
							bits &= bits - 1;
						}
					}
				}
			}, &vp->SlabUploadCounter, TrinyxJobs::Queue::Render);
		}
	}

	vp->bSlabUploadPending       = true;
	vp->PendingRenderAckFrame    = temporalHdr->FrameNumber;
	vp->PendingVolatileFrameLock = vp->LastVolatileFrame;
	vp->PendingTemporalFrameLock = vp->LastTemporalFrame;
}

void EditorRenderer::FlushViewportSlabUpload(WorldViewport* vp)
{
	if (!vp->bSlabUploadPending) return;

	TNX_ZONE_MEDIUM_NC("Slab_ViewportUploadFlush", TNX_COLOR_RENDERING)
	TrinyxJobs::WaitForCounter(&vp->SlabUploadCounter, TrinyxJobs::Queue::Render);

	vp->bHasSlabData       = true;
	vp->bSlabUploadPending = false;

	Registry* reg = vp->TargetWorld->GetRegistry();
	reg->RenderAck.store(vp->PendingRenderAckFrame, std::memory_order_release);
	reg->RenderHasAcked = true;

	reg->GetVolatileCache()->UnlockFrameRead(vp->PendingVolatileFrameLock);
#ifdef TNX_ENABLE_ROLLBACK
	reg->GetTemporalCache()->UnlockFrameRead(vp->PendingTemporalFrameLock);
#endif
}

// -----------------------------------------------------------------------
// Fill GpuFrameData for a viewport — uses viewport's slab addresses and
// its world's camera, but the FrameSync's scratch buffer addresses.
// -----------------------------------------------------------------------

void EditorRenderer::FillGpuFrameDataForViewport(WorldViewport* vp, FrameSync& frame)
{
	auto* data = static_cast<GpuFrameData*>(vp->GpuData[CurrentFrame].MappedPtr);
	std::memset(data, 0, sizeof(GpuFrameData));

	Registry* reg            = vp->TargetWorld->GetRegistry();
	ComponentCacheBase* tc   = reg->GetTemporalCache();
	TemporalFrameHeader* hdr = tc->GetFrameHeader(vp->LastTemporalFrame);

	const ViewCamera cam = ResolveViewCamera(*vp, *hdr);

	data->Position[0] = cam.Position[0];
	data->Position[1] = cam.Position[1];
	data->Position[2] = cam.Position[2];
	data->FoV         = cam.FoVDeg;
	data->Rotation[0] = cam.Rotation.x;
	data->Rotation[1] = cam.Rotation.y;
	data->Rotation[2] = cam.Rotation.z;
	data->Rotation[3] = cam.Rotation.w;

	// Previous camera state (for GPU interpolation)
	data->OldPosition[0] = cam.PrevPosition[0];
	data->OldPosition[1] = cam.PrevPosition[1];
	data->OldPosition[2] = cam.PrevPosition[2];
	data->OldFoV         = cam.PrevFoVDeg;
	data->OldRotation[0] = cam.PrevRotation.x;
	data->OldRotation[1] = cam.PrevRotation.y;
	data->OldRotation[2] = cam.PrevRotation.z;
	data->OldRotation[3] = cam.PrevRotation.w;

	data->AspectRatio = vp->Width > 0 ? static_cast<float>(vp->Width) / static_cast<float>(vp->Height) : 1.0f;

	// Scratch buffers from shared FrameSync
	data->VerticesAddr          = MeshManager::Get().GetVertexBufferAddr();
	data->InstancesAddr         = frame.InstancesBuffer.DeviceAddr;
	data->ScanAddr              = frame.ScanBuffer.DeviceAddr;
	data->CompactCounterAddr    = frame.CompactCounterBuffer.DeviceAddr;
	data->DrawArgsAddr          = frame.DrawArgsBuffer.DeviceAddr;
	data->UnsortedInstancesAddr = frame.UnsortedInstancesBuffer.DeviceAddr;
	data->MeshHistogramAddr     = frame.MeshHistogramBuffer.DeviceAddr;
	data->MeshWriteIdxAddr      = frame.MeshWriteIdxBuffer.DeviceAddr;
	data->MeshTableAddr         = MeshManager::Get().GetMeshTableAddr();
	data->MeshCount             = MeshManager::Get().GetMeshCount();

	LogicThreadBase* logic = vp->TargetWorld->GetLogicThread();
	data->Alpha            = logic ? static_cast<float>(std::clamp(logic->GetFixedAlpha(), 0.0, 1.0)) : 1.0f;
	data->EntityCount      = static_cast<uint32_t>(ConfigPtr->MAX_CACHED_ENTITIES);
	data->OutFieldStride   = static_cast<uint32_t>(ConfigPtr->MAX_CACHED_ENTITIES);

	// +1 for the always-on EntityCacheIdx slot (GpuTotalFieldCount slab fields + 1).
	// Scatter iterates FieldCount; missing this entry skips SemEntityCacheIdx, which
	// means it never writes entity cache indices to the sorted SoA (breaking GPU picking)
	// and never registers skeletal entities in SkeletalIdxByEntityAddr (breaking LBS).
	constexpr uint32_t kFieldCount = GpuTotalFieldCount + 1;
	data->FieldCount               = kFieldCount;

	data->SkinMatrixAddr           = Skinning.GetSkinMatrixAddr();
	data->SkeletalListAddr         = Skinning.GetSkeletalListAddr();
	data->SkeletalIdxByEntityAddr  = Skinning.GetSkeletalIdxByEntityAddr();
	data->GpuBoneDataAddr          = SkeletonManager::Get().GetBoneDataAddr();
	data->GpuBoneParentAddr        = SkeletonManager::Get().GetBoneParentAddr();
	data->AnimTrackAddr            = AnimationManager::Get().GetTrackBufferAddr();
	data->AnimKeyframeAddr         = AnimationManager::Get().GetKeyframeBufferAddr();
	data->SkinWeightAddr           = MeshManager::Get().GetSkinWeightAddr();
	data->SkinSlotTableAddr        = MeshManager::Get().GetSkinSlotTableAddr();
	data->SkeletalDispatchArgsAddr = Skinning.GetSkeletalDispatchArgsAddr();
	data->GpuSkeletonSlotAddr      = SkeletonManager::Get().GetSkeletonSlotAddr();
	data->GpuAnimSlotAddr          = AnimationManager::Get().GetAnimSlotAddr();

	// Field addresses from viewport's slabs — semantics from shared SlabFieldDescs.
	const VkDeviceSize fieldStride = static_cast<VkDeviceSize>(ConfigPtr->MAX_CACHED_ENTITIES) * sizeof(float);
	const uint64_t currBase        = vp->FieldSlabs[vp->CurrentFieldSlab].DeviceAddr;
	const uint64_t prevBase        = vp->FieldSlabs[vp->PrevFieldSlab].DeviceAddr;

	for (uint32_t f = 0; f < kFieldCount; ++f)
	{
		const GpuSlabFieldDesc& desc = SlabFieldDescs[f];
		if (desc.kind == GpuSlabKind::EntityIndex)
		{
			data->CurrFieldAddrs[f] = 0;
			data->PrevFieldAddrs[f] = 0;
		}
		else
		{
			data->CurrFieldAddrs[f] = currBase + static_cast<uint64_t>(f) * fieldStride;
			data->PrevFieldAddrs[f] = prevBase + static_cast<uint64_t>(f) * fieldStride;
		}
		data->FieldSemantics[f]   = desc.sem;
		data->FieldElementSize[f] = sizeof(float);
	}

	vp->GPUActiveFrame = vp->CurrentFieldSlab;
	vp->GPUPrevFrame   = vp->PrevFieldSlab;

	// Viewport background gradient colors (written once per frame, read by gradient shader via BDA)
	const ImVec4 ic       = LINEAR_FROM_SRGB(TnxStyle::Color::ViewportInner);
	const ImVec4 oc       = LINEAR_FROM_SRGB(TnxStyle::Color::ViewportOuter);
	data->BgInnerColor[0] = ic.x;
	data->BgInnerColor[1] = ic.y;
	data->BgInnerColor[2] = ic.z;
	data->BgInnerColor[3] = ic.w;
	data->BgOuterColor[0] = oc.x;
	data->BgOuterColor[1] = oc.y;
	data->BgOuterColor[2] = oc.z;
	data->BgOuterColor[3] = oc.w;

#ifdef TNX_DEBUG_RENDERING
	data->DebugDrawMode = Editor ? Editor->GetState().DebugDrawMode : 0u;
#endif
}

// -----------------------------------------------------------------------
// RecordViewportScenePass — compute dispatches + scene draw to a
// viewport's offscreen color + depth targets.
// -----------------------------------------------------------------------

void EditorRenderer::RecordViewportScenePass(VkCommandBuffer cmd, FrameSync& frame, WorldViewport* vp)
{
	const VkExtent2D ext = { vp->Width, vp->Height };

#ifdef TNX_GPU_PICKING
	// Only pick for the editor viewport — PIE viewports don't have a pick target.
	const bool bIsEditorVP = (vp == &EditorViewport);
	bool bDoPick           = false;
	int32_t pickX = 0, pickY = 0;

#if defined(TNX_GPU_PICKING_FAST)
	// FAST: pick every frame at the mouse position, but only for the editor viewport.
	// SDL_GetGlobalMouseState returns global coords; ViewportPanelPos is in global coords
	// (ImGui main-window-relative + SDL window origin). Subtraction gives viewport-local coords.
	// Pick target is at logical pixel dimensions; no DPI scaling applied.
	if (bIsEditorVP && Editor)
	{
		float mx, my;
		SDL_GetGlobalMouseState(&mx, &my);
		pickX   = static_cast<int32_t>(mx - Editor->GetViewportPanelPos().x);
		pickY   = static_cast<int32_t>(my - Editor->GetViewportPanelPos().y);
		bDoPick = true;
	}
#else
	// On-demand: pick only when EditorContext called RequestPick() on a click.
	if (bIsEditorVP && bPickRequested.load(std::memory_order_acquire))
	{
		pickX = PickX.load(std::memory_order_relaxed);
		pickY = PickY.load(std::memory_order_relaxed);
		bPickRequested.store(false, std::memory_order_relaxed);
		bDoPick = true;
	}
#endif
#endif

	// Barriers: offscreen color + depth (+ pick when requested) to attachment optimal
	{
		VkImageMemoryBarrier2 barriers[3]{};
		uint32_t barrierCount = 2;

		barriers[0].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barriers[0].srcStageMask     = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		barriers[0].srcAccessMask    = 0;
		barriers[0].dstStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		barriers[0].dstAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		barriers[0].oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[0].newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barriers[0].image            = static_cast<VkImage>(vp->ColorTarget.Image);
		barriers[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

		barriers[1].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barriers[1].srcStageMask     = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		barriers[1].srcAccessMask    = 0;
		barriers[1].dstStageMask     = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
		barriers[1].dstAccessMask    = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		barriers[1].oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[1].newLayout        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		barriers[1].image            = static_cast<VkImage>(vp->DepthTarget.Image);
		barriers[1].subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };

#ifdef TNX_GPU_PICKING
		if (bDoPick && vp->PickTarget.IsValid())
		{
			barriers[2].sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			barriers[2].srcStageMask     = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
			barriers[2].srcAccessMask    = 0;
			barriers[2].dstStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
			barriers[2].dstAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
			barriers[2].oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
			barriers[2].newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			barriers[2].image            = static_cast<VkImage>(vp->PickTarget.Image);
			barriers[2].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			barrierCount                 = 3;
		}
#endif

		VkDependencyInfo dep{};
		dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dep.imageMemoryBarrierCount = barrierCount;
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

		// Zero CompactCounter, MeshHistogram, and SkeletalDispatchArgs.x before scatter.
		// SkeletalIdxByEntity cleared to UINT32_MAX so non-skeletal entities keep the sentinel.
		vkCmdFillBuffer(cmd, static_cast<VkBuffer>(frame.CompactCounterBuffer.Buffer), 0, sizeof(uint32_t), 0u);
		vkCmdFillBuffer(cmd, static_cast<VkBuffer>(frame.MeshHistogramBuffer.Buffer), 0,
			MaxMeshSlots * sizeof(uint32_t), 0u);
		vkCmdFillBuffer(cmd, Skinning.GetSkeletalDispatchBuffer(), 0, sizeof(uint32_t), 0u);
		vkCmdFillBuffer(cmd, Skinning.GetSkeletalIdxByEntityBuffer(), 0, VK_WHOLE_SIZE, 0xFFFFFFFFu);
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

		vkCmdPushConstants(cmd, *PipelineLayout,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
			0, sizeof(uint64_t), &gpuDataAddr);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, PredicatePipeline);
		vkCmdDispatch(cmd, dispatchX, 1, 1);
		ComputeBarrier();

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, PrefixSumPipeline);
		vkCmdDispatch(cmd, dispatchX, 1, 1);
		ComputeBarrier();

#ifdef TNX_GPU_PICKING
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
			bDoPick ? ScatterPickPipeline : ScatterPipeline);
#else
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ScatterPipeline);
#endif
		vkCmdDispatch(cmd, dispatchX, 1, 1);
		ComputeBarrier();

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, BuildDrawsPipeline);
		vkCmdDispatch(cmd, 1, 1, 1);
		ComputeBarrier();

#ifdef TNX_GPU_PICKING
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
			bDoPick ? SortPickPipeline : SortInstancesPipeline);
#else
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, SortInstancesPipeline);
#endif
		vkCmdDispatch(cmd, dispatchX, 1, 1);

		// Barrier: sort done → skinning compute (reads sorted buffer + indirect dispatch args)
		{
			VkMemoryBarrier2 mb{};
			mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
			mb.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
			mb.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
			mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
			VkDependencyInfo d{};
			d.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			d.memoryBarrierCount = 1;
			d.pMemoryBarriers    = &mb;
			vkCmdPipelineBarrier2(cmd, &d);
		}

		// Pass 6: skinning — one thread per skeletal entity, indirect dispatch
		{
			TNX_VKDBG_SCOPE(cmd, "SkinningPass (indirect)", VulkanDebug::ColorCompute);
			Skinning.Dispatch(cmd, gpuDataAddr);
		}

		// Final barrier: skin matrices + sorted instances + draw args → vertex shader + indirect draw
		{
			VkMemoryBarrier2 mb{};
			mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
			mb.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
			mb.dstStageMask  = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
			mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
			VkDependencyInfo d{};
			d.sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			d.memoryBarrierCount = 1;
			d.pMemoryBarriers    = &mb;
			vkCmdPipelineBarrier2(cmd, &d);
		}
	}

	const bool bDrawGradient = !!*GradientPipeline;

	// Dynamic viewport/scissor — valid outside render passes, persists into both gradient and scene passes.
	VkViewport viewport{};
	viewport.width    = static_cast<float>(ext.width);
	viewport.height   = static_cast<float>(ext.height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor{};
	scissor.extent = ext;
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	// Gradient mini-pass — own 1-color-attachment render pass so it's unaffected by
	// GPU_PICKING_FAST (which forces bDoPick=true, making the scene pass use 2 attachments).
	if (bDrawGradient)
	{
		VkRenderingAttachmentInfo gradColorAttach{};
		gradColorAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		gradColorAttach.imageView   = static_cast<VkImageView>(vp->ColorTarget.View);
		gradColorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		gradColorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		gradColorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

		VkRenderingInfo gradRI{};
		gradRI.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
		gradRI.renderArea           = { { 0, 0 }, ext };
		gradRI.layerCount           = 1;
		gradRI.colorAttachmentCount = 1;
		gradRI.pColorAttachments    = &gradColorAttach;

		vkCmdBeginRendering(cmd, &gradRI);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *GradientPipeline);
		vkCmdDraw(cmd, 3, 1, 0, 0);
		vkCmdEndRendering(cmd);

		VkImageMemoryBarrier2 gradBarrier{};
		gradBarrier.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		gradBarrier.srcStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		gradBarrier.srcAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		gradBarrier.dstStageMask     = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
		gradBarrier.dstAccessMask    = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
		gradBarrier.oldLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		gradBarrier.newLayout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		gradBarrier.image            = static_cast<VkImage>(vp->ColorTarget.Image);
		gradBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkDependencyInfo gradDep{};
		gradDep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		gradDep.imageMemoryBarrierCount = 1;
		gradDep.pImageMemoryBarriers    = &gradBarrier;
		vkCmdPipelineBarrier2(cmd, &gradDep);
	}

	// Scene render pass to offscreen targets
	{
		VkClearValue colorClear{};
		colorClear.color.float32[0] = TnxStyle::SrgbChan(TnxStyle::Color::BgViewport.x);
		colorClear.color.float32[1] = TnxStyle::SrgbChan(TnxStyle::Color::BgViewport.y);
		colorClear.color.float32[2] = TnxStyle::SrgbChan(TnxStyle::Color::BgViewport.z);
		colorClear.color.float32[3] = TnxStyle::Color::BgViewport.w;

		VkClearValue depthClear{};
		depthClear.depthStencil = { 1.0f, 0 };

		VkRenderingAttachmentInfo colorAttach{};
		colorAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		colorAttach.imageView   = static_cast<VkImageView>(vp->ColorTarget.View);
		colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAttach.loadOp      = bDrawGradient ? VK_ATTACHMENT_LOAD_OP_LOAD
												: VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttach.clearValue  = colorClear;

		VkRenderingAttachmentInfo depthAttach{};
		depthAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		depthAttach.imageView   = static_cast<VkImageView>(vp->DepthTarget.View);
		depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depthAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAttach.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttach.clearValue  = depthClear;

#ifdef TNX_GPU_PICKING
		VkRenderingAttachmentInfo colorAttachments[2];
		uint32_t colorAttachCount = 1;
		colorAttachments[0]       = colorAttach;

		if (bDoPick && vp->PickTarget.IsValid())
		{
			VkClearValue pickClear{};
			pickClear.color.uint32[0] = UINT32_MAX;

			VkRenderingAttachmentInfo pickAttach{};
			pickAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
			pickAttach.imageView   = static_cast<VkImageView>(vp->PickTarget.View);
			pickAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			pickAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
			pickAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
			pickAttach.clearValue  = pickClear;

			colorAttachments[1] = pickAttach;
			colorAttachCount    = 2;
		}

		VkRenderingInfo ri{};
		ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
		ri.renderArea           = { { 0, 0 }, ext };
		ri.layerCount           = 1;
		ri.colorAttachmentCount = colorAttachCount;
		ri.pColorAttachments    = colorAttachments;
		ri.pDepthAttachment     = &depthAttach;
#else
		VkRenderingInfo ri{};
		ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
		ri.renderArea           = { { 0, 0 }, ext };
		ri.layerCount           = 1;
		ri.colorAttachmentCount = 1;
		ri.pColorAttachments    = &colorAttach;
		ri.pDepthAttachment     = &depthAttach;
#endif

		vkCmdBeginRendering(cmd, &ri);

#ifdef TNX_GPU_PICKING
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
			(bDoPick && vp->PickTarget.IsValid()) ? *PickPipeline : *Pipeline);
#else
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, *Pipeline);
#endif

		VkBuffer indexBuf = MeshManager::Get().GetIndexBufferHandle();
		vkCmdBindIndexBuffer(cmd, indexBuf, 0, VK_INDEX_TYPE_UINT32);

		VkBuffer drawBuf = static_cast<VkBuffer>(frame.DrawArgsBuffer.Buffer);
		vkCmdDrawIndexedIndirect(cmd, drawBuf, 0, MeshManager::Get().GetMeshCount(),
			sizeof(VkDrawIndexedIndirectCommand));

		vkCmdEndRendering(cmd);
	}

#ifdef TNX_GPU_PICKING
	// Copy clicked pixel from pick attachment to readback buffer
	if (bDoPick && vp->PickTarget.IsValid())
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
		pickToTransfer.image            = static_cast<VkImage>(vp->PickTarget.Image);
		pickToTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

		VkDependencyInfo pickDep{};
		pickDep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		pickDep.imageMemoryBarrierCount = 1;
		pickDep.pImageMemoryBarriers    = &pickToTransfer;
		vkCmdPipelineBarrier2(cmd, &pickDep);

		VkBufferImageCopy2 copyRegion{};
		copyRegion.sType            = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
		copyRegion.bufferOffset     = 0;
		copyRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copyRegion.imageOffset      = { px, py, 0 };
		copyRegion.imageExtent      = { 1, 1, 1 };

		VkCopyImageToBufferInfo2 copyInfo{};
		copyInfo.sType          = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2;
		copyInfo.srcImage       = static_cast<VkImage>(vp->PickTarget.Image);
		copyInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		copyInfo.dstBuffer      = static_cast<VkBuffer>(frame.PickReadbackBuffer.Buffer);
		copyInfo.regionCount    = 1;
		copyInfo.pRegions       = &copyRegion;

		vkCmdCopyImageToBuffer2(cmd, &copyInfo);
		PickReadbackFrame = CurrentFrame;
	}
#endif

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
		barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

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

		FlushViewportSlabUpload(vp);
		FillGpuFrameDataForViewport(vp, frame);
		RecordViewportScenePass(cmd, frame, vp);
		needScratchBarrier = true;
	}

	// Clear and transition skipped viewport color targets UNDEFINED → SHADER_READ_ONLY
	// so ImGui can safely sample them in the composite pass.
	// Viewports with no slab data never reach RecordViewportScenePass (which owns the clear),
	// so we must clear them here to avoid displaying garbage/gray from undefined image memory.
	for (WorldViewport* vp : ActiveViewports)
	{
		if (!vp->bActive || !vp->TargetWorld) continue;
		if (vp->bHasSlabData) continue; // Already handled by RecordViewportScenePass

		// UNDEFINED → TRANSFER_DST so vkCmdClearColorImage can write.
		{
			VkImageMemoryBarrier2 barrier{};
			barrier.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			barrier.srcStageMask     = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
			barrier.srcAccessMask    = 0;
			barrier.dstStageMask     = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			barrier.dstAccessMask    = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			barrier.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
			barrier.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.image            = static_cast<VkImage>(vp->ColorTarget.Image);
			barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			VkDependencyInfo dep{};
			dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			dep.imageMemoryBarrierCount = 1;
			dep.pImageMemoryBarriers    = &barrier;
			vkCmdPipelineBarrier2(cmd, &dep);
		}

		VkClearColorValue clearVal{};
		clearVal.float32[0] = TnxStyle::SrgbChan(TnxStyle::Color::BgViewport.x);
		clearVal.float32[1] = TnxStyle::SrgbChan(TnxStyle::Color::BgViewport.y);
		clearVal.float32[2] = TnxStyle::SrgbChan(TnxStyle::Color::BgViewport.z);
		clearVal.float32[3] = TnxStyle::Color::BgViewport.w;
		VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage(cmd, static_cast<VkImage>(vp->ColorTarget.Image),
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearVal, 1, &range);

		// TRANSFER_DST → SHADER_READ_ONLY for ImGui sampling.
		{
			VkImageMemoryBarrier2 barrier{};
			barrier.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			barrier.srcStageMask     = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			barrier.srcAccessMask    = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			barrier.dstStageMask     = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
			barrier.dstAccessMask    = VK_ACCESS_2_SHADER_READ_BIT;
			barrier.oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barrier.image            = static_cast<VkImage>(vp->ColorTarget.Image);
			barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			VkDependencyInfo dep{};
			dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
			dep.imageMemoryBarrierCount = 1;
			dep.pImageMemoryBarriers    = &barrier;
			vkCmdPipelineBarrier2(cmd, &dep);
		}
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
		barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

		VkDependencyInfo dep{};
		dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dep.imageMemoryBarrierCount = 1;
		dep.pImageMemoryBarriers    = &barrier;
		vkCmdPipelineBarrier2(cmd, &dep);
	}

	// ImGui composite pass onto swapchain (clears, then draws all panels)
	{
		VkClearValue clearColor{};
		clearColor.color.float32[0] = TnxStyle::SrgbChan(TnxStyle::Color::BgViewport.x);
		clearColor.color.float32[1] = TnxStyle::SrgbChan(TnxStyle::Color::BgViewport.y);
		clearColor.color.float32[2] = TnxStyle::SrgbChan(TnxStyle::Color::BgViewport.z);
		clearColor.color.float32[3] = TnxStyle::Color::BgViewport.w;

		VkRenderingAttachmentInfo colorAttach{};
		colorAttach.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
		colorAttach.imageView   = swapView;
		colorAttach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colorAttach.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttach.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttach.clearValue  = clearColor;

		VkRenderingInfo ri{};
		ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
		ri.renderArea           = { { 0, 0 }, { ext.width, ext.height } };
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
		barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

		VkDependencyInfo dep{};
		dep.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dep.imageMemoryBarrierCount = 1;
		dep.pImageMemoryBarriers    = &barrier;
		vkCmdPipelineBarrier2(cmd, &dep);
	}

	vkEndCommandBuffer(cmd);
}
