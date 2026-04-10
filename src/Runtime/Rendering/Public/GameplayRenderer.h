#pragma once
#include <memory>
#include "HudContext.h"
#include "RendererCore.h"

// -----------------------------------------------------------------------
// GameplayRenderer — RendererCore with an optional runtime HUD overlay.
//
// The HUD layer defaults to NullHudContext (zero overhead).  Game code
// replaces it at startup:
//
//   renderer.SetHudContext(std::make_unique<ArenaHud>(&world));
//
// See docs/UI.md for the full HudContext design rationale.
// -----------------------------------------------------------------------

class GameplayRenderer : public RendererCore<GameplayRenderer>
{
public:
	GameplayRenderer()
		: HudLayer(std::make_unique<NullHudContext>())
	{
	}

	~GameplayRenderer() = default;

	/// Replace the active HUD implementation.  Called before the first frame.
	void SetHudContext(std::unique_ptr<HudContext> hud)
	{
		HudLayer = std::move(hud);
	}

	HudContext* GetHudContext() const { return HudLayer.get(); }

private:
	friend class RendererCore<GameplayRenderer>;

	std::unique_ptr<HudContext> HudLayer;

	// CRTP hooks
	void OnPostStart()
	{
	}

	void OnShutdown()
	{
	}

	void OnPreRecord()
	{
		HudLayer->BuildFrame();
	}

	void RecordOverlay(VkCommandBuffer)
	{
	}

	void UpdateViewportSlabs()
	{
	}

	void RecordFrame(FrameSync& frame, uint32_t imageIndex)
	{
		RecordCommandBuffer(frame, imageIndex);
	}
};
