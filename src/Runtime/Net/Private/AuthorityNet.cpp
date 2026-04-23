#include "AuthorityNet.h"

#include "EngineConfig.h"
#include "FlowManagerBase.h"
#include "LogicThread.h"
#include "NetChannel.h"
#include "NetConnectionManager.h"
#include "NetTypes.h"
#include "RPC.h"
#include "ReplicationSystem.h"
#include "World.h"
#include "WorldBase.h"
#include "Logger.h"

#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// AuthoritySim::RunSimInput — non-template core of the input injection pass.
// Called from OnSimInput<TLogic> with the 3 logic-dependent values pre-extracted.
// Sets outRollbackFrame to the frame that should be passed to RequestRollback,
// or leaves it as UINT32_MAX if no rollback is needed this tick.
// ---------------------------------------------------------------------------

bool AuthoritySim::RunSimInput(uint32_t frameNumber, bool isResimulating,
                               uint32_t lastCompleted, uint32_t& outRollbackFrame)
{
	ReplicationSystem* repl = Replicator;

	// Coalesced rollback pre-pass: fire the earliest pending input-mismatch resim frame
	// as one consolidated rollback. Deferred here so recursive rollback requests are
	// never issued inside ExecuteRollback.
	if (!isResimulating && PendingInputResimFrame != UINT32_MAX)
	{
		outRollbackFrame       = PendingInputResimFrame;
		PendingInputResimFrame = UINT32_MAX;
	}

	const int maxLead = Config ? Config->MaxClientInputLead : 16;

	// Ack pre-pass: update LastAckedClientFrame for all active owners from
	// LastReceivedFrame before the stall check. Without this, a stall freeze would
	// leave the client's drop floor stuck, causing permanent deadlock.
	if (maxLead >= 0 && repl)
	{
		for (uint8_t ownerID : repl->GetActiveOwnerIDs())
		{
			const PlayerInputLog* log = GetInputLog(ownerID);
			if (!log || !log->bActive) continue;
			if (ConnectionInfo* ci = ConnMgr->FindConnectionByOwnerID(ownerID, /*requireServerSide=*/true))
				ci->LastAckedClientFrame = static_cast<uint32_t>(static_cast<int64_t>(log->LastReceivedFrame) - log->FrameOffset);
		}

		// Pass 1 — stall check: if any active player is beyond the lead budget, hold
		// the entire sim this tick.
		for (uint8_t ownerID : repl->GetActiveOwnerIDs())
		{
			PlayerInputLog* log = GetInputLog(ownerID);
			if (!log || !log->bActive) continue;

			if (frameNumber > log->LastReceivedFrame + static_cast<uint32_t>(maxLead))
			{
				const bool shouldLog = (log->LastStallLogFrame == UINT32_MAX)
					|| (frameNumber >= log->LastStallLogFrame + 512);
				if (shouldLog)
				{
					log->LastStallLogFrame = frameNumber;
					FlowManagerBase* flow  = NetWorld ? NetWorld->GetFlowManager() : nullptr;
					Soul* soul             = flow ? flow->GetSoul(ownerID) : nullptr;
					LOG_NET_WARN_F(soul, "[ServerNet] Stalling sim for ownerID %u: frame %u, lastReceived %u, lastConsumed %u, lead %d",
								   ownerID, frameNumber, log->LastReceivedFrame, log->LastConsumedFrame, maxLead);
				}
				return true;
			}
		}
	}

	if (!repl) return false;

	// Pass 2 — injection: all players are within the lead window.
	for (uint8_t ownerID : repl->GetActiveOwnerIDs())
	{
		PlayerInputLog* log = GetInputLog(ownerID);
		if (!log || !log->bActive) continue;

		InputBuffer* buf = NetWorld->GetPlayerSimInput(ownerID);

		InputConsumeResult result = log->ConsumeFrame(frameNumber);
		if (result)
		{
			if (buf)
			{
				buf->InjectState(result.Entry->State.KeyState,
								 result.Entry->State.MouseDX,
								 result.Entry->State.MouseDY,
								 result.Entry->State.MouseButtons);
				if (InputBuffer* viz = NetWorld->GetPlayerVizInput(ownerID))
				{
					viz->InjectState(result.Entry->State.KeyState,
									 result.Entry->State.MouseDX,
									 result.Entry->State.MouseDY,
									 result.Entry->State.MouseButtons);
					viz->Swap();
				}
				// TODO: inject discrete events into player event queue
			}

			if (frameNumber % 512 == 0)
			{
				const char* tag = (result.Reason == InputMissReason::Hit) ? "Hit" : "Predicted";
				LOG_ENG_DEBUG_F("[Injector] ownerID=%u frame=%u %s anyKey=%d k[0]=0x%02X k[3]=0x%02X lastRecv=%u lastConsumed=%u",
								ownerID, frameNumber, tag,
								(result.Entry->State.KeyState[0] || result.Entry->State.KeyState[3]) ? 1 : 0,
								result.Entry->State.KeyState[0], result.Entry->State.KeyState[3],
								log->LastReceivedFrame, log->LastConsumedFrame);
			}
		}
		else if (result.Reason == InputMissReason::LateOrAliased)
		{
			FlowManagerBase* flow = NetWorld ? NetWorld->GetFlowManager() : nullptr;
			Soul* soul            = flow ? flow->GetSoul(ownerID) : nullptr;
			LOG_NET_WARN_F(soul, "[ServerNet] Input data loss for ownerID %u at frame %u", ownerID, frameNumber);
		}

		if (buf) buf->Swap();

		if (ConnectionInfo* ci = ConnMgr->FindConnectionByOwnerID(ownerID, /*requireServerSide=*/true))
			ci->LastAckedClientFrame = static_cast<uint32_t>(static_cast<int64_t>(log->LastReceivedFrame) - log->FrameOffset);

		if (log->IsDirty())
		{
			const uint32_t resimFrom = log->EarliestDirtyFrame;
			log->ClearDirty();

			const uint32_t ringDepth = log->Depth;
			const bool inWindow      = (lastCompleted < ringDepth || resimFrom >= lastCompleted - ringDepth);

			if (inWindow)
			{
				FlowManagerBase* flow = NetWorld ? NetWorld->GetFlowManager() : nullptr;
				Soul* soul            = flow ? flow->GetSoul(ownerID) : nullptr;
				LOG_NET_INFO_F(soul, "[ServerNet] Input mismatch for ownerID %u, queuing resim from frame %u", ownerID, resimFrom);

				if (resimFrom < PendingInputResimFrame) PendingInputResimFrame = resimFrom;
				repl->AddPendingResim(ownerID, resimFrom);
			}
			else
			{
				FlowManagerBase* flow = NetWorld ? NetWorld->GetFlowManager() : nullptr;
				Soul* soul            = flow ? flow->GetSoul(ownerID) : nullptr;
				LOG_NET_WARN_F(soul, "[ServerNet] Input correction for ownerID %u too old (frame %u, window [%u..%u]) — accepting divergence",
							   ownerID, resimFrom, lastCompleted >= ringDepth ? lastCompleted - ringDepth : 0u, lastCompleted);
			}
		}
	}

	return false;
}

