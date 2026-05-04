#pragma once

// ---------------------------------------------------------------------------
// GNSContext — thin RAII wrapper around GameNetworkingSockets library lifecycle.
//
// Call Initialize() once at engine startup (before any networking),
// Shutdown() at teardown. GNS runs in manual poll mode — no background service
// thread is spawned. Call Poll() from your NetThread once per tick to drive all
// GNS I/O inline with deterministic timing.
// ---------------------------------------------------------------------------

class ISteamNetworkingSockets;
struct SteamNetConnectionStatusChangedCallback_t;

/// Function pointer type for connection status change notifications.
/// Passed into Initialize so GNS can route callbacks to the engine.
using GNSStatusChangedFn = void(*)(SteamNetConnectionStatusChangedCallback_t*);

struct SocketHandle
{
	bool bIsInitialized              = false;
	ISteamNetworkingSockets* Sockets = nullptr;

	ISteamNetworkingSockets* operator->() const { return bIsInitialized ? Sockets : nullptr; }
	bool operator==(const SocketHandle& rhs) const { return bIsInitialized == rhs.bIsInitialized; }

	explicit operator bool() const { return bIsInitialized && Sockets != nullptr; }
	
	static SocketHandle& Invalid()
	{
		static SocketHandle handle;
		return handle;
	}
};

class GNSContext
{
public:
	GNSContext() = default;
	~GNSContext();

	/// Initialize the GNS library. Must be called before any socket operations.
	/// GNS runs in manual poll mode — the background service thread is never spawned.
	/// Call Poll() from your NetThread to drive all GNS I/O inline.
	/// statusFn is invoked during Poll() when connection state changes.
	bool Initialize(GNSStatusChangedFn statusFn = nullptr);

	/// Tear down the GNS library. Safe to call multiple times or if never initialized.
	void Shutdown();

	/// Drive GNS I/O inline. Call once per NetThread tick before RunCallbacks().
	/// msWait=0: non-blocking (process pending I/O and return immediately).
	void Poll(int msWait = 0);

	/// Returns the GNS interface pointer. nullptr if not initialized.
	const SocketHandle& GetInterface() const { return SocketsHandle; }

	bool IsInitialized() const { return bInitialized; }

private:
	SocketHandle SocketsHandle;
	ISteamNetworkingSockets* SocketsInterface = nullptr;
	bool bInitialized = false;
	bool bOwnsGNS     = false; // true only if this instance called GameNetworkingSockets_Init
};