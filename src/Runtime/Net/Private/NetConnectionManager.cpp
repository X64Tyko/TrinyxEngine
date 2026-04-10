#include "NetConnectionManager.h"

#include <SDL3/SDL_timer.h>

#include "GNSContext.h"

#include <steam/isteamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include "Logger.h"

// GNS constant aliases — clearer names without the k_/k prefixes
namespace GNS
{
	// Connection states
	constexpr auto Connecting             = k_ESteamNetworkingConnectionState_Connecting;
	constexpr auto Connected              = k_ESteamNetworkingConnectionState_Connected;
	constexpr auto ClosedByPeer           = k_ESteamNetworkingConnectionState_ClosedByPeer;
	constexpr auto ProblemDetectedLocally = k_ESteamNetworkingConnectionState_ProblemDetectedLocally;

	// Config keys
	constexpr auto ConfigStatusCallback = k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged;

	// Results
	constexpr auto ResultOK = k_EResultOK;

	// Send flags
	constexpr auto SendReliable   = k_nSteamNetworkingSend_Reliable;
	constexpr auto SendUnreliable = k_nSteamNetworkingSend_Unreliable;

	// Polling
	constexpr int MaxMessagesPerPoll = 64;
}

// Static instance for GNS callback routing
NetConnectionManager* NetConnectionManager::s_Instance = nullptr;

NetConnectionManager::~NetConnectionManager()
{
	Shutdown();
}

void NetConnectionManager::Initialize(GNSContext* gns)
{
	Sockets    = gns->GetInterface();
	s_Instance = this;

	// Create a poll group so we can receive from all connections in one call
	PollGroup = Sockets->CreatePollGroup();
	if (PollGroup == 0)
	{
		LOG_ERROR("[NetConnectionManager] Failed to create poll group");
	}

	LOG_INFO("[NetConnectionManager] Initialized");
}

void NetConnectionManager::Shutdown()
{
	StopListening();

	// Close all active connections
	for (auto& ci : Connections)
	{
		if (ci.Handle != 0)
		{
			Sockets->CloseConnection(ci.Handle, 0, "Shutdown", false);
		}
	}
	Connections.clear();

	if (PollGroup != 0 && Sockets)
	{
		Sockets->DestroyPollGroup(PollGroup);
		PollGroup = 0;
	}

	if (s_Instance == this) s_Instance = nullptr;

	Sockets = nullptr;
	LOG_INFO("[NetConnectionManager] Shutdown");
}

// ---------------------------------------------------------------------------
// Server API
// ---------------------------------------------------------------------------

bool NetConnectionManager::Listen(uint16_t port)
{
	if (!Sockets) return false;

	SteamNetworkingIPAddr addr;
	addr.Clear();
	addr.m_port = port;

	SteamNetworkingConfigValue_t opt;
	opt.SetPtr(GNS::ConfigStatusCallback,
			   reinterpret_cast<void*>(&NetConnectionManager::OnConnectionStatusChanged));

	ListenSocket = Sockets->CreateListenSocketIP(addr, 1, &opt);
	if (ListenSocket == 0)
	{
		LOG_ERROR_F("[NetConnectionManager] Failed to listen on port %u", port);
		return false;
	}

	LOG_INFO_F("[NetConnectionManager] Listening on port %u", port);
	return true;
}

void NetConnectionManager::StopListening()
{
	if (ListenSocket != 0 && Sockets)
	{
		Sockets->CloseListenSocket(ListenSocket);
		ListenSocket = 0;
		LOG_INFO("[NetConnectionManager] Stopped listening");
	}
}

void NetConnectionManager::AcceptConnection(HSteamNetConnection conn)
{
	if (!Sockets) return;

	if (Sockets->AcceptConnection(conn) != GNS::ResultOK)
	{
		LOG_ERROR_F("[NetConnectionManager] Failed to accept connection %u", conn);
		Sockets->CloseConnection(conn, 0, "AcceptFailed", false);
		return;
	}

	// Add to poll group so PollIncoming picks up messages from this connection
	if (PollGroup != 0)
	{
		Sockets->SetConnectionPollGroup(conn, PollGroup);
	}

	AddConnection(conn);
	if (ConnectionInfo* ci = FindConnection(conn)) ci->bServerSide = true;
	LOG_INFO_F("[NetConnectionManager] Accepted connection %u (server-side)", conn);
}

