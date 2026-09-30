#include "ReplicationSystem.h"

#include <algorithm>
#include "Archetype.h"
#include "NetDelta.h"
#include "CacheSlotMeta.h"
#include "CColor.h"
#include "ConstructRegistry.h"
#include "FlowManagerBase.h"
#include "FlowState.h"
#include "Logger.h"
#include "LogicThread.h"
#include "CMeshRef.h"
#include "NetConnectionManager.h"
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
	for (auto& ch : Channels)
		ch.reset();
	ActiveOwnerIDs.clear();
	for (auto& f : PendingResimFrames)
		f.store(UINT32_MAX, std::memory_order_relaxed);
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

void ReplicationSystem::DispatchFrameJobs()
{
	if (!AuthorityWorld) return;

	LogicThreadBase* logic = AuthorityWorld->GetLogicThread();
	if (!logic) return;

	const uint32_t published = logic->GetLastCompletedFrame();
	if (published == LastDispatchedFrame) return;

	Stats.Reset();

	DispatchSpawnJobs(published);
	DispatchActivateJobs(published);
	DispatchConstructSpawnJobs(published);
	DispatchConstructDestroyJobs(published);
	DispatchCorrectionJobs(published);
	DispatchDeltaCorrectionJobs(published);

	Stats.ActiveChannelCount = static_cast<uint32_t>(ActiveOwnerIDs.size());
	Stats.DirtyEntityCount   = static_cast<uint32_t>(DirtyCache.size());
	Stats.Commit(published);

	LastDispatchedFrame = published;
}

bool ReplicationSystem::EnqueuePlayerConfirm(uint8_t ownerID, uint32_t serverFrame,
	const RPCHeader& rpcHdr, const void* params, uint16_t paramSize)
{
	ServerClientChannel* ch = GetChannelIfActive(ownerID);
	if (!ch || !ch->CI) return false;

	const uint32_t clientFrame = ch->CI->ToClientFrame(serverFrame);
	const uint16_t payloadSize = static_cast<uint16_t>(sizeof(RPCHeader) + paramSize);
	PacketHeader hdr           = ch->Channel.PrepareHeader(NetMessageType::SoulRPC, payloadSize, clientFrame);

	PendingPacket pkt;
	pkt.Header = hdr;
	pkt.Payload.resize(payloadSize);
	std::memcpy(pkt.Payload.data(), &rpcHdr, sizeof(RPCHeader));
	if (params && paramSize > 0)
		std::memcpy(pkt.Payload.data() + sizeof(RPCHeader), params, paramSize);
	pkt.Reliable = true;

	ch->PendingPlayerConfirm           = std::move(pkt);
	ch->PendingPlayerConfirmSpawnFrame = serverFrame + 1; // SpawnAndWait returns before the spawn tick publishes.
	LOG_ENG_INFO_F("[Replication] EnqueuePlayerConfirm: ownerID=%u clientFrame=%u minDispatch=%u",
		ownerID, clientFrame, ch->PendingPlayerConfirmSpawnFrame);
	return true;
}

void ReplicationSystem::Flush(NetConnectionManager* connMgr)
{
	if (!AuthorityWorld || !connMgr) return;

	bool hasPendingConfirm = false;
	for (uint8_t oid : ActiveOwnerIDs)
		if (ServerClientChannel* ch = GetChannelIfActive(oid))
			if (ch->PendingPlayerConfirm.has_value())
			{
				hasPendingConfirm = true;
				break;
			}

	if (hasPendingConfirm)
	{
		// Re-run dispatch in case the spawn tick published between DispatchFrameJobs and now.
		DispatchFrameJobs();

		bool anyReady = false;
		for (uint8_t oid : ActiveOwnerIDs)
		{
			ServerClientChannel* ch = GetChannelIfActive(oid);
			if (ch && ch->PendingPlayerConfirm.has_value()
				&& LastDispatchedFrame >= ch->PendingPlayerConfirmSpawnFrame)
			{
				anyReady = true;
				break;
			}
		}

		if (anyReady)
		{
			WaitForBuildJobs();
			for (uint8_t oid : ActiveOwnerIDs)
			{
				ServerClientChannel* ch = GetChannelIfActive(oid);
				if (!ch || !ch->PendingPlayerConfirm.has_value()) continue;
				if (LastDispatchedFrame < ch->PendingPlayerConfirmSpawnFrame) continue;
				ch->SendQueue.Push(std::move(*ch->PendingPlayerConfirm));
				ch->PendingPlayerConfirm.reset();
				ch->PendingPlayerConfirmSpawnFrame = 0;
			}
		}
	}

	FlushSendQueues(connMgr);
}

