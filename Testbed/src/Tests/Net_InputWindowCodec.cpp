#ifdef TNX_ENABLE_NETWORK

#include <cstring>

#include "TestFramework.h"
#include "InputWindowCodec.h"

namespace
{
NetInputFrame MakeFrame(uint32_t frame)
{
	NetInputFrame f{};
	f.Frame = frame;
	return f;
}

bool SameFrame(const NetInputFrame& a, const NetInputFrame& b)
{
	return a.Frame == b.Frame
		   && std::memcmp(&a.State, &b.State, sizeof(InputSnapshot)) == 0
		   && a.EventCount == b.EventCount
		   && std::memcmp(a.Events, b.Events, a.EventCount * sizeof(NetInputEvent)) == 0;
}
} // namespace

// Every section kind survives a round trip: persistent state that changes, persistent state that
// is inherited, per-frame values that reset, and discrete events.
TEST(Net_InputWindowCodecRoundTrip)
{
	NetInputFrame frames[4]     = { MakeFrame(10), MakeFrame(11), MakeFrame(12), MakeFrame(13) };
	frames[0].State.KeyState[3] = 0x10;
	frames[0].State.ViewYaw     = SimFloat(0.5f);

	frames[1].State           = frames[0].State; // held state unchanged
	frames[1].State.MouseDX   = SimFloat(2.0f);  // per-frame
	frames[1].State.ViewPitch = SimFloat(-0.25f);
	frames[1].EventCount      = 2;
	frames[1].Events[0]       = { 7, 100, 1, 0 };
	frames[1].Events[1]       = { 7, 900, 0, 0 };

	frames[2].State              = frames[1].State;
	frames[2].State.MouseDX      = SimFloat(0.0f); // resets, not inherited
	frames[2].State.MouseButtons = 1;
	frames[2].State.KeyState[3]  = 0;

	frames[3].State         = frames[2].State;
	frames[3].State.MouseDY = SimFloat(-3.0f);

	uint8_t buffer[InputWindowCodec::MaxPayloadBytes];
	InputWindowWriter writer(buffer, frames[0]);
	for (int i = 1; i < 4; ++i)
		writer.Append(frames[i]);
	const size_t size = writer.Finish();
	ASSERT(writer.GetLastFrame() == 13);

	InputWindowPacket decoded{};
	ASSERT(InputWindowCodec::Decode(buffer, size, decoded));
	ASSERT_EQ(decoded.FirstFrame, 10u);
	ASSERT_EQ(decoded.FrameCount, 4u);
	for (int i = 0; i < 4; ++i)
		ASSERT(SameFrame(decoded.Frames[i], frames[i]));

	// Unchanged held state costs only the frame number and flags byte.
	InputWindowWriter idle(buffer, frames[3]);
	NetInputFrame same = frames[3];
	same.Frame         = 14;
	same.State.MouseDY = SimFloat(0.0f);
	idle.Append(same);
	ASSERT_EQ(idle.Finish(), sizeof(InputDeltaPacketHeader) + sizeof(NetInputFrame) + sizeof(uint32_t) + 1);
}

// Any truncation is rejected rather than decoded into partial input.
TEST(Net_InputWindowCodecRejectsTruncation)
{
	NetInputFrame base     = MakeFrame(1);
	NetInputFrame next     = MakeFrame(2);
	next.State.KeyState[0] = 1;
	next.State.ViewYaw     = SimFloat(1.0f);

	uint8_t buffer[InputWindowCodec::MaxPayloadBytes];
	InputWindowWriter writer(buffer, base);
	writer.Append(next);
	const size_t size = writer.Finish();

	InputWindowPacket decoded{};
	for (size_t cut = 0; cut < size; ++cut)
		ASSERT(!InputWindowCodec::Decode(buffer, cut, decoded));
	ASSERT(InputWindowCodec::Decode(buffer, size, decoded));
}

#endif // TNX_ENABLE_NETWORK
