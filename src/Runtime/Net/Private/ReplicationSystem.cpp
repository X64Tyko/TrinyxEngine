#include "ReplicationSystem.h"

#include "Archetype.h"
#include "CacheSlotMeta.h"
#include "CColor.h"
#include "Construct.h"
#include "ConstructRegistry.h"
#include "FlowManager.h"
#include "FlowState.h"
#include "Logger.h"
#include "LogicThread.h"
#include "CMeshRef.h"
#include "NetConnectionManager.h"
#include "NetTypes.h"
#include "ReflectionRegistry.h"
#include "Registry.h"
#include "CScale.h"
#include "Soul.h"
#include "TemporalComponentCache.h"
#include "CTransform.h"
#include "World.h"

#include <cstring>

void ReplicationSystem::Initialize(World* serverWorld)
{
	ServerWorld = serverWorld;
	Replicated.clear();
	for (auto& f : PendingResimFrames) f.store(UINT32_MAX, std::memory_order_relaxed);
	LOG_ENG_INFO("[Replication] Initialized");
}

void ReplicationSystem::AddPendingResim(uint8_t ownerID, uint32_t serverFrame)
{
	if (ownerID == 0 || ownerID >= MaxOwnerIDs) return;
	uint32_t current = PendingResimFrames[ownerID].load(std::memory_order_relaxed);
	while (serverFrame < current)
	{
		if (PendingResimFrames[ownerID].compare_exchange_weak(current, serverFrame,
															  std::memory_order_release, std::memory_order_relaxed))
			break;
	}
}

// ---------------------------------------------------------------------------
// Tick — called from NetThread at NetworkUpdateHz
// ---------------------------------------------------------------------------

