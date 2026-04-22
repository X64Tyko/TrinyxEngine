#pragma once
#include <atomic>
#include <thread>
#include <vector>
#include <SDL3/SDL_video.h>

#include "Input.h"
#include "MeshManager.h"
#include "VulkanContext.h"
#include "VulkanMemory.h"

// Forward declarations
class Registry;
class LogicThreadBase;
struct EngineConfig;
struct SDL_Window;

// -----------------------------------------------------------------------
// FrameSync — per-frame synchronization and scratch resources.
// -----------------------------------------------------------------------

static constexpr int MaxFramesInFlight   = 2;
static constexpr int InstanceBufferCount = 5;

struct FrameSync
{
	VkCommandBuffer Cmd = VK_NULL_HANDLE;
	vk::raii::Semaphore Acquired{nullptr};
	vk::raii::Fence Fence{nullptr};
	VulkanBuffer GpuData;

	VulkanImage DepthAttachment;

	VulkanBuffer ScanBuffer;
	VulkanBuffer CompactCounterBuffer;
	VulkanBuffer DrawArgsBuffer;          // MeshCount × VkDrawIndexedIndirectCommand
	VulkanBuffer InstancesBuffer;         // Sorted instance SoA (draw reads from here)
	VulkanBuffer UnsortedInstancesBuffer; // Scatter output (pre-sort)
	VulkanBuffer MeshHistogramBuffer;     // uint[256] counting sort histogram
	VulkanBuffer MeshWriteIdxBuffer;      // uint[256] atomic write indices for sort

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
					LogicThreadBase* logic,
					const EngineConfig* config,
					VulkanContext* vkCtx,
					VulkanMemory* vkMem,
					SDL_Window* window, InputBuffer* vizInput);
	void Start();
	void Stop();
	void Join();
	bool IsRunning() const { return bIsRunning.load(std::memory_order_relaxed); }

	void NotifyResize() { bResizeRequested.store(true, std::memory_order_release); }

	/// Block until all submitted GPU work completes. Call before freeing in-flight resources.
	void WaitForGPU() { if (Device) vkDeviceWaitIdle(Device); }

protected:
	Derived& Self() { return *static_cast<Derived*>(this); }

	// ---- References (non-owning) ----
	Registry* RegistryPtr         = nullptr;
	LogicThreadBase* LogicPtr         = nullptr;
	const EngineConfig* ConfigPtr = nullptr;
	VulkanContext* VkCtx          = nullptr;
	VulkanMemory* VkMem           = nullptr;
	SDL_Window* WindowPtr         = nullptr;
	VkDevice Device               = nullptr;
	VkQueue GraphicsQueue         = nullptr;
	InputBuffer* VizInputPtr      = nullptr;

	// ---- Thread lifecycle ----
	std::thread Thread;
	std::atomic<bool> bIsRunning{false};
	std::atomic<bool> bResizeRequested{false};

	// ---- Thread-owned Vulkan resources ----
	VkFormat DepthFormat = VK_FORMAT_UNDEFINED;

	FrameSync Frames[MaxFramesInFlight];
	uint32_t CurrentFrame      = 0;
	uint32_t LastRenderedFrame = 0;
	uint64_t LastVolatileFrame = 0;
	uint64_t LastTemporalFrame = 0;

	std::vector<vk::raii::Semaphore> RenderedSems;

	VkShaderModule VertShader = VK_NULL_HANDLE;
	VkShaderModule FragShader = VK_NULL_HANDLE;

	VulkanBuffer FieldSlabs[InstanceBufferCount];
	uint32_t PrevFieldSlab    = 0;
	uint32_t CurrentFieldSlab = 0;

	uint32_t GPUActiveFrame = 0;
	uint32_t GPUPrevFrame   = 0;

	// ── GPU dirty bitplanes ─────────────────────────────────────────────
	// One bitplane per GPU field slab. Each bit = one entity.
	// Only tracks MAX_RENDERABLE_ENTITIES — PHYS/LOGIC partitions are never rendered.
	// When writing to slab K: upload entities marked in DirtyPlane[K], then clear it.
	// After scanning slab Flags for bit 30, OR the result into ALL planes.
	// This ensures each plane accumulates all changes since its last write.
	// Heap-allocated in Initialize() based on config MAX_RENDERABLE_ENTITIES.
	uint64_t* DirtyPlanes[InstanceBufferCount]{};
	uint64_t* DirtySnapshot = nullptr;
	uint32_t DirtyWordCount = 0;
	bool FirstSlabWrite[InstanceBufferCount]{true, true, true, true, true};

	MeshManager Meshes;

	vk::raii::PipelineLayout PipelineLayout{nullptr};
	vk::raii::Pipeline Pipeline{nullptr};

	VkPipeline PredicatePipeline     = VK_NULL_HANDLE;
	VkPipeline PrefixSumPipeline     = VK_NULL_HANDLE;
	VkPipeline ScatterPipeline       = VK_NULL_HANDLE;
	VkPipeline BuildDrawsPipeline    = VK_NULL_HANDLE;
	VkPipeline SortInstancesPipeline = VK_NULL_HANDLE;

#ifdef TNX_GPU_PICKING
	// Pick pipeline: same layout as main pipeline, but fragment shader outputs
	// to two attachments (color + R32_UINT entity cache index).
	VkShaderModule PickVertShader  = VK_NULL_HANDLE;
	VkShaderModule PickFragShader  = VK_NULL_HANDLE;
	VkPipeline ScatterPickPipeline = VK_NULL_HANDLE;
	VkPipeline SortPickPipeline    = VK_NULL_HANDLE;
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
	uint64_t FrameInputTimestamp[MaxFramesInFlight]{};
	double LatencyAccumMs   = 0.0;
	uint32_t LatencySamples = 0;
	double DisplayRefreshMs = 0.0;
#endif

	// ── Methods accessible to derived renderers ────────────────────────
	void FillGpuFrameData(FrameSync& frame);
	void WriteToFrameSlab();
	void RecordCommandBuffer(FrameSync& frame, uint32_t imageIndex);

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