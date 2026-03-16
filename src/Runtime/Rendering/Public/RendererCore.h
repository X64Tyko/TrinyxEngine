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
// FrameSync — per-frame synchronization and scratch resources.
// -----------------------------------------------------------------------

static constexpr int kMaxFramesInFlight   = 2;
static constexpr int kInstanceBufferCount = 5;

struct FrameSync
{
	VkCommandBuffer Cmd = VK_NULL_HANDLE;
	vk::raii::Semaphore Acquired{nullptr};
	vk::raii::Fence Fence{nullptr};
	VulkanBuffer GpuData;

	VulkanImage DepthAttachment;

	VulkanBuffer ScanBuffer;
	VulkanBuffer CompactCounterBuffer;
	VulkanBuffer DrawArgsBuffer;
	VulkanBuffer InstancesBuffer;

#ifdef TNX_GPU_PICKING
	VulkanImage PickAttachment;      // R32_UINT color attachment for entity cache index
	VulkanBuffer PickReadbackBuffer; // Staging buffer for single-pixel readback
#endif
};

// -----------------------------------------------------------------------
// RendererCore<Derived>  (CRTP)
//
// All shared Vulkan state, pipelines, field slabs, and the complete
// rendering pipeline. Editor/gameplay specialization happens through
// hooks that Derived implements:
//
//   void OnPostStart()               — after GPU resources created
//   void OnShutdown()                — before device idle teardown
//   void OnPreRecord()               — before command buffer recording (ImGui frame build)
//   void RecordOverlay(VkCommandBuffer) — within rendering pass (ImGui draw)
//
// Command buffer recording stays on the C API path (vkCmd*).
// -----------------------------------------------------------------------

template <typename Derived>
class RendererCore
{
public:
	RendererCore()  = default;
	~RendererCore() = default;

	RendererCore(const RendererCore&)            = delete;
	RendererCore& operator=(const RendererCore&) = delete;

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

	void NotifyResize() { bResizeRequested.store(true, std::memory_order_release); }

protected:
	Derived& Self() { return *static_cast<Derived*>(this); }

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
	std::atomic<bool> bResizeRequested{false};

	// ---- Thread-owned Vulkan resources ----
	VkFormat DepthFormat = VK_FORMAT_UNDEFINED;

	FrameSync Frames[kMaxFramesInFlight];
	uint32_t CurrentFrame      = 0;
	uint64_t LastVolatileFrame = 0;
	uint64_t LastTemporalFrame = 0;

	std::vector<vk::raii::Semaphore> RenderedSems;

	VkShaderModule VertShader = VK_NULL_HANDLE;
	VkShaderModule FragShader = VK_NULL_HANDLE;

	VulkanBuffer FieldSlabs[kInstanceBufferCount];
	uint32_t PrevFieldSlab    = 0;
	uint32_t CurrentFieldSlab = 0;

	uint32_t GPUActiveFrame = 0;
	uint32_t GPUPrevFrame   = 0;

	VulkanBuffer VertexBuffer;
	VulkanBuffer IndexBuffer;

	vk::raii::PipelineLayout PipelineLayout{nullptr};
	vk::raii::Pipeline Pipeline{nullptr};

	VkPipeline PredicatePipeline = VK_NULL_HANDLE;
	VkPipeline PrefixSumPipeline = VK_NULL_HANDLE;
	VkPipeline ScatterPipeline   = VK_NULL_HANDLE;

#ifdef TNX_GPU_PICKING
	// Pick pipeline: same layout as main pipeline, but fragment shader outputs
	// to two attachments (color + R32_UINT entity cache index).
	VkShaderModule PickVertShader  = VK_NULL_HANDLE;
	VkShaderModule PickFragShader  = VK_NULL_HANDLE;
	VkPipeline ScatterPickPipeline = VK_NULL_HANDLE;
	vk::raii::Pipeline PickPipeline{nullptr};

	// On-demand pick request (TNX_GPU_PICKING without FAST).
	// FAST mode always renders to pick attachment and copies the mouse pixel.
	std::atomic<bool> bPickRequested{false};
	std::atomic<int32_t> PickX{0};
	std::atomic<int32_t> PickY{0};

	// Pick result: written by render thread after readback, read by external code.
	std::atomic<uint32_t> PickResult{UINT32_MAX}; // UINT32_MAX = no pick / invalid
	std::atomic<bool> bPickResultReady{false};

	// Tracks which frame slot's readback is pending (FAST mode reads previous frame's result).
	uint32_t PickReadbackFrame = 0;
#endif

	double RenderFpsTimer     = 0.0;
	double RenderLastFPSCheck = 0.0;
	uint32_t RenderFrameCount = 0;

#if TNX_DEV_METRICS
	uint64_t FrameInputTimestamp[kMaxFramesInFlight]{};
	double LatencyAccumMs   = 0.0;
	uint32_t LatencySamples = 0;
	double DisplayRefreshMs = 0.0;
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

#ifdef TNX_GPU_PICKING
	bool CreatePickImages();
	bool LoadPickShaders();
	bool CreatePickPipeline();
	void DestroyPickShaders();
#endif

public:
#ifdef TNX_GPU_PICKING
	/// Request a pick at pixel (x,y). Thread-safe. Result available next frame via ConsumePickResult().
	void RequestPick(int32_t x, int32_t y);
	/// Consume the pick result. Returns the entity cache index, or UINT32_MAX if no hit.
	/// Caller must resolve cache index → EntityRecord. Returns false if no result ready.
	bool ConsumePickResult(uint32_t& outCacheIdx);
#endif
};