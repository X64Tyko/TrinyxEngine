#pragma once
#include <atomic>
#include <thread>

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
//   Step 1  Clear to solid color
//             acquire → vkCmdClearColorImage → present
//             Validates: semaphores, fences, swapchain lifecycle
//
//   Step 2  Full-screen triangle (hardcoded in vertex shader)
//             Dynamic rendering, graphics pipeline, no vertex buffers
//             Validates: pipeline creation, draw call, depth attachment
//
//   Step 3  Indexed cube, hardcoded world transform
//             VMA staging upload, vertex + index buffers, push constants
//             Validates: VulkanMemory, BDA, camera math
//
//   Step 4  Single entity read from TemporalComponentCache
//             Validates: CPU→GPU data path, TemporalFrameHeader access
//
//   Step 5  N entities, direct DrawIndexedInstanced
//             Per-entity SoA → GPU upload, instance buffer
//             Validates: field buffer layout, instancing
//
//   Step 6  GPU-driven: predicate → prefix-sum → scatter → DrawIndexedIndirect
//             Compute pipeline, compaction, one indirect draw call
//             Validates: full compute-driven pipeline
//
//   Step 7  PBR material system + bindless descriptors
//
// To link the Slang runtime API when needed:
//   target_link_libraries(StrigidEngine PRIVATE Slang::Compiler)
//   #include "slang.h"  (vendored in libs/slang/include/)
// -----------------------------------------------------------------------
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

private:
	void ThreadMain();

	bool CreateDepthImage();

	// ---- References (non-owning) ----
	Registry* RegistryPtr         = nullptr;
	LogicThread* LogicPtr         = nullptr;
	const EngineConfig* ConfigPtr = nullptr;
	VulkanContext* VkCtx          = nullptr;
	VulkanMemory* VkMem           = nullptr;
	SDL_Window* WindowPtr         = nullptr;

	// ---- Thread lifecycle ----
	std::thread Thread;
	std::atomic<bool> bIsRunning{false};

	// ---- Thread owned data ----
	VkImage DepthImage;
	VmaAllocation DepthImageAllocation;
	VkImageView DepthImageView;
};
