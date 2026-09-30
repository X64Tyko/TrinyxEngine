#include "TestFramework.h"
#include "Input.h"

#ifdef TNX_ENABLE_NETWORK
#include "PlayerInputLog.h"
#endif

// View angles are absolute, persistent input: they must survive buffer swaps unchanged until the
// Owner writes new ones, so every sim frame carries the current facing.
TEST(Input_ViewAnglesCarryAcrossSwap)
{
	InputBuffer buf;
	buf.SetViewAngles(SimFloat(1.25f), SimFloat(-0.5f));
	buf.Swap();
	ASSERT(buf.GetViewYaw() == SimFloat(1.25f));
	ASSERT(buf.GetViewPitch() == SimFloat(-0.5f));

	// No new angles written this frame — the previous facing carries forward.
	buf.Swap();
	ASSERT(buf.GetViewYaw() == SimFloat(1.25f));
	ASSERT(buf.GetViewPitch() == SimFloat(-0.5f));

	// Mouse deltas, by contrast, are per-frame and reset on swap.
	buf.AddMouseDelta(SimFloat(3.0f), SimFloat(4.0f));
	buf.Swap();
	ASSERT(buf.GetMouseDX() == SimFloat(3.0f));
	buf.Swap();
	ASSERT(buf.GetMouseDX() == SimFloat(0.0f));
}

#ifdef TNX_ENABLE_NETWORK
// When the Authority has to extrapolate a missing frame, it must repeat the last known absolute
// facing (not drop rotation like a zeroed delta would), and a late real frame with a different
// facing must be flagged so rollback can resimulate it.
TEST(Input_PredictedFrameKeepsViewAngles)
{
	PlayerInputLog log;
	log.Initialize(32);

	InputWindowPacket pkt{};
	pkt.FirstFrame                = 100;
	pkt.FrameCount                = 1;
	pkt.Frames[0].Frame           = 100;
	pkt.Frames[0].State.ViewYaw   = SimFloat(1.25f);
	pkt.Frames[0].State.ViewPitch = SimFloat(-0.5f);
	pkt.Frames[0].State.MouseDX   = SimFloat(3.0f);
	log.Store(pkt);

	InputConsumeResult hit = log.ConsumeFrame(100);
	ASSERT(hit);
	ASSERT(hit.Reason == InputMissReason::Hit);
	ASSERT(hit.Entry->State.ViewYaw == SimFloat(1.25f));

	// Frame 101 hasn't arrived: extrapolated from frame 100.
	InputConsumeResult predicted = log.ConsumeFrame(101);
	ASSERT(predicted);
	ASSERT(predicted.Reason == InputMissReason::NotYetReceived);
	ASSERT(predicted.Entry->bPredicted);
	ASSERT(predicted.Entry->State.ViewYaw == SimFloat(1.25f));
	ASSERT(predicted.Entry->State.ViewPitch == SimFloat(-0.5f));
	ASSERT(predicted.Entry->State.MouseDX == SimFloat(0.0f));

	// The real frame 101 arrives late with a different facing — the log must mark it dirty.
	ASSERT(!log.IsDirty());
	InputWindowPacket late{};
	late.FirstFrame                = 101;
	late.FrameCount                = 1;
	late.Frames[0].Frame           = 101;
	late.Frames[0].State.ViewYaw   = SimFloat(1.30f);
	late.Frames[0].State.ViewPitch = SimFloat(-0.5f);
	log.Store(late);
	ASSERT(log.IsDirty());
}
#endif
