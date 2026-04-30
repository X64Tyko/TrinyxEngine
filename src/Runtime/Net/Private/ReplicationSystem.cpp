#include "ReplicationSystem.h"

#include <algorithm>
#include "Archetype.h"
#include "CacheSlotMeta.h"
#include "CColor.h"
#include "ConstructRegistry.h"
#include "FlowManagerBase.h"
#include "FlowState.h"
#include "Logger.h"
#include "LogicThread.h"
#include "CMeshRef.h"
#include "NetConnectionManager.h"
#include "NetTypes.h"
#include "Registry.h"
#include "CScale.h"
#include "Soul.h"
#include "TemporalComponentCache.h"
#include "CTransform.h"
#include "WorldBase.h"

#include <cstring>

void ReplicationSystem::Initialize(WorldBase* serverWorld)
{
	AuthorityWorld = serverWorld;
	for (auto& ch : Channels) ch.reset();
	ActiveOwnerIDs.clear();
	for (auto& f : PendingResimFrames) f.store(UINT32_MAX, std::memory_order_relaxed);
	LOG_ENG_INFO("[Replication] Initialized");
}

void ReplicationSystem::OpenChannel(uint8_t ownerID, uint32_t logDepth, ConnectionInfo* ci, NetConnectionManager* mgr)
{
	if (ownerID == 0 || ownerID >= MaxOwnerIDs) return;
	Channels[ownerID] = std::make_unique<ServerClientChannel>();
	Channels[ownerID]->Open(ownerID, logDepth, ci, mgr);
	ActiveOwnerIDs.push_back(ownerID);
}

void ReplicationSystem::CloseChannel(uint8_t ownerID)
{
	if (ownerID == 0 || ownerID >= MaxOwnerIDs) return;
	Channels[ownerID].reset();
	ActiveOwnerIDs.erase(std::remove(ActiveOwnerIDs.begin(), ActiveOwnerIDs.end(), ownerID), ActiveOwnerIDs.end());
}

ServerClientChannel* ReplicationSystem::GetChannelIfActive(uint8_t ownerID)
{
	if (ownerID == 0 || ownerID >= MaxOwnerIDs) return nullptr;
	return Channels[ownerID].get();
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
// DispatchFrameJobs — called from Sentinel on every loop tick.
// Fires spawn/correction build jobs for each newly published logic frame.
// Only does work when LastPublishedFrame has advanced past LastDispatchedFrame.
// ---------------------------------------------------------------------------

void ReplicationSystem::DispatchFrameJobs()
{
	if (!AuthorityWorld) return;

	LogicThreadBase* logic = AuthorityWorld->GetLogicThread();
	if (!logic) return;

	const uint32_t published = logic->GetLastCompletedFrame();
	if (published == LastDispatchedFrame) return;

	DispatchSpawnJobs(published);
	DispatchConstructSpawnJobs(published);
	DispatchConstructDestroyJobs(published);
	DispatchCorrectionJobs(published);

	LastDispatchedFrame = published;
}

// ---------------------------------------------------------------------------
// Flush — called from Sentinel at NetworkUpdateHz.
// Drains each channel's send queue to the wire.
// ---------------------------------------------------------------------------

void ReplicationSystem::Flush(NetConnectionManager* connMgr)
{
	if (!AuthorityWorld || !connMgr) return;
	FlushSendQueues(connMgr);
}

// ---------------------------------------------------------------------------
// DispatchConstructSpawnJobs — per-channel build jobs for pending ConstructSpawn payloads.
// Sentinel: stamps header, dispatches job. Job: patches SpawnFrame, pushes PendingPacket.
// ---------------------------------------------------------------------------

void ReplicationSystem::DispatchConstructSpawnJobs(uint32_t frameNumber)
{
	if (PendingConstructSpawns.empty()) return;
	if (!TrinyxJobs::IsRunning()) return;

	for (uint8_t oid : ActiveOwnerIDs)
	{
		ServerClientChannel* ch = GetChannelIfActive(oid);
		if (!ch || !ch->CI) continue;
		if (ch->CI->RepState < ClientRepState::Loaded) continue;

		const uint32_t clientFrame = ch->CI->ToClientFrame(frameNumber);

		for (const std::vector<uint8_t>& buf : PendingConstructSpawns)
		{
			if (buf.size() < sizeof(ConstructSpawnPayload)) continue;

			// Stamp the header on Sentinel before the job runs to fix SequenceNum.
			PacketHeader hdr = ch->Channel.PrepareHeader(
				NetMessageType::ConstructSpawn,
				static_cast<uint16_t>(buf.size()),
				clientFrame);

			struct Capture
			{
				ServerClientChannel* Channel;
				PacketHeader Header;
				std::vector<uint8_t> Buf;
				uint32_t ServerSpawnFrame;
			};
			auto* cap             = new Capture{ch, hdr, buf, 0};
			cap->ServerSpawnFrame = reinterpret_cast<const ConstructSpawnPayload*>(buf.data())->SpawnFrame;

			TrinyxJobs::Dispatch([cap](uint32_t)
			{
				// Translate SpawnFrame into client-local frame space before sending.
				auto perClientBuf       = cap->Buf;
				auto* pl                = reinterpret_cast<ConstructSpawnPayload*>(perClientBuf.data());
				pl->SpawnFrame          = cap->Channel->CI->ToClientFrame(cap->ServerSpawnFrame);
				cap->Header.PayloadSize = static_cast<uint16_t>(perClientBuf.size());

				PendingPacket pkt;
				pkt.Header   = cap->Header;
				pkt.Payload  = std::move(perClientBuf);
				pkt.Reliable = true;
				cap->Channel->SendQueue.Push(std::move(pkt));
				delete cap;
			}, &BuildCounter, TrinyxJobs::Queue::General);
		}
	}

	PendingConstructSpawns.clear();
}

// ---------------------------------------------------------------------------
// RegisterEntity / AssignNetHandle — pre-register a server entity with a NetHandle
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

	reg->GlobalEntityRegistry.NetToRecord.set(netIndex, gHandle);

	EntityRecord* record = reg->GlobalEntityRegistry.Records[gHandle.GetIndex()];
	if (record) record->NetworkID = netHandle;

	return netHandle;
}