void ReplicationSystem::DispatchConstructSpawnJobs(uint32_t frameNumber)
{
	if (!TrinyxJobs::IsRunning()) return;

	struct Capture
	{
		ServerClientChannel* Channel;
		PacketHeader Header;
		std::vector<uint8_t> Buf;
		uint32_t ServerSpawnFrame;
	};

	auto dispatchBuf = [&](ServerClientChannel* ch, uint32_t clientFrame, const std::vector<uint8_t>& buf)
	{
		if (buf.size() < sizeof(ConstructSpawnPayload)) return;

		PacketHeader hdr = ch->Channel.PrepareHeader(
			NetMessageType::ConstructSpawn,
			static_cast<uint16_t>(buf.size()),
			clientFrame);

		auto* cap             = new Capture{ ch, hdr, buf, 0 };
		cap->ServerSpawnFrame = reinterpret_cast<const ConstructSpawnPayload*>(buf.data())->SpawnFrame;

		TrinyxJobs::Dispatch([cap](uint32_t)
		{
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
	};

	for (uint8_t oid : ActiveOwnerIDs)
	{
		ServerClientChannel* ch = GetChannelIfActive(oid);
		if (!ch || !ch->CI) continue;
		if (ch->CI->RepState < ClientRepState::Loaded) continue;

		const uint32_t clientFrame = ch->CI->ToClientFrame(frameNumber);

		if (!ch->CI->bInitialConstructFlushed)
		{
			// Walk all live constructs and build payloads from current view state.
			// CollectViewHandlesFn reads the live EntityHandle from each ConstructView,
			// so defrag relocations are handled correctly without any cached data.
			if (ConstructReg)
			{
				Registry* entityReg = AuthorityWorld ? AuthorityWorld->GetRegistry() : nullptr;
				std::vector<EntityHandle> viewHandles;

				for (auto& bucket : ConstructReg->Buckets)
				{
					for (const auto& entry : bucket)
					{
						if (!entry.NetHandle.Value || !entry.CollectViewHandlesFn) continue;
						const ConstructRecord* rec = ConstructReg->Records.try_get_ptr(entry.NetHandle.NetIndex);
						if (!rec || !rec->IsValid()) continue;

						entry.CollectViewHandlesFn(entry.Ptr, viewHandles);

						std::vector<uint32_t> netHandleValues(viewHandles.size(), 0);
						if (entityReg)
						{
							for (size_t k = 0; k < viewHandles.size(); ++k)
							{
								GlobalEntityHandle gH = entityReg->GlobalEntityRegistry.LookupGlobalHandle(viewHandles[k]);
								if (gH.GetIndex() == 0) continue;
								const EntityRecord* entRec = entityReg->GlobalEntityRegistry.Records[gH.GetIndex()];
								if (entRec) netHandleValues[k] = entRec->NetworkID.Value;
							}
						}

						const uint8_t viewCount  = static_cast<uint8_t>(viewHandles.size());
						const size_t payloadSize = sizeof(ConstructSpawnPayload) + viewCount * sizeof(uint32_t);
						std::vector<uint8_t> buf(payloadSize, 0);
						auto* pl   = reinterpret_cast<ConstructSpawnPayload*>(buf.data());
						pl->Handle = rec->NetworkID.Value;
						ConstructNetManifest mf{};
						mf.PrefabIndex  = rec->TypeHash;
						pl->Manifest    = mf.Value;
						pl->SpawnFrame  = rec->SpawnFrame;
						pl->ViewCount   = viewCount;
						uint32_t* trail = reinterpret_cast<uint32_t*>(buf.data() + sizeof(ConstructSpawnPayload));
						for (uint8_t j = 0; j < viewCount; ++j)
							trail[j] = netHandleValues[j];

						dispatchBuf(ch, clientFrame, buf);
					}
				}
			}
			ch->CI->bInitialConstructFlushed = true;
		}
		else
		{
			for (const std::vector<uint8_t>& buf : PendingConstructSpawns)
				dispatchBuf(ch, clientFrame, buf);
		}
	}

	PendingConstructSpawns.clear();
}

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

// Initial flush: all Alive entities → Owner; ServerReady triggers bulk Alive→Active.
// Incremental: Active|Alive → live immediately; Alive-only → queued PendingActivations.
// AssignNetHandle mutations stay on Sentinel — registry writes cannot go to jobs.
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

	const int32_t Active        = static_cast<int32_t>(TemporalFlagBits::Active);
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
			if (!(f & ConfirmedDead))
			{
				if (horizon >= frameNumber)
				{
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
			}
			else
			{
				GlobalEntityHandle gH = reg->GlobalEntityRegistry.LookupGlobalHandle(static_cast<EntityCacheHandle>(i));
				EntityRecord* record  = reg->GlobalEntityRegistry.Records[gH.GetIndex()];
				if (!record) continue;

				allDestroys.push_back({ i, record->NetworkID.Value });
			}
		}
		else if (f & Alive)
		{
			GlobalEntityHandle gH = reg->GlobalEntityRegistry.LookupGlobalHandle(static_cast<EntityCacheHandle>(i));
			EntityRecord* record  = reg->GlobalEntityRegistry.Records[gH.GetIndex()];
			if (!record || !record->IsValid()) continue;

			EntityNetHandle nh = record->NetworkID;
			if (nh.NetIndex == 0) nh = AssignNetHandle(reg, gH);

			EntityNetManifest manifest{};
			manifest.ClassType = record->Arch ? record->Arch->ArchClassID : 0;

			allCandidates.push_back({ i, nh, manifest, static_cast<uint8_t>(record->GetGeneration()) });
		}
	}

	FlowManagerBase* serverFlow = AuthorityWorld ? AuthorityWorld->GetFlowManager() : nullptr;

	// ServerReady must reach LevelLoaded channels even with no spawn candidates —
	// scene entities aren't individually Replicated; the client loads the scene file itself.
	bool anyInitialFlush = false;
	for (uint8_t oid : ActiveOwnerIDs)
	{
		if (ServerClientChannel* ch = GetChannelIfActive(oid))
			if (ch->CI && ch->CI->RepState == ClientRepState::LevelLoaded && !ch->CI->bInitialSpawnFlushed
				&& (serverFlow ? serverFlow->IsGameModeReady() : true))
				anyInitialFlush = true;
	}

	if (allCandidates.empty() && allDestroys.empty() && !anyInitialFlush) return;

	struct SpawnCapture
	{
		ServerClientChannel* Channel;
		PacketHeader SpawnHeader;
		PacketHeader DestroyHeader;
		PacketHeader ReadyHeader;
		std::vector<SpawnCandidate> Candidates;
		std::vector<DestroyCandidate> Destroys;
		bool bInitialFlush;
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

	for (uint8_t oid : ActiveOwnerIDs)
	{
		ServerClientChannel* ch = GetChannelIfActive(oid);
		if (!ch || !ch->CI) continue;

		const bool bInitialFlush = ch->CI->RepState == ClientRepState::LevelLoaded && !ch->CI->bInitialSpawnFlushed
								   && (serverFlow ? serverFlow->IsGameModeReady() : true);
		const bool bIncremental  = ch->CI->RepState >= ClientRepState::Loaded && ch->CI->bInitialSpawnFlushed;
		if (!bInitialFlush && !bIncremental) continue;

		ch->EnsureCapacity(maxEntities);
		const uint32_t clientFrame = ch->CI->ToClientFrame(frameNumber);

		PacketHeader spawnHdr   = ch->Channel.PrepareHeader(NetMessageType::EntitySpawn, 0, clientFrame);
		PacketHeader destroyHdr = ch->Channel.PrepareHeader(NetMessageType::EntityDestroy, 0, clientFrame);
		PacketHeader readyHdr{};

		if (bInitialFlush)
		{
			readyHdr = ch->Channel.PrepareHeader(NetMessageType::FlowEvent, sizeof(FlowEventPayload), clientFrame);

			ch->CI->bInitialSpawnFlushed = true;
			ch->CI->RepState             = ClientRepState::Loaded;

			Soul* soul     = serverFlow ? serverFlow->GetSoul(oid) : nullptr;
			Registry* lReg = AuthorityWorld->GetRegistry();
			AuthorityWorld->PostAndWait([lReg, soul](uint32_t)
			{
				int count = lReg->SweepAliveFlagsToActive();
				LOG_NET_INFO_F(soul, "[Replication] ServerReady: authority swept %d Alive→Active", count);
			});

			ch->PendingActivations.reserve(allCandidates.size());
			for (const auto& cand : allCandidates)
				ch->PendingActivations.push_back(cand.netHandle.Value);
		}
		else
		{
			for (const auto& cand : allCandidates)
			{
				if (!(flags[cand.slabIndex] & Active))
					ch->PendingActivations.push_back(cand.netHandle.Value);
			}
		}

		auto* cap = new SpawnCapture{
			ch, spawnHdr, destroyHdr, readyHdr, allCandidates, allDestroys, bInitialFlush,
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

				if (!cap->bInitialFlush && cap->Channel->IsReplicated(i)) continue;
				cap->Channel->MarkReplicated(i);

				const bool bEntityActive = (cap->flags[i] & static_cast<int32_t>(TemporalFlagBits::Active)) != 0;
				const uint32_t flagBits  = (!cap->bInitialFlush && bEntityActive)
											   ? static_cast<uint32_t>(TemporalFlagBits::Active | TemporalFlagBits::Alive)
											   : static_cast<uint32_t>(TemporalFlagBits::Alive);

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
					batch.size(), cap->bInitialFlush ? "initial" : "incremental");
			}

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

			if (cap->bInitialFlush)
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

void ReplicationSystem::DispatchActivateJobs(uint32_t frameNumber)
{
	if (!TrinyxJobs::IsRunning()) return;

	for (uint8_t oid : ActiveOwnerIDs)
	{
		ServerClientChannel* ch = GetChannelIfActive(oid);
		if (!ch || !ch->CI) continue;
		if (ch->CI->RepState != ClientRepState::Playing) continue;
		if (ch->PendingActivations.empty()) continue;

		const uint32_t clientFrame = ch->CI->ToClientFrame(frameNumber);
		PacketHeader hdr           = ch->Channel.PrepareHeader(NetMessageType::EntityActivate, 0, clientFrame);

		struct Capture
		{
			ServerClientChannel* Channel;
			PacketHeader Header;
			std::vector<uint32_t> Handles;
		};
		auto* cap = new Capture{ ch, hdr, std::move(ch->PendingActivations) };

		TrinyxJobs::Dispatch([cap](uint32_t)
		{
			PendingPacket pkt;
			pkt.Header             = cap->Header;
			pkt.Header.PayloadSize = static_cast<uint16_t>(cap->Handles.size() * sizeof(uint32_t));
			pkt.Payload.resize(cap->Handles.size() * sizeof(uint32_t));
			std::memcpy(pkt.Payload.data(), cap->Handles.data(), pkt.Payload.size());
			pkt.Reliable = true;
			cap->Channel->SendQueue.Push(std::move(pkt));

			LOG_NET_DEBUG_F(nullptr, "[Replication] %zu EntityActivate(s) queued", cap->Handles.size());
			delete cap;
		}, &BuildCounter, TrinyxJobs::Queue::General);
	}
}

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
		auto* cap = new Capture{ ch, hdr, handles };

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
		DirtyCache.push_back({ i, record->NetworkID.Value, oid });
	}

	if (DirtyCache.empty()) return;

	// StateCorrection is expensive (fixed 64B per entity). Send it only when a
	// per-client resim is pending (immediate rollback trigger) or on a slow
	// heartbeat. EntityDelta handles every-frame state sync for the normal path.
	const bool bHeartbeat = (frameNumber - LastCorrectionHeartbeat >= CorrectionHeartbeatFrames);
	if (bHeartbeat)
	{
		LastCorrectionHeartbeat = frameNumber;
		Stats.bHeartbeatFired   = true;
	}

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
		NetFrameStats* Stats;
	};

	for (uint8_t oid : ActiveOwnerIDs)
	{
		ServerClientChannel* ch = GetChannelIfActive(oid);
		if (!ch || !ch->CI) continue;
		if (!ch->CI->bConnected || !ch->CI->bAuthoritySide) continue;

		const uint32_t clientFrame = ch->CI->ToClientFrame(frameNumber);
		const uint32_t lastAcked   = ch->CI->LastAckedClientFrame;
		const bool bClientHasResim = ResimCache[oid].delta > 0;

		if (!bClientHasResim && !bHeartbeat) continue;

		PacketHeader hdr = ch->Channel.PrepareHeader(
			NetMessageType::StateCorrection, 0, clientFrame);

		auto* cap = new CorrectionCapture{
			ch, hdr, dirtyPtr, dirtyCount, resimPtr,
			posX, posY, posZ, rotQx, rotQy, rotQz, rotQw, clientFrame, &Stats
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

			const uint16_t sentBytes = pkt.Header.PayloadSize;
			cap->Channel->SendQueue.Push(std::move(pkt));
			cap->Stats->StateCorrectionBytes.fetch_add(sentBytes, std::memory_order_relaxed);
			delete cap;
		}, &BuildCounter, TrinyxJobs::Queue::General);
	}
}

