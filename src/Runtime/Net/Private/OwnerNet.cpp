#include "OwnerNet.h"

#include "CacheSlotMeta.h"
#include "EntityBuilder.h"
#include "CColor.h"
#include "CMeshRef.h"
#include "CScale.h"
#include "CTransform.h"
#include "CVisualTransform.h"
#include "ConstructRegistry.h"
#include "EngineConfig.h"
#include "FlowManagerBase.h"
#include "FlowState.h"
#include "Input.h"
#include "InputWindowCodec.h"
#include "LogicThread.h"
#include "NetChannel.h"
#include "NetConnectionManager.h"
#include "NetDelta.h"
#include "NetTypes.h"
#include "RPC.h"
#include "Registry.h"
#include "ReplicationSystem.h"
#include "TemporalComponentCache.h"
#include "WorldBase.h"
#include "Logger.h"

#include <SDL3/SDL_timer.h>

// ---------------------------------------------------------------------------
// WriteEntitySpawnFields — write one EntitySpawnPayload into an entity's field arrays.
// Called for both write and read frames so FieldProxy sees correct data immediately.
// ---------------------------------------------------------------------------

void OwnerNet::WriteEntitySpawnFields([[maybe_unused]] Registry* reg, EntityRecord* record,
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

		auto* floatArr = static_cast<SimFloat*>(fieldBase);
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
				case 0:
					floatArr[localIdx] = payload.PosX;
					break;
				case 1:
					floatArr[localIdx] = payload.PosY;
					break;
				case 2:
					floatArr[localIdx] = payload.PosZ;
					break;
				case 3:
					floatArr[localIdx] = payload.RotQx;
					break;
				case 4:
					floatArr[localIdx] = payload.RotQy;
					break;
				case 5:
					floatArr[localIdx] = payload.RotQz;
					break;
				case 6:
					floatArr[localIdx] = payload.RotQw;
					break;
				default: break;
			}
		}
		else if (compID == CScale<>::StaticTypeID())
		{
			switch (fdesc.componentSlotIndex)
			{
				case 0:
					floatArr[localIdx] = payload.ScaleX;
					break;
				case 1:
					floatArr[localIdx] = payload.ScaleY;
					break;
				case 2:
					floatArr[localIdx] = payload.ScaleZ;
					break;
				default: break;
			}
		}
		else if (compID == CColor<>::StaticTypeID())
		{
			switch (fdesc.componentSlotIndex)
			{
				case 0:
					floatArr[localIdx] = payload.ColorR;
					break;
				case 1:
					floatArr[localIdx] = payload.ColorG;
					break;
				case 2:
					floatArr[localIdx] = payload.ColorB;
					break;
				case 3:
					floatArr[localIdx] = payload.ColorA;
					break;
				default: break;
			}
		}
		else if (compID == CMeshRef<>::StaticTypeID())
		{
			if (fdesc.componentSlotIndex == 0) uintArr[localIdx] = payload.MeshID;
		}
		else if (compID == CVisualTransform<>::StaticTypeID())
		{
			switch (fdesc.componentSlotIndex)
			{
				case 0: floatArr[localIdx] = payload.PosX; break;
				case 1: floatArr[localIdx] = payload.PosY; break;
				case 2: floatArr[localIdx] = payload.PosZ; break;
				default: break;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// HandleEntitySpawn — create an entity on the client from a received payload.
// ---------------------------------------------------------------------------

void OwnerNet::HandleEntitySpawn(Registry* reg, const EntitySpawnPayload& payload, [[maybe_unused]] uint32_t frame)
{
	EntityNetHandle receivedNetHandle{};
	receivedNetHandle.Value = payload.NetHandle;

	EntityNetManifest manifest{};
	manifest.Value    = payload.Manifest;
	ClassID classType = static_cast<ClassID>(manifest.ClassType);

	GlobalEntityHandle gHandle;
	reg->CreateInternal(classType, { &gHandle, 1 });
	if (gHandle.GetIndex() == 0)
	{
		LOG_ENG_WARN_F("[Replication] Failed to create entity ClassID %u", classType);
		return;
	}

	reg->GlobalEntityRegistry.NetToRecord.set(receivedNetHandle.GetHandleIndex(), gHandle);

	EntityRecord* record = reg->GlobalEntityRegistry.Records[gHandle.GetIndex()];
	if (!record || !record->IsValid())
	{
		LOG_ENG_WARN("[Replication] Entity created but record invalid");
		return;
	}
	record->NetworkID = receivedNetHandle;

	Archetype* arch   = record->Arch;
	Chunk* chunk      = record->TargetChunk;
	uint32_t localIdx = record->LocalIndex;

	// Apply entity-type defaults (e.g. VisBlend) before writing the replicated fields.
	// Mirrors the InitializeInternal() call in Registry::Create<T> which this low-level
	// path bypasses. Run for both write and read frames so all ring slots see correct defaults.
	if (InitFunc initFn = ReflectionRegistry::Get().EntityGetters[classType].Initialize)
	{
		void* initTable[MAX_FIELDS_PER_ARCHETYPE];
		arch->BuildFieldArrayTable(chunk, initTable,
			reg->GetTemporalCache()->GetActiveWriteFrame(),
			reg->GetVolatileCache()->GetActiveWriteFrame());
		initFn(initTable, initTable[0], localIdx);

		arch->BuildFieldArrayTable(chunk, initTable,
			reg->GetTemporalCache()->GetActiveReadFrame(),
			reg->GetVolatileCache()->GetActiveReadFrame());
		initFn(initTable, initTable[0], localIdx);
	}

#ifdef TNX_ENABLE_ROLLBACK
	// Skip writing fields into the current frame — the entity's initial state belongs at
	// spawnFrame in the ring buffer, not at the current frame. EnqueueSpawnRollback (called
	// in FlushDeferredEntitySpawns) drives a resim to spawnFrame where GetActiveWriteFrame()
	// points to the correct historical slot; PropagateFrameResim then fills forward.
	{
		GlobalEntityHandle capturedGH      = gHandle;
		EntitySpawnPayload capturedPayload = payload;
		reg->PushServerEvent({ frame,
			[capturedGH, capturedPayload, reg]()
		{
			EntityRecord* rec = reg->GlobalEntityRegistry.Records[capturedGH.GetIndex()];
			if (!rec || !rec->IsValid()) return;
			WriteEntitySpawnFields(reg, rec, capturedPayload,
				reg->GetTemporalCache()->GetActiveWriteFrame(),
				reg->GetVolatileCache()->GetActiveWriteFrame());
			// Set DirtiedFrame so PropagateFrameResim scatter-copies this entity forward.
			ComponentCacheBase* c      = reg->GetTemporalCache();
			TemporalFrameHeader* wHdr  = c->GetFrameHeader(c->GetActiveWriteFrame());
			const ComponentTypeID slot = CacheSlotMeta<>::StaticTemporalIndex();
			auto* wFlags               = static_cast<int32_t*>(c->GetFieldData(wHdr, slot, 0));
			if (wFlags)
			{
				const uint32_t slabIdx = static_cast<uint32_t>(rec->CacheEntityIndex);
				wFlags[slabIdx] |= static_cast<int32_t>(TemporalFlagBits::Dirty)
								   | static_cast<int32_t>(TemporalFlagBits::DirtiedFrame);
			}
		} });
	}
#else
	// Write into both write and read frames so FieldProxy reads see correct data immediately.
	WriteEntitySpawnFields(reg, record, payload,
		reg->GetTemporalCache()->GetActiveWriteFrame(),
		reg->GetVolatileCache()->GetActiveWriteFrame());
	WriteEntitySpawnFields(reg, record, payload,
		reg->GetTemporalCache()->GetActiveReadFrame(),
		reg->GetVolatileCache()->GetActiveReadFrame());

	// WriteEntitySpawnFields bypasses FieldProxy so Dirty|DirtiedFrame are never ORed in.
	// Set them explicitly on the write frame so the GPU dirty-bit upload sees this entity.
	{
		ComponentCacheBase* cache       = reg->GetTemporalCache();
		TemporalFrameHeader* writeHdr   = cache->GetFrameHeader(cache->GetActiveWriteFrame());
		const ComponentTypeID flagsSlot = CacheSlotMeta<>::StaticTemporalIndex();
		auto* writeFlags                = static_cast<int32_t*>(cache->GetFieldData(writeHdr, flagsSlot, 0));
		if (writeFlags)
		{
			const uint32_t slabIdx = static_cast<uint32_t>(record->CacheEntityIndex);
			writeFlags[slabIdx] |= static_cast<int32_t>(TemporalFlagBits::Dirty)
								   | static_cast<int32_t>(TemporalFlagBits::DirtiedFrame);
		}
	}
#endif
}

// ---------------------------------------------------------------------------
// HandleEntityActivate — flip Active on pre-spawned alive entities.
// Called on the Logic thread via a Post() lambda.
// ---------------------------------------------------------------------------

void OwnerNet::HandleEntityActivate(Registry* reg, const uint32_t* netHandles, uint32_t count, [[maybe_unused]] uint32_t frame)
{
	ComponentCacheBase* cache       = reg->GetTemporalCache();
	TemporalFrameHeader* writeHdr   = cache->GetFrameHeader(cache->GetActiveWriteFrame());
	TemporalFrameHeader* readHdr    = cache->GetFrameHeader(cache->GetActiveReadFrame());
	const ComponentTypeID flagsSlot = CacheSlotMeta<>::StaticTemporalIndex();

	auto* writeFlags = static_cast<int32_t*>(cache->GetFieldData(writeHdr, flagsSlot, 0));
	auto* readFlags  = static_cast<int32_t*>(cache->GetFieldData(readHdr, flagsSlot, 0));
	if (!writeFlags) return;

	const int32_t activeBit = static_cast<int32_t>(TemporalFlagBits::Active);
	const int32_t dirtyBits = static_cast<int32_t>(TemporalFlagBits::Dirty)
							  | static_cast<int32_t>(TemporalFlagBits::DirtiedFrame);

	for (uint32_t k = 0; k < count; ++k)
	{
		EntityNetHandle nh{};
		nh.Value             = netHandles[k];
		EntityRecord* record = reg->GlobalEntityRegistry.GetRecordPtr(nh);
		if (!record || !record->IsValid()) continue;

		const uint32_t slabIdx = static_cast<uint32_t>(record->CacheEntityIndex);
		writeFlags[slabIdx] |= activeBit | dirtyBits;
		if (readFlags) readFlags[slabIdx] |= activeBit;
	}

#ifdef TNX_ENABLE_ROLLBACK
	{
		std::vector<uint32_t> capturedHandles(netHandles, netHandles + count);
		reg->PushAppliedServerEvent([reg, capturedHandles = std::move(capturedHandles)]()
		{
			ComponentCacheBase* c      = reg->GetTemporalCache();
			TemporalFrameHeader* wHdr  = c->GetFrameHeader(c->GetActiveWriteFrame());
			const ComponentTypeID slot = CacheSlotMeta<>::StaticTemporalIndex();
			auto* wFlags               = static_cast<int32_t*>(c->GetFieldData(wHdr, slot, 0));
			if (!wFlags) return;
			const int32_t bits = static_cast<int32_t>(TemporalFlagBits::Active)
								 | static_cast<int32_t>(TemporalFlagBits::Dirty)
								 | static_cast<int32_t>(TemporalFlagBits::DirtiedFrame);
			for (uint32_t nhVal : capturedHandles)
			{
				EntityNetHandle nh{};
				nh.Value          = nhVal;
				EntityRecord* rec = reg->GlobalEntityRegistry.GetRecordPtr(nh);
				if (!rec || !rec->IsValid()) continue;
				wFlags[static_cast<uint32_t>(rec->CacheEntityIndex)] |= bits;
			}
		});
	}
#endif
}

// ---------------------------------------------------------------------------
// HandleStateCorrections — apply authoritative transforms on the client.
// ---------------------------------------------------------------------------

void OwnerNet::HandleStateCorrections(Registry* reg, const StateCorrectionEntry* entries,
	uint32_t count, [[maybe_unused]] uint32_t clientFrame,
	[[maybe_unused]] WorldBase* world, [[maybe_unused]] uint32_t LastAckedFrame)
{
#ifdef TNX_ENABLE_ROLLBACK
	constexpr SimFloat kDivergenceThresholdSq = SimFloat(0.01f * 0.01f);

	const auto* temporal      = reg->GetTemporalCache();
	const uint32_t ringSize   = temporal->GetTotalFrameCount();
	const uint32_t currentF   = temporal->GetFrameHeader()->FrameNumber;
	const uint32_t oldestSlab = (currentF >= ringSize - 1) ? (currentF - (ringSize - 1)) : 0u;

	if (clientFrame < oldestSlab)
	{
		LOG_ENG_DEBUG_F("[Replication] Skipping stale StateCorrection: frame=%u (oldest=%u, ring depth=%u)",
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

		if (entry.ResimFrameDelta > 0 && clientFrame >= entry.ResimFrameDelta)
		{
			const uint32_t clientResimFrame = clientFrame - entry.ResimFrameDelta;
			if (clientResimFrame >= oldestSlab)
			{
				void* resimFieldTable[MAX_FIELDS_PER_ARCHETYPE];
				arch->BuildFieldArrayTable(chunk, resimFieldTable, clientResimFrame, volatileFrame);

				SimFloat resimX = 0.f, resimY = 0.f, resimZ = 0.f;
				for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
				{
					if (fdesc.componentID != CTransform<>::StaticTypeID()) continue;
					void* base = resimFieldTable[fdesc.fieldSlotIndex];
					if (!base) continue;
					auto* fa = static_cast<SimFloat*>(base);
					switch (fdesc.componentSlotIndex)
					{
						case 0:
							resimX = fa[localIdx];
							break;
						case 1:
							resimY = fa[localIdx];
							break;
						case 2:
							resimZ = fa[localIdx];
							break;
						default: break;
					}
				}

				const SimFloat rdx = resimX - entry.ResimPosX;
				const SimFloat rdy = resimY - entry.ResimPosY;
				const SimFloat rdz = resimZ - entry.ResimPosZ;
				if (rdx * rdx + rdy * rdy + rdz * rdz > kDivergenceThresholdSq)
				{
					LOG_ENG_WARN_F("[Replication] ResimRoot divergence: netHandle=%u resimFrame=%u dist=%.4fm",
						entry.NetHandle, clientResimFrame,
						Sqrt(rdx * rdx + rdy * rdy + rdz * rdz).ToDouble());
					bPushCorrection = true;
				}
			}
		}

		void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
		arch->BuildFieldArrayTable(chunk, fieldArrayTable, clientFrame, volatileFrame);

		SimFloat predictedX = 0.f, predictedY = 0.f, predictedZ = 0.f;
		for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
		{
			if (fdesc.componentID != CTransform<>::StaticTypeID()) continue;
			void* base = fieldArrayTable[fdesc.fieldSlotIndex];
			if (!base) continue;
			auto* fa = static_cast<SimFloat*>(base);
			switch (fdesc.componentSlotIndex)
			{
				case 0:
					predictedX = fa[localIdx];
					break;
				case 1:
					predictedY = fa[localIdx];
					break;
				case 2:
					predictedZ = fa[localIdx];
					break;
				default: break;
			}
		}

		const SimFloat dx = predictedX - entry.PosX;
		const SimFloat dy = predictedY - entry.PosY;
		const SimFloat dz = predictedZ - entry.PosZ;
		if (dx * dx + dy * dy + dz * dz > kDivergenceThresholdSq)
		{
			LOG_ENG_WARN_F("[Replication] Divergence: netHandle=%u frame=%u dist=%.4fm",
				entry.NetHandle, clientFrame, Sqrt(dx * dx + dy * dy + dz * dz).ToDouble());
			bPushCorrection = true;
		}

		if (bPushCorrection)
		{
			if (entry.ResimFrameDelta > 0)
			{
				corrections.push_back({ entry.NetHandle, clientFrame - entry.ResimFrameDelta,
					entry.ResimPosX, entry.ResimPosY, entry.ResimPosZ,
					entry.ResimRotQx, entry.ResimRotQy, entry.ResimRotQz, entry.ResimRotQw });
			}
			else if (clientFrame < currentF)
			{
				corrections.push_back({ entry.NetHandle, clientFrame,
					entry.PosX, entry.PosY, entry.PosZ,
					entry.RotQx, entry.RotQy, entry.RotQz, entry.RotQw });
			}
			else
			{
				predictedCorrections.push_back({ entry.NetHandle, clientFrame,
					entry.PosX, entry.PosY, entry.PosZ,
					entry.RotQx, entry.RotQy, entry.RotQz, entry.RotQw });
			}
		}
	}

	if (!corrections.empty() && world)
	{
		uint32_t earliest = UINT32_MAX;
		for (const auto& c : corrections)
			earliest = std::min(earliest, c.ClientFrame);
		world->EnqueueCorrections(std::move(corrections), earliest);
	}

	if (!predictedCorrections.empty() && world) world->EnqueuePredictedCorrections(std::move(predictedCorrections));

#else
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

		Registry::WriteEntityTransformFields(fieldArrayTable, arch, localIdx,
			entry.PosX, entry.PosY, entry.PosZ,
			entry.RotQx, entry.RotQy, entry.RotQz, entry.RotQw);
	}
#endif
}

// ---------------------------------------------------------------------------
// HandleEntityDelta — apply per-component delta corrections from the Authority.
//
// Wire format mirrors DispatchDeltaCorrectionJobs in ReplicationSystem.cpp:
//   uint16_t EntityCount
//   For each entity:
//     uint32_t NetHandle
//     uint8_t  ComponentCount
//     For each changed component:
//       uint8_t  CacheSlot   (StaticTemporalIndex of the component)
//       uint8_t  DeltaLen    (byte count of the delta payload)
//       [DeltaLen bytes]     (field-presence mask + changed values)
//
// Applied directly to the current temporal write frame. Unknown entities and
// unregistered components are skipped; exactly DeltaLen bytes are always consumed.
// ---------------------------------------------------------------------------

void OwnerNet::HandleEntityDelta(Registry* reg, const uint8_t* payload, uint32_t size)
{
	if (size < 2) return;

	NetDeltaReader pkt{ payload, 0, size };
	const uint16_t entityCount = pkt.ReadU16();
	if (entityCount == 0 || !pkt.IsOk()) return;

	ComponentCacheBase* cache     = reg->GetTemporalCache();
	TemporalFrameHeader* writeHdr = cache->GetFrameHeader(cache->GetActiveWriteFrame());
	if (!writeHdr) return;

	// Build CacheSlot → fns lookup once per message.
	constexpr size_t MaxCacheSlots                 = 256;
	const ComponentDeltaFns* bySlot[MaxCacheSlots] = {};
	ComponentDeltaRegistry::Get().ForEach([&](ComponentTypeID, const ComponentDeltaFns& fns)
	{
		bySlot[fns.CacheSlot] = &fns;
	});

	constexpr size_t MaxFields = 32;

	for (uint16_t e = 0; e < entityCount; ++e)
	{
		const uint32_t netHandleVal = pkt.ReadU32();
		const uint8_t compCount     = pkt.ReadU8();
		if (!pkt.IsOk()) break;

		EntityNetHandle nh{};
		nh.Value = netHandleVal;

		GlobalEntityHandle gHandle = reg->GlobalEntityRegistry.LookupGlobalHandle(nh);
		EntityRecord* record       = nullptr;
		if (gHandle.GetIndex() != 0)
		{
			record = reg->GlobalEntityRegistry.Records[gHandle.GetIndex()];
			if (record && !record->IsValid()) record = nullptr;
		}

		for (uint8_t c = 0; c < compCount; ++c)
		{
			const uint8_t cacheSlot = pkt.ReadU8();
			const uint8_t deltaLen  = pkt.ReadU8();
			if (!pkt.IsOk()) break;

			const uint32_t compDataStart = pkt.Pos;

			if (!record || !bySlot[cacheSlot])
			{
				// Unknown entity or unregistered component — skip payload bytes.
				pkt.Pos = compDataStart + deltaLen;
				continue;
			}

			const ComponentDeltaFns* fns = bySlot[cacheSlot];
			const uint32_t slabIdx       = record->CacheEntityIndex;

			void* fieldPtrs[MaxFields];
			size_t fieldCount = 0;
			for (size_t fi = 0; fi < MaxFields; ++fi)
			{
				void* ptr = cache->GetFieldData(writeHdr, cacheSlot, fi);
				if (!ptr) break;
				fieldPtrs[fi] = ptr;
				++fieldCount;
			}

			if (fieldCount > 0)
			{
				NetDeltaReader compReader{ payload + compDataStart, 0, deltaLen };
				ComponentDeltaCtx ctx{ fieldPtrs, nullptr, slabIdx };
				if (!fns->Deserialize(ctx, compReader))
					LOG_ENG_WARN_F("[ClientNet] EntityDelta: deserialize error entity=%u slot=%u",
						netHandleVal, cacheSlot);
			}

			// Always consume exactly DeltaLen bytes regardless of deserialize outcome.
			pkt.Pos = compDataStart + deltaLen;
		}

		if (record)
		{
			const ComponentTypeID flagsSlot = CacheSlotMeta<>::StaticTemporalIndex();
			auto* flags                     = static_cast<int32_t*>(cache->GetFieldData(writeHdr, flagsSlot, 0));
			if (flags)
				flags[record->CacheEntityIndex] |= static_cast<int32_t>(TemporalFlagBits::Dirty)
												   | static_cast<int32_t>(TemporalFlagBits::DirtiedFrame);
		}

		if (!pkt.IsOk()) break;
	}
}

// ---------------------------------------------------------------------------
// HandleConstructSpawn — create a client Construct from a received payload.
// Returns false if any entity view is not yet in the client registry (defer + retry).
// ---------------------------------------------------------------------------

bool OwnerNet::HandleConstructSpawn(ConstructRegistry* reg, Registry* entityReg,
	WorldBase* clientWorld, const uint8_t* data, size_t len)
{
	if (len < sizeof(ConstructSpawnPayload))
	{
		LOG_NET_WARN(nullptr, "[Replication] HandleConstructSpawn: payload too small");
		return true;
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

	FlowManagerBase* flow = clientWorld ? clientWorld->GetFlowManager() : nullptr;
	Soul* soul            = flow ? flow->GetSoul(ownerID) : nullptr;

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

	const ReflectionRegistry::ConstructClientFactory factory =
		ReflectionRegistry::Get().FindConstructClientFactory(typeHash);

	if (!factory)
	{
		LOG_NET_WARN_F(soul, "[Replication] HandleConstructSpawn: no factory for typeHash=%u", typeHash);
		return true;
	}

	if (!soul && flow) soul = flow->EnsureEchoSoul(ownerID);

	void* raw = factory(reg, clientWorld, resolvedHandles, resolvedCount, soul);
	if (!raw)
	{
		LOG_NET_WARN(soul, "[Replication] HandleConstructSpawn: factory returned null");
		return true;
	}

	ConstructNetHandle serverHandle{};
	serverHandle.Value = header->Handle;

	ConstructNetManifest wireManifest{};
	wireManifest.PrefabIndex = typeHash;

	ConstructRef ref = reg->WireNetRef(raw, serverHandle, wireManifest, typeHash, header->SpawnFrame);

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

void OwnerNet::HandleMessage(const ReceivedMessage& msg)
{
	// Every server-sent header carries the last client input frame it consumed.
	// Advance LastServerAckedFrame so TickInputSend() widens from there, not from last send.
	if (msg.Header.AckedClientFrame > 0)
	{
		if (ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection))
		{
			if (msg.Header.AckedClientFrame > ci->LastServerAckedFrame)
			{
				ci->LastServerAckedFrame = msg.Header.AckedClientFrame;
			}
		}
	}

	auto type = static_cast<NetMessageType>(msg.Header.Type);

	switch (type)
	{
		case NetMessageType::InputFrame:
		{
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
				const SimFloat rtt  = SimFloat(static_cast<uint16_t>(now - sent));
				if (ci->RTT_ms <= 0.0f)
					ci->RTT_ms = rtt;
				else
					ci->RTT_ms = ci->RTT_ms * SimFloat(0.875f) + rtt * SimFloat(0.125f);

				if (ci->bOwnerInitiated
					&& ci->RepState == ClientRepState::Synchronizing
					&& ci->ClockSyncProbesRecvd < 8)
				{
					ci->ClockSyncProbesRecvd++;
				}
			}
			break;
		}

		case NetMessageType::ConnectionHandshake:
		{
			ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
			if (!ci)
			{
				LOG_ENG_WARN_F("[ClientNet] ConnectionHandshake from unknown connection %u", msg.Connection);
				break;
			}

			if (msg.Header.SenderID == 0)
			{
				LOG_ENG_WARN("[ClientNet] Invalid HandshakeAccept — SenderID is 0");
				break;
			}

			ConnectionMgr->AssignOwnerID(msg.Connection, msg.Header.SenderID);

			// Promote the world from the temporary slot-0 registration (set in
			// PIENetThread::AddClient) to the real ownerID slot so subsequent handlers
			// (TravelNotify, FlowEvent/ServerReady) can find it before
			// PIENetThread::UpdateClientOwnerID is called from the startup loop.
			// PIE serializes connections so this is race-free. Concurrent production
			// connections are not safe here — addressed when GNS multi-connection support lands.
			if (WorldMap[msg.Header.SenderID] == nullptr && WorldMap[0] != nullptr) WorldMap[msg.Header.SenderID] = WorldMap[0];

			if (msg.Payload.size() >= sizeof(HandshakePayload))
			{
				const auto* hsPay          = reinterpret_cast<const HandshakePayload*>(msg.Payload.data());
				ci->ServerFrameAtHandshake = hsPay->ServerFrame;

				// Record our own frame so we can translate local frame numbers to
				// server-relative frames when building InputFrame packets.
				WorldBase* w                    = WorldMap[msg.Header.SenderID];
				ci->ClientLocalFrameAtHandshake = (w && w->GetLogicThread())
													  ? w->GetLogicThread()->GetLastCompletedFrame()
													  : 0;
				if (w) w->SetServerFrameOffset(ci->GetFrameOffset());
			}
			ci->RepState = ClientRepState::Synchronizing;

			{
				WorldBase* origWorld = WorldMap[msg.Header.SenderID];
				if (origWorld && origWorld->GetFlowManager()) origWorld->GetFlowManager()->OnLocalOwnerConnected(msg.Header.SenderID);
				Soul* soul = (origWorld && origWorld->GetFlowManager()) ? origWorld->GetFlowManager()->GetSoul(msg.Header.SenderID) : nullptr;
				LOG_NET_INFO_F(soul, "[ClientNet] HandshakeAccept received — OwnerID=%u serverFrame=%u",
					msg.Header.SenderID, ci->ServerFrameAtHandshake);
			}
			break;
		}

		case NetMessageType::ClockSync:
		{
			ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
			if (!ci) break;

			if (msg.Payload.size() < sizeof(ClockSyncPayload))
			{
				LOG_ENG_WARN_F("[ClientNet] ClockSync payload too small (%zu)", msg.Payload.size());
				break;
			}

			const uint32_t tickRate = (Config->FixedUpdateHz == EngineConfig::Unset)
										  ? 128u
										  : static_cast<uint32_t>(Config->FixedUpdateHz);
			const float stepMs      = 1000.0f / static_cast<float>(tickRate);
			// InputLead is kept on ConnectionInfo for diagnostics but is no longer used
			// to offset packet frame tags — the logic thread stamps each frame exactly.
			ci->InputLead = static_cast<uint32_t>(ci->RTT_ms.ToFloat() * 0.5f / stepMs) + 2u;

			// Guard: ClockSync (unreliable) can arrive after TravelNotify (reliable).
			if (ci->RepState == ClientRepState::Synchronizing)
			{
				ci->RepState         = ClientRepState::Loading;
				WorldBase* origWorld = WorldMap[ci->OwnerID];
				Soul* soul           = (origWorld && origWorld->GetFlowManager()) ? origWorld->GetFlowManager()->GetSoul(ci->OwnerID) : nullptr;
				LOG_NET_INFO_F(soul, "[ClientNet] ClockSync complete — InputLead=%u RTT=%.1fms → Loading",
					ci->InputLead, ci->RTT_ms.ToFloat());
			}
			else
			{
				WorldBase* origWorld = WorldMap[ci->OwnerID];
				Soul* soul           = (origWorld && origWorld->GetFlowManager()) ? origWorld->GetFlowManager()->GetSoul(ci->OwnerID) : nullptr;
				LOG_NET_WARN_F(soul,
					"[ClientNet] ClockSync arrived late (RepState=%d, already past Synchronizing) — "
					"InputLead=%u updated, state unchanged",
					static_cast<int>(ci->RepState), ci->InputLead);
			}
			break;
		}

		case NetMessageType::TravelNotify:
		{
			ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
			if (!ci) break;

			if (msg.Payload.size() < sizeof(TravelPayload))
			{
				LOG_ENG_WARN_F("[ClientNet] TravelNotify payload too small (%zu)", msg.Payload.size());
				break;
			}

			const auto* travelMsg = reinterpret_cast<const TravelPayload*>(msg.Payload.data());
			ci->RepState          = ClientRepState::LevelLoading;

			{
				WorldBase* clientWorld = WorldMap[ci->OwnerID];
				FlowManagerBase* flow  = clientWorld ? clientWorld->GetFlowManager() : nullptr;
				Soul* soul             = flow ? flow->GetSoul(ci->OwnerID) : nullptr;
				LOG_NET_INFO_F(soul, "[ClientNet] TravelNotify received — loading level '%s'", travelMsg->LevelPath);
				if (flow) flow->PostTravelNotify(travelMsg->LevelPath);
			}
			// LevelReady is deferred — AcknowledgeLevelReady() fires once the
			// StreamingManager completes the background load batch.
			break;
		}

		case NetMessageType::FlowEvent:
		{
			if (msg.Payload.size() < sizeof(FlowEventPayload))
			{
				LOG_ENG_WARN_F("[ClientNet] FlowEvent payload too small (%zu)", msg.Payload.size());
				break;
			}

			const auto* ev     = reinterpret_cast<const FlowEventPayload*>(msg.Payload.data());
			ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
			if (!ci) break;

			{
				WorldBase* clientWorld = WorldMap[ci->OwnerID];
				FlowManagerBase* flow  = clientWorld ? clientWorld->GetFlowManager() : nullptr;
				Soul* soul             = flow ? flow->GetSoul(ci->OwnerID) : nullptr;

				if (ci->RepState == ClientRepState::LevelLoaded
					&& ev->EventID == static_cast<uint8_t>(FlowEventID::ServerReady))
				{
					ci->RepState = ClientRepState::Loaded;

					// Flush deferred spawns so entities exist before the Alive→Active sweep.
					FlushDeferredEntitySpawns();
					{
						auto it = DeferredConstructSpawns.begin();
						while (it != DeferredConstructSpawns.end())
						{
							if (TrySpawnDeferred(*it))
								it = DeferredConstructSpawns.erase(it);
							else
								++it;
						}
					}

					if (clientWorld)
					{
						Registry* sweepReg = clientWorld->GetRegistry();
						clientWorld->PostAndWait([sweepReg](uint32_t)
						{
							sweepReg->SweepAliveFlagsToActive();
#ifdef TNX_ENABLE_ROLLBACK
							sweepReg->PushAppliedServerEvent([sweepReg]()
							{
								sweepReg->SweepAliveFlagsToActive();
							});
#endif
						});
					}

					if (flow) flow->SendPlayerBeginRequest(NetChannel(ci, ConnectionMgr), msg.Header.FrameNumber, ci->Predictions);
					ci->PlayerBeginSentAt = SDL_GetTicks();

					LOG_NET_INFO(soul, "[ClientNet] FlowEvent::ServerReady → Loaded");
				}
				else if (ev->EventID == static_cast<uint8_t>(FlowEventID::ServerReady))
				{
					LOG_NET_WARN_F(soul,
						"[ClientNet] FlowEvent::ServerReady in unexpected RepState=%d "
						"(expected LevelLoaded=%d) -- player begin skipped",
						static_cast<int>(ci->RepState),
						static_cast<int>(ClientRepState::LevelLoaded));
				}

				if (flow) flow->PostNetEvent(ev->EventID);
			}
			break;
		}

		case NetMessageType::EntitySpawn:
		{
			if (msg.Payload.size() < sizeof(EntitySpawnPayload) || msg.Payload.size() % sizeof(EntitySpawnPayload) != 0)
			{
				LOG_ENG_WARN_F("[ClientNet] EntitySpawn payload bad size (%zu)", msg.Payload.size());
				break;
			}

			ConnectionInfo* ci    = ConnectionMgr->FindConnection(msg.Connection);
			const uint8_t ownerID = ci ? ci->OwnerID : 0;
			if (!WorldMap[ownerID])
			{
				LOG_ENG_WARN_F("[ClientNet] EntitySpawn but no client world for OwnerID %u", ownerID);
				break;
			}

			// Defer to TickReplication — never block the fast-path message loop.
			DeferredEntitySpawn deferred{ ownerID, msg.Header.FrameNumber, msg.Payload };
			DeferredEntitySpawns.push_back(std::move(deferred));
			break;
		}

		case NetMessageType::ConstructSpawn:
		{
			if (msg.Payload.size() < sizeof(ConstructSpawnPayload))
			{
				LOG_ENG_WARN_F("[ClientNet] ConstructSpawn payload too small (%zu)", msg.Payload.size());
				break;
			}

			ConnectionInfo* ci     = ConnectionMgr->FindConnection(msg.Connection);
			const uint8_t ownerID  = ci ? ci->OwnerID : 0;
			WorldBase* clientWorld = WorldMap[ownerID];
			if (!clientWorld)
			{
				LOG_ENG_WARN_F("[ClientNet] ConstructSpawn but no client world for OwnerID %u", ownerID);
				break;
			}

			DeferredConstructSpawn deferred{ ownerID, msg.Header.FrameNumber, msg.Payload };

			// Try immediately — if entities aren't ready yet, push to deferred queue.
			if (!TrySpawnDeferred(deferred)) DeferredConstructSpawns.push_back(std::move(deferred));
			break;
		}

		case NetMessageType::StateCorrection:
		{
			if (msg.Payload.size() < sizeof(StateCorrectionEntry))
			{
				LOG_ENG_WARN("[ClientNet] StateCorrection payload too small");
				break;
			}

			ConnectionInfo* ci     = ConnectionMgr->FindConnection(msg.Connection);
			const uint8_t ownerID  = ci ? ci->OwnerID : 0;
			WorldBase* clientWorld = WorldMap[ownerID];
			if (!clientWorld || !ci) break;

			const uint32_t clientFrame = msg.Header.FrameNumber;

			const uint32_t entryCount = static_cast<uint32_t>(
				msg.Payload.size() / sizeof(StateCorrectionEntry));
			const auto* entries = reinterpret_cast<const StateCorrectionEntry*>(msg.Payload.data());

			Registry* corrReg = clientWorld->GetRegistry();

			// Heap-allocate so the fire-and-forget Post lambda can safely outlive this scope.
			// The lambda owns the vector and deletes it after use.
			struct CorrCapture
			{
				Registry* reg;
				WorldBase* world;
				std::vector<StateCorrectionEntry>* corrs;
				uint32_t clientFrame;
				uint32_t LastAckedFrame;
			};
			static_assert(sizeof(CorrCapture) <= 48, "CorrCapture exceeds job payload limit");

			auto* corrHeap = new std::vector<StateCorrectionEntry>(entries, entries + entryCount);
			CorrCapture cap{ corrReg, clientWorld, corrHeap, clientFrame, ci->LastServerAckedFrame };
			clientWorld->Post([cap](uint32_t)
			{
				HandleStateCorrections(cap.reg, cap.corrs->data(),
					static_cast<uint32_t>(cap.corrs->size()), cap.clientFrame, cap.world, cap.LastAckedFrame);
				delete cap.corrs;
			});
			break;
		}

		case NetMessageType::EntityDestroy:
		{
			const uint32_t count = static_cast<uint32_t>(msg.Payload.size() / sizeof(uint32_t));
			if (count == 0) break;

			ConnectionInfo* ci     = ConnectionMgr->FindConnection(msg.Connection);
			const uint8_t ownerID  = ci ? ci->OwnerID : 0;
			WorldBase* clientWorld = WorldMap[ownerID];
			if (!clientWorld) break;

			Registry* entityReg = clientWorld->GetRegistry();

			// Copy handles for the Post lambda — payload lifetime is not guaranteed.
			auto* handlesCopy = new std::vector<uint32_t>(
				reinterpret_cast<const uint32_t*>(msg.Payload.data()),
				reinterpret_cast<const uint32_t*>(msg.Payload.data()) + count);

			clientWorld->Post([entityReg, handlesCopy](uint32_t)
			{
				for (uint32_t val : *handlesCopy)
				{
					EntityNetHandle nh{};
					nh.Value             = val;
					EntityRecord* Record = entityReg->GlobalEntityRegistry.GetRecordPtr(nh);
					if (!Record) continue;
					entityReg->DestroyRecord(*Record);
				}
				delete handlesCopy;
			});
			break;
		}

		case NetMessageType::EntityActivate:
		{
			const uint32_t count = static_cast<uint32_t>(msg.Payload.size() / sizeof(uint32_t));
			if (count == 0) break;

			ConnectionInfo* ci     = ConnectionMgr->FindConnection(msg.Connection);
			const uint8_t ownerID  = ci ? ci->OwnerID : 0;
			WorldBase* clientWorld = WorldMap[ownerID];
			if (!clientWorld) break;

			Registry* entityReg = clientWorld->GetRegistry();

			auto* handlesCopy = new std::vector<uint32_t>(
				reinterpret_cast<const uint32_t*>(msg.Payload.data()),
				reinterpret_cast<const uint32_t*>(msg.Payload.data()) + count);

			const uint32_t activateFrame = msg.Header.FrameNumber;
			clientWorld->Post([entityReg, handlesCopy, activateFrame](uint32_t)
			{
				HandleEntityActivate(entityReg, handlesCopy->data(),
					static_cast<uint32_t>(handlesCopy->size()), activateFrame);
				delete handlesCopy;
			});
			break;
		}

		case NetMessageType::ConstructDestroy:
		{
			const uint32_t count = static_cast<uint32_t>(msg.Payload.size() / sizeof(uint32_t));
			if (count == 0) break;

			ConnectionInfo* ci     = ConnectionMgr->FindConnection(msg.Connection);
			const uint8_t ownerID  = ci ? ci->OwnerID : 0;
			WorldBase* clientWorld = WorldMap[ownerID];
			if (!clientWorld) break;

			ConstructRegistry* constructs = clientWorld->GetConstructRegistry();
			if (!constructs) break;

			auto* handlesCopy = new std::vector<uint32_t>(
				reinterpret_cast<const uint32_t*>(msg.Payload.data()),
				reinterpret_cast<const uint32_t*>(msg.Payload.data()) + count);

			clientWorld->Post([constructs, handlesCopy](uint32_t)
			{
				for (uint32_t val : *handlesCopy)
				{
					ConstructNetHandle nh{};
					nh.Value = val;
					constructs->DestroyByNetHandle(nh);
				}
				delete handlesCopy;
			});
			break;
		}

		case NetMessageType::SoulRPC:
		{
			ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
			if (!ci) break;

			if (msg.Payload.size() >= sizeof(RPCHeader))
			{
				const auto* rpcHdrPeek = reinterpret_cast<const RPCHeader*>(msg.Payload.data());
				LOG_ENG_INFO_F("[ClientNet] SoulRPC received: ownerID=%u methodID=%u repState=%d",
					ci->OwnerID, rpcHdrPeek->MethodID, static_cast<int>(ci->RepState));
			}

			if (msg.Payload.size() < sizeof(RPCHeader))
			{
				LOG_ENG_WARN_F("[ClientNet] SoulRPC payload too small (%zu bytes)", msg.Payload.size());
				break;
			}

			const auto* rpcHdr      = reinterpret_cast<const RPCHeader*>(msg.Payload.data());
			const uint8_t* params   = msg.Payload.data() + sizeof(RPCHeader);
			const size_t paramBytes = msg.Payload.size() - sizeof(RPCHeader);

			if (paramBytes < rpcHdr->ParamSize)
			{
				LOG_ENG_WARN_F("[ClientNet] SoulRPC param underrun (MethodID=%u, want=%u, got=%zu)",
					rpcHdr->MethodID, rpcHdr->ParamSize, paramBytes);
				break;
			}

			{
				WorldBase* clientWorld = WorldMap[ci->OwnerID];
				if (FlowManagerBase* flow = clientWorld ? clientWorld->GetFlowManager() : nullptr)
				{
					if (Soul* soul = flow->GetSoul(ci->OwnerID))
					{
						RPCContext ctx{ ci, ConnectionMgr };
						soul->DispatchClientRPC(ctx, *rpcHdr, params);
					}
					else
					{
						LOG_NET_WARN_F(soul, "[ClientNet] SoulRPC: no Soul for ownerID=%u (MethodID=%u)",
							ci->OwnerID, rpcHdr->MethodID);
					}
				}
			}
			break;
		}

		// Legacy cases — superseded by SoulRPC. Kept for wire-compat; remove once server is updated.
		case NetMessageType::PlayerBeginConfirm:
			LOG_ENG_WARN("[ClientNet] Received legacy PlayerBeginConfirm — server should use SoulRPC");
			break;

		case NetMessageType::PlayerBeginReject:
			LOG_ENG_WARN("[ClientNet] Received legacy PlayerBeginReject — server should use SoulRPC");
			break;

		case NetMessageType::GameModeManifest:
		{
			struct BaseManifest : GameModeManifestPayload<BaseManifest>
			{
			};
			if (msg.Header.PayloadSize < sizeof(BaseManifest))
			{
				LOG_ENG_WARN_F("[ClientNet] GameModeManifest too small (got %u)", msg.Header.PayloadSize);
				break;
			}
			// TODO: forward raw bytes to GameMode::OnGameModeManifest(payload, size)
			const auto* base = reinterpret_cast<const BaseManifest*>(msg.Payload.data());
			LOG_ENG_INFO_F("[ClientNet] GameModeManifest received (seq=%u, mode='%s') — GameMode routing not yet wired",
				base->SequenceID, base->ModeName);
			break;
		}

		case NetMessageType::EntityDelta:
		{
			ConnectionInfo* ci     = ConnectionMgr->FindConnection(msg.Connection);
			const uint8_t ownerID  = ci ? ci->OwnerID : 0;
			WorldBase* clientWorld = WorldMap[ownerID];
			if (!clientWorld || !ci || msg.Payload.empty()) break;

			auto* payloadCopy = new std::vector<uint8_t>(msg.Payload);
			Registry* reg     = clientWorld->GetRegistry();
			clientWorld->Post([reg, payloadCopy](uint32_t)
			{
				HandleEntityDelta(reg, payloadCopy->data(),
					static_cast<uint32_t>(payloadCopy->size()));
				delete payloadCopy;
			});
			break;
		}

		case NetMessageType::StreamLoad:
		{
			if (msg.Payload.size() < sizeof(StreamLoadPayload))
			{
				LOG_ENG_WARN_F("[ClientNet] StreamLoad payload too small (%zu)", msg.Payload.size());
				break;
			}
			const auto* pl = reinterpret_cast<const StreamLoadPayload*>(msg.Payload.data());

			ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
			if (!ci) break;
			WorldBase* clientWorld = WorldMap[ci->OwnerID];
			Registry* reg          = clientWorld ? clientWorld->GetRegistry() : nullptr;
			if (!reg) break;

			const int64_t assetIDRaw     = pl->AssetID;
			const uint16_t instanceIndex = pl->InstanceIndex;
			const bool bAutoActivate     = pl->bAutoActivate != 0;

			AssetID assetID{ assetIDRaw };
			clientWorld->SpawnAndWait([reg, assetID, instanceIndex, bAutoActivate](uint32_t)
			{
				size_t count = EntityBuilder::StreamChunkTracked(reg, assetID, instanceIndex);
				LOG_ENG_INFO_F("[ClientNet] StreamLoad: spawned %zu entities (assetID=%ld, inst=%u)",
					count, assetID.Raw, static_cast<unsigned>(instanceIndex));

				if (bAutoActivate)
				{
					auto slabs = EntityBuilder::ActivateStreamedChunk(reg, assetID.Raw, instanceIndex);
					LOG_ENG_INFO_F("[ClientNet] StreamLoad auto-activated %zu entities", slabs.size());
#ifdef TNX_ENABLE_ROLLBACK
					if (!slabs.empty())
					{
						auto slabsCopy = std::make_shared<std::vector<uint32_t>>(std::move(slabs));
						reg->PushAppliedServerEvent([reg, slabsCopy]()
						{
							ComponentCacheBase* cache  = reg->GetTemporalCache();
							TemporalFrameHeader* hdr   = cache->GetFrameHeader(cache->GetActiveWriteFrame());
							const ComponentTypeID slot = CacheSlotMeta<>::StaticTemporalIndex();
							auto* flags                = static_cast<int32_t*>(cache->GetFieldData(hdr, slot, 0));
							if (!flags) return;
							constexpr int32_t mask = static_cast<int32_t>(
								static_cast<uint32_t>(TemporalFlagBits::Active) | static_cast<uint32_t>(TemporalFlagBits::Dirty) | static_cast<uint32_t>(TemporalFlagBits::DirtiedFrame));
							for (uint32_t idx : *slabsCopy)
								flags[idx] |= mask;
						});
					}
#endif
				}
			});

			if (!bAutoActivate)
			{
				StreamReadyPayload ready{};
				ready.AssetID       = assetIDRaw;
				ready.InstanceIndex = instanceIndex;
				NetChannel(ci, ConnectionMgr).Send(NetMessageType::StreamReady, ready, /*reliable=*/true);
			}
			break;
		}

		case NetMessageType::ChunkActivate:
		{
			if (msg.Payload.size() < sizeof(ChunkActivatePayload))
			{
				LOG_ENG_WARN_F("[ClientNet] ChunkActivate payload too small (%zu)", msg.Payload.size());
				break;
			}
			const auto* pl = reinterpret_cast<const ChunkActivatePayload*>(msg.Payload.data());

			ConnectionInfo* ci = ConnectionMgr->FindConnection(msg.Connection);
			if (!ci) break;
			WorldBase* clientWorld = WorldMap[ci->OwnerID];
			Registry* reg          = clientWorld ? clientWorld->GetRegistry() : nullptr;
			if (!reg) break;

			const int64_t assetIDRaw     = pl->AssetID;
			const uint16_t instanceIndex = pl->InstanceIndex;

			clientWorld->PostAndWait([reg, assetIDRaw, instanceIndex](uint32_t)
			{
				auto slabs = EntityBuilder::ActivateStreamedChunk(reg, assetIDRaw, instanceIndex);
				LOG_ENG_INFO_F("[ClientNet] ChunkActivate: activated %zu entities (assetID=%ld, inst=%u)",
					slabs.size(), assetIDRaw, static_cast<unsigned>(instanceIndex));
#ifdef TNX_ENABLE_ROLLBACK
				if (!slabs.empty())
				{
					auto slabsCopy = std::make_shared<std::vector<uint32_t>>(std::move(slabs));
					reg->PushAppliedServerEvent([reg, slabsCopy]()
					{
						ComponentCacheBase* cache  = reg->GetTemporalCache();
						TemporalFrameHeader* hdr   = cache->GetFrameHeader(cache->GetActiveWriteFrame());
						const ComponentTypeID slot = CacheSlotMeta<>::StaticTemporalIndex();
						auto* flags                = static_cast<int32_t*>(cache->GetFieldData(hdr, slot, 0));
						if (!flags) return;
						constexpr int32_t mask = static_cast<int32_t>(
							static_cast<uint32_t>(TemporalFlagBits::Active) | static_cast<uint32_t>(TemporalFlagBits::Dirty) | static_cast<uint32_t>(TemporalFlagBits::DirtiedFrame));
						for (uint32_t idx : *slabsCopy)
							flags[idx] |= mask;
					});
				}
#endif
			});
			break;
		}

		default:
			LOG_ENG_WARN_F("[ClientNet] Unhandled message type %u", msg.Header.Type);
			break;
	}
}

// ---------------------------------------------------------------------------

bool OwnerNet::TrySpawnDeferred(const DeferredConstructSpawn& entry)
{
	WorldBase* clientWorld = WorldMap[entry.OwnerID];
	if (!clientWorld) return true; // World gone — drop it

	ConstructRegistry* constructs = clientWorld->GetConstructRegistry();
	Registry* entityReg           = clientWorld->GetRegistry();
	std::vector<uint8_t> payload  = entry.Payload;

	bool done                  = false;
	const uint8_t* payloadData = payload.data();
	const size_t payloadSize   = payload.size();
	bool* pDone                = &done;
	clientWorld->SpawnAndWait([constructs, entityReg, clientWorld, payloadData, payloadSize, pDone](uint32_t)
	{
		*pDone = HandleConstructSpawn(
			constructs, entityReg, clientWorld, payloadData, payloadSize);
	});

	// If the construct was successfully created, request a rollback to its server spawn frame
	// so any backing entity temporal data is populated from the correct historical frame.
#ifdef TNX_ENABLE_ROLLBACK
	if (done && entry.ServerSpawnFrame > 0 && payload.size() >= sizeof(ConstructSpawnPayload))
	{
		if (clientWorld) clientWorld->EnqueueSpawnRollback(entry.ServerSpawnFrame);
	}
#endif

	return done;
}

void OwnerNet::FlushDeferredEntitySpawns()
{
	for (auto it = DeferredEntitySpawns.begin(); it != DeferredEntitySpawns.end();)
	{
		WorldBase* clientWorld = WorldMap[it->OwnerID];
		if (!clientWorld)
		{
			it = DeferredEntitySpawns.erase(it);
			continue;
		}

		const size_t payloadSize = it->Payload.size();
		if (payloadSize < sizeof(EntitySpawnPayload) || payloadSize % sizeof(EntitySpawnPayload) != 0)
		{
			LOG_ENG_WARN_F("[ClientNet] Deferred EntitySpawn payload bad size (%zu)", payloadSize);
			it = DeferredEntitySpawns.erase(it);
			continue;
		}

		const size_t count              = payloadSize / sizeof(EntitySpawnPayload);
		const EntitySpawnPayload* batch = reinterpret_cast<const EntitySpawnPayload*>(it->Payload.data());
		Registry* spawnReg              = clientWorld->GetRegistry();
		const uint32_t spawnFrame       = it->ServerSpawnFrame;

		clientWorld->SpawnAndWait([spawnReg, batch, count, spawnFrame](uint32_t)
		{
			for (size_t k = 0; k < count; ++k)
				HandleEntitySpawn(spawnReg, batch[k], spawnFrame);
		});

		// Request a rollback to the entity's server spawn frame so the entities are inserted
		// at the correct historical ring slot. ReplayServerEventsAt will re-hydrate them.
#ifdef TNX_ENABLE_ROLLBACK
		if (spawnFrame > 0) clientWorld->EnqueueSpawnRollback(spawnFrame);
#endif

		it = DeferredEntitySpawns.erase(it);
	}
}

void OwnerNet::TickReplication()
{
	// Retry PlayerBeginRequest for connections stuck in Loaded state.
	constexpr uint64_t RetryIntervalMs = 150;
	const uint64_t now                 = SDL_GetTicks();

	for (auto& ci : ConnectionMgr->GetConnections())
	{
		if (!ci.bOwnerInitiated || !ci.bConnected) continue;
		if (ci.RepState != ClientRepState::Loaded) continue;
		if (ci.PlayerBeginSentAt == 0) continue;
		if (now - ci.PlayerBeginSentAt < RetryIntervalMs) continue;

		WorldBase* world      = WorldMap[ci.OwnerID];
		FlowManagerBase* flow = world ? world->GetFlowManager() : nullptr;
		if (!flow) continue;

		LOG_ENG_WARN_F("[ClientNet] PlayerBeginRequest retry (ownerID=%u, %.0fms since last send)",
			ci.OwnerID, static_cast<float>(now - ci.PlayerBeginSentAt));

		ConnectionInfo* mci = ConnectionMgr->FindConnection(ci.Handle);
		if (!mci) continue;

		flow->SendPlayerBeginRequest(NetChannel(mci, ConnectionMgr), 0, mci->Predictions);
		mci->PlayerBeginSentAt = now;
	}

	// EntitySpawns must land before ConstructSpawns reference them.
	FlushDeferredEntitySpawns();

	if (DeferredConstructSpawns.empty()) return;

	auto it = DeferredConstructSpawns.begin();
	while (it != DeferredConstructSpawns.end())
	{
		if (TrySpawnDeferred(*it))
			it = DeferredConstructSpawns.erase(it);
		else
			++it;
	}
}

void OwnerNet::TickInputSend()
{
	if (!TrinyxJobs::IsRunning()) return;

	// Skip if the previous job is still running — single-consumer MPSC ring
	// can't safely be accessed by two jobs concurrently.
	if (SendCounter.Value.load(std::memory_order_acquire) != 0) return;

	OwnerNet* self = this;
	TrinyxJobs::Dispatch([self](uint32_t)
	{
		self->ExecuteInputSend();
	}, &SendCounter, TrinyxJobs::Queue::General);
}

void OwnerNet::ExecuteInputSend()
{
	std::vector<HSteamNetConnection> clientHandles;
	for (const auto& ci : ConnectionMgr->GetConnections())
		if (ci.bOwnerInitiated && ci.bConnected && ci.OwnerID != 0) clientHandles.push_back(ci.Handle);

	for (HSteamNetConnection handle : clientHandles)
	{
		ConnectionInfo* ci = ConnectionMgr->FindConnection(handle);
		if (!ci) continue;

		if (ci->RepState < ClientRepState::Playing) continue;

		WorldBase* world = WorldMap[ci->OwnerID];
		if (!world) continue;

		auto* consumer = world->GetInputAccumConsumer();
		if (!consumer) continue;

		// Drop floor — entirely in client-local frame space.
		// Pre-handshake junk: frames before ClientLocalFrameAtHandshake are pre-connection noise.
		// Already-acked: LastServerAckedFrame is echoed back by the server in client-local
		// space (server converts from server-frame via log->FrameOffset before stamping).
		const uint32_t preHandshakeFloor = ci->ClientLocalFrameAtHandshake > 0
											   ? ci->ClientLocalFrameAtHandshake - 1
											   : 0u;

		// Window backstop: if acks stall (e.g. server is stalled and frozen its ack), the
		// accumulator can grow well past MaxWindowFrames. The client would then keep sending
		// the oldest MaxWindowFrames entries — frames the server already has — which can
		// never advance LastReceivedFrame and permanently deadlocks the stall.
		// Anchor the drop floor so the accumulator never exceeds MaxWindowFrames entries.
		uint32_t windowBackstop = 0;
		if (consumer->Size() > static_cast<size_t>(MaxWindowFrames))
		{
			NetInputFrame tail;
			if (consumer->TryPeekAt(consumer->Size() - 1, tail) && tail.Frame >= MaxWindowFrames) windowBackstop = tail.Frame - MaxWindowFrames;
		}

		const uint32_t dropFloor = std::max({ ci->LastServerAckedFrame, preHandshakeFloor, windowBackstop });

		size_t dropCount = 0;
		while (dropCount < consumer->Size())
		{
			NetInputFrame front;
			if (!consumer->TryPeekAt(dropCount, front)) break;
			if (front.Frame > dropFloor) break;
			++dropCount;
		}
		if (dropCount > 0) consumer->DropFront(dropCount);

		// Peek up to MaxWindowFrames unacked entries.  Frame numbers are client-local —
		// the server translates to server-frame space via PlayerInputLog::FrameOffset.
		const uint32_t frameCount = static_cast<uint32_t>(
			std::min(consumer->Size(), static_cast<size_t>(MaxWindowFrames)));
		if (frameCount == 0) continue;

		NetInputFrame base;
		if (!consumer->TryPeekAt(0, base)) continue;

		// Delta-encode into a stack buffer, peeking one entry at a time.
		uint8_t encodedBuf[InputWindowCodec::MaxPayloadBytes];
		InputWindowWriter writer(encodedBuf, base);
		for (uint32_t i = 1; i < frameCount; ++i)
		{
			NetInputFrame cur;
			if (!consumer->TryPeekAt(i, cur)) break;
			writer.Append(cur);
		}

		const auto encodedSize         = static_cast<uint16_t>(writer.Finish());
		const uint32_t lastClientFrame = writer.GetLastFrame();

		PacketHeader header{};
		header.Type        = static_cast<uint8_t>(NetMessageType::InputFrameDelta);
		header.Flags       = PacketFlag::DefaultFlags;
		header.PayloadSize = encodedSize;
		header.FrameNumber = lastClientFrame; // client-local; server translates via FrameOffset
		header.SenderID    = ci->OwnerID;
		ConnectionMgr->Send(handle, header, encodedBuf, false, /*noNagle=*/true);
	}
}

void OwnerNet::AcknowledgeLevelReady(StreamingRequestID requestID)
{
	if (!ConnectionMgr) return;
	const uint8_t ownerID = requestID.GetOwnerID();
	for (ConnectionInfo& ci : ConnectionMgr->GetConnections())
	{
		if (!ci.bConnected || !ci.bOwnerInitiated) continue;
		if (ci.OwnerID != ownerID) continue;
		if (ci.RepState != ClientRepState::LevelLoading) continue;

		ci.RepState = ClientRepState::LevelLoaded;
		NetChannel(&ci, ConnectionMgr).SendHeaderOnly(NetMessageType::LevelReady, /*reliable=*/true);

		WorldBase* w          = WorldMap[ci.OwnerID];
		FlowManagerBase* flow = w ? w->GetFlowManager() : nullptr;
		Soul* soul            = flow ? flow->GetSoul(ci.OwnerID) : nullptr;
		LOG_NET_INFO(soul, "[ClientNet] LevelReady sent → client LevelLoaded");
	}
}