void ReplicationSystem::Tick(NetConnectionManager* connMgr)
{
	if (!ServerWorld || !connMgr) return;
	if (connMgr->GetConnectionCount() == 0) return;

	const uint32_t frameNumber = ServerWorld->GetLogicThread()
									 ? ServerWorld->GetLogicThread()->GetLastCompletedFrame()
									 : 0;

	SendSpawns(connMgr, frameNumber);
	SendConstructSpawns(connMgr, frameNumber);
	SendStateCorrections(connMgr, frameNumber);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// SendConstructSpawns — send pre-built ConstructSpawn payloads to all Loaded+ clients.
// Payloads were fully assembled (EntityNetHandles resolved) at RegisterConstruct time.
// ---------------------------------------------------------------------------

void ReplicationSystem::SendConstructSpawns(NetConnectionManager* connMgr, uint32_t frameNumber)
{
	if (PendingConstructSpawns.empty()) return;

	auto& connections = connMgr->GetConnections();

	bool anyReady = false;
	for (const auto& ci : connections)
	{
		if (ci.bConnected && ci.bServerSide && ci.OwnerID > 0 &&
			ci.RepState >= ClientRepState::Loaded)
		{
			anyReady = true;
			break;
		}
	}
	if (!anyReady) return;

	for (const std::vector<uint8_t>& buf : PendingConstructSpawns)
	{
		if (buf.size() < sizeof(ConstructSpawnPayload)) continue;

		const auto* payload = reinterpret_cast<const ConstructSpawnPayload*>(buf.data());

		PacketHeader header{};
		header.Type        = static_cast<uint8_t>(NetMessageType::ConstructSpawn);
		header.Flags       = PacketFlag::DefaultFlags;
		header.PayloadSize = static_cast<uint16_t>(buf.size());
		header.SenderID    = 0;

		for (const auto& ci : connections)
		{
			if (!ci.bConnected || !ci.bServerSide || ci.OwnerID == 0) continue;
			if (ci.RepState < ClientRepState::Loaded) continue;

			const uint32_t ciClientFrame = ci.ToClientFrame(frameNumber);
			header.FrameNumber           = ciClientFrame;

			// Patch SpawnFrame inside a per-connection copy so each client receives its own frame-space value.
			std::vector<uint8_t> perClientBuf = buf;
			auto* perClientPayload            = reinterpret_cast<ConstructSpawnPayload*>(perClientBuf.data());
			perClientPayload->SpawnFrame      = ci.ToClientFrame(perClientPayload->SpawnFrame);

			connMgr->Send(ci.Handle, header, perClientBuf.data(), /*reliable=*/true);
			{
				Soul* soul = (ServerWorld && ServerWorld->GetFlowManager())
								 ? ServerWorld->GetFlowManager()->GetSoul(ci.OwnerID)
								 : nullptr;
				LOG_NET_INFO_F(soul, "[Replication] ConstructSpawn → ownerID=%u viewCount=%u",
							   ci.OwnerID, payload->ViewCount);
			}
		}
	}

	PendingConstructSpawns.clear();
}

// ---------------------------------------------------------------------------
// HandleConstructSpawn — client-side: create a Construct from a received message.
// Returns false if any required entity view is not yet in the client registry.
// The caller should defer the raw payload bytes and retry next tick.
// ---------------------------------------------------------------------------

bool ReplicationSystem::HandleConstructSpawn(ConstructRegistry* reg, Registry* entityReg,
											 World* clientWorld, const uint8_t* data, size_t len)
{
	if (len < sizeof(ConstructSpawnPayload))
	{
		LOG_NET_WARN(nullptr, "[Replication] HandleConstructSpawn: payload too small");
		return true; // Malformed — don't retry
	}

	const auto* header             = reinterpret_cast<const ConstructSpawnPayload*>(data);
	const uint32_t* viewNetHandles = reinterpret_cast<const uint32_t*>(data + sizeof(ConstructSpawnPayload));

	ConstructNetHandle netHandle{};
	netHandle.Value = header->Handle;

	ConstructNetManifest manifest{};
	manifest.Value = header->Manifest;

	const uint16_t typeHash = static_cast<uint16_t>(manifest.PrefabIndex);
	const uint8_t ownerID   = netHandle.GetOwnerID();
	const uint8_t viewCount = header->ViewCount;

	// Hoist soul lookup so all early-return log paths can show the role tag.
	FlowManager* flow = clientWorld ? clientWorld->GetFlowManager() : nullptr;
	Soul* soul        = flow ? flow->GetSoul(ownerID) : nullptr;

	// Resolve EntityNetHandles → local EntityHandles.
	// If any handle is unresolved, the EntitySpawn hasn't been processed yet — defer.
	constexpr uint8_t MaxViews = 8;
	EntityHandle resolvedHandles[MaxViews];
	uint8_t resolvedCount = 0;

	for (uint8_t i = 0; i < viewCount && i < MaxViews; ++i)
	{
		EntityNetHandle enh{};
		enh.Value = viewNetHandles[i];

		GlobalEntityHandle gH = entityReg->GlobalEntityRegistry.NetToRecord.get(enh.GetHandleIndex());
		if (gH.GetIndex() == 0)
		{
			LOG_NET_DEBUG_F(soul, "[Replication] HandleConstructSpawn: EntityNetHandle %u not yet available — deferring", enh.Value);
			return false;
		}

		EntityRecord* entRec = entityReg->GlobalEntityRegistry.Records[gH.GetIndex()];
		if (!entRec || !entRec->IsValid())
		{
			LOG_NET_DEBUG_F(soul, "[Replication] HandleConstructSpawn: EntityNetHandle %u record invalid — deferring", enh.Value);
			return false;
		}

		resolvedHandles[resolvedCount] = entityReg->MakeEntityHandle(gH, entRec->Arch->ArchClassID);
		resolvedCount++;
	}

	// Find the client factory for this Construct type
	const ReflectionRegistry::ConstructClientFactory factory =
		ReflectionRegistry::Get().FindConstructClientFactory(typeHash);

	if (!factory)
	{
		LOG_NET_WARN_F(soul, "[Replication] HandleConstructSpawn: no factory for typeHash=%u", typeHash);
		return true; // Bad type — don't retry
	}

	// If no Soul exists for this ownerID, lazily create an Echo Soul so the
	// Construct can be claimed and input-gated correctly (e.g., server-owned
	// constructs received on the client have ownerID=0 and no prior Soul).
	if (!soul && flow)
		soul = flow->EnsureEchoSoul(ownerID);

	// Create the client-side Construct via the replication path
	void* raw = factory(reg, clientWorld, resolvedHandles, resolvedCount, soul);
	if (!raw)
	{
		LOG_NET_WARN(soul, "[Replication] HandleConstructSpawn: factory returned null");
		return true;
	}

	// Wire ConstructRecord using the server-assigned net handle from the payload.
	ConstructNetHandle serverHandle{};
	serverHandle.Value = header->Handle;

	ConstructNetManifest wireManifest{};
	wireManifest.PrefabIndex = typeHash;

	ConstructRef ref = reg->WireNetRef(raw, serverHandle, wireManifest, typeHash, header->SpawnFrame);

	// Deliver to the owning Soul
	if (soul)
	{
		soul->ClaimBody(ref);
		LOG_NET_INFO_F(soul, "[Replication] HandleConstructSpawn: ClaimBody → Soul ownerID=%u role=%u", ownerID, static_cast<uint8_t>(soul->GetRole()));
	}
	else
	{
		LOG_NET_WARN_F(soul, "[Replication] HandleConstructSpawn: no Soul and no FlowManager for ownerID=%u", ownerID);
	}
	return true;
}

// ---------------------------------------------------------------------------
// AssignNetHandle — allocate a unique EntityNetHandle for a server entity
// ---------------------------------------------------------------------------

void ReplicationSystem::RegisterEntity(Registry* reg, EntityHandle localHandle, uint8_t ownerID)
{
	GlobalEntityHandle gHandle = reg->GlobalEntityRegistry.LookupGlobalHandle(localHandle);
	if (gHandle.GetIndex() == 0)
	{
		LOG_ENG_WARN("[Replication] RegisterEntity: invalid handle");
		return;
	}
	AssignNetHandle(reg, gHandle, ownerID);
}

EntityNetHandle ReplicationSystem::AssignNetHandle(Registry* reg, GlobalEntityHandle gHandle, uint8_t ownerID)
{
	uint32_t netIndex = reg->AllocateNetIndex();

	EntityNetHandle netHandle{};
	netHandle.NetOwnerID = ownerID;
	netHandle.NetIndex   = netIndex;

	// Wire NetToRecord so we can look up the GlobalEntityHandle from the NetHandle
	reg->GlobalEntityRegistry.NetToRecord.set(netIndex, gHandle);

	// Store the NetworkID on the record itself
	EntityRecord* record = reg->GlobalEntityRegistry.Records[gHandle.GetIndex()];
	if (record) record->NetworkID = netHandle;

	return netHandle;
}

// ---------------------------------------------------------------------------
// SendSpawns — reliable EntitySpawn for entities not yet replicated
// ---------------------------------------------------------------------------

void ReplicationSystem::SendSpawns(NetConnectionManager* connMgr, uint32_t frameNumber)
{
	Registry* reg = ServerWorld->GetRegistry();

	ComponentCacheBase* temporalCache = reg->GetTemporalCache();
	ComponentCacheBase* volatileCache = reg->GetVolatileCache();

	uint32_t temporalFrame = temporalCache->GetActiveReadFrame();
	uint32_t volatileFrame = volatileCache->GetActiveReadFrame();

	TemporalFrameHeader* temporalHdr = temporalCache->GetFrameHeader(temporalFrame);
	TemporalFrameHeader* volatileHdr = volatileCache->GetFrameHeader(volatileFrame);

	// Component slot indices
	const ComponentTypeID flagsSlot     = CacheSlotMeta<>::StaticTemporalIndex();
	const ComponentTypeID transformSlot = CTransform<>::StaticTemporalIndex();
	const ComponentTypeID scaleSlot     = CScale<>::StaticTemporalIndex();
	const ComponentTypeID colorSlot     = CColor<>::StaticTemporalIndex();
	const ComponentTypeID meshRefSlot   = CMeshRef<>::StaticTemporalIndex();

	// Read field arrays from caches
	const auto* flags  = static_cast<const int32_t*>(temporalCache->GetFieldData(temporalHdr, flagsSlot, 0));
	const auto* posX   = static_cast<const float*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 0));
	const auto* posY   = static_cast<const float*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 1));
	const auto* posZ   = static_cast<const float*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 2));
	const auto* rotQx  = static_cast<const float*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 3));
	const auto* rotQy  = static_cast<const float*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 4));
	const auto* rotQz  = static_cast<const float*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 5));
	const auto* rotQw  = static_cast<const float*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 6));
	const auto* scX    = static_cast<const float*>(volatileCache->GetFieldData(volatileHdr, scaleSlot, 0));
	const auto* scY    = static_cast<const float*>(volatileCache->GetFieldData(volatileHdr, scaleSlot, 1));
	const auto* scZ    = static_cast<const float*>(volatileCache->GetFieldData(volatileHdr, scaleSlot, 2));
	const auto* colR   = static_cast<const float*>(volatileCache->GetFieldData(volatileHdr, colorSlot, 0));
	const auto* colG   = static_cast<const float*>(volatileCache->GetFieldData(volatileHdr, colorSlot, 1));
	const auto* colB   = static_cast<const float*>(volatileCache->GetFieldData(volatileHdr, colorSlot, 2));
	const auto* colA   = static_cast<const float*>(volatileCache->GetFieldData(volatileHdr, colorSlot, 3));
	const auto* meshID = static_cast<const uint32_t*>(volatileCache->GetFieldData(volatileHdr, meshRefSlot, 0));

	if (!flags || !posX) return; // No entities allocated

	const uint32_t maxEntities = temporalCache->GetMaxCachedEntityCount();
	if (Replicated.size() < maxEntities) Replicated.resize(maxEntities, false);

	// Helper: build and send one EntitySpawn for entity at index i.
	// clientFrame must already be translated to the receiver's local frame space.
	auto SendOneSpawn = [&](uint32_t i, HSteamNetConnection handle, bool bAliveOnly, uint32_t clientFrame)
	{
		GlobalEntityHandle gHandle = reg->GlobalEntityRegistry.LookupGlobalHandle(static_cast<EntityCacheHandle>(i));
		EntityRecord* record       = reg->GlobalEntityRegistry.Records[gHandle.GetIndex()];
		if (!record || !record->IsValid()) return;

		EntityNetHandle netHandle = record->NetworkID;
		if (netHandle.NetIndex == 0) netHandle = AssignNetHandle(reg, gHandle);

		EntitySpawnPayload spawnMsg{};
		spawnMsg.NetHandle = netHandle.Value;

		EntityNetManifest manifest{};
		manifest.ClassType = record->Arch ? record->Arch->ArchClassID : 0;
		spawnMsg.Manifest  = manifest.Value;

		// SpawnFlags: TemporalFlagBits in high 16 bits, generation in low 16 bits.
		// Receiver writes GetFlags() directly to CacheSlotMeta — no translation needed.
		const uint32_t flagBits = bAliveOnly
									  ? static_cast<uint32_t>(TemporalFlagBits::Alive)
									  : static_cast<uint32_t>(TemporalFlagBits::Active | TemporalFlagBits::Alive);
		spawnMsg.SpawnFlags = EntitySpawnPayload::Pack(flagBits, record->GetGeneration());

		spawnMsg.PosX  = posX ? posX[i] : 0.0f;
		spawnMsg.PosY  = posY ? posY[i] : 0.0f;
		spawnMsg.PosZ  = posZ ? posZ[i] : 0.0f;
		spawnMsg.RotQx = rotQx ? rotQx[i] : 0.0f;
		spawnMsg.RotQy = rotQy ? rotQy[i] : 0.0f;
		spawnMsg.RotQz = rotQz ? rotQz[i] : 0.0f;
		spawnMsg.RotQw = rotQw ? rotQw[i] : 1.0f;

		spawnMsg.ScaleX = scX ? scX[i] : 1.0f;
		spawnMsg.ScaleY = scY ? scY[i] : 1.0f;
		spawnMsg.ScaleZ = scZ ? scZ[i] : 1.0f;

		spawnMsg.ColorR = colR ? colR[i] : 1.0f;
		spawnMsg.ColorG = colG ? colG[i] : 1.0f;
		spawnMsg.ColorB = colB ? colB[i] : 1.0f;
		spawnMsg.ColorA = colA ? colA[i] : 1.0f;

		spawnMsg.MeshID = meshID ? meshID[i] : 0;

		PacketHeader spawnHeader{};
		spawnHeader.Type        = static_cast<uint8_t>(NetMessageType::EntitySpawn);
		spawnHeader.Flags       = PacketFlag::DefaultFlags;
		spawnHeader.PayloadSize = sizeof(EntitySpawnPayload);
		spawnHeader.FrameNumber = clientFrame;
		spawnHeader.SenderID    = 0;
		connMgr->Send(handle, spawnHeader, reinterpret_cast<const uint8_t*>(&spawnMsg), true);
	};

	// --- Pass 1: Initial flush for newly LevelLoaded connections ---
	// Send all active+replicated entities with Alive-only flag.
	// After flushing, send ServerReady and advance RepState → Loaded.
	auto& connections = connMgr->GetConnections();
	for (auto& ci : connections)
	{
		if (!ci.bConnected || !ci.bServerSide || ci.OwnerID == 0) continue;
		if (ci.RepState != ClientRepState::LevelLoaded || ci.bInitialSpawnFlushed) continue;

		const uint32_t ciClientFrame = ci.ToClientFrame(frameNumber);

		int flushCount = 0;
		for (uint32_t i = 0; i < maxEntities; ++i)
		{
			if (!(flags[i] & static_cast<int32_t>(TemporalFlagBits::Alive))) continue;
			if (!(flags[i] & static_cast<int32_t>(TemporalFlagBits::Replicated))) continue;
			SendOneSpawn(i, ci.Handle, /*bAliveOnly=*/true, ciClientFrame);
			flushCount++;
		}

		ci.bInitialSpawnFlushed = true;

		// Send ServerReady — client will sweep Alive→Active then send PlayerBeginRequest.
		FlowEventPayload serverReadyMsg{};
		serverReadyMsg.EventID = static_cast<uint8_t>(FlowEventID::ServerReady);

		PacketHeader serverReadyHeader{};
		serverReadyHeader.Type        = static_cast<uint8_t>(NetMessageType::FlowEvent);
		serverReadyHeader.Flags       = PacketFlag::DefaultFlags;
		serverReadyHeader.SequenceNum = ci.NextSeqOut++;
		serverReadyHeader.SenderID    = 0;
		serverReadyHeader.FrameNumber = ciClientFrame;
		serverReadyHeader.PayloadSize = sizeof(FlowEventPayload);
		connMgr->Send(ci.Handle, serverReadyHeader, reinterpret_cast<const uint8_t*>(&serverReadyMsg), true);

		ci.RepState = ClientRepState::Loaded;
		{
			FlowManager* serverFlow = ServerWorld ? ServerWorld->GetFlowManager() : nullptr;
			Soul* soul              = serverFlow ? serverFlow->GetSoul(ci.OwnerID) : nullptr;
			LOG_NET_INFO_F(soul, "[Replication] Initial flush: %d entities → ownerID=%u, ServerReady sent → Loaded",
						   flushCount, ci.OwnerID);

			const FlowState* activeState = serverFlow ? serverFlow->GetActiveState() : nullptr;
			if (activeState && activeState->GetRequirements().SweepsAliveFlagsOnServerReady && ServerWorld)
			{
				Registry* lReg = ServerWorld->GetRegistry();
				ServerWorld->PostAndWait([lReg, soul](uint32_t)
				{
					int count = lReg->SweepAliveFlagsToActive();
					LOG_NET_INFO_F(soul, "[Replication] ServerReady: server swept %d Alive→Active", count);
				});
			}
		}
	}

	// --- Pass 2: Incremental spawns for fully loaded connections ---
	// Send entities not yet in the global Replicated set to all Loaded+ connections.
	int spawnCount = 0;
	for (uint32_t i = 0; i < maxEntities; ++i)
	{
		if (!(flags[i] & static_cast<int32_t>(TemporalFlagBits::Alive))) continue;
		if (!(flags[i] & static_cast<int32_t>(TemporalFlagBits::Replicated))) continue;
		if (Replicated[i]) continue;

		for (const auto& ci : connections)
		{
			if (!ci.bConnected || !ci.bServerSide || ci.OwnerID == 0) continue;
			if (ci.RepState < ClientRepState::Loaded) continue; // Not ready yet
			if (!ci.bInitialSpawnFlushed) continue;

			const uint32_t ciClientFrame = ci.ToClientFrame(frameNumber);
			SendOneSpawn(i, ci.Handle, /*bAliveOnly=*/false, ciClientFrame);
		}

		Replicated[i] = true;
		spawnCount++;
	}

	if (spawnCount > 0)
		LOG_NET_DEBUG_F(nullptr, "[Replication] Sent %d incremental EntitySpawn(s)", spawnCount);
}