// ---------------------------------------------------------------------------
// SendSpawns — reliable EntitySpawn for entities not yet replicated
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// DispatchSpawnJobs — per-channel build jobs for entity spawns.
//
// Pass 1 (initial flush): Sentinel scans entities, marks Replicated[], transitions
// RepState, then dispatches a job to build+push the payload.
// Pass 2 (incremental): Sentinel scans unreplicated entities per channel, marks them,
// then dispatches a job to build+push the payload.
// AssignNetHandle mutations stay on Sentinel — registry writes cannot go to jobs.
// ---------------------------------------------------------------------------

void ReplicationSystem::DispatchSpawnJobs(uint32_t frameNumber)
{
	if (!TrinyxJobs::IsRunning()) return;

	Registry* reg = AuthorityWorld->GetRegistry();

	ComponentCacheBase* temporalCache = reg->GetTemporalCache();
	ComponentCacheBase* volatileCache = reg->GetVolatileCache();

	TemporalFrameHeader* temporalHdr = temporalCache->GetFrameHeader(temporalCache->GetActiveReadFrame());
	TemporalFrameHeader* volatileHdr = volatileCache->GetFrameHeader(volatileCache->GetActiveReadFrame());

	const ComponentTypeID flagsSlot     = CacheSlotMeta<>::StaticTemporalIndex();
	const ComponentTypeID transformSlot = CTransform<>::StaticTemporalIndex();
	const ComponentTypeID scaleSlot     = CScale<>::StaticTemporalIndex();
	const ComponentTypeID colorSlot     = CColor<>::StaticTemporalIndex();
	const ComponentTypeID meshRefSlot   = CMeshRef<>::StaticTemporalIndex();

	const auto* flags  = static_cast<const int32_t*>(temporalCache->GetFieldData(temporalHdr, flagsSlot, 0));
	const auto* posX   = static_cast<const SimFloat*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 0));
	const auto* posY   = static_cast<const SimFloat*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 1));
	const auto* posZ   = static_cast<const SimFloat*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 2));
	const auto* rotQx  = static_cast<const SimFloat*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 3));
	const auto* rotQy  = static_cast<const SimFloat*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 4));
	const auto* rotQz  = static_cast<const SimFloat*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 5));
	const auto* rotQw  = static_cast<const SimFloat*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 6));
	const auto* scX    = static_cast<const SimFloat*>(volatileCache->GetFieldData(volatileHdr, scaleSlot, 0));
	const auto* scY    = static_cast<const SimFloat*>(volatileCache->GetFieldData(volatileHdr, scaleSlot, 1));
	const auto* scZ    = static_cast<const SimFloat*>(volatileCache->GetFieldData(volatileHdr, scaleSlot, 2));
	const auto* colR   = static_cast<const SimFloat*>(volatileCache->GetFieldData(volatileHdr, colorSlot, 0));
	const auto* colG   = static_cast<const SimFloat*>(volatileCache->GetFieldData(volatileHdr, colorSlot, 1));
	const auto* colB   = static_cast<const SimFloat*>(volatileCache->GetFieldData(volatileHdr, colorSlot, 2));
	const auto* colA   = static_cast<const SimFloat*>(volatileCache->GetFieldData(volatileHdr, colorSlot, 3));
	const auto* meshID = static_cast<const uint32_t*>(volatileCache->GetFieldData(volatileHdr, meshRefSlot, 0));

	if (!flags || !posX) return;

	const uint32_t maxEntities = temporalCache->GetMaxCachedEntityCount();
	const uint32_t horizon     = CommittedFrameHorizon;

	// Sentinel pre-scan: assign net handles (registry mutation) and collect candidates.
	// IsReplicated/MarkReplicated run per-connection inside each job.
	struct SpawnCandidate
	{
		uint32_t slabIndex;
		EntityNetHandle netHandle;
		EntityNetManifest manifest;
		uint8_t generation;
	};

	struct DestroyCandidate
	{
		uint32_t slabIndex;
		uint32_t netHandleValue;
	};

	std::vector<SpawnCandidate> allCandidates;
	std::vector<DestroyCandidate> allDestroys;
	allCandidates.reserve(64);

	const int32_t Alive         = static_cast<int32_t>(TemporalFlagBits::Alive);
	const int32_t Replicated    = static_cast<int32_t>(TemporalFlagBits::Replicated);
	const int32_t Tombstoned    = static_cast<int32_t>(TemporalFlagBits::Tombstone);
	const int32_t ConfirmedDead = static_cast<int32_t>(TemporalFlagBits::NetConfirmedDead);

	for (uint32_t i = 0; i < maxEntities; ++i)
	{
		const int32_t f = flags[i];

		if (!(f & Replicated)) continue;

		if (f & Tombstoned)
		{
			// Tombstoned entities enter the net destruction cycle regardless of Alive state.
			// Alive is still set until deferred destroy reclaims the slot.
			if (!(f & ConfirmedDead))
			{
				// Tombstone not yet confirmed — post a world queue job to stamp
				// NetConfirmedDead once CommittedFrameHorizon has passed this frame.
				// The flag will be visible in the next published frame.
				if (horizon >= frameNumber)
				{
					// Already past horizon — stamp immediately via Logic.
					const uint32_t slotIdx = i;
					AuthorityWorld->Post([reg, slotIdx](uint32_t)
					{
						ComponentCacheBase* cache   = reg->GetTemporalCache();
						TemporalFrameHeader* hdr    = cache->GetFrameHeader(cache->GetActiveWriteFrame());
						const ComponentTypeID fSlot = CacheSlotMeta<>::StaticTemporalIndex();
						auto* wFlags                = static_cast<int32_t*>(cache->GetFieldData(hdr, fSlot, 0));
						if (wFlags) wFlags[slotIdx] |= static_cast<int32_t>(TemporalFlagBits::NetConfirmedDead);
						reg->ConfirmTombstone(slotIdx);
					});
				}
				// else: horizon hasn't passed yet — will be picked up next tick
			}
			else
			{
				// Confirmed dead — collect for EntityDestroy this pass.
				GlobalEntityHandle gH = reg->GlobalEntityRegistry.LookupGlobalHandle(static_cast<EntityCacheHandle>(i));
				EntityRecord* record  = reg->GlobalEntityRegistry.Records[gH.GetIndex()];
				if (!record) continue;

				allDestroys.push_back({i, record->NetworkID.Value});
			}
		}
		else if (f & Alive)
		{
			// Live replicated entity — collect for spawn/state replication.
			GlobalEntityHandle gH = reg->GlobalEntityRegistry.LookupGlobalHandle(static_cast<EntityCacheHandle>(i));
			EntityRecord* record  = reg->GlobalEntityRegistry.Records[gH.GetIndex()];
			if (!record || !record->IsValid()) continue;

			EntityNetHandle nh = record->NetworkID;
			if (nh.NetIndex == 0) nh = AssignNetHandle(reg, gH);

			EntityNetManifest manifest{};
			manifest.ClassType = record->Arch ? record->Arch->ArchClassID : 0;

			allCandidates.push_back({i, nh, manifest, static_cast<uint8_t>(record->GetGeneration())});
		}
	}

	if (allCandidates.empty() && allDestroys.empty()) return;

	// Per-connection: stamp state transitions + headers on Sentinel, then dispatch the build job.
	// Each job is the sole writer to its channel's Replicated[] bitfield.
	struct SpawnCapture
	{
		ServerClientChannel* Channel;
		PacketHeader SpawnHeader;
		PacketHeader DestroyHeader;
		PacketHeader ReadyHeader;
		std::vector<SpawnCandidate> Candidates;
		std::vector<DestroyCandidate> Destroys;
		bool bPass1;
		const int32_t* flags;
		const SimFloat* posX;
		const SimFloat* posY;
		const SimFloat* posZ;
		const SimFloat* rotQx;
		const SimFloat* rotQy;
		const SimFloat* rotQz;
		const SimFloat* rotQw;
		const SimFloat* scX;
		const SimFloat* scY;
		const SimFloat* scZ;
		const SimFloat* colR;
		const SimFloat* colG;
		const SimFloat* colB;
		const SimFloat* colA;
		const uint32_t* meshID;
	};

	FlowManagerBase* serverFlow = AuthorityWorld ? AuthorityWorld->GetFlowManager() : nullptr;

	for (uint8_t oid : ActiveOwnerIDs)
	{
		ServerClientChannel* ch = GetChannelIfActive(oid);
		if (!ch || !ch->CI) continue;

		const bool bPass1 = ch->CI->RepState == ClientRepState::LevelLoaded && !ch->CI->bInitialSpawnFlushed;
		const bool bPass2 = ch->CI->RepState >= ClientRepState::Loaded && ch->CI->bInitialSpawnFlushed;
		if (!bPass1 && !bPass2) continue;

		ch->EnsureCapacity(maxEntities);
		const uint32_t clientFrame = ch->CI->ToClientFrame(frameNumber);

		PacketHeader spawnHdr   = ch->Channel.PrepareHeader(NetMessageType::EntitySpawn, 0, clientFrame);
		PacketHeader destroyHdr = ch->Channel.PrepareHeader(NetMessageType::EntityDestroy, 0, clientFrame);
		PacketHeader readyHdr{};

		if (bPass1)
		{
			readyHdr = ch->Channel.PrepareHeader(NetMessageType::FlowEvent, sizeof(FlowEventPayload), clientFrame);

			ch->CI->bInitialSpawnFlushed = true;
			ch->CI->RepState             = ClientRepState::Loaded;

			Soul* soul                   = serverFlow ? serverFlow->GetSoul(oid) : nullptr;
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

		auto* cap = new SpawnCapture{
			ch, spawnHdr, destroyHdr, readyHdr, allCandidates, allDestroys, bPass1,
			flags, posX, posY, posZ, rotQx, rotQy, rotQz, rotQw,
			scX, scY, scZ, colR, colG, colB, colA, meshID
		};

		TrinyxJobs::Dispatch([cap](uint32_t)
		{
			std::vector<EntitySpawnPayload> batch;
			batch.reserve(cap->Candidates.size());

			for (const auto& cand : cap->Candidates)
			{
				const uint32_t i = cand.slabIndex;

				if (!cap->bPass1 && cap->Channel->IsReplicated(i)) continue;
				cap->Channel->MarkReplicated(i);

				const uint32_t flagBits = cap->bPass1
											  ? static_cast<uint32_t>(TemporalFlagBits::Alive)
											  : static_cast<uint32_t>(TemporalFlagBits::Active | TemporalFlagBits::Alive);

				EntitySpawnPayload entry{};
				entry.NetHandle  = cand.netHandle.Value;
				entry.Manifest   = cand.manifest.Value;
				entry.SpawnFlags = EntitySpawnPayload::Pack(flagBits, cand.generation);
				entry.PosX       = cap->posX ? cap->posX[i] : 0.0f;
				entry.PosY       = cap->posY ? cap->posY[i] : 0.0f;
				entry.PosZ       = cap->posZ ? cap->posZ[i] : 0.0f;
				entry.RotQx      = cap->rotQx ? cap->rotQx[i] : 0.0f;
				entry.RotQy      = cap->rotQy ? cap->rotQy[i] : 0.0f;
				entry.RotQz      = cap->rotQz ? cap->rotQz[i] : 0.0f;
				entry.RotQw      = cap->rotQw ? cap->rotQw[i] : 1.0f;
				entry.ScaleX     = cap->scX ? cap->scX[i] : 1.0f;
				entry.ScaleY     = cap->scY ? cap->scY[i] : 1.0f;
				entry.ScaleZ     = cap->scZ ? cap->scZ[i] : 1.0f;
				entry.ColorR     = cap->colR ? cap->colR[i] : 1.0f;
				entry.ColorG     = cap->colG ? cap->colG[i] : 1.0f;
				entry.ColorB     = cap->colB ? cap->colB[i] : 1.0f;
				entry.ColorA     = cap->colA ? cap->colA[i] : 1.0f;
				entry.MeshID     = cap->meshID ? cap->meshID[i] : 0;
				batch.push_back(entry);
			}

			if (!batch.empty())
			{
				PendingPacket pkt;
				pkt.Header = cap->SpawnHeader;
				pkt.Payload.resize(batch.size() * sizeof(EntitySpawnPayload));
				pkt.Header.PayloadSize = static_cast<uint16_t>(pkt.Payload.size());
				std::memcpy(pkt.Payload.data(), batch.data(), pkt.Payload.size());
				pkt.Reliable = true;
				cap->Channel->SendQueue.Push(std::move(pkt));

				LOG_NET_DEBUG_F(nullptr, "[Replication] %zu %s EntitySpawn(s) queued",
								batch.size(), cap->bPass1 ? "initial" : "incremental");
			}

			// EntityDestroy — send for any confirmed-dead entities this client knew about.
			{
				std::vector<uint32_t> destroyBatch;
				destroyBatch.reserve(cap->Destroys.size());
				for (const auto& d : cap->Destroys)
				{
					if (!cap->Channel->IsReplicated(d.slabIndex)) continue;
					cap->Channel->ClearReplicated(d.slabIndex);
					destroyBatch.push_back(d.netHandleValue);
				}
				if (!destroyBatch.empty())
				{
					PendingPacket pkt;
					pkt.Header = cap->DestroyHeader;
					pkt.Payload.resize(destroyBatch.size() * sizeof(uint32_t));
					pkt.Header.PayloadSize = static_cast<uint16_t>(pkt.Payload.size());
					std::memcpy(pkt.Payload.data(), destroyBatch.data(), pkt.Payload.size());
					pkt.Reliable = true;
					cap->Channel->SendQueue.Push(std::move(pkt));

					LOG_NET_DEBUG_F(nullptr, "[Replication] %zu EntityDestroy(s) queued",
									destroyBatch.size());
				}
			}

			if (cap->bPass1)
			{
				FlowEventPayload serverReadyMsg{};
				serverReadyMsg.EventID = static_cast<uint8_t>(FlowEventID::ServerReady);
				PendingPacket readyPkt;
				readyPkt.Header             = cap->ReadyHeader;
				readyPkt.Header.PayloadSize = sizeof(FlowEventPayload);
				readyPkt.Payload.resize(sizeof(FlowEventPayload));
				std::memcpy(readyPkt.Payload.data(), &serverReadyMsg, sizeof(FlowEventPayload));
				readyPkt.Reliable = true;
				cap->Channel->SendQueue.Push(std::move(readyPkt));
			}

			delete cap;
		}, &BuildCounter, TrinyxJobs::Queue::General);
	}

	// Net slot recycles are safe once destroy packets are queued — GNS reliable ordering
	// ensures clients receive EntityDestroy before any spawn that reuses the net index.
	if (!allDestroys.empty()) reg->ConfirmNetRecycles();
}

