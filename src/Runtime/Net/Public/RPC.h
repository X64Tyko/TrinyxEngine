#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

#include "NetTypes.h" // RPCHeader, RPCContext, NetMessageType::SoulRPC

// ---------------------------------------------------------------------------
// RPC — shared infrastructure for Soul, Construct, and Entity remote calls.
//
// RPCMethodID<T>() assigns a unique uint16_t to each params type on first call.
// IDs are stable within a process run; baking to a file (same pattern as entity
// ClassIDs) will make them stable across runs — tracked as future work.
//
// Engine band [0, 127] is claimed by engine-defined RPCs in Soul.cpp.
// User band [128, 65535] is available for game-defined RPCs.
// The ordering between static initialisers across TUs is not guaranteed today —
// engine RPCs register first in practice (linked earlier), but baking will
// enforce this correctly.
//
// Usage in class body (.h):
//   TNX_SERVER(Server_Fire, FireParams);   // client calls this → sends to server
//   TNX_CLIENT(Client_HitConfirm, HitConfirmParams); // server calls this → sends to client
//
// Usage in impl file (.cpp) — macro opens the handler body, user closes it:
//   TNX_IMPL_SERVER(ArenaShooterSoul, Server_Fire, FireParams)
//   {
//       // server-side handler — ctx and params are available
//   }
//
// TParams must be trivially copyable — it is a wire format.
// The static_assert inside TNX_IMPL_SERVER/CLIENT enforces this at compile time.
//
// TNX_IMPL_* requires ReflectionRegistry to be included in the same .cpp
// before the macro expands. Soul.cpp and game Soul subclass .cpp files do this
// via their own include lists.
// ---------------------------------------------------------------------------

// One global counter — shared across all RPC targets (Soul, Construct, Entity).
// Function-local static guarantees thread-safe init (C++11). fetch_add is
// safe if registration races at static init, which doesn't happen in practice.
inline std::atomic<uint16_t> g_RPCCounter{ 0 };

// Free function template — ID assigned once on first call per type T.
// T does not need to know about this; it stays a plain POD struct.
template<typename T>
uint16_t RPCMethodID()
{
	static const uint16_t id = g_RPCCounter.fetch_add(1, std::memory_order_relaxed);
	return id;
}

// Handler function pointer type stored per MethodID in ReflectionRegistry.
class Soul;
using SoulRPCHandler = void(*)(Soul*, const RPCContext&, const uint8_t*);

// ---------------------------------------------------------------------------
// Class-body declaration macros
//
// TNX_SERVER(Name, TParams) — declares a server-targeted RPC on this Soul.
//   The first overload is the CLIENT THUNK: user or engine calls Name(params)
//   on the client; the macro-generated body serialises and sends to server.
//   The second overload is the SERVER HANDLER: user writes the body in .cpp
//   using TNX_IMPL_SERVER.
//
// TNX_CLIENT(Name, TParams) — declares a client-targeted RPC (symmetric).
//   First overload: SERVER THUNK (sends back to the originating client).
//   Second overload: CLIENT HANDLER.
// ---------------------------------------------------------------------------
#define TNX_SERVER(Name, TParams)                          \
	bool Name(const TParams& params);                      \
	void Name(const RPCContext& ctx, const TParams& params)

#define TNX_CLIENT(Name, TParams)                          \
	bool Name(const TParams& params);                      \
	void Name(const RPCContext& ctx, const TParams& params)

// ---------------------------------------------------------------------------
// Implementation macros (in .cpp files)
//
// Each macro generates three things:
//   1. The thunk body — serialises RPCHeader + TParams, sends via NetChannel.
//   2. A file-scope static bool registrar — calls RegisterServerRPC/RegisterClientRPC
//      at static init time, binding MethodID → handler lambda.
//   3. The handler signature — user writes { ... } immediately after the macro.
//
// The static bool trick (= []() -> bool { ... }()) is the same pattern used
// by TNX_REGISTER_SCHEMA and TNX_REGISTER_ENTITY. It runs before main().
// ---------------------------------------------------------------------------
#define TNX_IMPL_SERVER(Class, Name, TParams)                                          \
	bool Class::Name(const TParams& params)                                            \
	{                                                                                  \
		static_assert(std::is_trivially_copyable_v<TParams>,                           \
			#TParams " must be trivially copyable — it is a wire format");             \
		RPCHeader hdr{ RPCMethodID<TParams>(),                                         \
		               static_cast<uint16_t>(sizeof(TParams)) };                       \
		return GetNetChannel().SendRPC(hdr, params);                                   \
	}                                                                                  \
	static bool _##Class##_##Name##_srpc_registered = []() -> bool {                  \
		static_assert(std::is_trivially_copyable_v<TParams>,                           \
			#TParams " must be trivially copyable — it is a wire format");             \
		ReflectionRegistry::Get().RegisterServerRPC(                                   \
			RPCMethodID<TParams>(),                                                    \
			static_cast<uint16_t>(sizeof(TParams)),                                    \
			[](Soul* self, const RPCContext& ctx, const uint8_t* data) {              \
				static_cast<Class*>(self)->Name(                                       \
					ctx, *reinterpret_cast<const TParams*>(data));                     \
			});                                                                        \
		return true;                                                                   \
	}();                                                                               \
	void Class::Name(const RPCContext& ctx, const TParams& params)

#define TNX_IMPL_CLIENT(Class, Name, TParams)                                          \
	bool Class::Name(const TParams& params)                                            \
	{                                                                                  \
		static_assert(std::is_trivially_copyable_v<TParams>,                           \
			#TParams " must be trivially copyable — it is a wire format");             \
		RPCHeader hdr{ RPCMethodID<TParams>(),                                         \
		               static_cast<uint16_t>(sizeof(TParams)) };                       \
		return GetNetChannel().SendRPC(hdr, params);                                   \
	}                                                                                  \
	static bool _##Class##_##Name##_crpc_registered = []() -> bool {                  \
		static_assert(std::is_trivially_copyable_v<TParams>,                           \
			#TParams " must be trivially copyable — it is a wire format");             \
		ReflectionRegistry::Get().RegisterClientRPC(                                   \
			RPCMethodID<TParams>(),                                                    \
			static_cast<uint16_t>(sizeof(TParams)),                                    \
			[](Soul* self, const RPCContext& ctx, const uint8_t* data) {              \
				static_cast<Class*>(self)->Name(                                       \
					ctx, *reinterpret_cast<const TParams*>(data));                     \
			});                                                                        \
		return true;                                                                   \
	}();                                                                               \
	void Class::Name(const RPCContext& ctx, const TParams& params)
