#include "ReplicationSystem.h"

#include "Archetype.h"
#include "CacheSlotMeta.h"
#include "ColorData.h"
#include "Logger.h"
#include "MeshRef.h"
#include "NetConnectionManager.h"
#include "NetTypes.h"
#include "Registry.h"
#include "Scale.h"
#include "TemporalComponentCache.h"
#include "TransRot.h"
#include "World.h"

#include <cstring>

void ReplicationSystem::Initialize(World* serverWorld)
{
	ServerWorld = serverWorld;
	Replicated.clear();
	LOG_INFO("[Replication] Initialized");
}

// ---------------------------------------------------------------------------
// Tick — called from NetThread at NetworkUpdateHz
// ---------------------------------------------------------------------------

void ReplicationSystem::Tick(NetConnectionManager* connMgr, uint32_t frameNumber)
{
	if (!ServerWorld || !connMgr) return;
	if (connMgr->GetConnectionCount() == 0) return;

	SendSpawns(connMgr, frameNumber);
	SendStateCorrections(connMgr, frameNumber);
}

// ---------------------------------------------------------------------------
// AssignNetHandle — allocate a unique EntityNetHandle for a server entity
// ---------------------------------------------------------------------------

EntityNetHandle ReplicationSystem::AssignNetHandle(Registry* reg, GlobalEntityHandle gHandle)
{
	uint32_t netIndex = reg->AllocateNetIndex();

	EntityNetHandle netHandle{};
	netHandle.NetOwnerID = 0; // Server-owned
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
	const ComponentTypeID transformSlot = TransRot<>::StaticTemporalIndex();
	const ComponentTypeID scaleSlot     = Scale<>::StaticTemporalIndex();
	const ComponentTypeID colorSlot     = ColorData<>::StaticTemporalIndex();
	const ComponentTypeID meshRefSlot   = MeshRef<>::StaticTemporalIndex();

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

	// Grow replicated tracker if needed
	if (Replicated.size() < maxEntities) Replicated.resize(maxEntities, false);

	int spawnCount = 0;

	for (uint32_t i = 0; i < maxEntities; ++i)
	{
		// Skip inactive or already-replicated entities
		if (!(flags[i] & static_cast<int32_t>(TemporalFlagBits::Active))) continue;
		if (Replicated[i]) continue;

		// Look up the GlobalEntityHandle for this cache index
		GlobalEntityHandle gHandle = reg->GlobalEntityRegistry.LookupGlobalHandle(static_cast<EntityCacheHandle>(i));
		EntityRecord* record       = reg->GlobalEntityRegistry.Records[gHandle.GetIndex()];
		if (!record || !record->IsValid()) continue;

		// Assign a net handle to this entity
		EntityNetHandle netHandle = AssignNetHandle(reg, gHandle);

		// Build spawn payload
		EntitySpawnPayload payload{};
		payload.NetHandle = netHandle.Value;
		EntityNetManifest manifest{};
		manifest.ClassType = record->Arch ? record->Arch->ArchClassID : 0;
		payload.Manifest   = manifest.Value;

		payload.PosX  = posX ? posX[i] : 0.0f;
		payload.PosY  = posY ? posY[i] : 0.0f;
		payload.PosZ  = posZ ? posZ[i] : 0.0f;
		payload.RotQx = rotQx ? rotQx[i] : 0.0f;
		payload.RotQy = rotQy ? rotQy[i] : 0.0f;
		payload.RotQz = rotQz ? rotQz[i] : 0.0f;
		payload.RotQw = rotQw ? rotQw[i] : 1.0f;

		payload.ScaleX = scX ? scX[i] : 1.0f;
		payload.ScaleY = scY ? scY[i] : 1.0f;
		payload.ScaleZ = scZ ? scZ[i] : 1.0f;

		payload.ColorR = colR ? colR[i] : 1.0f;
		payload.ColorG = colG ? colG[i] : 1.0f;
		payload.ColorB = colB ? colB[i] : 1.0f;
		payload.ColorA = colA ? colA[i] : 1.0f;

		payload.MeshID = meshID ? meshID[i] : 0;

		// Send reliable to all connected clients
		PacketHeader header{};
		header.Type        = static_cast<uint8_t>(NetMessageType::EntitySpawn);
		header.Flags       = PacketFlag::DefaultFlags;
		header.PayloadSize = sizeof(EntitySpawnPayload);
		header.FrameNumber = frameNumber;
		header.SenderID    = 0; // Server

		for (const auto& ci : connMgr->GetConnections())
		{
			if (ci.bConnected && ci.OwnerID > 0 && ci.bServerSide) // Only send on server-accepted handles
			{
				connMgr->Send(ci.Handle, header,
							  reinterpret_cast<const uint8_t*>(&payload), true);
			}
		}

		Replicated[i] = true;
		spawnCount++;
	}

	if (spawnCount > 0)
		LOG_DEBUG_F("[Replication] Sent %d EntitySpawn messages", spawnCount);
}

// ---------------------------------------------------------------------------
// SendStateCorrections — unreliable batched transforms for all replicated entities
// ---------------------------------------------------------------------------

