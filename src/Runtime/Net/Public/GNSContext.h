#pragma once

// ---------------------------------------------------------------------------
// GNSContext — thin RAII wrapper around GameNetworkingSockets library lifecycle.
//
// Call Initialize() once at engine startup (before any networking),
// Shutdown() at teardown. The GNS service thread and internal state are
// managed by the library itself; this wrapper just gates init/kill and
// exposes the ISteamNetworkingSockets interface pointer.
// ---------------------------------------------------------------------------

class ISteamNetworkingSockets;
struct SteamNetConnectionStatusChangedCallback_t;

/// Function pointer type for connection status change notifications.
/// Passed into Initialize so GNS can route callbacks to the engine.
using GNSStatusChangedFn = void(*)(SteamNetConnectionStatusChangedCallback_t*);

class GNSContext
{
public:
	GNSContext() = default;
	~GNSContext();

	/// Initialize the GNS library. Must be called before any socket operations.
	/// statusFn is invoked by GNS on its service thread when connection state changes —
	/// the callback should be thread-safe (typically queues events for the NetThread).
	bool Initialize(GNSStatusChangedFn statusFn = nullptr);

	/// Tear down the GNS library. Safe to call multiple times or if never initialized.
	void Shutdown();

	/// Returns the GNS interface pointer. nullptr if not initialized.
	ISteamNetworkingSockets* GetInterface() const { return SocketsInterface; }

	bool IsInitialized() const { return bInitialized; }

private:
	ISteamNetworkingSockets* SocketsInterface = nullptr;
	bool bInitialized                         = false;
};