#pragma once
#if !defined(TNX_ENABLE_EDITOR)
#error "WorldViewport.h requires TNX_ENABLE_EDITOR"
#endif

#include <cstdint>
#include "VulkanMemory.h"
#include "RendererCore.h" // MaxFramesInFlight

class World;

// -----------------------------------------------------------------------
// WorldViewport — per-world GPU resource bundle for editor multi-viewport.
//
// Each rendered world needs its own:
//   - Offscreen color + depth targets (rendered scene)
//   - ImGui descriptor set (for ImGui::Image() compositing)
//   - Field slabs (entity data for GPU compute pipeline)
//   - Dirty tracking bitplanes
//   - Frame tracking state
//
// Compute scratch buffers (ScanBuffer, CompactCounter, DrawArgs, etc.)
// are shared across viewports because worlds render sequentially within
// a frame — not in parallel. Only FieldSlabs, DirtyPlanes, and offscreen
// images need per-world allocation.
// -----------------------------------------------------------------------

static constexpr int kViewportSlabCount = 5;

struct WorldViewport
{
	World* TargetWorld = nullptr;

	// ── Offscreen render targets ────────────────────────────────────────
	VulkanImage ColorTarget;                       // RGBA8, rendered scene
	VulkanImage DepthTarget;                       // D32_SFLOAT
	VkDescriptorSet ImGuiTexture = VK_NULL_HANDLE; // For ImGui::Image()
#ifdef TNX_GPU_PICKING
	VulkanImage PickTarget;                        // R32_UINT, entity cache index per pixel
#endif

	// ── Per-viewport sampler (for ImGui::Image compositing) ─────────────
	VkSampler ImGuiSampler = VK_NULL_HANDLE;

	// ── Per-viewport GpuFrameData (written by CPU, read by GPU via BDA) ─
	// One buffer per FrameSync slot — prevents the CPU from overwriting
	// slot N's GpuData while the GPU is still executing slot N-1 (which
	// reads from the same buffer via BDA push constant).
	VulkanBuffer GpuData[MaxFramesInFlight];

	// ── Per-world field slabs (same count as main renderer) ─────────────
	VulkanBuffer FieldSlabs[kViewportSlabCount];
	uint32_t CurrentFieldSlab               = 0;
	uint32_t PrevFieldSlab                  = 0;
	uint32_t GPUActiveFrame                 = 0;
	uint32_t GPUPrevFrame                   = 0;
	bool FirstSlabWrite[kViewportSlabCount] = {true, true, true, true, true};

	// ── Per-world dirty tracking ────────────────────────────────────────
	uint64_t* DirtyPlanes[kViewportSlabCount] = {};
	uint64_t* DirtySnapshot                   = nullptr;
	uint32_t DirtyWordCount                   = 0;

	// ── Per-world frame tracking ────────────────────────────────────────
	uint64_t LastVolatileFrame = 0;
	uint64_t LastTemporalFrame = 0;

	// ── Viewport dimensions ─────────────────────────────────────────────
	uint32_t Width    = 0;
	uint32_t Height   = 0;
	bool bActive      = true;
	bool bHasSlabData = false; // True after first WriteToViewportSlab — prevents GPU dispatches on uninitialized data

	// ── Lifecycle helpers ───────────────────────────────────────────────

	/// Allocate dirty tracking arrays. Called by renderer on viewport creation.
	void AllocateDirtyPlanes(uint32_t wordCount)
	{
		DirtyWordCount = wordCount;
		DirtySnapshot  = new uint64_t[wordCount]();
		for (auto& plane : DirtyPlanes) plane = new uint64_t[wordCount]();
	}

	/// Free dirty tracking arrays. Called by renderer on viewport destruction.
	void FreeDirtyPlanes()
	{
		delete[] DirtySnapshot;
		DirtySnapshot = nullptr;
		for (auto& plane : DirtyPlanes)
		{
			delete[] plane;
			plane = nullptr;
		}
		DirtyWordCount = 0;
	}
};