void ReplicationSystem::SendStateCorrections(NetConnectionManager* connMgr, uint32_t frameNumber)
{
	Registry* reg = ServerWorld->GetRegistry();

	ComponentCacheBase* temporalCache = reg->GetTemporalCache();
	uint32_t temporalFrame            = temporalCache->GetActiveReadFrame();
	TemporalFrameHeader* temporalHdr  = temporalCache->GetFrameHeader(temporalFrame);

	const ComponentTypeID flagsSlot     = CacheSlotMeta<>::StaticTemporalIndex();
	const ComponentTypeID transformSlot = TransRot<>::StaticTemporalIndex();

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
		batch.push_back(entry);
	}

	if (batch.empty()) return;

	// Send as single unreliable message. If it exceeds MTU, GNS handles fragmentation.
	uint32_t payloadSize = static_cast<uint32_t>(batch.size() * sizeof(StateCorrectionEntry));

	PacketHeader header{};
	header.Type        = static_cast<uint8_t>(NetMessageType::StateCorrection);
	header.Flags       = PacketFlag::DefaultFlags;
	header.PayloadSize = static_cast<uint16_t>(payloadSize > 65535 ? 65535 : payloadSize);
	header.FrameNumber = frameNumber;
	header.SenderID    = 0;

	for (const auto& ci : connMgr->GetConnections())
	{
		if (ci.bConnected && ci.OwnerID > 0)
		{
			connMgr->Send(ci.Handle, header,
						  reinterpret_cast<const uint8_t*>(batch.data()), false);
		}
	}
}

// ---------------------------------------------------------------------------
// HandleEntitySpawn — client-side: create entity from received spawn payload
// Uses Registry internals via friend access (GlobalEntityHandle, CreateInternal)
// ---------------------------------------------------------------------------

void ReplicationSystem::HandleEntitySpawn(Registry* reg, const EntitySpawnPayload& payload)
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
		LOG_WARN_F("[Replication] Failed to create entity ClassID %u", classType);
		return;
	}

	// Wire the received NetHandle to our local GlobalEntityHandle
	reg->GlobalEntityRegistry.NetToRecord.set(receivedNetHandle.GetHandleIndex(), gHandle);

	// Store NetworkID on the record
	EntityRecord* record = reg->GlobalEntityRegistry.Records[gHandle.GetIndex()];
	if (!record || !record->IsValid())
	{
		LOG_WARN("[Replication] Entity created but record invalid");
		return;
	}
	record->NetworkID = receivedNetHandle;

	// Write field data into the newly created entity's archetype slot
	Archetype* arch   = record->Arch;
	Chunk* chunk      = record->TargetChunk;
	uint32_t localIdx = record->LocalIndex;

	void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
	arch->BuildFieldArrayTable(chunk, fieldArrayTable,
							   reg->GetTemporalCache()->GetActiveWriteFrame(),
							   reg->GetVolatileCache()->GetActiveWriteFrame());

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
			if (fdesc.componentSlotIndex == 0) intArr[localIdx] = static_cast<int32_t>(TemporalFlagBits::Active);
		}
		else if (compID == TransRot<>::StaticTypeID())
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
		else if (compID == Scale<>::StaticTypeID())
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
		else if (compID == ColorData<>::StaticTypeID())
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
		else if (compID == MeshRef<>::StaticTypeID())
		{
			if (fdesc.componentSlotIndex == 0) uintArr[localIdx] = payload.MeshID;
		}
	}
}

// ---------------------------------------------------------------------------
// HandleStateCorrections — client-side: apply authoritative transforms
// Looks up entities by NetHandle → GlobalEntityHandle → EntityRecord
// ---------------------------------------------------------------------------

void ReplicationSystem::HandleStateCorrections(Registry* reg, const StateCorrectionEntry* entries, uint32_t count)
{
	for (uint32_t i = 0; i < count; ++i)
	{
		const StateCorrectionEntry& entry = entries[i];

		EntityNetHandle netHandle{};
		netHandle.Value = entry.NetHandle;

		// Look up GlobalEntityHandle from the received NetHandle
		GlobalEntityHandle gHandle = reg->GlobalEntityRegistry.LookupGlobalHandle(netHandle);
		if (gHandle.GetIndex() == 0) continue; // Unknown entity — spawn not received yet

		EntityRecord* record = reg->GlobalEntityRegistry.Records[gHandle.GetIndex()];
		if (!record || !record->IsValid()) continue;

		// Write transforms directly into the entity's archetype fields
		Archetype* arch   = record->Arch;
		Chunk* chunk      = record->TargetChunk;
		uint32_t localIdx = record->LocalIndex;

		void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
		arch->BuildFieldArrayTable(chunk, fieldArrayTable,
								   reg->GetTemporalCache()->GetActiveWriteFrame(),
								   reg->GetVolatileCache()->GetActiveWriteFrame());

		for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
		{
			if (fdesc.componentID != TransRot<>::StaticTypeID()) continue;

			void* fieldBase = fieldArrayTable[fdesc.fieldSlotIndex];
			if (!fieldBase) continue;

			auto* floatArr = static_cast<float*>(fieldBase);

			switch (fdesc.componentSlotIndex)
			{
				case 0: floatArr[localIdx] = entry.PosX;
					break;
				case 1: floatArr[localIdx] = entry.PosY;
					break;
				case 2: floatArr[localIdx] = entry.PosZ;
					break;
				case 3: floatArr[localIdx] = entry.RotQx;
					break;
				case 4: floatArr[localIdx] = entry.RotQy;
					break;
				case 5: floatArr[localIdx] = entry.RotQz;
					break;
				case 6: floatArr[localIdx] = entry.RotQw;
					break;
				default: break;
			}
		}
	}
}
