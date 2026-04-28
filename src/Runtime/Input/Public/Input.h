#pragma once
#include <atomic>
#include <cstring>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include "Types.h"

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
	Fire,
	ToggleCamera,
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
	{SDL_SCANCODE_V, Action::ToggleCamera},
};
inline constexpr int DefaultBindingCount = static_cast<int>(sizeof(DefaultBindings) / sizeof(DefaultBindings[0]));

// Mouse button indices (SDL convention)
inline constexpr uint8_t MOUSE_BUTTON_LEFT  = 1;
inline constexpr uint8_t MOUSE_BUTTON_RIGHT = 3;
inline constexpr uint8_t MAX_MOUSE_BUTTONS  = 5; // SDL supports 1-5

// ── Event queue entry ────────────────────────────────────────────────────────
struct InputData
{
	alignas(16) SDL_Scancode Key;
	uint16_t FrameUSOffset; // μs since last Swap — deterministic frame mapping via shared frameTimeUS constant
	uint8_t Pressed;        // 1 = down, 0 = up
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

	// ── Mouse state (continuous) ─────────────────────────────────────────
	alignas(16) SimFloat MouseDX[2]{};
	SimFloat MouseDY[2]{};
	uint8_t MouseButtons[2]{}; // bitmask per slot: bit N = button N+1

	// ── Slot management ──────────────────────────────────────────────────
	uint64_t SwapTimeUS = 0; // SDL_GetTicksNS() / 1000 at last Swap — microsecond base for FrameUSOffset
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

		// Push event. FrameUSOffset is μs since last Swap — deterministically maps to sim frame
		// on any peer using the shared constant: simFrame = First + FrameUSOffset / frameTimeUS
		// where frameTimeUS = 1,000,000 / FixedUpdateHz. Cap at uint16_t max (65535μs ≈ 65ms).
		uint16_t eventIdx = EventCount[slot];
		if (eventIdx < 1024)
		{
			const uint64_t deltaUS  = SDL_GetTicksNS() / 1000u - SwapTimeUS;
			const uint16_t offsetUS = static_cast<uint16_t>(deltaUS < 65535u ? deltaUS : 65535u);
			Events[slot][eventIdx] = {
				key,
				offsetUS,
				static_cast<uint8_t>(down ? 1 : 0)
			};
			EventCount[slot] = eventIdx + 1;
		}
	}

	void AddMouseDelta(SimFloat dx, SimFloat dy)
	{
		uint8_t slot  = WriteSlot.load(std::memory_order_relaxed);
		MouseDX[slot] += dx;
		MouseDY[slot] += dy;
	}

	void PushMouseButton(uint8_t button, bool down)
	{
		if (button == 0 || button > MAX_MOUSE_BUTTONS) return;
		uint8_t slot = WriteSlot.load(std::memory_order_relaxed);
		uint8_t bit  = static_cast<uint8_t>(1u << (button - 1));
		if (down) MouseButtons[slot] |= bit;
		else MouseButtons[slot]      &= static_cast<uint8_t>(~bit);
	}

	// ── Network-side (writer) ───────────────────────────────────────────
	// Bulk state injection for network-sourced input. NetThread deserializes
	// an InputFrame message and writes the full state in one shot.
	// Same write slot as Sentinel — on a server, NetThread is the sole writer.

	/// Replace the entire key state + mouse delta + mouse buttons for the current write slot.
	void InjectState(const uint8_t* keyData, SimFloat mouseDX, SimFloat mouseDY, uint8_t mouseButtons = 0)
	{
		uint8_t slot = WriteSlot.load(std::memory_order_relaxed);
		std::memcpy(KeyState[slot], keyData, 64);
		MouseDX[slot]      = mouseDX;
		MouseDY[slot]      = mouseDY;
		MouseButtons[slot] = mouseButtons;
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

		// Carry held state forward so keys/buttons stay pressed across frames
		std::memcpy(KeyState[newWrite], KeyState[ReadSlot], 64);
		MouseButtons[newWrite] = MouseButtons[ReadSlot];

		// Clear the new write slot's event queue and mouse delta
		EventCount[newWrite] = 0;
		MouseDX[newWrite]    = 0.0f;
		MouseDY[newWrite]    = 0.0f;

		ReadCursor = 0;
		SwapTimeUS = SDL_GetTicksNS() / 1000u;

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

	bool IsMouseButtonDown(uint8_t button) const
	{
		if (button == 0 || button > MAX_MOUSE_BUTTONS) return false;
		return (MouseButtons[ReadSlot] & (1u << (button - 1))) != 0;
	}

	bool IsActionDown(Action action) const
	{
		// Mouse-bound actions
		if (action == Action::Fire && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) return true;

		// Key-bound actions
		for (int i = 0; i < DefaultBindingCount; ++i)
		{
			if (DefaultBindings[i].Mapping == action && IsKeyDown(DefaultBindings[i].Key)) return true;
		}
		return false;
	}

	SimFloat GetMouseDX() const { return MouseDX[ReadSlot]; }
	SimFloat GetMouseDY() const { return MouseDY[ReadSlot]; }

	// Network-side snapshot — copy the ReadSlot key state and mouse button mask
	// for sending in an InputFrame. Call NetInput->Swap() before this to ensure
	// ReadSlot contains the full delta since the last net tick.
	void SnapshotKeyState(uint8_t* dst, size_t size) const
	{
		std::memcpy(dst, KeyState[ReadSlot], size < 64 ? size : 64);
	}

	uint8_t GetMouseButtonMask() const { return MouseButtons[ReadSlot]; }
#if TNX_DEV_METRICS
	uint64_t GetSwapPerfCount() const { return LastSwapPerfCount; }
	uint64_t GetCurrentSwapTime() const { return CurrentSwapTime; }
#endif
};
