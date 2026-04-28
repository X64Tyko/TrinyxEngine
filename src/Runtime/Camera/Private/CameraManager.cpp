#include "CameraManager.h"
#include "Logger.h"
#include <algorithm>
#include <cmath>

bool CameraManager::AddLayerRaw(CameraSlot slot, CameraLayer* layer)
{
	Slot& s = Slots[static_cast<uint8_t>(slot)];
	if (s.Count >= MaxLayersPerSlot)
	{
		LOG_ENG_WARN_F("[CameraManager] AddLayer: slot %u is full (max %u layers)",
			static_cast<uint8_t>(slot), MaxLayersPerSlot);
		return false;
	}
	s.Layers[s.Count++] = layer;
	return true;
}

void CameraManager::RemoveLayer(CameraSlot slot, uint32_t ownerHandle)
{
	Slot& s = Slots[static_cast<uint8_t>(slot)];
	for (uint8_t i = 0; i < s.Count; )
	{
		if (s.Layers[i]->OwnerHandle == ownerHandle)
			s.Layers[i] = s.Layers[--s.Count]; // swap-erase
		else
			++i;
	}
}

void CameraManager::RemoveLayer(CameraSlot slot, CameraLayer* layer)
{
	Slot& s = Slots[static_cast<uint8_t>(slot)];
	for (uint8_t i = 0; i < s.Count; ++i)
	{
		if (s.Layers[i] == layer)
		{
			s.Layers[i] = s.Layers[--s.Count];
			s.Layers[s.Count] = nullptr;
			return;
		}
	}
}

void CameraManager::RemoveAllOwnerLayers(uint32_t ownerHandle)
{
	for (uint8_t s = 0; s < CameraSlotCount; ++s)
		RemoveLayer(static_cast<CameraSlot>(s), ownerHandle);
}

void CameraManager::Tick(SimFloat dt)
{
	for (uint8_t s = 0; s < CameraSlotCount; ++s)
	{
		Slot& slot = Slots[s];
		for (uint8_t l = 0; l < slot.Count; ++l)
		{
			CameraLayer* layer = slot.Layers[l];
			if (!layer->Active) continue;

			const SimFloat target = layer->BlendAlpha;
			const SimFloat step   = layer->TransitionSpeed * dt;
			if (layer->TransitionAlpha < target)
				layer->TransitionAlpha = std::min(layer->TransitionAlpha + step, target);
			else if (layer->TransitionAlpha > target)
				layer->TransitionAlpha = std::max(layer->TransitionAlpha - step, target);
		}
	}
}

bool CameraManager::HasActiveLayers() const
{
	for (uint8_t s = 0; s < CameraSlotCount; ++s)
		for (uint8_t l = 0; l < Slots[s].Count; ++l)
			if (Slots[s].Layers[l]->Active && Slots[s].Layers[l]->TransitionAlpha > 0.0f)
				return true;
	return false;
}

WorldCameraState CameraManager::Resolve() const
{
	WorldCameraState state;

	for (uint8_t s = 0; s < CameraSlotCount; ++s)
	{
		const Slot& slot = Slots[s];
		bool slotTouched = false;

		// Pipeline pass — StateMix layers modify state in push order.
		for (uint8_t l = 0; l < slot.Count; ++l)
		{
			const CameraLayer* layer = slot.Layers[l];
			if (!layer->Active || layer->TransitionAlpha <= 0.0f || !layer->StateFn) continue;
			layer->StateFn(const_cast<CameraLayer*>(layer), state);
			state.Valid  = true;
			slotTouched  = true;
		}

		// Blend pass — BlendMix layers contribute a weighted average.
		SimFloat totalWeight = 0.0f;
		WorldCameraState blended;

		for (uint8_t l = 0; l < slot.Count; ++l)
		{
			const CameraLayer* layer = slot.Layers[l];
			if (!layer->Active || layer->TransitionAlpha <= 0.0f || !layer->BlendFn) continue;

			const SimFloat w = layer->TransitionAlpha *
				(layer->BlendWeightFn ? layer->BlendWeightFn(const_cast<CameraLayer*>(layer)) : 1.0f);
			if (w <= 0.0f) continue;

			WorldCameraState contrib = state;
			layer->BlendFn(const_cast<CameraLayer*>(layer), w, contrib);

			blended.Position = blended.Position + contrib.Position * w;
			blended.Yaw     += contrib.Yaw   * w;
			blended.Pitch   += contrib.Pitch * w;
			blended.FOV     += contrib.FOV   * w;
			totalWeight     += w;
			slotTouched      = true;
		}

		if (totalWeight > 0.0f)
		{
			const SimFloat inv = 1.0f / totalWeight;
			state.Position   = blended.Position * inv;
			state.Yaw        = blended.Yaw      * inv;
			state.Pitch      = blended.Pitch    * inv;
			state.FOV        = blended.FOV      * inv;
			state.Valid      = true;
		}

		(void)slotTouched;
	}

	return state;
}

void CameraManager::DispatchOrientationDelta(float dyaw, float dpitch)
{
	// Highest-priority slot first (Cinematic → World), last-pushed layer first within slot.
	for (int8_t s = static_cast<int8_t>(CameraSlotCount) - 1; s >= 0; --s)
	{
		const Slot& slot = Slots[s];
		bool slotConsumed = false;

		for (int8_t l = static_cast<int8_t>(slot.Count) - 1; l >= 0; --l)
		{
			CameraLayer* layer = slot.Layers[l];
			if (!layer->Active || !layer->OrientationFn) continue;

			layer->OrientationFn(layer, dyaw, dpitch);

			if (layer->Consume == ConsumeScope::Stack) return;
			if (layer->Consume == ConsumeScope::Slot)  { slotConsumed = true; break; }
			// ConsumeScope::None — continue
		}

		if (slotConsumed) continue;
	}
}
