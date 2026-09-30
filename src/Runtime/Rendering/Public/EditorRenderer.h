#pragma once
#if !defined(TNX_ENABLE_EDITOR)
#error "EditorRenderer.h requires TNX_ENABLE_EDITOR"
#endif

#include <atomic>
#include "RendererCore.h"
#include "WorldViewport.h"

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

	/// True while an editor tool (viewport fly) needs relative mouse mode without routing input
	/// to the engine. Sentinel applies it alongside EditorOwnsKeyboard.
	bool EditorCapturesMouse() const { return bEditorCapturesMouse.load(std::memory_order_relaxed); }
	void SetEditorCapturesMouse(bool captures) { bEditorCapturesMouse.store(captures, std::memory_order_relaxed); }

	/// Relative mouse motion and wheel accumulated from this frame's drained events. Render thread only.
	float GetMouseRelX() const { return MouseRelX; }
	float GetMouseRelY() const { return MouseRelY; }
	float GetMouseWheel() const { return MouseWheel; }


	// ── Editor viewport ──────────────────────────────────────────────────
	/// Called by EditorContext when the "Viewport" panel resizes.
	void ResizeEditorViewport(uint32_t width, uint32_t height);

	/// ImGui texture handle for the editor's main viewport. Valid after OnPostStart.
	VkDescriptorSet GetEditorViewportTexture() const;

	/// Enable or disable rendering the editor world. Call with false during PIE to
	/// avoid running the full compute pipeline for a world that isn't displayed.
	void SetEditorViewportActive(bool active) { EditorViewport.bActive = active; }

	/// The editor's main viewport — owns the edit camera. Render thread only.
	WorldViewport& GetEditorViewport() { return EditorViewport; }

	// ── Multi-viewport (PIE) ────────────────────────────────────────────
	void AddViewport(WorldViewport* vp);
	void RemoveViewport(WorldViewport* vp);
	void AllocateViewportResources(WorldViewport* vp, uint32_t width, uint32_t height);
	void FreeViewportResources(WorldViewport* vp);
	/// Resize a PIE viewport's GPU resources if its panel size changed.
	void ResizeViewport(WorldViewport* vp, uint32_t width, uint32_t height);

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

	// Viewport gradient pre-pass
	bool LoadGradientShaders();
	bool CreateGradientPipeline();
	void DestroyGradientShaders();

	// PIE viewport rendering
	void WriteToViewportSlab(WorldViewport* vp);
	void FlushViewportSlabUpload(WorldViewport* vp);
	void FillGpuFrameDataForViewport(WorldViewport* vp, FrameSync& frame);
	void RecordViewportScenePass(VkCommandBuffer cmd, FrameSync& frame, WorldViewport* vp);
	void RecordPIEFrame(FrameSync& frame, uint32_t imageIndex);

	// Viewport gradient pre-pass pipeline
	VkShaderModule GradientVertShader = VK_NULL_HANDLE;
	VkShaderModule GradientFragShader = VK_NULL_HANDLE;
	vk::raii::Pipeline GradientPipeline{ nullptr };

	// Editor-specific state
	VkDescriptorPool ImGuiDescriptorPool = VK_NULL_HANDLE;
	bool bImGuiInitialized               = false;
	ImGuiEventQueue* EventQueue          = nullptr;
	EditorContext* Editor                = nullptr;
	TrinyxEngine* EnginePtr              = nullptr;
	std::atomic<bool> bEditorOwnsKeyboard{ true };
	std::atomic<bool> bEditorCapturesMouse{ false };
	float MouseRelX  = 0.0f;
	float MouseRelY  = 0.0f;
	float MouseWheel = 0.0f;

	// Editor's own persistent viewport (always active, main world)
	WorldViewport EditorViewport;

	// Multi-viewport state
	std::vector<WorldViewport*> ActiveViewports;
};