// Wire format (EntityDelta, unreliable): uint16_t EntityCount | per entity: uint32_t NetHandle,
// uint8_t ComponentCount | per component: uint8_t CacheSlot, uint8_t DeltaLen, [DeltaLen bytes].
// Baseline: ch->LastAckedSimFrame. Zero-mask or zero-component entities are omitted.
// Must run after DispatchCorrectionJobs (DirtyCache must be populated).
void ReplicationSystem::DispatchDeltaCorrectionJobs(uint32_t frameNumber)
{
	if (!TrinyxJobs::IsRunning()) return;
	if (DirtyCache.empty()) return;

	Registry* reg                     = AuthorityWorld->GetRegistry();
	ComponentCacheBase* temporalCache = reg->GetTemporalCache();
	const uint32_t ringSize           = temporalCache->GetTotalFrameCount();

	TemporalFrameHeader* currentHdr =
		temporalCache->GetFrameHeader(temporalCache->GetActiveReadFrame());
	if (!currentHdr) return;

	struct ComponentInfo
	{
		ComponentDeltaFns Fns;
	};

	std::vector<ComponentInfo> components;
	ComponentDeltaRegistry::Get().ForEach([&](ComponentTypeID, const ComponentDeltaFns& fns)
	{
		if (fns.IsValid())
			components.push_back({ fns });
	});

	if (components.empty()) return;

	const DirtyEntityInfo* dirtyPtr = DirtyCache.data();
	const uint32_t dirtyCount       = static_cast<uint32_t>(DirtyCache.size());

	for (uint8_t oid : ActiveOwnerIDs)
	{
		ServerClientChannel* ch = GetChannelIfActive(oid);
		if (!ch || !ch->CI) continue;
		if (!ch->CI->bConnected || !ch->CI->bAuthoritySide) continue;

		const uint32_t baselineSlot      = ch->LastAckedSimFrame % ringSize;
		TemporalFrameHeader* baselineHdr = temporalCache->GetFrameHeader(baselineSlot);
		if (!baselineHdr) continue;

		const uint32_t clientFrame = ch->CI->ToClientFrame(frameNumber);
		PacketHeader hdr           = ch->Channel.PrepareHeader(
			NetMessageType::EntityDelta, 0, clientFrame);

		struct DeltaCapture
		{
			ServerClientChannel* Channel;
			PacketHeader Header;
			const DirtyEntityInfo* Dirty;
			uint32_t DirtyCount;
			ComponentCacheBase* Cache;
			TemporalFrameHeader* CurrentHdr;
			TemporalFrameHeader* BaselineHdr;
			std::vector<ComponentInfo> Components;
			NetFrameStats* Stats;
		};

		auto* cap = new DeltaCapture{
			ch, hdr, dirtyPtr, dirtyCount,
			temporalCache, currentHdr, baselineHdr,
			components, &Stats
		};

		TrinyxJobs::Dispatch([cap](uint32_t)
		{
			std::vector<uint8_t> payload;
			payload.reserve(cap->DirtyCount * 12);

			payload.push_back(0);
			payload.push_back(0);
			uint16_t entityCount = 0;

			constexpr size_t MaxFields = 32;

			for (uint32_t d = 0; d < cap->DirtyCount; ++d)
			{
				const DirtyEntityInfo& de = cap->Dirty[d];
				const uint32_t idx        = de.slabIndex;

				if (!cap->Channel->IsReplicated(idx)) continue;

				const size_t entityHeaderPos = payload.size();

				payload.resize(entityHeaderPos + 5);
				uint8_t componentCount = 0;

				for (const ComponentInfo& comp : cap->Components)
				{
					// Probe field arrays — GetFieldData returns nullptr for unregistered fields.
					void* curPtrs[MaxFields];
					void* basePtrs[MaxFields];
					size_t fieldCount = 0;

					for (size_t fi = 0; fi < MaxFields; ++fi)
					{
						void* cur  = cap->Cache->GetFieldData(cap->CurrentHdr, comp.Fns.CacheSlot, fi);
						void* base = cap->Cache->GetFieldData(cap->BaselineHdr, comp.Fns.CacheSlot, fi);
						if (!cur || !base) break;
						curPtrs[fi]  = cur;
						basePtrs[fi] = base;
						++fieldCount;
					}

					if (fieldCount == 0) continue;

					ComponentDeltaCtx ctx{ curPtrs, basePtrs, idx };

					uint8_t compBuf[256];
					NetDeltaWriter w{ compBuf, 0, sizeof(compBuf) };
					comp.Fns.Serialize(ctx, w);

					if (!w.IsOk() || w.BytesWritten() == 0) continue;

					// If only mask bytes written and all zero, nothing changed — skip.
					const uint32_t maskBytes = (fieldCount <= 8) ? 1u : (fieldCount <= 16) ? 2u
																						   : 4u;
					if (w.BytesWritten() <= maskBytes)
					{
						uint32_t maskVal = 0;
						std::memcpy(&maskVal, compBuf, w.BytesWritten());
						if (maskVal == 0) continue;
					}

					const uint8_t deltaLen = static_cast<uint8_t>(
						w.BytesWritten() > 255 ? 255 : w.BytesWritten());

					payload.push_back(comp.Fns.CacheSlot);
					payload.push_back(deltaLen);
					for (uint8_t b = 0; b < deltaLen; ++b)
						payload.push_back(compBuf[b]);

					++componentCount;
				}

				if (componentCount == 0)
				{
					payload.resize(entityHeaderPos);
					continue;
				}

				std::memcpy(payload.data() + entityHeaderPos, &de.netHandleValue, 4);
				payload[entityHeaderPos + 4] = componentCount;
				++entityCount;
			}

			if (entityCount == 0)
			{
				delete cap;
				return;
			}

			std::memcpy(payload.data(), &entityCount, 2);

			const uint32_t payloadSize = static_cast<uint32_t>(payload.size());
			PendingPacket pkt;
			pkt.Header             = cap->Header;
			pkt.Header.PayloadSize = static_cast<uint16_t>(payloadSize > 65535 ? 65535 : payloadSize);
			pkt.Payload            = std::move(payload);
			pkt.Reliable           = false;

			const uint16_t sentBytes = pkt.Header.PayloadSize;
			cap->Channel->SendQueue.Push(std::move(pkt));
			cap->Stats->EntityDeltaBytes.fetch_add(sentBytes, std::memory_order_relaxed);
			cap->Stats->EntityDeltaEntityCount.fetch_add(entityCount, std::memory_order_relaxed);

			LOG_NET_DEBUG_F(nullptr, "[Replication] EntityDelta: %u entities queued", entityCount);
			delete cap;
		}, &BuildCounter, TrinyxJobs::Queue::General);
	}
}

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

