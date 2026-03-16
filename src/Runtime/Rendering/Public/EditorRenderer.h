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

private:
	friend class RendererCore<EditorRenderer>;

	// CRTP hooks called by RendererCore
	void OnPostStart();
	void OnShutdown();
	void OnPreRecord();
	void RecordOverlay(VkCommandBuffer cmd);

	// ImGui lifecycle
	bool InitImGui();
	void ShutdownImGui();
	void DrainImGuiEvents();
	void BuildImGuiFrame();

	// Editor-specific state
	VkDescriptorPool ImGuiDescriptorPool = VK_NULL_HANDLE;
	bool bImGuiInitialized               = false;
	ImGuiEventQueue* EventQueue          = nullptr;
	EditorContext* Editor                = nullptr;
	TrinyxEngine* EnginePtr              = nullptr;
	std::atomic<bool> bEditorOwnsKeyboard{true};
};
