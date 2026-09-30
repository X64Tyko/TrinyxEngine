#pragma once
#include <cstdint>

// ---------------------------------------------------------------------------
// SimFrame — mode-switched net time reference.
//
// Under TNX_DETERMINISM:  SimFrame wraps a uint32_t logic frame counter.
//   All peers run locked-step at 512Hz; frame numbers are the canonical
//   shared reference. Operations have exact integer semantics.
//
// Without TNX_DETERMINISM: SimFrame wraps simulation-elapsed milliseconds
//   derived from LogicThreadBase::SimulationTime (= FrameNumber × fixedStepTime).
//   Peers can run at variable rates or carry different local frame offsets;
//   sim-elapsed ms becomes the shared reference once time origins are aligned
//   via ClockSync. All arithmetic is wrapping (unsigned overflow is intentional
//   and well-defined). This is NOT wall clock time.
//
// Both representations are 4 bytes — wire layout in PacketHeader::FrameNumber
// is unchanged regardless of build mode. The tag type prevents accidental
// mixing of frame counts and timestamps at compile time.
//
// Usage:
//   SimFrame now(CurrentFrameOrMs());
//   SimFrame ahead = now + 16;
//   int32_t delta  = ahead - now;   // signed wrapping delta
//
// Not yet wired into PacketHeader::FrameNumber — that requires coordinated
// changes across AuthoritySim, OwnerSim, and ClockSync.
// ---------------------------------------------------------------------------

struct FrameNumberTag
{
}; // TNX_DETERMINISM=ON  — counts 512Hz logic frames
struct TimestampMsTag
{
}; // TNX_DETERMINISM=OFF — simulation-elapsed ms (LogicThreadBase::SimulationTime * 1000)

template <typename TTag>
struct SimFrameImpl
{
	uint32_t Value = 0;

	SimFrameImpl() = default;
	constexpr explicit SimFrameImpl(uint32_t v)
		: Value(v)
	{
	}

	// Raw accessor for wire serialization — prefer named helpers over this in logic code
	uint32_t Raw() const { return Value; }

	// Prefix / postfix increment and decrement
	SimFrameImpl& operator++()
	{
		++Value;
		return *this;
	}
	SimFrameImpl& operator--()
	{
		--Value;
		return *this;
	}
	SimFrameImpl operator++(int) { return SimFrameImpl(Value++); }
	SimFrameImpl operator--(int) { return SimFrameImpl(Value--); }

	// Offset arithmetic (wrapping — intentional for both frame counts and timestamps)
	SimFrameImpl operator+(uint32_t n) const { return SimFrameImpl(Value + n); }
	SimFrameImpl operator-(uint32_t n) const { return SimFrameImpl(Value - n); }
	SimFrameImpl& operator+=(uint32_t n)
	{
		Value += n;
		return *this;
	}
	SimFrameImpl& operator-=(uint32_t n)
	{
		Value -= n;
		return *this;
	}

	// Signed wrapping delta: positive = A is ahead of B, negative = A is behind B.
	// Correct for values within half the uint32_t range of each other (~2B frames / ~24 days).
	int32_t operator-(SimFrameImpl other) const
	{
		return static_cast<int32_t>(Value - other.Value);
	}

	// True if this is strictly ahead of other within the half-range window
	bool IsAheadOf(SimFrameImpl other) const { return (*this - other) > 0; }

	// Comparison (wrapping-safe unsigned ordering — use IsAheadOf for sequence logic)
	friend bool operator==(const SimFrameImpl&, const SimFrameImpl&)  = default;
	friend auto operator<=>(const SimFrameImpl&, const SimFrameImpl&) = default;
};

static_assert(sizeof(SimFrameImpl<FrameNumberTag>) == 4, "SimFrame must be 4 bytes");
static_assert(sizeof(SimFrameImpl<TimestampMsTag>) == 4, "SimFrame must be 4 bytes");

// Simulation time reference — frame counter under determinism, ms timestamp otherwise.
#ifdef TNX_DETERMINISM
using SimFrame = SimFrameImpl<FrameNumberTag>;
#else
using SimFrame = SimFrameImpl<TimestampMsTag>;
#endif