// ---------------------------------------------------------------------------
// Client API
// ---------------------------------------------------------------------------

HSteamNetConnection NetConnectionManager::Connect(const char* address, uint16_t port)
{
	if (!Sockets) return 0;

	SteamNetworkingIPAddr addr;
	addr.Clear();
	if (!addr.ParseString(address))
	{
		LOG_ERROR_F("[NetConnectionManager] Failed to parse address: %s", address);
		return 0;
	}
	addr.m_port = port;

	SteamNetworkingConfigValue_t opt;
	opt.SetPtr(GNS::ConfigStatusCallback,
			   reinterpret_cast<void*>(&NetConnectionManager::OnConnectionStatusChanged));

	HSteamNetConnection conn = Sockets->ConnectByIPAddress(addr, 1, &opt);
	if (conn == 0)
	{
		LOG_ERROR_F("[NetConnectionManager] Failed to connect to %s:%u", address, port);
		return 0;
	}

	// Add to poll group
	if (PollGroup != 0)
	{
		Sockets->SetConnectionPollGroup(conn, PollGroup);
	}

	AddConnection(conn);
	LOG_INFO_F("[NetConnectionManager] Connecting to %s:%u (handle %u)", address, port, conn);
	return conn;
}

// ---------------------------------------------------------------------------
// Shared API
// ---------------------------------------------------------------------------

void NetConnectionManager::CloseConnection(HSteamNetConnection conn, const char* reason)
{
	if (Sockets)
	{
		Sockets->CloseConnection(conn, 0, reason, false);
	}
	RemoveConnection(conn);
}

int NetConnectionManager::PollIncoming(std::vector<ReceivedMessage>& outMessages)
{
	if (!Sockets || PollGroup == 0) return 0;

	SteamNetworkingMessage_t* incomingMsgs[GNS::MaxMessagesPerPoll];

	int numMsgs = Sockets->ReceiveMessagesOnPollGroup(PollGroup, incomingMsgs, GNS::MaxMessagesPerPoll);
	if (numMsgs <= 0) return 0;

	for (int i = 0; i < numMsgs; ++i)
	{
		SteamNetworkingMessage_t* msg = incomingMsgs[i];
		const auto* buf               = static_cast<const uint8_t*>(msg->m_pData);
		const uint32_t bufSize        = static_cast<uint32_t>(msg->m_cbSize);

		ReceivedMessage received;
		const uint8_t* payloadPtr = PacketHeader::Deserialize(buf, bufSize, received.Header);

		if (payloadPtr && received.Header.PayloadSize > 0)
		{
			received.Payload.assign(payloadPtr, payloadPtr + received.Header.PayloadSize);
		}

		received.Connection = msg->m_conn;

		// Update ack tracking for this connection
		ConnectionInfo* ci = FindConnection(msg->m_conn);
		if (ci)
		{
			uint32_t seq = received.Header.SequenceNum;
			if (seq > ci->LastSeqIn)
			{
				// Shift bitfield to account for the gap
				uint32_t delta = seq - ci->LastSeqIn;
				if (delta < 32) ci->AckBitfield = (ci->AckBitfield << delta) | (1u << (delta - 1));
				else ci->AckBitfield            = 0; // Gap too large, reset
				ci->LastSeqIn = seq;
			}
			else if (seq < ci->LastSeqIn)
			{
				// Old packet — set its bit in the ack bitfield
				uint32_t delta = ci->LastSeqIn - seq;
				if (delta <= 32) ci->AckBitfield |= (1u << (delta - 1));
			}
		}

		outMessages.push_back(std::move(received));
		msg->Release();
	}

	return numMsgs;
}

bool NetConnectionManager::Send(HSteamNetConnection conn, const PacketHeader& header,
								const uint8_t* payload, bool reliable)
{
	uint8_t buf[sizeof(PacketHeader) + 65535]; // Stack buffer — PayloadSize is uint16
	uint32_t totalSize = PacketHeader::Serialize(buf, header, payload);
	return SendRaw(conn, buf, totalSize, reliable);
}