// ---------------------------------------------------------------------------
// SendStateCorrections — unreliable batched transforms for all replicated entities
// ---------------------------------------------------------------------------

void ReplicationSystem::SendStateCorrections(NetConnectionManager* connMgr, uint32_t frameNumber)
{
	//LOG_ENG_INFO_F("[Replication] Sending state corrections for frame %u", frameNumber);
	Registry* reg = ServerWorld->GetRegistry();

	ComponentCacheBase* temporalCache = reg->GetTemporalCache();
	uint32_t temporalFrame            = temporalCache->GetActiveReadFrame();
	TemporalFrameHeader* temporalHdr  = temporalCache->GetFrameHeader(temporalFrame);

	const ComponentTypeID flagsSlot     = CacheSlotMeta<>::StaticTemporalIndex();
	const ComponentTypeID transformSlot = CTransform<>::StaticTemporalIndex();

	const auto* flags = static_cast<const int32_t*>(temporalCache->GetFieldData(temporalHdr, flagsSlot, 0));
	const auto* posX  = static_cast<const float*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 0));
	const auto* posY  = static_cast<const float*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 1));
	const auto* posZ  = static_cast<const float*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 2));
	const auto* rotQx = static_cast<const float*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 3));
	const auto* rotQy = static_cast<const float*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 4));
	const auto* rotQz = static_cast<const float*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 5));
	const auto* rotQw = static_cast<const float*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 6));

	if (!flags || !posX) return;

	const uint32_t maxEntities = temporalCache->GetMaxCachedEntityCount();
	const uint32_t ringSize    = temporalCache->GetTotalFrameCount();

	// Snapshot pending resim frames for this tick and pre-fetch their field arrays.
	// Indexed by ownerID. delta==0 means no resim pending for that owner.
	struct ResimArrays
	{
		const float* posX  = nullptr;
		const float* posY  = nullptr;
		const float* posZ  = nullptr;
		const float* rotQx = nullptr;
		const float* rotQy = nullptr;
		const float* rotQz = nullptr;
		const float* rotQw = nullptr;
		uint32_t delta     = 0; // frameNumber - resimFrom; 0 = nothing pending
	};
	ResimArrays resimArrays[MaxOwnerIDs]{};
	bool bHasResimFrames = false;

	for (uint32_t oid = 1; oid < MaxOwnerIDs; ++oid)
	{
		const uint32_t resimFrom = PendingResimFrames[oid].exchange(UINT32_MAX, std::memory_order_acq_rel);
		if (resimFrom == UINT32_MAX || resimFrom >= frameNumber) continue;

		bHasResimFrames               = true;
		const uint32_t delta          = frameNumber - resimFrom;
		TemporalFrameHeader* resimHdr = temporalCache->GetFrameHeader(resimFrom % ringSize);
		resimArrays[oid].delta        = delta;
		resimArrays[oid].posX         = static_cast<const float*>(temporalCache->GetFieldData(resimHdr, transformSlot, 0));
		resimArrays[oid].posY         = static_cast<const float*>(temporalCache->GetFieldData(resimHdr, transformSlot, 1));
		resimArrays[oid].posZ         = static_cast<const float*>(temporalCache->GetFieldData(resimHdr, transformSlot, 2));
		resimArrays[oid].rotQx        = static_cast<const float*>(temporalCache->GetFieldData(resimHdr, transformSlot, 3));
		resimArrays[oid].rotQy        = static_cast<const float*>(temporalCache->GetFieldData(resimHdr, transformSlot, 4));
		resimArrays[oid].rotQz        = static_cast<const float*>(temporalCache->GetFieldData(resimHdr, transformSlot, 5));
		resimArrays[oid].rotQw        = static_cast<const float*>(temporalCache->GetFieldData(resimHdr, transformSlot, 6));
	}

	// Build correction batch — gather all replicated active entities
	std::vector<StateCorrectionEntry> batch;
	batch.reserve(256);

	for (uint32_t i = 0; i < maxEntities; ++i)
	{
		if (i >= Replicated.size() || !Replicated[i]) continue;
		if (!(flags[i] & static_cast<int32_t>(TemporalFlagBits::Active))) continue;

		// Look up the entity's assigned NetHandle from its record
		GlobalEntityHandle gHandle = reg->GlobalEntityRegistry.LookupGlobalHandle(static_cast<EntityCacheHandle>(i));
		EntityRecord* record       = reg->GlobalEntityRegistry.Records[gHandle.GetIndex()];
		if (!record || !record->IsValid()) continue;

		StateCorrectionEntry entry{};
		entry.NetHandle = record->NetworkID.Value;
		entry.PosX      = posX[i];
		entry.PosY      = posY[i];
		entry.PosZ      = posZ[i];
		entry.RotQx     = rotQx ? rotQx[i] : 0.0f;
		entry.RotQy     = rotQy ? rotQy[i] : 0.0f;
		entry.RotQz     = rotQz ? rotQz[i] : 0.0f;
		entry.RotQw     = rotQw ? rotQw[i] : 1.0f;

		const uint32_t oid = record->NetworkID.NetOwnerID;
		if (oid > 0 && oid < MaxOwnerIDs)
		{
			const ResimArrays& ra = resimArrays[oid];
			if (ra.delta > 0 && ra.posX)
			{
				entry.ResimFrameDelta = ra.delta;
				entry.ResimPosX       = ra.posX[i];
				entry.ResimPosY       = ra.posY ? ra.posY[i] : 0.0f;
				entry.ResimPosZ       = ra.posZ ? ra.posZ[i] : 0.0f;
				entry.ResimRotQx      = ra.rotQx ? ra.rotQx[i] : 0.0f;
				entry.ResimRotQy      = ra.rotQy ? ra.rotQy[i] : 0.0f;
				entry.ResimRotQz      = ra.rotQz ? ra.rotQz[i] : 0.0f;
				entry.ResimRotQw      = ra.rotQw ? ra.rotQw[i] : 1.0f;
			}
		}

		batch.push_back(entry);
	}

	if (batch.empty()) return;

	// Send as single unreliable message per connection with per-connection client-frame translation.
	uint32_t payloadSize = static_cast<uint32_t>(batch.size() * sizeof(StateCorrectionEntry));

	PacketHeader header{};
	header.Type        = static_cast<uint8_t>(NetMessageType::StateCorrection);
	header.Flags       = PacketFlag::DefaultFlags;
	header.PayloadSize = static_cast<uint16_t>(payloadSize > 65535 ? 65535 : payloadSize);
	header.SenderID    = 0;

	for (const auto& ci : connMgr->GetConnections())
	{
		if (ci.bConnected && ci.bServerSide && ci.OwnerID > 0)
		{
			if (frameNumber > ci.LastAckedClientFrame && !bHasResimFrames) continue;

			header.FrameNumber = ci.ToClientFrame(frameNumber);
			connMgr->Send(ci.Handle, header,
						  reinterpret_cast<const uint8_t*>(batch.data()), false);
		}
	}
}

