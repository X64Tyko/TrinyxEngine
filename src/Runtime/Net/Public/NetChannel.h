#pragma once
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "NetConnectionManager.h"
#include "NetTypes.h"

// ---------------------------------------------------------------------------
// NetChannel — typed send wrapper for a single GNS connection.
//
// Eliminates the repeated PacketHeader construction boilerplate at every
// send site. Owns no state beyond the pointers to its connection and manager.
//
// Usage (server — one NetChannel per connected client):
//   NetChannel ch(ci, ConnectionMgr, /*frameSource=*/nullptr);
//   ch.Send(NetMessageType::ClockSync, resp, /*reliable=*/false);
//   ch.SendHeaderOnly(NetMessageType::LevelReady, /*reliable=*/true);
//
// Usage (client — one NetChannel for the server connection):
//   NetChannel ch(ci, ConnectionMgr, /*frameSource=*/nullptr);
//   ch.Send(NetMessageType::PlayerBeginRequest, payload, /*reliable=*/true);
//
// FrameNumber in the header is provided at send time (caller passes the
// current server or logic frame). Pass 0 if not meaningful for the message.
//
// Thread safety: not thread-safe. Each thread that sends must own its
// channel or synchronize externally.
// ---------------------------------------------------------------------------
class NetChannel
{
public:
	NetChannel() = default;

	NetChannel(ConnectionInfo* ci, NetConnectionManager* mgr)
		: CI(ci)
		, Mgr(mgr)
	{
	}

	// Send a typed payload. Automatically fills Type, Flags, SequenceNum,
	// Timestamp, SenderID, and PayloadSize in the header.
	template <typename T>
	bool Send(NetMessageType type, const T& payload, bool reliable, uint32_t frameNumber = 0)
	{
		PacketHeader hdr = MakeHeader(type, sizeof(T), frameNumber);
		return SendInternal(hdr, reinterpret_cast<const uint8_t*>(&payload), sizeof(T), reliable);
	}

	// Send a game-layer typed payload. MsgType is a NetMessageType NTTP — the
	// wire message ID is resolved at compile time with zero runtime overhead.
	// TPayload must derive from BaseNetPayload<TPayload>.
	//
	//   ch.Send<NetMessageType::GameModeManifest>(royaleData, /*reliable=*/true);
	//
	// Two GameModes can both use GameModeManifest with different payload structs —
	// the engine routes by TypeID, the GameMode interprets the bytes.
	template <NetMessageType MsgType, typename TPayload>
	bool Send(const TPayload& payload, bool reliable, uint32_t frameNumber = 0)
	{
		static_assert(std::is_base_of_v<BaseNetPayload<TPayload>, TPayload>,
					  "TPayload must derive from BaseNetPayload<TPayload>");
		TPayload::ValidateTrivial();
		constexpr uint16_t size = TPayload::PayloadSize;
		PacketHeader hdr        = MakeHeader(MsgType, size, frameNumber);
		return SendInternal(hdr, reinterpret_cast<const uint8_t*>(&payload), size, reliable);
	}

	// Validate an incoming payload size against a known type before handing
	// bytes to the GameMode. Returns false if size mismatches — caller drops.
	template <typename TPayload>
	static bool ValidatePayload(const PacketHeader& hdr)
	{
		static_assert(std::is_base_of_v<BaseNetPayload<TPayload>, TPayload>,
					  "TPayload must derive from BaseNetPayload<TPayload>");
		TPayload::ValidateTrivial();
		return hdr.PayloadSize == TPayload::PayloadSize;
	}

	// Send a header-only message (no payload body).
	bool SendHeaderOnly(NetMessageType type, bool reliable, uint32_t frameNumber = 0)
	{
		PacketHeader hdr = MakeHeader(type, 0, frameNumber);
		return SendInternal(hdr, nullptr, 0, reliable);
	}

	// Send a SoulRPC — packs RPCHeader + TParams into one contiguous payload.
	// TParams must be trivially copyable (enforced by the TNX_IMPL_* macros).
	template<typename TParams>
	bool SendRPC(const RPCHeader& rpcHdr, const TParams& params,
	             bool reliable = true, uint32_t frameNumber = 0)
	{
		// Stack-allocate the combined payload: RPCHeader followed by TParams bytes.
		constexpr uint16_t totalSize = static_cast<uint16_t>(sizeof(RPCHeader) + sizeof(TParams));
		alignas(alignof(RPCHeader)) uint8_t buf[totalSize];
		std::memcpy(buf,                  &rpcHdr, sizeof(RPCHeader));
		std::memcpy(buf + sizeof(RPCHeader), &params, sizeof(TParams));
		PacketHeader hdr = MakeHeader(NetMessageType::SoulRPC, totalSize, frameNumber);
		return SendInternal(hdr, buf, totalSize, reliable);
	}



	// Send a Pong echoing the sequence and frame from the received Ping header.
	// The sequence is echoed rather than incremented — Pong is a mirror, not a new message.
	bool SendPong(const PacketHeader& pingHeader);

	bool     IsValid()  const { return CI != nullptr && Mgr != nullptr; }
	uint8_t  OwnerID()  const { return CI ? CI->OwnerID : 0; }

private:
	PacketHeader MakeHeader(NetMessageType type, uint16_t payloadSize, uint32_t frameNumber) const;
	bool SendInternal(const PacketHeader& hdr, const uint8_t* payload, uint32_t size, bool reliable);

	ConnectionInfo*      CI  = nullptr;
	NetConnectionManager* Mgr = nullptr;
};