bool NetConnectionManager::SendRaw(HSteamNetConnection conn, const uint8_t* data,
								   uint32_t size, bool reliable)
{
	if (!Sockets) return false;

	int flags = reliable ? GNS::SendReliable : GNS::SendUnreliable;

	EResult result = Sockets->SendMessageToConnection(conn, data, size, flags, nullptr);
	return result == GNS::ResultOK;
}

void NetConnectionManager::RunCallbacks()
{
	if (Sockets)
	{
		Sockets->RunCallbacks();
	}
}

// ---------------------------------------------------------------------------
// Connection bookkeeping
// ---------------------------------------------------------------------------

ConnectionInfo* NetConnectionManager::FindConnection(HSteamNetConnection conn)
{
	for (auto& ci : Connections)
	{
		if (ci.Handle == conn) return &ci;
	}
	return nullptr;
}

void NetConnectionManager::AssignOwnerID(HSteamNetConnection conn, uint8_t ownerID)
{
	ConnectionInfo* ci = FindConnection(conn);
	if (ci)
	{
		ci->OwnerID = ownerID;
		LOG_INFO_F("[NetConnectionManager] Assigned OwnerID %u to connection %u", ownerID, conn);
	}
}

void NetConnectionManager::GenerateNetID(HSteamNetConnection conn)
{
	static uint32_t netID = 1;
	ConnectionInfo* ci    = FindConnection(conn);
	if (ci)
	{
		ci->OwnerID = static_cast<uint8_t>(netID++);
		LOG_INFO_F("[NetConnectionManager] Generated OwnerID %u for connection %u", ci->OwnerID, conn);
	}
}

void NetConnectionManager::AddConnection(HSteamNetConnection conn)
{
	// Don't add duplicates
	if (FindConnection(conn)) return;

	ConnectionInfo ci;
	ci.Handle     = conn;
	ci.bConnected = true;
	Connections.push_back(ci);
}

void NetConnectionManager::RemoveConnection(HSteamNetConnection conn)
{
	for (auto it = Connections.begin(); it != Connections.end(); ++it)
	{
		if (it->Handle == conn)
		{
			Connections.erase(it);
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// GNS status callback (static)
// ---------------------------------------------------------------------------

void NetConnectionManager::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* info)
{
	if (!s_Instance || !s_Instance->Sockets) return;

	auto* mgr = s_Instance;

	switch (info->m_info.m_eState)
	{
		case GNS::Connecting:
			{
				// Only accept truly incoming connections (m_hListenSocket != 0).
				// Outgoing connections (from ConnectByIPAddress) also start in
				// Connecting state but have m_hListenSocket == 0.
				if (info->m_info.m_hListenSocket != 0)
				{
					LOG_INFO_F("[NetConnectionManager] Incoming connection %u", info->m_hConn);
					mgr->AcceptConnection(info->m_hConn);
				}
				break;
			}

		case GNS::Connected:
			{
				ConnectionInfo* ci = mgr->FindConnection(info->m_hConn);
				if (ci)
				{
					ci->bConnected = true;
					LOG_INFO_F("[NetConnectionManager] Connection %u established", info->m_hConn);

					if (ci->bServerSide)
					{
						mgr->OnClientConnected(*ci);
					}
					else
					{
						// Begin handshake
						PacketHeader handshakeHeader{};
						handshakeHeader.Type        = static_cast<uint8_t>(NetMessageType::ConnectionHandshake);
						handshakeHeader.Flags       = PacketFlag::HasAck;
						handshakeHeader.SequenceNum = 1;
						handshakeHeader.FrameNumber = 0;
						handshakeHeader.SenderID    = 0;
						handshakeHeader.Timestamp   = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
						handshakeHeader.PayloadSize = 0;
						mgr->Send(info->m_hConn, handshakeHeader, nullptr, true);
					}
				}
				break;
			}

		case GNS::ClosedByPeer:
		case GNS::ProblemDetectedLocally:
			{
				LOG_INFO_F("[NetConnectionManager] Connection %u closed: %s",
						   info->m_hConn, info->m_info.m_szEndDebug);
				mgr->Sockets->CloseConnection(info->m_hConn, 0, nullptr, false);
				mgr->RemoveConnection(info->m_hConn);
				break;
			}

		default: break;
	}
}