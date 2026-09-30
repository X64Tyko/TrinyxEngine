#pragma once

#include <cstddef>
#include <cstdint>

#include "NetTypes.h"

// ---------------------------------------------------------------------------
// Input window codec — the wire format of NetMessageType::InputFrameDelta.
//
//   [InputDeltaPacketHeader]
//   [NetInputFrame]            first frame of the window, sent whole
//   [delta frame] x (FrameCount - 1)
//
// Each delta frame is [Frame u32][Flags u8], then every InputSnapshot section whose flag is set,
// then the frame's discrete events when it has any. The sections are one table in
// InputWindowCodec.cpp shared by the writer and the reader, so adding a field to InputSnapshot
// is one table row; nothing on either side counts bytes by hand.
// ---------------------------------------------------------------------------
namespace InputWindowCodec
{
/// Upper bound for one delta frame: every section plus a full event list. InputSnapshot's
/// sections are a subset of its bytes, so its size bounds them without restating the table.
inline constexpr size_t MaxDeltaFrameBytes = sizeof(NetInputFrame::Frame) + sizeof(uint8_t) /* flags */
											 + sizeof(InputSnapshot)
											 + sizeof(NetInputFrame::EventCount) + sizeof(NetInputFrame::Events);

/// Upper bound for a whole encoded window.
inline constexpr size_t MaxPayloadBytes = sizeof(InputDeltaPacketHeader) + sizeof(NetInputFrame)
										  + (MaxWindowFrames - 1) * MaxDeltaFrameBytes;
static_assert(MaxPayloadBytes <= UINT16_MAX, "an encoded window must fit PacketHeader::PayloadSize");

/// Decode a received window. Returns false on a malformed or truncated payload.
bool Decode(const uint8_t* data, size_t size, InputWindowPacket& out);
} // namespace InputWindowCodec

/// Encodes one window into a caller-provided buffer of InputWindowCodec::MaxPayloadBytes,
/// one frame at a time, so the sender never has to materialize the whole window.
class InputWindowWriter
{
public:
	InputWindowWriter(uint8_t* buffer, const NetInputFrame& base);

	/// Append the next frame of the window, delta-encoded against the previous one.
	void Append(const NetInputFrame& frame);

	/// Patch the frame count into the header and return the encoded size in bytes.
	size_t Finish();

	uint32_t GetLastFrame() const { return Prev.Frame; }

private:
	uint8_t* Begin;
	uint8_t* Cursor;
	NetInputFrame Prev;
	uint32_t FrameCount = 1;
};