// ---------------------------------------------------------------------------
// DispatchConstructDestroyJobs — drain PendingConstructDestroys (pushed by
// OnConstructDestroyed hook on the Logic thread) and send ConstructDestroy
// packets to all loaded Owners.
// ---------------------------------------------------------------------------

void ReplicationSystem::DispatchConstructDestroyJobs(uint32_t frameNumber)
{
	if (!TrinyxJobs::IsRunning()) return;

	std::vector<uint32_t> handles;
	handles.reserve(8);
	PendingConstructDestroys.Drain(handles);
	if (handles.empty()) return;

	for (uint8_t oid : ActiveOwnerIDs)
	{
		ServerClientChannel* ch = GetChannelIfActive(oid);
		if (!ch || !ch->CI) continue;
		if (ch->CI->RepState < ClientRepState::Loaded) continue;

		const uint32_t clientFrame = ch->CI->ToClientFrame(frameNumber);
		PacketHeader hdr           = ch->Channel.PrepareHeader(
			NetMessageType::ConstructDestroy,
			static_cast<uint16_t>(handles.size() * sizeof(uint32_t)),
			clientFrame);

		struct Capture
		{
			ServerClientChannel* Channel;
			PacketHeader Header;
			std::vector<uint32_t> Handles;
		};
		auto* cap = new Capture{ch, hdr, handles};

		TrinyxJobs::Dispatch([cap](uint32_t)
		{
			PendingPacket pkt;
			pkt.Header             = cap->Header;
			pkt.Header.PayloadSize = static_cast<uint16_t>(cap->Handles.size() * sizeof(uint32_t));
			pkt.Payload.resize(cap->Handles.size() * sizeof(uint32_t));
			std::memcpy(pkt.Payload.data(), cap->Handles.data(), pkt.Payload.size());
			pkt.Reliable = true;
			cap->Channel->SendQueue.Push(std::move(pkt));

			LOG_NET_DEBUG_F(nullptr, "[Replication] %zu ConstructDestroy(s) queued", cap->Handles.size());
			delete cap;
		}, &BuildCounter, TrinyxJobs::Queue::General);
	}
}

