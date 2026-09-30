#pragma once
#if !defined(TNX_ENABLE_EDITOR)
#error "TnxPalette.h requires TNX_ENABLE_EDITOR"
#endif

#include <functional>

namespace TnxPalette
{

struct Command
{
	const char* title;
	const char* subtitle = nullptr;
	const char* icon     = nullptr;
	const char* keybind  = nullptr;
	std::function<void()> exec;
};

/// Add a command to the global registry. Call during initialization.
void Register(Command c);

/// Remove all registered commands (call before re-registering on re-init).
void Clear();

/// Request the palette be opened on the next frame.
void Open();

/// Draw the palette overlay. Call once per frame in BuildFrame(), before EndFrame.
void Draw();

} // namespace TnxPalette
