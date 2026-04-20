#include "NetChannel.h"

#include <SDL3/SDL_timer.h>

#include "Logger.h"
#include "NetConnectionManager.h"
#include "Soul.h"

PacketHeader NetChannel::MakeHeader(NetMessageType type, uint16_t payloadSize, uint32_t frameNumber) const
{
	PacketHeader hdr{};
	hdr.Type        = static_cast<uint8_t>(type);
	hdr.Flags       = PacketFlag::DefaultFlags;
	hdr.SequenceNum = CI ? CI->NextSeqOut++ : 0;
	hdr.FrameNumber = frameNumber;
	hdr.SenderID    = CI ? CI->OwnerID : 0;
	hdr.Timestamp        = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
	hdr.PayloadSize      = payloadSize;
	hdr.AckedClientFrame = CI ? CI->LastAckedClientFrame : 0;
	return hdr;
}

bool NetChannel::SendPong(const PacketHeader& pingHeader)
{
	PacketHeader hdr{};
	hdr.Type             = static_cast<uint8_t>(NetMessageType::Pong);
	hdr.Flags            = PacketFlag::DefaultFlags;
	hdr.SequenceNum      = pingHeader.SequenceNum; // echo, not incremented
	hdr.FrameNumber      = pingHeader.FrameNumber;
	hdr.SenderID         = CI ? CI->OwnerID : 0;
	hdr.Timestamp        = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
	hdr.PayloadSize      = 0;
	hdr.AckedClientFrame = CI ? CI->LastAckedClientFrame : 0;
	return SendInternal(hdr, nullptr, 0, false);
}

bool NetChannel::SendInternal(const PacketHeader& hdr, const uint8_t* payload, uint32_t /*size*/, bool reliable)
{
	if (!CI || !Mgr) return false;
	const bool ok = Mgr->Send(CI->Handle, hdr, payload, reliable);
	if (ok) [[likely]]
	{
		char hdrStr[128];
		LOG_NET_DEBUG_F(NetSoul, "[NetConnectionManager] Sending %s", hdr.ToString(hdrStr, sizeof(hdrStr)));
	}
	else [[unlikely]]
		LOG_NET_WARN_F(NetSoul, "[NetChannel] Send failed: handle=%u type=%u ownerID=%u reliable=%d",
					   CI->Handle, hdr.Type, CI->OwnerID, static_cast<int>(reliable));
	return ok;
}