// File-scope helper: write EntitySpawnPayload fields into the given temporal + volatile frames.
// Called at spawn time (write + read frames) and by the server event log during resim (write frame only).
static void WriteEntitySpawnFields([[maybe_unused]] Registry* reg, EntityRecord* record,
								   const EntitySpawnPayload& payload,
								   uint32_t temporalFrame, uint32_t volatileFrame)
{
	Archetype* arch   = record->Arch;
	Chunk* chunk      = record->TargetChunk;
	uint32_t localIdx = record->LocalIndex;

	void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
	arch->BuildFieldArrayTable(chunk, fieldArrayTable, temporalFrame, volatileFrame);

	for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
	{
		void* fieldBase = fieldArrayTable[fdesc.fieldSlotIndex];
		if (!fieldBase) continue;

		auto* floatArr = static_cast<float*>(fieldBase);
		auto* intArr   = static_cast<int32_t*>(fieldBase);
		auto* uintArr  = static_cast<uint32_t*>(fieldBase);

		ComponentTypeID compID = fdesc.componentID;

		if (compID == CacheSlotMeta<>::StaticTypeID())
		{
			if (fdesc.componentSlotIndex == 0) intArr[localIdx] = EntitySpawnPayload::GetFlags(payload.SpawnFlags);
		}
		else if (compID == CTransform<>::StaticTypeID())
		{
			switch (fdesc.componentSlotIndex)
			{
				case 0: floatArr[localIdx] = payload.PosX;
					break;
				case 1: floatArr[localIdx] = payload.PosY;
					break;
				case 2: floatArr[localIdx] = payload.PosZ;
					break;
				case 3: floatArr[localIdx] = payload.RotQx;
					break;
				case 4: floatArr[localIdx] = payload.RotQy;
					break;
				case 5: floatArr[localIdx] = payload.RotQz;
					break;
				case 6: floatArr[localIdx] = payload.RotQw;
					break;
				default: break;
			}
		}
		else if (compID == CScale<>::StaticTypeID())
		{
			switch (fdesc.componentSlotIndex)
			{
				case 0: floatArr[localIdx] = payload.ScaleX;
					break;
				case 1: floatArr[localIdx] = payload.ScaleY;
					break;
				case 2: floatArr[localIdx] = payload.ScaleZ;
					break;
				default: break;
			}
		}
		else if (compID == CColor<>::StaticTypeID())
		{
			switch (fdesc.componentSlotIndex)
			{
				case 0: floatArr[localIdx] = payload.ColorR;
					break;
				case 1: floatArr[localIdx] = payload.ColorG;
					break;
				case 2: floatArr[localIdx] = payload.ColorB;
					break;
				case 3: floatArr[localIdx] = payload.ColorA;
					break;
				default: break;
			}
		}
		else if (compID == CMeshRef<>::StaticTypeID())
		{
			if (fdesc.componentSlotIndex == 0) uintArr[localIdx] = payload.MeshID;
		}
	}
}