// ---------------------------------------------------------------------------
// Moved out of ReplicationSystem.h
// ---------------------------------------------------------------------------

ReplicationSystem::~ReplicationSystem()
{
	if (ConstructReg) ConstructReg->ClearNetDestroyHooks();
}

void ReplicationSystem::OnConstructDestroyed(void* ctx, ConstructNetHandle handle)
{
	auto* self = static_cast<ReplicationSystem*>(ctx);
	self->PendingConstructDestroys.Push(handle.Value);
}

ConstructRef ReplicationSystem::RegisterConstructCore(ConstructRegistry* reg, void* ptr, const std::vector<EntityHandle>& viewHandles,
	uint8_t ownerID, uint16_t typeHash, int64_t prefabIDRaw)
{
	ConstructNetManifest manifest{};
	manifest.PrefabIndex = typeHash;
	manifest.NetFlags    = 0;

	const uint32_t spawnFrame = AuthorityWorld && AuthorityWorld->GetLogicThread()
									? AuthorityWorld->GetLogicThread()->GetLastCompletedFrame()
									: 0;

	ConstructRef ref = reg->AllocateNetRef(ptr, ownerID, manifest, typeHash, prefabIDRaw, spawnFrame);

	// Assign net handles for any view entity that doesn't have one yet.
	Registry* entityReg = AuthorityWorld ? AuthorityWorld->GetRegistry() : nullptr;
	std::vector<uint32_t> netHandleValues(viewHandles.size(), 0);
	if (entityReg)
	{
		for (size_t i = 0; i < viewHandles.size(); ++i)
		{
			GlobalEntityHandle gH = entityReg->GlobalEntityRegistry.LookupGlobalHandle(viewHandles[i]);
			if (gH.GetIndex() == 0) continue;
			EntityRecord* entRec = entityReg->GlobalEntityRegistry.Records[gH.GetIndex()];
			if (!entRec) continue;
			if (entRec->NetworkID.NetIndex == 0) entRec->NetworkID = AssignNetHandle(entityReg, gH, ownerID);
			netHandleValues[i] = entRec->NetworkID.Value;
		}
	}

	const uint8_t viewCount  = static_cast<uint8_t>(viewHandles.size());
	const size_t payloadSize = sizeof(ConstructSpawnPayload) + viewCount * sizeof(uint32_t);
	std::vector<uint8_t> buf(payloadSize, 0);
	auto* payload       = reinterpret_cast<ConstructSpawnPayload*>(buf.data());
	payload->Handle     = ref.Handle.Value;
	payload->Manifest   = manifest.Value;
	payload->SpawnFrame = spawnFrame;
	payload->ViewCount  = viewCount;
	uint32_t* trailing  = reinterpret_cast<uint32_t*>(buf.data() + sizeof(ConstructSpawnPayload));
	for (uint8_t i = 0; i < viewCount; ++i)
		trailing[i] = netHandleValues[i];

	PendingConstructSpawns.push_back(std::move(buf));

	reg->SetNetDestroyHook(ptr, ref.Handle, &ReplicationSystem::OnConstructDestroyed, this);
	ConstructReg = reg;

	LOG_ENG_INFO_F("[Replication] RegisterConstruct: ownerID=%u typeHash=%u netIndex=%u views=%u",
		ownerID, typeHash, ref.Handle.NetIndex, viewCount);
	return ref;
}

