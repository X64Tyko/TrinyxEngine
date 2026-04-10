#pragma once

// ---------------------------------------------------------------------------
// HudContext — abstract interface for the runtime in-game HUD.
//
// The active implementation is owned by GameplayRenderer and called from
// RecordOverlay() on the Encoder thread, between NewFrame() and Render()
// of whatever UI backend is in use (ImGui by default during R&D).
//
// Game code registers a concrete HudContext subclass at startup:
//
//   renderer.SetHudContext(std::make_unique<ArenaHud>(&world));
//
// The default is NullHudContext — zero overhead for builds without a HUD.
//
// See docs/UI.md for the full rationale and the ECS integration pattern.
// ---------------------------------------------------------------------------

class HudContext
{
public:
	virtual ~HudContext() = default;

	/// Called once per render frame on the Encoder thread.
	/// Build all HUD elements here.  For ImGui-backed implementations, this
	/// typically runs between ImGui::NewFrame() and ImGui::Render(); the exact
	/// contract depends on the backend chosen by the concrete subclass.
	/// Do not write ECS data from this call — read the published read-side
	/// slab only.
	virtual void BuildFrame() = 0;
};

// ---------------------------------------------------------------------------
// NullHudContext — default no-op implementation.  Zero overhead.
// ---------------------------------------------------------------------------
class NullHudContext final : public HudContext
{
public:
	void BuildFrame() override
	{
	}
};
