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
	AuthorityWorld = serverWorld;
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
	if (!AuthorityWorld || !connMgr) return;
	if (connMgr->GetConnectionCount() == 0) return;

	const uint32_t frameNumber = AuthorityWorld->GetLogicThread()
									 ? AuthorityWorld->GetLogicThread()->GetLastCompletedFrame()
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
		if (ci.bConnected && ci.bAuthoritySide && ci.OwnerID > 0 &&
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
			if (!ci.bConnected || !ci.bAuthoritySide || ci.OwnerID == 0) continue;
			if (ci.RepState < ClientRepState::Loaded) continue;

			const uint32_t ciClientFrame = ci.ToClientFrame(frameNumber);
			header.FrameNumber           = ciClientFrame;

			// Patch SpawnFrame inside a per-connection copy so each client receives its own frame-space value.
			std::vector<uint8_t> perClientBuf = buf;
			auto* perClientPayload            = reinterpret_cast<ConstructSpawnPayload*>(perClientBuf.data());
			perClientPayload->SpawnFrame      = ci.ToClientFrame(perClientPayload->SpawnFrame);

			connMgr->Send(ci.Handle, header, perClientBuf.data(), /*reliable=*/true);
			{
				Soul* soul = (AuthorityWorld && AuthorityWorld->GetFlowManager())
								 ? AuthorityWorld->GetFlowManager()->GetSoul(ci.OwnerID)
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
	if (!soul && flow) soul = flow->EnsureEchoSoul(ownerID);

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
	Registry* reg = AuthorityWorld->GetRegistry();

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

	// Helper: build one EntitySpawnPayload for entity at slab index i.
	// Returns false if the entity has no valid record and should be skipped.
	auto BuildSpawnEntry = [&](uint32_t i, bool bAliveOnly, EntitySpawnPayload& out) -> bool
	{
		GlobalEntityHandle gHandle = reg->GlobalEntityRegistry.LookupGlobalHandle(static_cast<EntityCacheHandle>(i));
		EntityRecord* record       = reg->GlobalEntityRegistry.Records[gHandle.GetIndex()];
		if (!record || !record->IsValid()) return false;

		EntityNetHandle netHandle = record->NetworkID;
		if (netHandle.NetIndex == 0) netHandle = AssignNetHandle(reg, gHandle);

		EntityNetManifest manifest{};
		manifest.ClassType = record->Arch ? record->Arch->ArchClassID : 0;

		const uint32_t flagBits = bAliveOnly
									  ? static_cast<uint32_t>(TemporalFlagBits::Alive)
									  : static_cast<uint32_t>(TemporalFlagBits::Active | TemporalFlagBits::Alive);

		out           = {};
		out.NetHandle = netHandle.Value;
		out.Manifest  = manifest.Value;
		out.SpawnFlags = EntitySpawnPayload::Pack(flagBits, record->GetGeneration());
		out.PosX  = posX  ? posX[i]  : 0.0f;
		out.PosY  = posY  ? posY[i]  : 0.0f;
		out.PosZ  = posZ  ? posZ[i]  : 0.0f;
		out.RotQx = rotQx ? rotQx[i] : 0.0f;
		out.RotQy = rotQy ? rotQy[i] : 0.0f;
		out.RotQz = rotQz ? rotQz[i] : 0.0f;
		out.RotQw = rotQw ? rotQw[i] : 1.0f;
		out.ScaleX  = scX   ? scX[i]   : 1.0f;
		out.ScaleY  = scY   ? scY[i]   : 1.0f;
		out.ScaleZ  = scZ   ? scZ[i]   : 1.0f;
		out.ColorR  = colR  ? colR[i]  : 1.0f;
		out.ColorG  = colG  ? colG[i]  : 1.0f;
		out.ColorB  = colB  ? colB[i]  : 1.0f;
		out.ColorA  = colA  ? colA[i]  : 1.0f;
		out.MeshID  = meshID ? meshID[i] : 0;
		return true;
	};

	// Helper: send a batch of spawn entries as a single EntitySpawn packet.
	auto FlushSpawnBatch = [&](HSteamNetConnection handle, const std::vector<EntitySpawnPayload>& batch, uint32_t clientFrame)
	{
		if (batch.empty()) return;
		PacketHeader hdr{};
		hdr.Type        = static_cast<uint8_t>(NetMessageType::EntitySpawn);
		hdr.Flags       = PacketFlag::DefaultFlags;
		hdr.FrameNumber = clientFrame;
		hdr.SenderID    = 0;
		hdr.PayloadSize = static_cast<uint32_t>(batch.size() * sizeof(EntitySpawnPayload));
		connMgr->Send(handle, hdr, reinterpret_cast<const uint8_t*>(batch.data()), true);
	};

	// --- Pass 1: Initial flush for newly LevelLoaded connections ---
	// Collect all Alive+Replicated entities into one batch per connection, then send.
	auto& connections = connMgr->GetConnections();
	std::vector<EntitySpawnPayload> spawnBatch;
	spawnBatch.reserve(64);

	for (auto& ci : connections)
	{
		if (!ci.bConnected || !ci.bAuthoritySide || ci.OwnerID == 0) continue;
		if (ci.RepState != ClientRepState::LevelLoaded || ci.bInitialSpawnFlushed) continue;

		const uint32_t ciClientFrame = ci.ToClientFrame(frameNumber);

		spawnBatch.clear();
		for (uint32_t i = 0; i < maxEntities; ++i)
		{
			if (!(flags[i] & static_cast<int32_t>(TemporalFlagBits::Alive))) continue;
			if (!(flags[i] & static_cast<int32_t>(TemporalFlagBits::Replicated))) continue;
			EntitySpawnPayload entry{};
			if (BuildSpawnEntry(i, /*bAliveOnly=*/true, entry))
				spawnBatch.push_back(entry);
		}

		FlushSpawnBatch(ci.Handle, spawnBatch, ciClientFrame);
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
			FlowManager* serverFlow = AuthorityWorld ? AuthorityWorld->GetFlowManager() : nullptr;
			Soul* soul              = serverFlow ? serverFlow->GetSoul(ci.OwnerID) : nullptr;
			LOG_NET_INFO_F(soul, "[Replication] Initial flush: %zu entities → ownerID=%u, ServerReady sent → Loaded",
						   spawnBatch.size(), ci.OwnerID);

			const FlowState* activeState = serverFlow ? serverFlow->GetActiveState() : nullptr;
			if (activeState && activeState->GetRequirements().SweepsAliveFlagsOnServerReady && AuthorityWorld)
			{
				Registry* lReg = AuthorityWorld->GetRegistry();
				AuthorityWorld->PostAndWait([lReg, soul](uint32_t)
				{
					int count = lReg->SweepAliveFlagsToActive();
					LOG_NET_INFO_F(soul, "[Replication] ServerReady: server swept %d Alive→Active", count);
				});
			}
		}
	}

	// --- Pass 2: Incremental spawns for fully loaded connections ---
	// Collect all new entities into one batch per connection, then send.
	// Outer loop over entities so Replicated[] is marked exactly once.
	// We need a per-connection batch — collect new indices first, then send per-connection.
	std::vector<uint32_t> newEntityIndices;
	for (uint32_t i = 0; i < maxEntities; ++i)
	{
		if (!(flags[i] & static_cast<int32_t>(TemporalFlagBits::Alive))) continue;
		if (!(flags[i] & static_cast<int32_t>(TemporalFlagBits::Replicated))) continue;
		if (Replicated[i]) continue;
		newEntityIndices.push_back(i);
		Replicated[i] = true;
	}

	if (!newEntityIndices.empty())
	{
		for (const auto& ci : connections)
		{
			if (!ci.bConnected || !ci.bAuthoritySide || ci.OwnerID == 0) continue;
			if (ci.RepState < ClientRepState::Loaded) continue;
			if (!ci.bInitialSpawnFlushed) continue;

			const uint32_t ciClientFrame = ci.ToClientFrame(frameNumber);
			spawnBatch.clear();
			for (uint32_t i : newEntityIndices)
			{
				EntitySpawnPayload entry{};
				if (BuildSpawnEntry(i, /*bAliveOnly=*/false, entry))
					spawnBatch.push_back(entry);
			}
			FlushSpawnBatch(ci.Handle, spawnBatch, ciClientFrame);
		}
		LOG_NET_DEBUG_F(nullptr, "[Replication] Sent %zu incremental EntitySpawn(s)", newEntityIndices.size());
	}
}