/// Frees nodes that were pushed but never drained.
ReplicationSystem::ConstructDestroyQueue::~ConstructDestroyQueue()
{
	Node* list = Head.exchange(nullptr, std::memory_order_acquire);
	while (list)
	{
		Node* next = list->Next;
		delete list;
		list = next;
	}
}

// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks) — ownership passes to Head via the CAS.
void ReplicationSystem::ConstructDestroyQueue::Push(uint32_t value)
{
	Node* node = new Node{ value, nullptr };
	Node* prev = Head.load(std::memory_order_relaxed);
	do
	{
		node->Next = prev;
	} while (!Head.compare_exchange_weak(prev, node,
		std::memory_order_release, std::memory_order_relaxed));
}
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

uint32_t ReplicationSystem::ConstructDestroyQueue::Drain(std::vector<uint32_t>& out)
{
	Node* list     = Head.exchange(nullptr, std::memory_order_acquire);
	Node* reversed = nullptr;
	while (list)
	{
		Node* next = list->Next;
		list->Next = reversed;
		reversed   = list;
		list       = next;
	}
	uint32_t count = 0;
	while (reversed)
	{
		out.push_back(reversed->Value);
		Node* next = reversed->Next;
		delete reversed;
		reversed = next;
		++count;
	}
	return count;
}
