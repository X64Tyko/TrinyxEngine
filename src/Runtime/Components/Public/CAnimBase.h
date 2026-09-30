#pragma once
#include "AssetRegistry.h"
#include "ComponentView.h"
#include "SchemaReflector.h"
#include "NetDelta.h"
#include "SimFloat.h"

/// Temporal (rollback + net-replicated) base animation layer.
///
/// Mode is discriminated by BaseAnimID: >0 = explicit clip (timestamp advanced by wide sweep),
/// 0 = blendspace mode (GPU evaluates at BlendCoordX/Y). Cross-fade uses FadeAnimID/FadeAlpha;
/// AnimConstruct drives FadeAlpha and clears it on completion.
/// StateNodeID preserves state machine position for rollback re-derivation.

template <FieldWidth WIDTH = FieldWidth::Scalar>
struct CAnimBase : ComponentView<CAnimBase, WIDTH>
{
	TNX_TEMPORAL_FIELDS(CAnimBase, Render,
		BaseAnimID, BlendspaceID,
		BaseTimestamp, BlendCoordX, BlendCoordY,
		FadeAnimID, FadeTimestamp, FadeAlpha,
		StateNodeID, Flags)

	UIntProxy<WIDTH> BaseAnimID{};     // explicit anim (0 = blendspace mode)
	UIntProxy<WIDTH> BlendspaceID{};   // blendspace asset (used when BaseAnimID == 0)
	FloatProxy<WIDTH> BaseTimestamp{}; // playback time — advanced by wide sweep
	FloatProxy<WIDTH> BlendCoordX{};   // blendspace X input (written by AnimConstruct)
	FloatProxy<WIDTH> BlendCoordY{};   // blendspace Y input
	UIntProxy<WIDTH> FadeAnimID{};     // outgoing clip during cross-fade (0 = none)
	FloatProxy<WIDTH> FadeTimestamp{}; // fade source playback time — advanced by wide sweep
	FloatProxy<WIDTH> FadeAlpha{};     // [0..1]: 0 = base only, 1 = fully on fade
	UIntProxy<WIDTH> StateNodeID{};    // AnimConstruct state machine node (for rollback)
	UIntProxy<WIDTH> Flags{};          // bit 0 = base loop, bit 1 = base rootMotion
									   // bit 2 = fade loop, bit 3 = fade rootMotion

	SimFloat GetBaseTimestamp() const
		requires(WIDTH == FieldWidth::Scalar)
	{
		return BaseTimestamp;
	}
	SimFloat GetFadeTimestamp() const
		requires(WIDTH == FieldWidth::Scalar)
	{
		return FadeTimestamp;
	}
	SimFloat GetFadeAlpha() const
		requires(WIDTH == FieldWidth::Scalar)
	{
		return FadeAlpha;
	}
	SimFloat GetBlendCoordX() const
		requires(WIDTH == FieldWidth::Scalar)
	{
		return BlendCoordX;
	}
	SimFloat GetBlendCoordY() const
		requires(WIDTH == FieldWidth::Scalar)
	{
		return BlendCoordY;
	}

	bool IsBlendspaceMode() const
		requires(WIDTH == FieldWidth::Scalar)
	{
		return BaseAnimID.Value() == 0 && BlendspaceID.Value() != 0;
	}

	bool HasFade() const
		requires(WIDTH == FieldWidth::Scalar)
	{
		return FadeAnimID.Value() != 0;
	}

	bool GetBaseLoop() const
		requires(WIDTH == FieldWidth::Scalar)
	{
		return (Flags.Value() >> 0) & 1u;
	}
	bool GetBaseRootMotion() const
		requires(WIDTH == FieldWidth::Scalar)
	{
		return (Flags.Value() >> 1) & 1u;
	}
	bool GetFadeLoop() const
		requires(WIDTH == FieldWidth::Scalar)
	{
		return (Flags.Value() >> 2) & 1u;
	}
	bool GetFadeRootMotion() const
		requires(WIDTH == FieldWidth::Scalar)
	{
		return (Flags.Value() >> 3) & 1u;
	}