// ---------------------------------------------------------------------------
// SendStateCorrections — per-connection dirty-entity correction batches.
//
// Pre-scan: collect replicated entities dirtied this frame or whose owner has a pending resim.
// Per-connection: build an annotated batch from the dirty list — resim data is only filled in
// for entities owned by the receiving client. This structure is the hook point for relevancy
// filtering and delta compression in the future.
// ---------------------------------------------------------------------------

void ReplicationSystem::SendStateCorrections(NetConnectionManager* connMgr, uint32_t frameNumber)
{
	Registry* reg = AuthorityWorld->GetRegistry();

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

	// Snapshot pending resim frames for this tick. Indexed by ownerID; delta==0 means no pending resim.
	struct ResimArrays
	{
		const float* posX  = nullptr;
		const float* posY  = nullptr;
		const float* posZ  = nullptr;
		const float* rotQx = nullptr;
		const float* rotQy = nullptr;
		const float* rotQz = nullptr;
		const float* rotQw = nullptr;
		uint32_t delta     = 0;
	};
	ResimArrays resimArrays[MaxOwnerIDs]{};

	for (uint32_t oid = 1; oid < MaxOwnerIDs; ++oid)
	{
		const uint32_t resimFrom = PendingResimFrames[oid].exchange(UINT32_MAX, std::memory_order_acq_rel);
		if (resimFrom == UINT32_MAX || resimFrom >= frameNumber) continue;

		TemporalFrameHeader* resimHdr = temporalCache->GetFrameHeader(resimFrom % ringSize);
		resimArrays[oid].delta        = frameNumber - resimFrom;
		resimArrays[oid].posX         = static_cast<const float*>(temporalCache->GetFieldData(resimHdr, transformSlot, 0));
		resimArrays[oid].posY         = static_cast<const float*>(temporalCache->GetFieldData(resimHdr, transformSlot, 1));
		resimArrays[oid].posZ         = static_cast<const float*>(temporalCache->GetFieldData(resimHdr, transformSlot, 2));
		resimArrays[oid].rotQx        = static_cast<const float*>(temporalCache->GetFieldData(resimHdr, transformSlot, 3));
		resimArrays[oid].rotQy        = static_cast<const float*>(temporalCache->GetFieldData(resimHdr, transformSlot, 4));
		resimArrays[oid].rotQz        = static_cast<const float*>(temporalCache->GetFieldData(resimHdr, transformSlot, 5));
		resimArrays[oid].rotQw        = static_cast<const float*>(temporalCache->GetFieldData(resimHdr, transformSlot, 6));
	}

	// Pre-scan: collect replicated entities that are dirty or have a pending owner resim.
	// Record lookups are done once here, shared across all per-connection batches.
	struct DirtyEntityInfo
	{
		uint32_t slabIndex;
		uint32_t netHandleValue;
		uint32_t ownerID;
	};
	std::vector<DirtyEntityInfo> dirtyEntities;
	dirtyEntities.reserve(128);

	constexpr int32_t kActive       = static_cast<int32_t>(TemporalFlagBits::Active);
	constexpr int32_t kDirtiedFrame = static_cast<int32_t>(TemporalFlagBits::Dirty);

	for (uint32_t i = 0; i < maxEntities; ++i)
	{
		if (i >= Replicated.size() || !Replicated[i]) continue;
		if (!(flags[i] & kActive)) continue;

		GlobalEntityHandle gHandle = reg->GlobalEntityRegistry.LookupGlobalHandle(static_cast<EntityCacheHandle>(i));
		EntityRecord* record       = reg->GlobalEntityRegistry.Records[gHandle.GetIndex()];
		if (!record || !record->IsValid()) continue;

		const uint32_t oid     = record->NetworkID.GetOwnerID();
		const bool bDirty      = (flags[i] & kDirtiedFrame) != 0;
		const bool bOwnerResim = (oid > 0 && oid < MaxOwnerIDs && resimArrays[oid].delta > 0);

		if (!bDirty && !bOwnerResim) continue;

		dirtyEntities.push_back({i, record->NetworkID.Value, oid});
	}

	if (dirtyEntities.empty()) return;

	// Per-connection: build annotated batch from the dirty list and send.
	// Resim data is only filled in for entities owned by the receiving client.
	// TODO: add relevancy filter and delta compression inside this loop.
	PacketHeader header{};
	header.Type     = static_cast<uint8_t>(NetMessageType::StateCorrection);
	header.Flags    = PacketFlag::DefaultFlags;
	header.SenderID = 0;

	std::vector<StateCorrectionEntry> batch;
	batch.reserve(dirtyEntities.size());

	for (const auto& ci : connMgr->GetConnections())
	{
		if (!ci.bConnected || !ci.bAuthoritySide || ci.OwnerID == 0) continue;

		// Don't send frames the client hasn't reached yet unless this client has a pending resim
		// (resim corrections must always land, even if the frame is technically in the client's future).
		const bool bClientHasResim = (ci.OwnerID < MaxOwnerIDs && resimArrays[ci.OwnerID].delta > 0);

		batch.clear();
		for (const auto& de : dirtyEntities)
		{
			const uint32_t i = de.slabIndex;

			StateCorrectionEntry entry{};
			entry.NetHandle = de.netHandleValue;
			entry.PosX      = posX[i];
			entry.PosY      = posY[i];
			entry.PosZ      = posZ[i];
			entry.RotQx     = rotQx ? rotQx[i] : 0.0f;
			entry.RotQy     = rotQy ? rotQy[i] : 0.0f;
			entry.RotQz     = rotQz ? rotQz[i] : 0.0f;
			entry.RotQw     = rotQw ? rotQw[i] : 1.0f;

			if (de.ownerID == ci.OwnerID)
			{
				const ResimArrays& ra = resimArrays[de.ownerID];
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

			if (ci.LastAckedClientFrame >= frameNumber || de.ownerID != ci.OwnerID || bClientHasResim) batch.push_back(entry);
		}

		if (batch.empty()) continue;

		const uint32_t payloadSize = static_cast<uint32_t>(batch.size() * sizeof(StateCorrectionEntry));
		header.FrameNumber         = ci.ToClientFrame(frameNumber);
		header.PayloadSize         = static_cast<uint16_t>(payloadSize > 65535 ? 65535 : payloadSize);
		connMgr->Send(ci.Handle, header, reinterpret_cast<const uint8_t*>(batch.data()), false);
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
	std::vector<EntityTransformCorrection> predictedCorrections;
	const uint32_t volatileFrame = reg->GetVolatileCache()->GetActiveWriteFrame();
	bool bPushCorrection         = false;

	for (uint32_t i = 0; i < count; ++i)
	{
		bPushCorrection                   = false;
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
					bPushCorrection = true;
				}
			}
		}

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
			bPushCorrection = true;
		}

		if (bPushCorrection)
		{
			if (entry.ResimFrameDelta > 0)
			{
				const uint32_t clientResimFrame = clientFrame - entry.ResimFrameDelta;
				corrections.push_back({
					entry.NetHandle, clientResimFrame,
					entry.ResimPosX, entry.ResimPosY, entry.ResimPosZ,
					entry.ResimRotQx, entry.ResimRotQy, entry.ResimRotQz, entry.ResimRotQw
				});
			}
			else if (clientFrame < currentF)
			{
				// Historical — client has already simulated this frame, rollback needed.
				corrections.push_back({
					entry.NetHandle, clientFrame,
					entry.PosX, entry.PosY, entry.PosZ,
					entry.RotQx, entry.RotQy, entry.RotQz, entry.RotQw
				});
			}
			else
			{
				// Future — client hasn't reached this frame yet, apply inline during PhysicsLoop.
				predictedCorrections.push_back({
					entry.NetHandle, clientFrame,
					entry.PosX, entry.PosY, entry.PosZ,
					entry.RotQx, entry.RotQy, entry.RotQz, entry.RotQw
				});
			}
		}
	}

	if (!corrections.empty() && logic)
	{
		uint32_t earliest = UINT32_MAX;
		for (const auto& c : corrections) earliest = std::min(earliest, c.ClientFrame);
			logic->EnqueueCorrections(std::move(corrections), earliest);
	}

	if (!predictedCorrections.empty() && logic)
		logic->EnqueuePredictedCorrections(std::move(predictedCorrections));

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

		Archetype* arch   = record->Arch;
		Chunk* chunk      = record->TargetChunk;
		uint32_t localIdx = record->LocalIndex;

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
				case 5: fa[localIdx] = entry.RotQz;
					break;
				case 6: fa[localIdx] = entry.RotQw;
					break;
				default: break;
			}
		}
	}
#endif
}