// ---------------------------------------------------------------------------
// HandleEntitySpawn — client-side: create entity from received spawn payload
// Uses Registry internals via friend access (GlobalEntityHandle, CreateInternal)
// ---------------------------------------------------------------------------

void ReplicationSystem::HandleEntitySpawn(Registry* reg, const EntitySpawnPayload& payload, [[maybe_unused]] uint32_t frame)
{
	EntityNetHandle receivedNetHandle{};
	receivedNetHandle.Value = payload.NetHandle;

	EntityNetManifest manifest{};
	manifest.Value    = payload.Manifest;
	ClassID classType = static_cast<ClassID>(manifest.ClassType);

	// Create entity via internal path — returns GlobalEntityHandle, no local handle allocated
	GlobalEntityHandle gHandle;
	reg->CreateInternal(classType, {&gHandle, 1});
	if (gHandle.GetIndex() == 0)
	{
		LOG_ENG_WARN_F("[Replication] Failed to create entity ClassID %u", classType);
		return;
	}

	// Wire the received NetHandle to our local GlobalEntityHandle
	reg->GlobalEntityRegistry.NetToRecord.set(receivedNetHandle.GetHandleIndex(), gHandle);

	// Store NetworkID on the record
	EntityRecord* record = reg->GlobalEntityRegistry.Records[gHandle.GetIndex()];
	if (!record || !record->IsValid())
	{
		LOG_ENG_WARN("[Replication] Entity created but record invalid");
		return;
	}
	record->NetworkID = receivedNetHandle;

	// Write field data into the newly created entity's archetype slot.
	// Newly spawned entities must be initialized in BOTH frames so that FieldProxy
	// reads (which target the read frame) see correct data immediately — without
	// waiting for the next PropagateFrame.
	WriteEntitySpawnFields(reg, record, payload,
						   reg->GetTemporalCache()->GetActiveWriteFrame(),
						   reg->GetVolatileCache()->GetActiveWriteFrame());
	WriteEntitySpawnFields(reg, record, payload,
						   reg->GetTemporalCache()->GetActiveReadFrame(),
						   reg->GetVolatileCache()->GetActiveReadFrame());

#ifdef TNX_ENABLE_ROLLBACK
	// Register a replay entry so rollback resim can re-hydrate this entity's temporal slot
	// at the spawn frame, preventing PropagateFrame from propagating zero/stale state over it.
	{
		GlobalEntityHandle capturedGH      = gHandle;
		EntitySpawnPayload capturedPayload = payload;
		reg->PushServerEvent({
			frame,
			[capturedGH, capturedPayload, reg]()
			{
				EntityRecord* rec = reg->GlobalEntityRegistry.Records[capturedGH.GetIndex()];
				if (!rec || !rec->IsValid()) return;
				WriteEntitySpawnFields(reg, rec, capturedPayload,
									   reg->GetTemporalCache()->GetActiveWriteFrame(),
									   reg->GetVolatileCache()->GetActiveWriteFrame());
			}
		});
	}
#endif
}

