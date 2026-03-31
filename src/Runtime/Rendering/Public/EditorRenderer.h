#pragma once
#if !defined(TNX_ENABLE_EDITOR)
#error "EditorRenderer.h requires TNX_ENABLE_EDITOR"
#endif

#include <atomic>
#include "RendererCore.h"

union SDL_Event;
class EditorContext;
class TrinyxEngine;
struct ImGuiEventQueue;
struct WorldViewport;

// -----------------------------------------------------------------------
// EditorRenderer — RendererCore + ImGui / editor overlay.
// -----------------------------------------------------------------------

class EditorRenderer : public RendererCore<EditorRenderer>
{
public:
	EditorRenderer()  = default;
	~EditorRenderer() = default;

	/// Set the engine pointer for editor use (spawn handshake, scene load, etc.)
	void SetEngine(TrinyxEngine* engine) { EnginePtr = engine; }

	/// Feed an SDL event to ImGui from the main thread. Thread-safe.
	void PushImGuiEvent(const SDL_Event& event);

	/// True when the editor owns keyboard input (default). False when the viewport
	/// is active (right-click held) or Play mode is running.
	bool EditorOwnsKeyboard() const { return bEditorOwnsKeyboard.load(std::memory_order_relaxed); }
	void SetEditorOwnsKeyboard(bool owns) { bEditorOwnsKeyboard.store(owns, std::memory_order_relaxed); }

	// ── Multi-viewport (PIE) ────────────────────────────────────────────
	void AddViewport(WorldViewport* vp);
	void RemoveViewport(WorldViewport* vp);
	void AllocateViewportResources(WorldViewport* vp, uint32_t width, uint32_t height);
	void FreeViewportResources(WorldViewport* vp);

private:
	friend class RendererCore<EditorRenderer>;

	// CRTP hooks called by RendererCore
	void OnPostStart();
	void OnShutdown();
	void OnPreRecord();
	void RecordOverlay(VkCommandBuffer cmd);
	void UpdateViewportSlabs();
	void RecordFrame(FrameSync& frame, uint32_t imageIndex);

	// ImGui lifecycle
	bool InitImGui();
	void ShutdownImGui();
	void DrainImGuiEvents();
	void BuildImGuiFrame();

	// PIE viewport rendering
	void WriteToViewportSlab(WorldViewport* vp);
	void FillGpuFrameDataForViewport(WorldViewport* vp, FrameSync& frame);
	void RecordViewportScenePass(VkCommandBuffer cmd, FrameSync& frame, WorldViewport* vp);
	void RecordPIEFrame(FrameSync& frame, uint32_t imageIndex);

	// Editor-specific state
	VkDescriptorPool ImGuiDescriptorPool = VK_NULL_HANDLE;
	bool bImGuiInitialized               = false;
	ImGuiEventQueue* EventQueue          = nullptr;
	EditorContext* Editor                = nullptr;
	TrinyxEngine* EnginePtr              = nullptr;
	std::atomic<bool> bEditorOwnsKeyboard{true};

	// Multi-viewport state
	std::vector<WorldViewport*> ActiveViewports;
};