	/// Queues an asset checkout; BaseAnimID is written when the asset arrives via OnLoaded.
	void SetAnim(TnxName name, SimFloat startTime = {}, bool loops = true, bool rootMotion = false)
		requires(WIDTH == FieldWidth::Scalar)
	{
		OnAnimLoad.Bind<CAnimBase, &CAnimBase::SetBaseAnimID>(this);
		OnAnimEvict.Bind<CAnimBase, &CAnimBase::ResetBaseAnimID>(this);
		AssetRegistry::RegisterPendingCheckout<AssetType::Animation>(name, OnAnimLoad, OnAnimEvict);
		BlendspaceID  = 0u;
		BaseTimestamp = startTime;
		uint32_t f    = Flags.Value();
		f             = loops ? (f | (1u << 0)) : (f & ~(1u << 0));
		f             = rootMotion ? (f | (1u << 1)) : (f & ~(1u << 1));
		Flags         = f;
	}

	/// Set an already-resolved animation slot directly (bypasses asset checkout).
	void SetExplicitAnim(uint32_t animID, SimFloat startTime, bool loops, bool rootMotion = false)
		requires(WIDTH == FieldWidth::Scalar)
	{
		BaseAnimID    = animID;
		BlendspaceID  = 0u;
		BaseTimestamp = startTime;
		uint32_t f    = Flags.Value();
		f             = loops ? (f | (1u << 0)) : (f & ~(1u << 0));
		f             = rootMotion ? (f | (1u << 1)) : (f & ~(1u << 1));
		Flags         = f;
	}

	/// Switch to blendspace mode; the GPU evaluates the pose at (coordX, coordY) each tick.
	void SetBlendspace(uint32_t bsID, SimFloat coordX, SimFloat coordY)
		requires(WIDTH == FieldWidth::Scalar)
	{
		BaseAnimID   = 0u;
		BlendspaceID = bsID;
		BlendCoordX  = coordX;
		BlendCoordY  = coordY;
	}

	void SetBaseTimestamp(SimFloat t)
		requires(WIDTH == FieldWidth::Scalar)
	{
		BaseTimestamp = t;
	}

	/// Begin a cross-fade: stores the outgoing clip as the fade source; AnimConstruct drives FadeAlpha to 1.
	void BeginFade(uint32_t fromAnimID, SimFloat fromTimestamp, bool fromLoops, bool fromRootMotion = false)
		requires(WIDTH == FieldWidth::Scalar)
	{
		FadeAnimID    = fromAnimID;
		FadeTimestamp = fromTimestamp;
		FadeAlpha     = 0.f;
		uint32_t f    = Flags.Value();
		f             = fromLoops ? (f | (1u << 2)) : (f & ~(1u << 2));
		f             = fromRootMotion ? (f | (1u << 3)) : (f & ~(1u << 3));
		Flags         = f;
	}

	void SetFadeAlpha(SimFloat alpha)
		requires(WIDTH == FieldWidth::Scalar)
	{
		FadeAlpha = alpha;
	}

	void ClearFade()
		requires(WIDTH == FieldWidth::Scalar)
	{
		FadeAnimID    = 0u;
		FadeTimestamp = SimFloat(0.f);
		FadeAlpha     = SimFloat(0.f);
		Flags         = Flags.Value() & ~(3u << 2);
	}

	void Clear()
		requires(WIDTH == FieldWidth::Scalar)
	{
		BaseAnimID    = 0u;
		BlendspaceID  = 0u;
		BaseTimestamp = SimFloat(0.f);
		BlendCoordX   = SimFloat(0.f);
		BlendCoordY   = SimFloat(0.f);
		FadeAnimID    = 0u;
		FadeTimestamp = SimFloat(0.f);
		FadeAlpha     = SimFloat(0.f);
		StateNodeID   = 0u;
		Flags         = 0u;
	}

private:
	AssetLoad OnAnimLoad;
	AssetEvict OnAnimEvict;

	void SetBaseAnimID(uint32_t id)
		requires(WIDTH == FieldWidth::Scalar)
	{
		BaseAnimID = id;
		OnAnimLoad.Reset();
	}
	void ResetBaseAnimID()
		requires(WIDTH == FieldWidth::Scalar)
	{
		BaseAnimID = 0u;
		OnAnimEvict.Reset();
	}
};

TNX_REGISTER_COMPONENT(CAnimBase)
TNX_NET_TEMPORAL(CAnimBase)
