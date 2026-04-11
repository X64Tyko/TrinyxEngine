#pragma once
#include <cstdint>

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
//   ch.Send(NetMessageType::SpawnRequest, payload, /*reliable=*/true);
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

	// Send a header-only message (no payload body).
	bool SendHeaderOnly(NetMessageType type, bool reliable, uint32_t frameNumber = 0)
	{
		PacketHeader hdr = MakeHeader(type, 0, frameNumber);
		return SendInternal(hdr, nullptr, 0, reliable);
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
