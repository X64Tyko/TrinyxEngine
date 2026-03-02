#pragma once
#include <atomic>
#include <thread>
#include <vector>

#include "VulkanContext.h"
#include "VulkanMemory.h"

// Forward declarations
class Registry;
class LogicThread;
struct EngineConfig;
struct SDL_Window;

// -----------------------------------------------------------------------
// VulkRender  (next-generation Encoder)
//
// Clean-room renderer built for the GPU-driven pipeline.
// RenderThread.cpp/.h is preserved as legacy reference.
//
// Build incrementally — one passing step before the next:
//
//   Step 1  Clear to solid color - Done
//             acquire → vkCmdClearColorImage → present
//             Validates: semaphores, fences, swapchain lifecycle
//
//   Step 2  Full-screen triangle (hardcoded in vertex shader) - Got the cube pushed and drawing
//             Dynamic rendering, graphics pipeline, no vertex buffers
//             Validates: pipeline creation, draw call, depth attachment
//
//   Step 3  Indexed cube, hardcoded world transform - What I said above
//             VMA staging upload, vertex + index buffers, push constants
//             Validates: VulkanMemory, BDA, camera math
//
//   Step 4  N entities, direct DrawIndexedInstanced
//             Per-entity SoA → GPU upload, instance buffer
//             Validates: field buffer layout, instancing
//
//   Step 5  GPU-driven: predicate → prefix-sum → scatter → DrawIndexedIndirect
//             Compute pipeline, compaction, one indirect draw call
//             Validates: full compute-driven pipeline
//
//   Step 6  PBR material system + bindless descriptors
//
// Command buffer recording (vkCmd*) stays on the C API path so volk's
// global function pointers are used directly — no overhead from dynamic dispatch.
// -----------------------------------------------------------------------

static constexpr int kMaxFramesInFlight = 2;

// Per-frame synchronization resources.
// Cmd stays as a raw VkCommandBuffer — all recording uses vkCmd* C API.
// Sync objects are vk::raii:: so they self-destruct without a manual Cleanup().
struct FrameSync
{
	VkCommandBuffer Cmd = VK_NULL_HANDLE; // C handle — recording uses vkCmd*
	vk::raii::Semaphore Acquired{nullptr};
	vk::raii::Fence Fence{nullptr};
	VulkanBuffer GpuData; // per-frame instance/uniform buffer
};

class VulkRender
{
public:
	VulkRender()  = default;
	~VulkRender() = default;

	VulkRender(const VulkRender&)            = delete;
	VulkRender& operator=(const VulkRender&) = delete;

	void Initialize(Registry* registry,
					LogicThread* logic,
					const EngineConfig* config,
					VulkanContext* vkCtx,
					VulkanMemory* vkMem,
					SDL_Window* window);
	void Start();
	void Stop();
	void Join();
	bool IsRunning() const { return bIsRunning.load(std::memory_order_relaxed); }

private:
	void ThreadMain();
	int RenderFrame();

	bool CreateDepthImage();
	bool CreateFrameSync();
	bool LoadShaders();
	void DestroyShaders();
	bool CreatePipeline();
	bool CreateMeshBuffers();
	void FillGpuFrameData(FrameSync& frame);
	void RecordCommandBuffer(FrameSync& frame, uint32_t imageIndex);
	void TrackFPS();
	void OnSwapchainResize();

	// ---- References (non-owning) ----
	Registry* RegistryPtr         = nullptr;
	LogicThread* LogicPtr         = nullptr;
	const EngineConfig* ConfigPtr = nullptr;
	VulkanContext* VkCtx          = nullptr;
	VulkanMemory* VkMem           = nullptr;
	SDL_Window* WindowPtr         = nullptr;
	VkDevice device               = nullptr;
	VkQueue graphicsQueue         = nullptr;

	// ---- Thread lifecycle ----
	std::thread Thread;
	std::atomic<bool> bIsRunning{false};

	// ---- Thread-owned Vulkan resources ----
	// Depth attachment — allocated via VMA, auto-destroys on VulkRender teardown.
	VulkanImage DepthAttachment;
	VkFormat DepthFormat = VK_FORMAT_UNDEFINED; // selected in CreateDepthImage, read by CreatePipeline

	// Per-frame sync + command buffers (populated in CreateFrameSync).
	FrameSync Frames[kMaxFramesInFlight];
	uint32_t CurrentFrame = 0;

	// One render-finished semaphore per swapchain image (populated in CreateFrameSync).
	// Indexed by imageIndex so the presentation engine never reuses a semaphore
	// it may still hold from the previous present of the same image.
	std::vector<vk::raii::Semaphore> RenderedSems;

	// Shader modules (loaded in LoadShaders, destroyed in DestroyShaders).
	VkShaderModule VertShader = VK_NULL_HANDLE;
	VkShaderModule FragShader = VK_NULL_HANDLE;

	// Mesh buffers (uploaded in CreateMeshBuffers, static for the lifetime of the renderer).
	VulkanBuffer VertexBuffer; // cube vertices read via BDA in vertex shader
	VulkanBuffer IndexBuffer;  // cube indices bound via vkCmdBindIndexBuffer

	// Pipeline (populated in Step 2).
	vk::raii::PipelineLayout PipelineLayout{nullptr};
	vk::raii::Pipeline Pipeline{nullptr};

	// FPS tracking (render thread only).
	double RenderFpsTimer     = 0.0;
	double RenderLastFPSCheck = 0.0;
	uint32_t RenderFrameCount = 0;
};