// build job per active channel. Jobs filter by per-client Replicated[] and push
// the correction packet to the channel's send queue.
// ---------------------------------------------------------------------------

void ReplicationSystem::DispatchCorrectionJobs(uint32_t frameNumber)
{
	if (!TrinyxJobs::IsRunning()) return;

	Registry* reg = AuthorityWorld->GetRegistry();

	ComponentCacheBase* temporalCache = reg->GetTemporalCache();
	TemporalFrameHeader* temporalHdr  = temporalCache->GetFrameHeader(temporalCache->GetActiveReadFrame());

	const ComponentTypeID flagsSlot     = CacheSlotMeta<>::StaticTemporalIndex();
	const ComponentTypeID transformSlot = CTransform<>::StaticTemporalIndex();

	const auto* flags = static_cast<const int32_t*>(temporalCache->GetFieldData(temporalHdr, flagsSlot, 0));
	const auto* posX  = static_cast<const SimFloat*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 0));
	const auto* posY  = static_cast<const SimFloat*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 1));
	const auto* posZ  = static_cast<const SimFloat*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 2));
	const auto* rotQx = static_cast<const SimFloat*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 3));
	const auto* rotQy = static_cast<const SimFloat*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 4));
	const auto* rotQz = static_cast<const SimFloat*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 5));
	const auto* rotQw = static_cast<const SimFloat*>(temporalCache->GetFieldData(temporalHdr, transformSlot, 6));

	if (!flags || !posX) return;

	const uint32_t maxEntities = temporalCache->GetMaxCachedEntityCount();
	const uint32_t ringSize    = temporalCache->GetTotalFrameCount();

	// Rebuild ResimCache for this flush — only active owners need checking.
	for (uint8_t oid : ActiveOwnerIDs)
	{
		ResimCache[oid]          = {};
		const uint32_t resimFrom = PendingResimFrames[oid].exchange(UINT32_MAX, std::memory_order_acq_rel);
		if (resimFrom == UINT32_MAX || resimFrom >= frameNumber) continue;

		TemporalFrameHeader* resimHdr = temporalCache->GetFrameHeader(resimFrom % ringSize);
		ResimCache[oid].delta         = frameNumber - resimFrom;
		ResimCache[oid].posX          = static_cast<const SimFloat*>(temporalCache->GetFieldData(resimHdr, transformSlot, 0));
		ResimCache[oid].posY          = static_cast<const SimFloat*>(temporalCache->GetFieldData(resimHdr, transformSlot, 1));
		ResimCache[oid].posZ          = static_cast<const SimFloat*>(temporalCache->GetFieldData(resimHdr, transformSlot, 2));
		ResimCache[oid].rotQx         = static_cast<const SimFloat*>(temporalCache->GetFieldData(resimHdr, transformSlot, 3));
		ResimCache[oid].rotQy         = static_cast<const SimFloat*>(temporalCache->GetFieldData(resimHdr, transformSlot, 4));
		ResimCache[oid].rotQz         = static_cast<const SimFloat*>(temporalCache->GetFieldData(resimHdr, transformSlot, 5));
		ResimCache[oid].rotQw         = static_cast<const SimFloat*>(temporalCache->GetFieldData(resimHdr, transformSlot, 6));
	}

	// Rebuild DirtyCache — one pass over all entities on Sentinel, shared by all correction jobs.
	DirtyCache.clear();
	DirtyCache.reserve(128);

	constexpr int32_t Active = static_cast<int32_t>(TemporalFlagBits::Active);
	constexpr int32_t Dirty  = static_cast<int32_t>(TemporalFlagBits::Dirty);

	for (uint32_t i = 0; i < maxEntities; ++i)
	{
		if (!(flags[i] & static_cast<int32_t>(TemporalFlagBits::Replicated))) continue;
		if (!(flags[i] & Active)) continue;

		GlobalEntityHandle gHandle = reg->GlobalEntityRegistry.LookupGlobalHandle(static_cast<EntityCacheHandle>(i));
		EntityRecord* record       = reg->GlobalEntityRegistry.Records[gHandle.GetIndex()];
		if (!record || !record->IsValid()) continue;

		const uint32_t oid     = record->NetworkID.GetOwnerID();
		const bool bDirty      = (flags[i] & Dirty) != 0;
		const bool bOwnerResim = (oid > 0 && oid < MaxOwnerIDs && ResimCache[oid].delta > 0);

		if (!bDirty && !bOwnerResim) continue;
		DirtyCache.push_back({i, record->NetworkID.Value, oid});
	}

	if (DirtyCache.empty()) return;

	// Dispatch one correction build job per active channel.
	// Jobs are read-only on DirtyCache, ResimCache, and slab ptrs.
	const DirtyEntityInfo* dirtyPtr = DirtyCache.data();
	const uint32_t dirtyCount       = static_cast<uint32_t>(DirtyCache.size());
	const ResimSnapshot* resimPtr   = ResimCache;

	struct CorrectionCapture
	{
		ServerClientChannel* Channel;
		PacketHeader Header;
		const DirtyEntityInfo* Dirty;
		uint32_t DirtyCount;
		const ResimSnapshot* Resim;
		const SimFloat* posX;
		const SimFloat* posY;
		const SimFloat* posZ;
		const SimFloat* rotQx;
		const SimFloat* rotQy;
		const SimFloat* rotQz;
		const SimFloat* rotQw;
		uint32_t frameNumber;
	};

	for (uint8_t oid : ActiveOwnerIDs)
	{
		ServerClientChannel* ch = GetChannelIfActive(oid);
		if (!ch || !ch->CI) continue;
		if (!ch->CI->bConnected || !ch->CI->bAuthoritySide) continue;

		const uint32_t clientFrame = ch->CI->ToClientFrame(frameNumber);
		const uint32_t lastAcked   = ch->CI->LastAckedClientFrame;
		const bool bClientHasResim = ResimCache[oid].delta > 0;

		PacketHeader hdr = ch->Channel.PrepareHeader(
			NetMessageType::StateCorrection, 0, clientFrame);

		auto* cap = new CorrectionCapture{
			ch, hdr, dirtyPtr, dirtyCount, resimPtr,
			posX, posY, posZ, rotQx, rotQy, rotQz, rotQw, clientFrame
		};

		TrinyxJobs::Dispatch([cap, lastAcked, bClientHasResim, oid](uint32_t)
		{
			std::vector<StateCorrectionEntry> batch;
			batch.reserve(cap->DirtyCount);

			for (uint32_t d = 0; d < cap->DirtyCount; ++d)
			{
				const DirtyEntityInfo& de = cap->Dirty[d];
				const uint32_t i          = de.slabIndex;

				if (!cap->Channel->IsReplicated(i)) continue;

				StateCorrectionEntry entry{};
				entry.NetHandle = de.netHandleValue;
				entry.PosX      = cap->posX[i];
				entry.PosY      = cap->posY[i];
				entry.PosZ      = cap->posZ[i];
				entry.RotQx     = cap->rotQx ? cap->rotQx[i] : 0.0f;
				entry.RotQy     = cap->rotQy ? cap->rotQy[i] : 0.0f;
				entry.RotQz     = cap->rotQz ? cap->rotQz[i] : 0.0f;
				entry.RotQw     = cap->rotQw ? cap->rotQw[i] : 1.0f;

				if (de.ownerID == oid)
				{
					const ResimSnapshot& rs = cap->Resim[oid];
					if (rs.delta > 0 && rs.posX)
					{
						entry.ResimFrameDelta = rs.delta;
						entry.ResimPosX       = rs.posX[i];
						entry.ResimPosY       = rs.posY ? rs.posY[i] : 0.0f;
						entry.ResimPosZ       = rs.posZ ? rs.posZ[i] : 0.0f;
						entry.ResimRotQx      = rs.rotQx ? rs.rotQx[i] : 0.0f;
						entry.ResimRotQy      = rs.rotQy ? rs.rotQy[i] : 0.0f;
						entry.ResimRotQz      = rs.rotQz ? rs.rotQz[i] : 0.0f;
						entry.ResimRotQw      = rs.rotQw ? rs.rotQw[i] : 1.0f;
					}
				}

				if (lastAcked >= cap->frameNumber || de.ownerID != oid || bClientHasResim) batch.push_back(entry);
			}

			if (batch.empty())
			{
				delete cap;
				return;
			}

			const uint32_t payloadSize = static_cast<uint32_t>(batch.size() * sizeof(StateCorrectionEntry));
			PendingPacket pkt;
			pkt.Header             = cap->Header;
			pkt.Header.PayloadSize = static_cast<uint16_t>(payloadSize > 65535 ? 65535 : payloadSize);
			pkt.Payload.resize(pkt.Header.PayloadSize);
			std::memcpy(pkt.Payload.data(), batch.data(), pkt.Payload.size());
			pkt.Reliable = false;
			cap->Channel->SendQueue.Push(std::move(pkt));
			delete cap;
		}, &BuildCounter, TrinyxJobs::Queue::General);
	}
}

// ---------------------------------------------------------------------------
// FlushSendQueues — drain each channel's MPSC queue and send all pending packets.
// Dispatches one drain job per non-empty channel — fire and forget.
// ---------------------------------------------------------------------------

void ReplicationSystem::FlushSendQueues(NetConnectionManager* connMgr)
{
	if (!TrinyxJobs::IsRunning()) return;

	for (uint8_t oid : ActiveOwnerIDs)
	{
		ServerClientChannel* ch = GetChannelIfActive(oid);
		if (!ch || ch->SendQueue.IsEmpty()) continue;

		TrinyxJobs::Dispatch([ch, connMgr](uint32_t)
		{
			std::vector<PendingPacket> packets;
			ch->SendQueue.Drain(packets);
			for (auto& pkt : packets)
			{
				connMgr->Send(ch->CI->Handle, pkt.Header,
							  pkt.Payload.empty() ? nullptr : pkt.Payload.data(),
							  pkt.Reliable);
			}
		}, &BuildCounter, TrinyxJobs::Queue::General);
	}
}
