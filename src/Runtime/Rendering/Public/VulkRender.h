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

#if TNX_ENABLE_EDITOR
union SDL_Event;
class EditorContext;
struct ImGuiEventQueue;
#endif

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

static constexpr int kMaxFramesInFlight   = 2;
static constexpr int kInstanceBufferCount = 5; // CPU cycles freely; GPU holds at most kMaxFramesInFlight at once

// Per-frame synchronization resources.
// Cmd stays as a raw VkCommandBuffer — all recording uses vkCmd* C API.
// Sync objects are vk::raii:: so they self-destruct without a manual Cleanup().
// All GPU-written scratch buffers (compute outputs) live here so concurrent
// in-flight frames never share mutable GPU resources.
struct FrameSync
{
	VkCommandBuffer Cmd = VK_NULL_HANDLE; // C handle — recording uses vkCmd*
	vk::raii::Semaphore Acquired{nullptr};
	vk::raii::Fence Fence{nullptr};
	VulkanBuffer GpuData; // per-frame GpuFrameData uniform buffer

	// Per-frame depth attachment — prevents depth races between in-flight frames.
	VulkanImage DepthAttachment;

	// Per-frame compute scratch/output buffers.
	// Must be per-frame: scatter writes and the indirect draw read these in the
	// same submission, but two frame slots can be GPU-in-flight simultaneously,
	// so sharing a single copy would cause write/read races across submissions.
	VulkanBuffer ScanBuffer;           // predicate 0/1 → exclusive-scan index; device local
	VulkanBuffer CompactCounterBuffer; // single uint atomicAdd target; device local
	VulkanBuffer DrawArgsBuffer;       // VkDrawIndexedIndirectCommand; scatter sets instanceCount
	VulkanBuffer InstancesBuffer;      // compact SoA output; scatter writes, vertex reads via BDA
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

#if TNX_ENABLE_EDITOR
	/// Feed an SDL event to ImGui from the main thread. Thread-safe.
	void PushImGuiEvent(const SDL_Event& event);
#endif

private:
	void ThreadMain();
	int RenderFrame();

	bool CreateDepthImage();
	bool CreateFrameSync();
	bool LoadShaders();
	void DestroyShaders();
	bool CreatePipeline();
	bool CreateComputePipelines();
	bool CreateMeshBuffers();
	void FillGpuFrameData(FrameSync& frame);
	void WriteToFrameSlab();
	void RecordCommandBuffer(FrameSync& frame, uint32_t imageIndex);
	void TrackFPS();
	void OnSwapchainResize();

#if TNX_ENABLE_EDITOR
	// ---- ImGui ----
	bool InitImGui();
	void ShutdownImGui();
	void DrainImGuiEvents();
	void BuildImGuiFrame();
#endif

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
	VkFormat DepthFormat = VK_FORMAT_UNDEFINED; // selected in CreateDepthImage, read by CreatePipeline

	// Per-frame sync + command buffers (populated in CreateFrameSync).
	// Each slot also owns its depth image and compute scratch buffers — see FrameSync.
	FrameSync Frames[kMaxFramesInFlight];
	uint32_t CurrentFrame      = 0;
	uint64_t LastVolatileFrame = 0;
	uint64_t LastTemporalFrame = 0;

	// One render-finished semaphore per swapchain image (populated in CreateFrameSync).
	// Indexed by imageIndex so the presentation engine never reuses a semaphore
	// it may still hold from the previous present of the same image.
	std::vector<vk::raii::Semaphore> RenderedSems;

	// Shader modules (loaded in LoadShaders, destroyed in DestroyShaders).
	VkShaderModule VertShader = VK_NULL_HANDLE;
	VkShaderModule FragShader = VK_NULL_HANDLE;

	// 5 field slabs cycling independently of GPU frame slots.
	// Each slab = kGpuOutFieldCount × MAX_CACHED_ENTITIES × sizeof(float).
	// Field f base address = slab.DeviceAddr + f * MAX_CACHED_ENTITIES * sizeof(float).
	// GPU reads at most kMaxFramesInFlight slabs at once; CPU has ≥3 free slots to write fresh data.
	VulkanBuffer FieldSlabs[kInstanceBufferCount];
	uint32_t PrevFieldSlab    = 0;
	uint32_t CurrentFieldSlab = 0;

	uint32_t GPUActiveFrame = 0;
	uint32_t GPUPrevFrame   = 0;

	// Mesh buffers (uploaded in CreateMeshBuffers, static for the lifetime of the renderer).
	VulkanBuffer VertexBuffer; // cube vertices read via BDA in vertex shader
	VulkanBuffer IndexBuffer;  // cube indices bound via vkCmdBindIndexBuffer

	// Graphics pipeline (shared layout also used by all compute pipelines).
	vk::raii::PipelineLayout PipelineLayout{nullptr};
	vk::raii::Pipeline Pipeline{nullptr};

	// Compute pipelines (raw handles — destroyed in DestroyShaders).
	VkPipeline PredicatePipeline = VK_NULL_HANDLE;
	VkPipeline PrefixSumPipeline = VK_NULL_HANDLE;
	VkPipeline ScatterPipeline   = VK_NULL_HANDLE;

	// FPS tracking (render thread only).
	double RenderFpsTimer     = 0.0;
	double RenderLastFPSCheck = 0.0;
	uint32_t RenderFrameCount = 0;

#if TNX_DEV_METRICS
	// Input-to-photon latency tracking.
	// Stamped per frame slot in FillGpuFrameData, measured after present.
	// DisplayRefreshMs is added to account for scanout latency after vkQueuePresentKHR returns.
	uint64_t FrameInputTimestamp[kMaxFramesInFlight]{};
	double LatencyAccumMs   = 0.0;
	uint32_t LatencySamples = 0;
	double DisplayRefreshMs = 0.0;
#endif

#if TNX_ENABLE_EDITOR
	// ---- ImGui / Editor ----
	// Raw pointers — lifetime managed by InitImGui/ShutdownImGui.
	// Avoids unique_ptr<incomplete type> forcing constructor/destructor out-of-line.
	VkDescriptorPool ImGuiDescriptorPool = VK_NULL_HANDLE;
	bool bImGuiInitialized               = false;
	ImGuiEventQueue* EventQueue          = nullptr;
	EditorContext* Editor                = nullptr;
#endif
};