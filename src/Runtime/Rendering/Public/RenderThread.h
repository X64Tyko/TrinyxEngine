#pragma once
#include <thread>
#include <atomic>
#include <vector>
#include <SDL3/SDL_gpu.h>

#include "SnapshotBuffer.h"

// Forward declarations
class Registry;
class LogicThread;
struct EngineConfig;
struct FramePacket;
struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;
struct SDL_GPUTexture;
struct SDL_GPUTransferBuffer;

struct alignas(16) InstanceData
{
	float PositionX, PositionY, PositionZ, _pad0; // offset  0 — 16 bytes
	float RotQx, RotQy, RotQz, RotQw;             // offset 16 — 16 bytes (quaternion)
	float ScaleX, ScaleY, ScaleZ, _pad2;          // offset 32 — 16 bytes
	float ColorR, ColorG, ColorB, ColorA;         // offset 48 — 16 bytes
};                                                // total: 64 bytes

static_assert(sizeof(InstanceData) == 64, "InstanceData must be 64 bytes");
static_assert(offsetof(InstanceData, RotQx) == 16, "Rotation must be at offset 16");
static_assert(offsetof(InstanceData, ScaleX) == 32, "Scale must be at offset 32");
static_assert(offsetof(InstanceData, ColorR) == 48, "Color must be at offset 48");

/**
 * RenderThread: The Encoder
 *
 * Consumes FramePackets from mailbox
 * Snapshots sparse arrays on Logic signal (new FrameNumber)
 * Requests GPU resources early (before interpolation)
 * Interpolates between snapshots directly to Transfer Buffer
 * Builds GPU command buffer and hands it back to main thread for submission
 *
 * Protocol with Main Thread:
 * 1. RenderThread requests resources early: bNeedsGPUResources = true
 * 2. RenderThread continues work (snapshot, prepare) while waiting
 * 3. Main checks bNeedsGPUResources when FramePacer releases fence
 * 4. Main acquires CmdBuffer + SwapchainTex, stores in atomics, clears bNeedsGPUResources
 * 5. RenderThread polls atomics, builds commands when ready
 * 6. RenderThread signals: bReadyToSubmit = true, stores CmdBuffer in atomic
 * 7. Main retrieves CmdBuffer and submits via SDL_SubmitGPUCommandBufferAndAcquireFence
 */
class RenderThread
{
public:
	RenderThread()  = default;
	~RenderThread() = default;

	void Initialize(Registry* registry, LogicThread* logic, const EngineConfig* config, SDL_GPUDevice* device,
					SDL_Window* window);
	void Start();
	void Stop();
	void Join();

	// Signals for main thread to poll
	bool NeedsGPUResources() const { return GPUSync.bNeedsGPUResources.load(std::memory_order_acquire); }
	bool ReadyToSubmit() const { return GPUSync.bReadyToSubmit.load(std::memory_order_acquire); }

	// Main thread provides resources
	void ProvideGPUResources(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* swapchain);

	// Main thread retrieves finished command buffer
	SDL_GPUCommandBuffer* TakeCommandBuffer();

	// In public interface:
	void NotifyFrameSubmitted() { GPUSync.bFrameSubmitted.store(true, std::memory_order_release); }

private:
	void ThreadMain(); // Thread entry point
	void ResizeTransferBuffer(size_t NewSize);
	void ResizeInstanceBuffer(size_t NewSize);

	// Lifecycle Methods
	bool InterpolateTemporalFrames(uint32_t frameNumber); // Interpolate between T-1 and T to interp buffer
	void RequestGPUResources();                           // Signal main thread early
	void WaitForGPUResources();                           // Spin-wait for atomics to be filled
	float CalculateInterpolationAlpha();                  // Calculate alpha from LogicThread's accumulator
	void WaitForCommandBuffer();
	bool BuildCopyPassAndUniforms();
	void WaitForSwapchainTexture();
	void BuildRenderPass();
	void SignalReadyToSubmit(); // Signal main thread to submit
	void CreateCubeMesh();
	void CreateInstanceBuffer(size_t Capacity);
	void CreateRenderPipeline();

	// References (non-owning)
	Registry* RegistryPtr                   = nullptr; // For accessing temporal cache
	LogicThread* LogicPtr                   = nullptr; // For frame number access
	const EngineConfig* ConfigPtr           = nullptr;
	class ComponentCacheBase* TemporalCache = nullptr;

	size_t InterpBufferCapacity = 0;
	size_t InterpBufferCount    = 0; // Actual number of entities interpolated
	uint32_t LastFrameNumber    = 0;

	// Current frame header (for accessing camera/view data)
	struct TemporalFrameHeader* CurrentFrameHeader = nullptr;

	// GPU Resources (provided by main thread via atomics)
	std::atomic<SDL_GPUCommandBuffer*> CmdBufferAtomic{nullptr};
	std::atomic<SDL_GPUTexture*> SwapchainTextureAtomic{nullptr};
	SDL_GPUDevice* GpuDevice              = nullptr;
	SDL_Window* EngineWindow              = nullptr;
	SDL_GPUGraphicsPipeline* Pipeline     = nullptr;
	SDL_GPUBuffer* VertexBuffer           = nullptr;
	SDL_GPUBuffer* IndexBuffer            = nullptr;
	SDL_GPUBuffer* InstanceBuffer         = nullptr;
	SDL_GPUShader* VertexShader           = nullptr;
	SDL_GPUShader* FragmentShader         = nullptr;
	SDL_GPUTransferBuffer* TransferBuffer = nullptr;
	size_t TransferBufferCapacity         = 0;
	size_t InstanceBufferCapacity         = 0;

	// Pack atomics to share cache line (64 bytes)
	alignas(64) struct GPUSyncAtomics
	{
		std::atomic<bool> bNeedsGPUResources{false};
		std::atomic<bool> bReadyToSubmit{false};
		std::atomic<bool> bFrameSubmitted{true};
		char padding[64 - 3 * sizeof(std::atomic<bool>)]; // Prevent false sharing with next data
	} GPUSync;

	// FPS tracking
	uint32_t FpsFrameCount  = 0;
	double FpsTimer         = 0.0;
	uint64_t LastFpsCounter = 0;

	// Threading
	std::thread Thread;
	std::atomic<bool> bIsRunning{false};
};