// ---------------------------------------------------------------------------
// HandleStateCorrections — client-side: apply authoritative transforms
//
// TNX_ENABLE_ROLLBACK: reads each entity's predicted transform at clientFrame,
// compares against the server value, and queues divergent entities as
// EntityTransformCorrections. EnqueueCorrections then triggers a rollback so
// the engine resimulates from clientFrame; on each correction's target frame the
// resimmed value is re-checked and overwritten only if still divergent.
//
// Without rollback: blind write to the current write frame (best-effort snap).
// ---------------------------------------------------------------------------

void ReplicationSystem::HandleStateCorrections(Registry* reg, const StateCorrectionEntry* entries,
											   uint32_t count, [[maybe_unused]] uint32_t clientFrame,
											   [[maybe_unused]] LogicThread* logic, [[maybe_unused]] uint32_t LastAckedFrame)
{
#ifdef TNX_ENABLE_ROLLBACK
	constexpr float kDivergenceThresholdSq = 0.01f * 0.01f; // 1cm

	const auto* temporal      = reg->GetTemporalCache();
	const uint32_t ringSize   = temporal->GetTotalFrameCount();
	const uint32_t currentF   = temporal->GetFrameHeader()->FrameNumber;
	const uint32_t oldestSlab = (currentF >= ringSize - 1) ? (currentF - (ringSize - 1)) : 0u;

	// If this correction predates the temporal ring the slot has been reused — we
	// have no predicted state to compare against, and rollback can't reach that far.
	// Skip the entry; the eventual smooth-interp path will live here instead.
	if (clientFrame < oldestSlab)
	{
		LOG_ENG_DEBUG_F("[Replication] Skipping stale StateCorrection: frame=%u "
						"(oldest=%u, ring depth=%u)",
						clientFrame, oldestSlab, ringSize);
		return;
	}

	std::vector<EntityTransformCorrection> corrections;
	const uint32_t volatileFrame = reg->GetVolatileCache()->GetActiveWriteFrame();

	for (uint32_t i = 0; i < count; ++i)
	{
		const StateCorrectionEntry& entry = entries[i];

		EntityNetHandle netHandle{};
		netHandle.Value = entry.NetHandle;

		GlobalEntityHandle gHandle = reg->GlobalEntityRegistry.LookupGlobalHandle(netHandle);
		if (gHandle.GetIndex() == 0) continue;

		EntityRecord* record = reg->GlobalEntityRegistry.Records[gHandle.GetIndex()];
		if (!record || !record->IsValid()) continue;

		Archetype* arch   = record->Arch;
		Chunk* chunk      = record->TargetChunk;
		uint32_t localIdx = record->LocalIndex;

		// If the server flagged a resim root, check whether our slab at that frame also
		// diverges. If so, enqueue a correction there so the rollback patches from the true
		// root rather than just snapping the current frame.
		if (entry.ResimFrameDelta > 0 && clientFrame >= entry.ResimFrameDelta)
		{
			const uint32_t clientResimFrame = clientFrame - entry.ResimFrameDelta;
			if (clientResimFrame >= oldestSlab)
			{
				void* resimFieldTable[MAX_FIELDS_PER_ARCHETYPE];
				arch->BuildFieldArrayTable(chunk, resimFieldTable, clientResimFrame, volatileFrame);

				float resimX = 0.f, resimY = 0.f, resimZ = 0.f;
				for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
				{
					if (fdesc.componentID != CTransform<>::StaticTypeID()) continue;
					void* base = resimFieldTable[fdesc.fieldSlotIndex];
					if (!base) continue;
					auto* fa = static_cast<float*>(base);
					switch (fdesc.componentSlotIndex)
					{
						case 0: resimX = fa[localIdx];
							break;
						case 1: resimY = fa[localIdx];
							break;
						case 2: resimZ = fa[localIdx];
							break;
						default: break;
					}
				}

				const float rdx = resimX - entry.ResimPosX;
				const float rdy = resimY - entry.ResimPosY;
				const float rdz = resimZ - entry.ResimPosZ;
				if (rdx * rdx + rdy * rdy + rdz * rdz > kDivergenceThresholdSq)
				{
					LOG_ENG_WARN_F("[Replication] ResimRoot divergence: netHandle=%u resimFrame=%u dist=%.4fm",
								   entry.NetHandle, clientResimFrame,
								   std::sqrt(rdx * rdx + rdy * rdy + rdz * rdz));
					corrections.push_back({
						entry.NetHandle, clientResimFrame,
						entry.ResimPosX, entry.ResimPosY, entry.ResimPosZ,
						entry.ResimRotQx, entry.ResimRotQy, entry.ResimRotQz, entry.ResimRotQw
					});
				}
			}
		}

		if (clientFrame <= LastAckedFrame)
		{
			// Read the predicted transform from the historical ring-buffer slot for clientFrame.
			// BuildFieldArrayTable modularly indexes by fieldFrameCount, so clientFrame wraps correctly.
			void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
			arch->BuildFieldArrayTable(chunk, fieldArrayTable, clientFrame, volatileFrame);

			float predictedX = 0.f, predictedY = 0.f, predictedZ = 0.f;
			for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
			{
				if (fdesc.componentID != CTransform<>::StaticTypeID()) continue;
				void* base = fieldArrayTable[fdesc.fieldSlotIndex];
				if (!base) continue;
				auto* fa = static_cast<float*>(base);
				switch (fdesc.componentSlotIndex)
				{
					case 0: predictedX = fa[localIdx];
						break;
					case 1: predictedY = fa[localIdx];
						break;
					case 2: predictedZ = fa[localIdx];
						break;
					default: break;
				}
			}

			const float dx = predictedX - entry.PosX;
			const float dy = predictedY - entry.PosY;
			const float dz = predictedZ - entry.PosZ;
			if (dx * dx + dy * dy + dz * dz > kDivergenceThresholdSq)
			{
				LOG_ENG_WARN_F("[Replication] Divergence: netHandle=%u frame=%u dist=%.4fm",
							   entry.NetHandle, clientFrame, std::sqrt(dx * dx + dy * dy + dz * dz));
				corrections.push_back({
					entry.NetHandle, clientFrame,
					entry.PosX, entry.PosY, entry.PosZ,
					entry.RotQx, entry.RotQy, entry.RotQz, entry.RotQw
				});
			}
		}
	}

	if (!corrections.empty() && logic) logic->EnqueueCorrections(std::move(corrections), clientFrame);

#else
	// Without rollback: blind write to current write frame
	for (uint32_t i = 0; i < count; ++i)
	{
		const StateCorrectionEntry& entry = entries[i];

		EntityNetHandle netHandle{};
		netHandle.Value = entry.NetHandle;

		GlobalEntityHandle gHandle = reg->GlobalEntityRegistry.LookupGlobalHandle(netHandle);
		if (gHandle.GetIndex() == 0) continue;

		EntityRecord* record = reg->GlobalEntityRegistry.Records[gHandle.GetIndex()];
		if (!record || !record->IsValid()) continue;

		Archetype* arch     = record->Arch;
		Chunk*     chunk    = record->TargetChunk;
		uint32_t   localIdx = record->LocalIndex;

		void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
		arch->BuildFieldArrayTable(chunk, fieldArrayTable,
								   reg->GetTemporalCache()->GetActiveWriteFrame(),
								   reg->GetVolatileCache()->GetActiveWriteFrame());

		for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
		{
			if (fdesc.componentID != CTransform<>::StaticTypeID()) continue;
			void* base = fieldArrayTable[fdesc.fieldSlotIndex];
			if (!base) continue;
			auto* fa = static_cast<float*>(base);
			switch (fdesc.componentSlotIndex)
			{
				case 0: fa[localIdx] = entry.PosX;
					break;
				case 1: fa[localIdx] = entry.PosY;
					break;
				case 2: fa[localIdx] = entry.PosZ;
					break;
				case 3: fa[localIdx] = entry.RotQx;
					break;
				case 4: fa[localIdx] = entry.RotQy;
					break;
				case 5: fa[localIdx] = entry.RotQz; break;
				case 6: fa[localIdx] = entry.RotQw; break;
				default: break;
			}
		}
	}
#endif
}
