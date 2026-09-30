#include "Input.h"

#include <SDL3/SDL_timer.h>

#include <cstring>

void InputBuffer::PushKey(SDL_Scancode key, bool down)
{
	uint8_t slot = WriteSlot.load(std::memory_order_relaxed);

	// Update state bitfield
	uint32_t idx = static_cast<uint32_t>(key);
	if (idx < 512)
	{
		uint8_t byteIdx = static_cast<uint8_t>(idx >> 3);
		uint8_t bit     = static_cast<uint8_t>(1u << (idx & 7));
		if (down)
			KeyState[slot][byteIdx] |= bit;
		else
			KeyState[slot][byteIdx] &= static_cast<uint8_t>(~bit);
	}

	// Push event. FrameUSOffset is μs since last Swap — deterministically maps to sim frame
	// on any peer using the shared constant: simFrame = First + FrameUSOffset / frameTimeUS
	// where frameTimeUS = 1,000,000 / FixedUpdateHz. Cap at uint16_t max (65535μs ≈ 65ms).
	uint16_t eventIdx = EventCount[slot];
	if (eventIdx < 1024)
	{
		const uint64_t deltaUS  = SDL_GetTicksNS() / 1000u - SwapTimeUS;
		const uint16_t offsetUS = static_cast<uint16_t>(deltaUS < 65535u ? deltaUS : 65535u);
		Events[slot][eventIdx]  = {
			key,
			offsetUS,
			static_cast<uint8_t>(down ? 1 : 0)
		};
		EventCount[slot] = eventIdx + 1;
	}
}

void InputBuffer::AddMouseDelta(SimFloat dx, SimFloat dy)
{
	uint8_t slot = WriteSlot.load(std::memory_order_relaxed);
	MouseDX[slot] += dx;
	MouseDY[slot] += dy;
}

void InputBuffer::PushMouseButton(uint8_t button, bool down)
{
	if (button == 0 || button > MAX_MOUSE_BUTTONS) return;
	uint8_t slot = WriteSlot.load(std::memory_order_relaxed);
	uint8_t bit  = static_cast<uint8_t>(1u << (button - 1));
	if (down)
		MouseButtons[slot] |= bit;
	else
		MouseButtons[slot] &= static_cast<uint8_t>(~bit);
}

void InputBuffer::InjectState(const uint8_t* keyData, SimFloat mouseDX, SimFloat mouseDY, uint8_t mouseButtons)
{
	uint8_t slot = WriteSlot.load(std::memory_order_relaxed);
	std::memcpy(KeyState[slot], keyData, 64);
	MouseDX[slot]      = mouseDX;
	MouseDY[slot]      = mouseDY;
	MouseButtons[slot] = mouseButtons;
}

void InputBuffer::Swap()
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
	ViewYaw[newWrite]      = ViewYaw[ReadSlot];
	ViewPitch[newWrite]    = ViewPitch[ReadSlot];

	// Clear the new write slot's event queue and mouse delta
	EventCount[newWrite] = 0;
	MouseDX[newWrite]    = 0.0f;
	MouseDY[newWrite]    = 0.0f;

	ReadCursor = 0;
	SwapTimeUS = SDL_GetTicksNS() / 1000u;

	WriteSlot.store(newWrite, std::memory_order_release);
}

void InputBuffer::SnapshotKeyState(uint8_t* dst, size_t size) const
{
	std::memcpy(dst, KeyState[ReadSlot], size < 64 ? size : 64);
}

bool InputBuffer::IsActionDown(Action action) const
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

void InputBuffer::SetViewAngles(SimFloat yaw, SimFloat pitch)
{
	const uint8_t slot = WriteSlot.load(std::memory_order_relaxed);
	ViewYaw[slot]      = yaw;
	ViewPitch[slot]    = pitch;
}