// ---------------------------------------------------------------------------
// AuthorityNet
// ---------------------------------------------------------------------------

void AuthorityNet::BindSoulCallbacks()
{
if (!ConnectionMgr || !AuthorityWorld) return;

ConnectionMgr->OnClientDisconnected.Bind<AuthorityNet, &AuthorityNet::OnClientDisconnectedCB>(this);
}

void AuthorityNet::OnClientDisconnectedCB(uint8_t ownerID)
{
if (ownerID != 0 && ownerID < MaxOwnerIDs && Replicator) Replicator->CloseChannel(ownerID);

if (FlowManagerBase* flow = AuthorityWorld ? AuthorityWorld->GetFlowManager() : nullptr) if (ownerID != 0) flow->OnClientDisconnected(ownerID);
}

void AuthorityNet::CreateInputLog(uint8_t ownerID)
{
if (ownerID == 0 || ownerID >= MaxOwnerIDs || !Replicator) return;

const uint32_t temporalFrameCount = (Config && Config->TemporalFrameCount != EngineConfig::Unset)
? static_cast<uint32_t>(Config->TemporalFrameCount)
: 32u;

const uint32_t maxLead  = static_cast<uint32_t>(Config ? Config->MaxClientInputLead : 16);
const uint32_t logDepth = std::max(temporalFrameCount, maxLead + 1);

ConnectionInfo* ci = ConnectionMgr ? ConnectionMgr->FindConnectionByOwnerID(ownerID, /*requireServerSide=*/true) : nullptr;
Replicator->OpenChannel(ownerID, logDepth, ci, ConnectionMgr);
}

