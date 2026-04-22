#pragma once

#include "Types.h"
#include "CurveHandle.h"
#include <type_traits>

// Five camera slots evaluated in priority order (World first, Cinematic last / highest).
enum class CameraSlot : uint8_t
{
	World     = 0, // base world/environment camera
	Gameplay  = 1, // player controller camera
	Tactical  = 2, // ability/targeting overrides
	Effect    = 3, // screen shake, hit flash
	Cinematic = 4, // cutscenes — overrides everything below at full alpha
};

static constexpr uint8_t CameraSlotCount = 5;

// Resolved camera state written into the frame header each tick.
struct WorldCameraState
{
	Vector3 Position{};
	float Yaw   = 0.0f;
	float Pitch = 0.0f;
	float FOV   = 60.0f;
	bool Valid  = false;
};

// Whether an orientation dispatch consumes or falls through.
enum class ConsumeScope : uint8_t
{
	None,  // never consumes — always falls through
	Slot,  // consumes within this slot; next slot still runs
	Stack, // default — stops all further processing
};

// ─── CameraLayer ─────────────────────────────────────────────────────────────
// Base for all user-defined camera layers. Subclass and optionally inherit
// CameraStateMix, CameraBlendMix, CameraOrientationMix.
//
// Ownership stays with the caller (usually a Construct member).
// Call RemoveLayer / RemoveAllOwnerLayers before the layer is destroyed.
struct CameraLayer
{
	uint32_t     OwnerHandle     = 0;
	CameraSlot   Slot            = CameraSlot::World;
	float        TransitionAlpha = 0.0f; // animated → BlendAlpha by Tick
	float        BlendAlpha      = 1.0f; // target weight
	float        TransitionSpeed = 4.0f; // alpha units/sec
	CurveHandle  TransitionCurve{};      // 0 = linear
	ConsumeScope Consume         = ConsumeScope::Stack;
	bool         Active          = true;

	// Wired by AddLayer<T> — do not set manually.
	void  (*StateFn)(void*, WorldCameraState&)        = nullptr;
	void  (*BlendFn)(void*, float, WorldCameraState&) = nullptr;
	float (*BlendWeightFn)(void*)                     = nullptr;
	void  (*OrientationFn)(void*, float, float)       = nullptr;
};

// ─── Mixins ──────────────────────────────────────────────────────────────────
// Inherit and implement the named method. AddLayer<T> detects the method at
// push time and wires the fn-ptr — no registration ceremony required.

template <typename Derived>
struct CameraStateMix
{
	// Participates in the pipeline pass (runs first within each slot, in push order).
	// void ApplyState(WorldCameraState& state);
};

template <typename Derived>
struct CameraBlendMix
{
	// Participates in the weighted blend pass within each slot.
	// void  ApplyBlend(float alpha, WorldCameraState& state);
	// float GetBlendWeight() const;
};

template <typename Derived>
struct CameraOrientationMix
{
	// Receives dyaw/dpitch dispatches. ConsumeScope on the CameraLayer controls fallthrough.
	// void ApplyOrientationDelta(float dyaw, float dpitch);
};

// ─── CameraManager ───────────────────────────────────────────────────────────
// Per-Soul camera layer stack. Owned by Soul, ticked by LogicThread each frame.
class CameraManager
{
public:
	static constexpr uint8_t MaxLayersPerSlot = 8;

	// Add a layer. Concept detection wires fn-ptrs for any implemented mixins.
	// Ownership stays with the caller — call RemoveLayer before the layer dies.
	template <typename T>
	void AddLayer(CameraSlot slot, T* layer);

	void RemoveLayer(CameraSlot slot, uint32_t ownerHandle);
	void RemoveLayer(CameraSlot slot, CameraLayer* layer); // remove by pointer (for owned members)
	void RemoveAllOwnerLayers(uint32_t ownerHandle);

	void Tick(SimFloat dt);
	bool HasActiveLayers() const;

	// Per-slot: pipeline pass (StateMix layers), then blend pass (BlendMix layers).
	WorldCameraState Resolve() const;

	// Dispatch an orientation delta from highest slot to lowest.
	// Each layer's ConsumeScope controls whether processing continues.
	void DispatchOrientationDelta(float dyaw, float dpitch);

private:
	struct Slot
	{
		CameraLayer* Layers[MaxLayersPerSlot] = {};
		uint8_t Count                         = 0;
	};

	Slot Slots[CameraSlotCount];

	// Non-template slot-full check + storage (logs via Logger.h in .cpp).
	bool AddLayerRaw(CameraSlot slot, CameraLayer* layer);
};

// ─── AddLayer<T> ─────────────────────────────────────────────────────────────
template <typename T>
void CameraManager::AddLayer(CameraSlot slot, T* layer)
{
	static_assert(std::is_base_of_v<CameraLayer, T>, "T must derive from CameraLayer");

	layer->Slot            = slot;
	layer->TransitionAlpha = 0.0f;

	if constexpr (requires(T* d, WorldCameraState& st) { d->ApplyState(st); })
		layer->StateFn = [](void* self, WorldCameraState& st)
			{ static_cast<T*>(self)->ApplyState(st); };

	if constexpr (requires(T* d, float a, WorldCameraState& st) { d->ApplyBlend(a, st); d->GetBlendWeight(); })
	{
		layer->BlendFn       = [](void* self, float a, WorldCameraState& st)
			{ static_cast<T*>(self)->ApplyBlend(a, st); };
		layer->BlendWeightFn = [](void* self) -> float
			{ return static_cast<T*>(self)->GetBlendWeight(); };
	}

	if constexpr (requires(T* d) { d->ApplyOrientationDelta(0.0f, 0.0f); })
		layer->OrientationFn = [](void* self, float dy, float dp)
			{ static_cast<T*>(self)->ApplyOrientationDelta(dy, dp); };

	AddLayerRaw(slot, layer);
}
