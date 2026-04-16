#pragma once

// Shared entity-writing helpers for Testbed tests.
// Include this in any test that spawns CubeEntity, SuperCube, or Projectile entities.

#include <vector>
#include <cstdint>

#include "Registry.h"
#include "Archetype.h"
#include "RegistryTypes.h"
#include "Public/CubeEntity.h"
#include "Public/Projectile.h"

// ---------------------------------------------------------------------------
// Global entity handle collections — tests that spawn persistent entities
// store their IDs here so other tests (e.g. entity count checks) can see them.
// ---------------------------------------------------------------------------
inline std::vector<EntityHandle> gPyramidIds;
inline std::vector<EntityHandle> gSuperCubeIds;
inline std::vector<EntityHandle> gProjectileIds;

// ---------------------------------------------------------------------------
// Setup structs
// ---------------------------------------------------------------------------
struct CubeSetup
{
	float x, y, z;
	float halfX, halfY, halfZ;
	float mass;
	float r, g, b;
	uint32_t motion;
};

struct ProjectileSetup
{
	float x, y, z;
	float velX, velY, velZ;
	float r, g, b, a;
};

// ---------------------------------------------------------------------------
// WriteCubeSetups — batch-creates CubeEntity entities and fills their fields.
// ---------------------------------------------------------------------------
inline void WriteCubeSetups(Registry* reg, const std::vector<CubeSetup>& setups,
                             std::vector<EntityHandle>& outIds)
{
	int32_t totalCount               = static_cast<int32_t>(setups.size());
	std::vector<EntityHandle> newIds = reg->Create<CubeEntity<>>(totalCount);
	outIds.insert(outIds.end(), newIds.begin(), newIds.end());

	uint32_t setupIdx              = 0;
	std::vector<Archetype*> arches = reg->ClassQuery<CubeEntity<>>();
	for (Archetype* arch : arches)
	{
		CubeEntity<> cube;
		for (size_t chunkIdx = 0; chunkIdx < arch->Chunks.size(); ++chunkIdx)
		{
			Chunk* chunk              = arch->Chunks[chunkIdx];
			uint32_t chunkEntityCount = arch->GetAllocatedChunkCount(chunkIdx);

			void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
			arch->BuildFieldArrayTable(chunk, fieldArrayTable,
				reg->GetTemporalCache()->GetActiveWriteFrame(),
				reg->GetVolatileCache()->GetActiveWriteFrame());
			cube.Hydrate(fieldArrayTable, fieldArrayTable[0]);

			for (uint32_t i = 0; i < chunkEntityCount; ++i, cube.Advance(1))
			{
				if (setupIdx >= setups.size()) break;
				const auto& s = setups[setupIdx++];

				cube.Flags.Flags    = static_cast<int32_t>(TemporalFlagBits::Active);
				cube.transform.PosX = s.x;
				cube.transform.PosY = s.y;
				cube.transform.PosZ = s.z;
				cube.transform.Rotation.SetIdentity();
				cube.scale.ScaleX = s.halfX * 2.0f;
				cube.scale.ScaleY = s.halfY * 2.0f;
				cube.scale.ScaleZ = s.halfZ * 2.0f;

				cube.velocity.vX = 0.0f;
				cube.velocity.vY = 0.0f;
				cube.velocity.vZ = 0.0f;

				cube.color.R = s.r;
				cube.color.G = s.g;
				cube.color.B = s.b;
				cube.color.A = 1.0f;

				cube.physBody.Shape       = JoltShapeType::Box;
				cube.physBody.HalfExtentX = s.halfX;
				cube.physBody.HalfExtentY = s.halfY;
				cube.physBody.HalfExtentZ = s.halfZ;
				cube.physBody.Motion      = s.motion;
				cube.physBody.Mass        = s.mass;
				cube.physBody.Friction    = 0.5f;
				cube.physBody.Restitution = 0.5f;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// WriteProjectileSetups
// ---------------------------------------------------------------------------
inline void WriteProjectileSetups(Registry* reg, const std::vector<ProjectileSetup>& setups,
                                   std::vector<EntityHandle>& outIds)
{
	int32_t totalCount               = static_cast<int32_t>(setups.size());
	std::vector<EntityHandle> newIds = reg->Create<Projectile<>>(totalCount);
	outIds.insert(outIds.end(), newIds.begin(), newIds.end());

	uint32_t setupIdx              = 0;
	std::vector<Archetype*> arches = reg->ClassQuery<Projectile<>>();
	for (Archetype* arch : arches)
	{
		Projectile<> proj;
		for (size_t chunkIdx = 0; chunkIdx < arch->Chunks.size(); ++chunkIdx)
		{
			Chunk* chunk              = arch->Chunks[chunkIdx];
			uint32_t chunkEntityCount = arch->GetAllocatedChunkCount(chunkIdx);

			void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
			arch->BuildFieldArrayTable(chunk, fieldArrayTable,
				reg->GetTemporalCache()->GetActiveWriteFrame(),
				reg->GetVolatileCache()->GetActiveWriteFrame());
			proj.Hydrate(fieldArrayTable, fieldArrayTable[0]);

			for (uint32_t i = 0; i < chunkEntityCount; ++i, proj.Advance(1))
			{
				if (setupIdx >= setups.size()) break;
				const auto& s = setups[setupIdx++];

				proj.Flags.Flags    = static_cast<int32_t>(TemporalFlagBits::Active);
				proj.transform.PosX = s.x;
				proj.transform.PosY = s.y;
				proj.transform.PosZ = s.z;
				proj.transform.Rotation.SetIdentity();

				proj.body.VelX = s.velX;
				proj.body.VelY = s.velY;
				proj.body.VelZ = s.velZ;

				proj.color.R = s.r;
				proj.color.G = s.g;
				proj.color.B = s.b;
				proj.color.A = s.a;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// WriteSuperCubeSetups
// ---------------------------------------------------------------------------
inline void WriteSuperCubeSetups(Registry* reg, const std::vector<CubeSetup>& setups,
                                  std::vector<EntityHandle>& outIds)
{
	int32_t totalCount               = static_cast<int32_t>(setups.size());
	std::vector<EntityHandle> newIds = reg->Create<SuperCube<>>(totalCount);
	outIds.insert(outIds.end(), newIds.begin(), newIds.end());

	uint32_t setupIdx              = 0;
	std::vector<Archetype*> arches = reg->ClassQuery<SuperCube<>>();
	for (Archetype* arch : arches)
	{
		SuperCube<> cube;
		for (size_t chunkIdx = 0; chunkIdx < arch->Chunks.size(); ++chunkIdx)
		{
			Chunk* chunk              = arch->Chunks[chunkIdx];
			uint32_t chunkEntityCount = arch->GetAllocatedChunkCount(chunkIdx);

			void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
			arch->BuildFieldArrayTable(chunk, fieldArrayTable,
				reg->GetTemporalCache()->GetActiveWriteFrame(),
				reg->GetVolatileCache()->GetActiveWriteFrame());
			cube.Hydrate(fieldArrayTable, fieldArrayTable[0]);

			for (uint32_t i = 0; i < chunkEntityCount; ++i, cube.Advance(1))
			{
				if (setupIdx >= setups.size()) break;
				const auto& s = setups[setupIdx++];

				cube.Flags.Flags    = static_cast<int32_t>(TemporalFlagBits::Active);
				cube.transform.PosX = s.x;
				cube.transform.PosY = s.y;
				cube.transform.PosZ = s.z;
				cube.transform.Rotation.SetIdentity();
				cube.scale.ScaleX = s.halfX * 2.0f;
				cube.scale.ScaleY = s.halfY * 2.0f;
				cube.scale.ScaleZ = s.halfZ * 2.0f;

				cube.velocity.vX = 0.0f;
				cube.velocity.vY = 0.0f;
				cube.velocity.vZ = 0.0f;

				cube.color.R = s.r;
				cube.color.G = s.g;
				cube.color.B = s.b;
				cube.color.A = 1.0f;
			}
		}
	}
}
