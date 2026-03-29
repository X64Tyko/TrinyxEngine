#include "EditorRenderer.h"

#include <mutex>
#include <string>
#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include "EditorContext.h"
#include "ImGuizmo.h"
#include "Logger.h"
#include "LogicThread.h"

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

	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
	ImGuizmo::BeginFrame();

	if (Editor) Editor->BuildFrame();

	ImGui::Render();
}
