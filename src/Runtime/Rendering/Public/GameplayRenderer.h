#pragma once
#include "RendererCore.h"

// -----------------------------------------------------------------------
// GameplayRenderer — RendererCore with no editor overlay.
// All hooks are no-ops; compiled out entirely by the optimizer.
// -----------------------------------------------------------------------

class GameplayRenderer : public RendererCore<GameplayRenderer>
{
public:
	GameplayRenderer()  = default;
	~GameplayRenderer() = default;

private:
	friend class RendererCore<GameplayRenderer>;

	// CRTP hooks — all empty
	void OnPostStart()
	{
	}

	void OnShutdown()
	{
	}

	void OnPreRecord()
	{
	}

	void RecordOverlay(VkCommandBuffer)
	{
	}
};
