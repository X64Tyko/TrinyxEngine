#pragma once
#include <atomic>
#include <SDL3/SDL_scancode.h>

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
	{ SDL_SCANCODE_W, Action::MoveForward },
	{ SDL_SCANCODE_S, Action::MoveBackward },
	{ SDL_SCANCODE_A, Action::MoveLeft },
	{ SDL_SCANCODE_D, Action::MoveRight },
	{ SDL_SCANCODE_SPACE, Action::MoveUp },
	{ SDL_SCANCODE_LCTRL, Action::MoveDown },
	{ SDL_SCANCODE_V, Action::ToggleCamera },
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

	// ── View angles (absolute, persistent) ───────────────────────────────
	// The Owner's control rotation, in radians. Absolute rather than a delta so that a missing or
	// extrapolated input frame repeats the last known facing instead of dropping rotation, and every
	// peer (and every resimulated frame) derives identical facing from the same frame's input.
	SimFloat ViewYaw[2]{};
	SimFloat ViewPitch[2]{};

	// ── Slot management ──────────────────────────────────────────────────
	uint64_t SwapTimeUS = 0; // SDL_GetTicksNS() / 1000 at last Swap — microsecond base for FrameUSOffset
	std::atomic<uint8_t> WriteSlot{ 0 };
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

	void PushKey(SDL_Scancode key, bool down);

	void AddMouseDelta(SimFloat dx, SimFloat dy);

	void PushMouseButton(uint8_t button, bool down);

	// ── Network-side (writer) ───────────────────────────────────────────
	// Bulk state injection for network-sourced input. NetThread deserializes
	// an InputFrame message and writes the full state in one shot.
	// Same write slot as Sentinel — on a server, NetThread is the sole writer.

	/// Replace the entire key state + mouse delta + mouse buttons for the current write slot.
	void InjectState(const uint8_t* keyData, SimFloat mouseDX, SimFloat mouseDY, uint8_t mouseButtons = 0);

	/// Set the absolute view angles for the current write slot (carried forward on Swap).
	/// Written by the local Owner's look integration, or injected from the network input log.
	void SetViewAngles(SimFloat yaw, SimFloat pitch);

	// ── Brain-side (reader) ──────────────────────────────────────────────

	// Call once at the top of each logic frame.
	void Swap();

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

	bool IsActionDown(Action action) const;

	SimFloat GetMouseDX() const { return MouseDX[ReadSlot]; }
	SimFloat GetMouseDY() const { return MouseDY[ReadSlot]; }

	/// Absolute view angles for this frame — the only input gameplay should derive facing from.
	SimFloat GetViewYaw() const { return ViewYaw[ReadSlot]; }
	SimFloat GetViewPitch() const { return ViewPitch[ReadSlot]; }

	// Network-side snapshot — copy the ReadSlot key state and mouse button mask
	// for sending in an InputFrame. Call NetInput->Swap() before this to ensure
	// ReadSlot contains the full delta since the last net tick.
	void SnapshotKeyState(uint8_t* dst, size_t size) const;

	uint8_t GetMouseButtonMask() const { return MouseButtons[ReadSlot]; }
#if TNX_DEV_METRICS
	uint64_t GetSwapPerfCount() const { return LastSwapPerfCount; }
	uint64_t GetCurrentSwapTime() const { return CurrentSwapTime; }
#endif
};
