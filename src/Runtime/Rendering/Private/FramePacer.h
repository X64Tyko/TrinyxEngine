#pragma once
#include <SDL3/SDL_gpu.h>

struct FrameContext
{
	SDL_GPUFence* fence              = nullptr;
	SDL_GPUCommandBuffer* cmd_buffer = nullptr;
};

class FramePacer
{
public:
	static constexpr int FRAMES_IN_FLIGHT = 3;

private:
	FrameContext frames[FRAMES_IN_FLIGHT];
	int frame_index       = 0;
	SDL_GPUDevice* device = nullptr;

public:
	int GetFrameIndex() const { return frame_index; }

	void Initialize(SDL_GPUDevice* _device)
	{
		device = _device;
		// Pre-allocate fences? No, SDL3 acquires them on demand usually, 
		// but we need handles to wait on.
		// Actually, SDL3's AcquireGPUFence creates a new handle.
		// We just hold the pointer.
	}

	// Call this AT THE START of your Render Loop.
	// It enforces the "Speed Limit" before you do any work.
	bool BeginFrame()
	{
		FrameContext& ctx = frames[frame_index];

		// 1. The Governor: If this slot is still busy on the GPU, WAIT.
		if (ctx.fence)
		{
			if (SDL_QueryGPUFence(device, ctx.fence))
			{
				SDL_ReleaseGPUFence(device, ctx.fence);
				ctx.fence = nullptr;
			}
			else
			{
				return false;
			}
		}

		// 2. The Setup: We are now safe to use this slot.
		return true;
	}

	// Call this AT THE END, right before Submit.
	void EndFrame(SDL_GPUCommandBuffer* cmd)
	{
		FrameContext& ctx = frames[frame_index];

		// 1. Get a fresh fence for this new submission
		ctx.fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);

		// 3. Cycle to the next slot
		frame_index = (frame_index + 1) % FRAMES_IN_FLIGHT;
	}
};
