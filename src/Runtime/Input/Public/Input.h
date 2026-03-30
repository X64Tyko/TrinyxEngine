#pragma once
#include <atomic>
#include <cstring>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

// ── Actions ──────────────────────────────────────────────────────────────────
// Named actions that game logic queries. Decoupled from physical keys.
enum class Action : uint8_t
{
	MoveForward,
	MoveBackward,
	MoveLeft,
	MoveRight,
	MoveUp,
	MoveDown,
	Count
};

struct ActionBinding
{
	SDL_Scancode Key;
	Action Mapping;
};

inline constexpr ActionBinding DefaultBindings[] = {
	{SDL_SCANCODE_W, Action::MoveForward},
	{SDL_SCANCODE_S, Action::MoveBackward},
	{SDL_SCANCODE_A, Action::MoveLeft},
	{SDL_SCANCODE_D, Action::MoveRight},
	{SDL_SCANCODE_SPACE, Action::MoveUp},
	{SDL_SCANCODE_LCTRL, Action::MoveDown},
};
inline constexpr int DefaultBindingCount = static_cast<int>(sizeof(DefaultBindings) / sizeof(DefaultBindings[0]));

// ── Event queue entry ────────────────────────────────────────────────────────
struct InputData
{
	alignas(16) SDL_Scancode Key;
	uint16_t FrameMSOffset;
	uint8_t Pressed; // 1 = down, 0 = up
};

// ── InputBuffer ──────────────────────────────────────────────────────────────
// Combines two layers in a single double-buffered structure:
//
//   1. Event queue  — ordered press/release events with timestamps.
//                     Use for discrete actions (jump, fire, combos, replay).
//
//   2. State snapshot — bitfield of currently-held keys + accumulated mouse delta.
//                       Use for continuous queries (WASD movement, mouse look).
//
// Sentinel (main thread) writes to WriteSlot. Brain calls Swap() once per
// logic frame, which atomically flips the slots: Brain gets a consistent
// snapshot, Sentinel starts filling a clean slot.

struct InputBuffer
{
	// ── Event queue (discrete) ───────────────────────────────────────────
	alignas(64) InputData Events[2][1024];
	alignas(64) uint16_t EventCount[2]{};

	// ── Key state bitfield (continuous) ──────────────────────────────────
	// 512 bits (64 bytes) per slot — covers all SDL scancodes
	alignas(64) uint8_t KeyState[2][64]{};

	// ── Mouse delta (continuous) ─────────────────────────────────────────
	alignas(16) float MouseDX[2]{};
	float MouseDY[2]{};

	// ── Slot management ──────────────────────────────────────────────────
	uint64_t SwapTime = 0;
	std::atomic<uint8_t> WriteSlot{0};
	uint8_t ReadSlot    = 1;
	uint16_t ReadCursor = 0;

#if TNX_DEV_METRICS
	// ── Latency tracking ─────────────────────────────────────────────────
	// Perf counter captured at the moment Brain calls Swap().
	// Carried through the frame header to VulkRender for input→photon measurement.
	uint64_t LastSwapPerfCount = 0;
	uint64_t CurrentSwapTime   = 0;
#endif

	// ── Sentinel-side (writer) ───────────────────────────────────────────

	void PushKey(SDL_Scancode key, bool down)
	{
		uint8_t slot = WriteSlot.load(std::memory_order_relaxed);

		// Update state bitfield
		uint32_t idx = static_cast<uint32_t>(key);
		if (idx < 512)
		{
			uint8_t byteIdx = static_cast<uint8_t>(idx >> 3);
			uint8_t bit     = static_cast<uint8_t>(1u << (idx & 7));
			if (down) KeyState[slot][byteIdx] |= bit;
			else KeyState[slot][byteIdx]      &= static_cast<uint8_t>(~bit);
		}

		// Push event
		uint16_t eventIdx = EventCount[slot];
		if (eventIdx < 1024)
		{
			Events[slot][eventIdx] = {
				key,
				static_cast<uint16_t>(SDL_GetTicks() - SwapTime),
				static_cast<uint8_t>(down ? 1 : 0)
			};
			EventCount[slot] = eventIdx + 1;
		}
	}

	void AddMouseDelta(float dx, float dy)
	{
		uint8_t slot  = WriteSlot.load(std::memory_order_relaxed);
		MouseDX[slot] += dx;
		MouseDY[slot] += dy;
	}

	// ── Network-side (writer) ───────────────────────────────────────────
	// Bulk state injection for network-sourced input. NetThread deserializes
	// an InputFrame message and writes the full state in one shot.
	// Same write slot as Sentinel — on a server, NetThread is the sole writer.

	/// Replace the entire key state + mouse delta for the current write slot.
	void InjectState(const uint8_t* keyData, float mouseDX, float mouseDY)
	{
		uint8_t slot = WriteSlot.load(std::memory_order_relaxed);
		std::memcpy(KeyState[slot], keyData, 64);
		MouseDX[slot] = mouseDX;
		MouseDY[slot] = mouseDY;
	}

	// ── Brain-side (reader) ──────────────────────────────────────────────

	// Call once at the top of each logic frame.
	void Swap()
	{
#if TNX_DEV_METRICS
		LastSwapPerfCount = CurrentSwapTime;
		CurrentSwapTime   = SDL_GetPerformanceCounter();
#endif
		ReadSlot         = WriteSlot.load(std::memory_order_acquire);
		uint8_t newWrite = ReadSlot ^ 1;

		// Carry held-key state forward so keys stay pressed across frames
		std::memcpy(KeyState[newWrite], KeyState[ReadSlot], 64);

		// Clear the new write slot's event queue and mouse delta
		EventCount[newWrite] = 0;
		MouseDX[newWrite]    = 0.0f;
		MouseDY[newWrite]    = 0.0f;

		ReadCursor = 0;
		SwapTime   = SDL_GetTicks();

		WriteSlot.store(newWrite, std::memory_order_release);
	}

	// Read next event from the queue (returns Key==0 when exhausted)
	InputData ReadEvent()
	{
		if (ReadCursor >= EventCount[ReadSlot]) return {};
		return Events[ReadSlot][ReadCursor++];
	}

	uint16_t GetEventCount() const { return EventCount[ReadSlot]; }

	// Continuous state queries
	bool IsKeyDown(SDL_Scancode key) const
	{
		uint32_t idx = static_cast<uint32_t>(key);
		if (idx >= 512) return false;
		return (KeyState[ReadSlot][idx >> 3] & (1u << (idx & 7))) != 0;
	}

	bool IsActionDown(Action action) const
	{
		for (int i = 0; i < DefaultBindingCount; ++i)
		{
			if (DefaultBindings[i].Mapping == action && IsKeyDown(DefaultBindings[i].Key)) return true;
		}
		return false;
	}

	float GetMouseDX() const { return MouseDX[ReadSlot]; }
	float GetMouseDY() const { return MouseDY[ReadSlot]; }
#if TNX_DEV_METRICS
	uint64_t GetSwapPerfCount() const { return LastSwapPerfCount; }
	uint64_t GetCurrentSwapTime() const { return CurrentSwapTime; }
#endif
};