void AuthorityNet::WireNetMode(WorldBase* world)
{
	if (world) world->BindAuthorityNet(this, ConnectionMgr);
}

void AuthorityNet::TickDispatch()
{
	if (Replicator) Replicator->DispatchFrameJobs();
}

void AuthorityNet::TickReplication()
{
if (Replicator) Replicator->Flush(ConnectionMgr);

// Heartbeat ping to each Playing client so AckedClientFrame propagates even during
// quiet frames (no corrections or spawns). NetChannel::MakeHeader stamps LastAckedClientFrame
// into every outbound header — the client reads it in HandleMessage before the switch/case.
if (!ConnectionMgr) return;
for (const auto& ci : ConnectionMgr->GetConnections())
{
if (!ci.bConnected || !ci.bAuthoritySide || ci.OwnerID == 0) continue;
if (ci.RepState < ClientRepState::Playing) continue;
ConnectionInfo* mutableCi = ConnectionMgr->FindConnection(ci.Handle);
if (!mutableCi) continue;
NetChannel(mutableCi, ConnectionMgr).SendHeaderOnly(NetMessageType::Ping, /*reliable=*/false);
}
}

void AuthorityNet::HandleMessage(const ReceivedMessage& msg)
{
auto type = static_cast<NetMessageType>(msg.Header.Type);

switch (type)
{
case NetMessageType::InputFrame:
{
if (msg.Payload.size() < sizeof(InputWindowPacket))
{
LOG_ENG_WARN_F("[ServerNet] InputFrame payload too small (%zu)", msg.Payload.size());
break;
}
const uint8_t ownerID = msg.Header.SenderID;
const auto* payload   = reinterpret_cast<const InputWindowPacket*>(msg.Payload.data());

PlayerInputLog* log = GetInputLog(ownerID);
if (!log)
{
LOG_ENG_WARN_F("[ServerNet] InputFrame from OwnerID %u — no log (not connected?)", ownerID);
break;
}

log->Store(*payload);

// Immediately ACK the new floor so the client can advance its drop window
// without waiting for the next injection pass or state-correction heartbeat.
if (ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection))
{
ci->LastAckedClientFrame = static_cast<uint32_t>(static_cast<int64_t>(log->LastReceivedFrame) - log->FrameOffset);
NetChannel(ci, ConnectionMgr).SendHeaderOnly(NetMessageType::InputFrame, /*reliable=*/false);
}
break;
}

case NetMessageType::Ping:
{
ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
if (ci) NetChannel(ci, ConnectionMgr).SendPong(msg.Header);
break;
}

case NetMessageType::Pong:
{
ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
if (ci)
{
const uint16_t now  = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
const uint16_t sent = msg.Header.Timestamp;
const float rtt     = static_cast<float>(static_cast<uint16_t>(now - sent));
if (ci->RTT_ms <= 0.0f) ci->RTT_ms = rtt;
else ci->RTT_ms                    = ci->RTT_ms * 0.875f + rtt * 0.125f;
}
break;
}

case NetMessageType::ConnectionHandshake:
{
ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
if (!ci)
{
LOG_ENG_WARN_F("[ServerNet] ConnectionHandshake from unknown connection %u", msg.Connection);
break;
}

if (msg.Header.SenderID != 0)
{
LOG_ENG_WARN_F("[ServerNet] ConnectionHandshake from client with existing ownerID %u", msg.Header.SenderID);
break;
}

ConnectionMgr->GenerateNetID(msg.Connection);

if (AuthorityWorld) AuthorityWorld->EnsurePlayerInputSlot(ci->OwnerID);

const uint32_t serverFrame = (AuthorityWorld && AuthorityWorld->GetLogicThread())
 ? AuthorityWorld->GetLogicThread()->GetLastCompletedFrame()
 : 0;
ci->ServerFrameAtHandshake = serverFrame;

CreateInputLog(ci->OwnerID);
ci->RepState = ClientRepState::Synchronizing;

HandshakePayload hsPay{};
hsPay.TickRate = static_cast<uint32_t>(
Config->FixedUpdateHz == EngineConfig::Unset ? 128 : Config->FixedUpdateHz);
hsPay.ServerFrame = serverFrame;

NetChannel(ci, ConnectionMgr).Send(
NetMessageType::ConnectionHandshake, hsPay, /*reliable=*/true, serverFrame);

{
Soul* soul = (AuthorityWorld && AuthorityWorld->GetFlowManager())
 ? AuthorityWorld->GetFlowManager()->GetSoul(ci->OwnerID)
 : nullptr;
LOG_NET_INFO_F(soul, "[ServerNet] HandshakeAccept → OwnerID=%u frame=%u tickRate=%u",
   ci->OwnerID, serverFrame, hsPay.TickRate);
}
break;
}

case NetMessageType::ClockSync:
{
ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
if (!ci) break;

if (msg.Payload.size() < sizeof(ClockSyncPayload))
{
LOG_ENG_WARN_F("[ServerNet] ClockSync payload too small (%zu)", msg.Payload.size());
break;
}

const auto* req = reinterpret_cast<const ClockSyncPayload*>(msg.Payload.data());

ci->ClientLocalFrameAtHandshake = req->LocalFrameAtHandshake;
if (PlayerInputLog* log = GetInputLog(ci->OwnerID)) log->FrameOffset = ci->GetFrameOffset();

const uint32_t serverFrame = (AuthorityWorld && AuthorityWorld->GetLogicThread())
 ? AuthorityWorld->GetLogicThread()->GetLastCompletedFrame()
 : 0;

ClockSyncPayload resp{};
resp.ClientTimestamp = req->ClientTimestamp;
resp.ServerFrame     = serverFrame;

NetChannel ch(ci, ConnectionMgr);
ch.Send(NetMessageType::ClockSync, resp, /*reliable=*/false, serverFrame);

const std::string localPath = (AuthorityWorld && AuthorityWorld->GetFlowManager())
  ? AuthorityWorld->GetFlowManager()->GetActiveLevelLocalPath()
  : std::string{};
if (!localPath.empty())
{
TravelPayload travelMsg{};
travelMsg.PathLength = static_cast<uint8_t>(std::min(localPath.size(), size_t(254)));
if (localPath.size() > 254)
LOG_ENG_WARN_F("[ServerNet] Level path truncated: %s", localPath.c_str());
std::memcpy(travelMsg.LevelPath, localPath.c_str(), travelMsg.PathLength);
travelMsg.LevelPath[travelMsg.PathLength] = '\0';

ch.Send(NetMessageType::TravelNotify, travelMsg, /*reliable=*/true, serverFrame);

ci->RepState = ClientRepState::LevelLoading;
{
Soul* soul = (AuthorityWorld && AuthorityWorld->GetFlowManager())
 ? AuthorityWorld->GetFlowManager()->GetSoul(ci->OwnerID)
 : nullptr;
LOG_NET_INFO_F(soul, "[ServerNet] ClockSyncResponse + TravelNotify → LevelLoading (frame=%u, level=%s)",
   serverFrame, travelMsg.LevelPath);
}
}
else
{
ci->RepState = ClientRepState::Loading;
{
Soul* soul = (AuthorityWorld && AuthorityWorld->GetFlowManager())
 ? AuthorityWorld->GetFlowManager()->GetSoul(ci->OwnerID)
 : nullptr;
LOG_NET_INFO_F(soul, "[ServerNet] ClockSyncResponse → Loading (frame=%u) [no level loaded]", serverFrame);
}
}
break;
}

case NetMessageType::LevelReady:
{
ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
if (!ci) break;

if (ci->RepState == ClientRepState::LevelLoading)
{
ci->RepState = ClientRepState::LevelLoaded;
if (FlowManagerBase* flow = AuthorityWorld ? AuthorityWorld->GetFlowManager() : nullptr) flow->OnClientLoaded(ci->OwnerID);

Soul* soul = (AuthorityWorld && AuthorityWorld->GetFlowManager())
 ? AuthorityWorld->GetFlowManager()->GetSoul(ci->OwnerID)
 : nullptr;
LOG_NET_INFO_F(soul, "[ServerNet] LevelReady received — client LevelLoaded (ownerID=%u)", ci->OwnerID);
}
break;
}

case NetMessageType::EntityDestroy:
// TODO
break;

case NetMessageType::SoulRPC:
{
LOG_ENG_INFO("Received Soul RPC");

ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
if (!ci) break;

if (msg.Payload.size() >= sizeof(RPCHeader))
{
const auto* rpcHdrPeek = reinterpret_cast<const RPCHeader*>(msg.Payload.data());
LOG_ENG_INFO_F("[ServerNet] SoulRPC received: ownerID=%u methodID=%u repState=%d",
   ci->OwnerID, rpcHdrPeek->MethodID, static_cast<int>(ci->RepState));
}

if (msg.Payload.size() < sizeof(RPCHeader))
{
LOG_ENG_WARN_F("[ServerNet] SoulRPC payload too small (%zu bytes)", msg.Payload.size());
break;
}

const auto* rpcHdr      = reinterpret_cast<const RPCHeader*>(msg.Payload.data());
const uint8_t* params   = msg.Payload.data() + sizeof(RPCHeader);
const size_t paramBytes = msg.Payload.size() - sizeof(RPCHeader);

if (paramBytes < rpcHdr->ParamSize)
{
LOG_ENG_WARN_F("[ServerNet] SoulRPC param underrun (MethodID=%u, want=%u, got=%zu)",
   rpcHdr->MethodID, rpcHdr->ParamSize, paramBytes);
break;
}

{
	if (FlowManagerBase* flow = AuthorityWorld ? AuthorityWorld->GetFlowManager() : nullptr)
{
if (Soul* soul = flow->GetSoul(ci->OwnerID))
{
RPCContext ctx{ci, ConnectionMgr};
soul->DispatchServerRPC(ctx, *rpcHdr, params);
}
else
{
LOG_NET_WARN_F(soul, "[ServerNet] SoulRPC: no Soul for ownerID=%u (MethodID=%u)",
   ci->OwnerID, rpcHdr->MethodID);
}
}
}
break;
}

case NetMessageType::PlayerBeginRequest:
LOG_ENG_WARN("[ServerNet] Received legacy PlayerBeginRequest — client should use SoulRPC");
break;

default:
LOG_ENG_WARN_F("[ServerNet] Unhandled message type %u", msg.Header.Type);
break;
}
}
