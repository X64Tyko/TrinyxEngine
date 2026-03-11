#include "Registry.h"
#include <iostream>
#include <random>

#include "TrinyxEngine.h"
#include "GameManager.h"
#include "TestEntity.h"
#include "Public/CubeEntity.h"
#include "Archetype.h"
#include "Logger.h"
#include "TestFramework.h"

using namespace tnx::Testing;

TEST (Registry_CreateEntities)
{
	Registry* Reg = Engine.GetRegistry();
	std::vector<EntityID> Entities;

	for (int i = 0; i < 5; ++i)
	{
		EntityID Id = Reg->Create<TestEntity<>>();
		Entities.push_back(Id);
	}

	ASSERT_EQ(Entities.size(), 5);

	Reg->ResetRegistry();
}

TEST (Registry_ValidEntityIDs)
{
	Registry* Reg = Engine.GetRegistry();
	std::vector<EntityID> Entities;

	for (int i = 0; i < 100; ++i)
	{
		Entities.push_back(Reg->Create<TestEntity<>>());
	}

	for (EntityID Id : Entities)
	{
		ASSERT(Id.IsValid());
	}
	Reg->ResetRegistry();
}

TEST (Registry_DestroyAndReuse)
{
	Registry* Reg = Engine.GetRegistry();
	std::vector<EntityID> Entities;

	for (int i = 0; i < 10; ++i)
	{
		Entities.push_back(Reg->Create<TestEntity<>>());
	}

	uint32_t firstIndex      = Entities[0].GetIndex();
	uint32_t firstGeneration = Entities[0].GetGeneration();

	Reg->Destroy(Entities[0]);
	Reg->ProcessDeferredDestructions();

	EntityID NewId = Reg->Create<TestEntity<>>();
	ASSERT_EQ(NewId.GetIndex(), firstIndex);
	ASSERT(NewId.GetGeneration() > firstGeneration);

	Reg->ResetRegistry();
}

/* Temporarily disabled, jobs aren't properly initialized until the Run loop is active.
TEST(DirtyBits_SetAfterPrePhys)
{
	Registry* Reg = Engine.GetRegistry();
	
	TrinyxJobs::Initialize(Reg->GetConfig);

	constexpr int kCount = 16; // exactly 2 dirty bytes
	Reg->Create<CubeEntity<>>(kCount);

	std::vector<Archetype*> arches = Reg->ClassQuery<CubeEntity<>>();
	ASSERT(!arches.empty());
	Chunk* chunk = arches[0]->Chunks[0];

	size_t gsi = chunk->Header.GlobalIndexStart;
	Reg->InvokePrePhys(1.0 / 128.0, 0);

	uint8_t* dirtyBytes = reinterpret_cast<uint8_t*>(Reg->DirtyBitsFrame(0)->data()) + (gsi / 8);

	LOG_INFO_F("[DirtyBits] GlobalIndexStart=%zu  byteOffset=%zu", gsi, gsi / 8);
	for (int b = 0; b < (kCount + 7) / 8; ++b)
		LOG_INFO_F("[DirtyBits]   byte[%d] = 0x%02X  (expected 0xFF)", b, dirtyBytes[b]);

	for (int b = 0; b < (kCount + 7) / 8; ++b)
		ASSERT_EQ((int)dirtyBytes[b], 0xFF);

	Reg->ResetRegistry();
}
*/

TEST (InitializeTestEntities)
{
	Registry* Reg = Engine.GetRegistry();
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_real_distribution<float> posX(-75.0f, 75.0f);
	std::uniform_real_distribution<float> posY(-10.0f, 50.0f);
	std::uniform_real_distribution<float> posZ(-100.0f, -50.0f);
	std::uniform_real_distribution<float> velX(-10.0f, 10.0f);
	std::uniform_real_distribution<float> velY(-10.0f, 10.0f);
	std::uniform_real_distribution<float> color(0.2f, 1.0f);

	static int32_t CubeCount  = 10000;
	static int32_t SuperCount = 90000;

	// Step 1: Create all entities first
	entityIDs.reserve(CubeCount + SuperCount);
	std::vector<EntityID> newIds = Reg->Create<SuperCube<>>(SuperCount);
	entityIDs.insert(entityIDs.end(), newIds.begin(), newIds.end());
	newIds = Reg->Create<CubeEntity<>>(CubeCount);
	entityIDs.insert(entityIDs.end(), newIds.begin(), newIds.end());

	LOG_ALWAYS_F("Created %i test entities", CubeCount + SuperCount);

	std::vector<Archetype*> Arches = Reg->ClassQuery<CubeEntity<>>();
	for (Archetype* cubeArch : Arches)
	{
		CubeEntity<> Cube;
		for (size_t chunkIdx = 0; chunkIdx < cubeArch->Chunks.size(); ++chunkIdx)
		{
			Chunk* chunk         = cubeArch->Chunks[chunkIdx];
			uint32_t entityCount = cubeArch->GetChunkCount(chunkIdx);

			// Build field array table
			void* fieldArrayTable[MAX_FIELD_ARRAYS];
			cubeArch->BuildFieldArrayTable(chunk, fieldArrayTable, 0, 0);
			Cube.Hydrate(fieldArrayTable, reinterpret_cast<uint8_t*>(Reg->DirtyBitsFrame(0)->data())
						 + (chunk->Header.GlobalIndexStart / 8));

			// Initialize all entities in this chunk
			for (uint32_t i = 0; i < entityCount; ++i, Cube.Advance(1))
			{
				Cube.Flags.Flags    = static_cast<int32_t>(TemporalFlagBits::Active);
				Cube.transform.PosX = posX(gen);
				Cube.transform.PosY = posY(gen);
				Cube.transform.PosZ = posZ(gen);
				Cube.transform.Rotation.SetIdentity();
				Cube.scale.ScaleX = 1.0f;
				Cube.scale.ScaleY = 1.0f;
				Cube.scale.ScaleZ = 1.0f;

				Cube.velocity.vX = velX(gen);
				Cube.velocity.vY = velY(gen);
				Cube.velocity.vZ = 0.0f;

				Cube.color.R = color(gen);
				Cube.color.G = color(gen);
				Cube.color.B = color(gen);
				Cube.color.A = 1.0f;

				Cube.physBody.Shape       = JoltShapeType::Box;
				Cube.physBody.HalfExtentX = 0.5f;
				Cube.physBody.HalfExtentY = 0.5f;
				Cube.physBody.HalfExtentZ = 0.5f;

				Cube.physBody.Motion      = JoltMotion::Dynamic;
				Cube.physBody.Mass        = 1.0f;
				Cube.physBody.Friction    = 0.5f;
				Cube.physBody.Restitution = 0.5f;
			}
		}
	}

	// Create a static floor entity
	{
		std::vector<EntityID> floorIds = Reg->Create<CubeEntity<>>(1);
		entityIDs.insert(entityIDs.end(), floorIds.begin(), floorIds.end());

		Arches = Reg->ClassQuery<CubeEntity<>>();
		// The floor entity is in the last slot of the last chunk
		for (Archetype* cubeArch : Arches)
		{
			CubeEntity<> Floor;
			size_t lastChunk     = cubeArch->Chunks.size() - 1;
			Chunk* chunk         = cubeArch->Chunks[lastChunk];
			uint32_t entityCount = cubeArch->GetChunkCount(lastChunk);

			void* fieldArrayTable[MAX_FIELD_ARRAYS];
			cubeArch->BuildFieldArrayTable(chunk, fieldArrayTable, 0, 0);
			Floor.Hydrate(fieldArrayTable, reinterpret_cast<uint8_t*>(Reg->DirtyBitsFrame(0)->data())
						  + (chunk->Header.GlobalIndexStart / 8));

			// Advance to the last entity in the chunk (the floor we just created)
			Floor.Advance(entityCount - 1);

			Floor.Flags.Flags    = static_cast<int32_t>(TemporalFlagBits::Active);
			Floor.transform.PosX = 0.0f;
			Floor.transform.PosY = -15.0f;
			Floor.transform.PosZ = -75.0f;
			Floor.transform.Rotation.SetIdentity();
			Floor.scale.ScaleX = 200.0f;
			Floor.scale.ScaleY = 1.0f;
			Floor.scale.ScaleZ = 200.0f;

			Floor.color.R = 0.3f;
			Floor.color.G = 0.3f;
			Floor.color.B = 0.3f;
			Floor.color.A = 1.0f;

			Floor.physBody.Shape       = JoltShapeType::Box;
			Floor.physBody.HalfExtentX = 100.0f;
			Floor.physBody.HalfExtentY = 0.5f;
			Floor.physBody.HalfExtentZ = 100.0f;
			Floor.physBody.Motion      = JoltMotion::Static;
			Floor.physBody.Mass        = 0.0f;
			Floor.physBody.Friction    = 0.8f;
			Floor.physBody.Restitution = 0.3f;
		}
	}

	// Step 2: Initialize by iterating through archetypes/chunks
	Arches = Reg->ClassQuery<SuperCube<>>();
	for (Archetype* cubeArch : Arches)
	{
		SuperCube<> Cube;
		for (size_t chunkIdx = 0; chunkIdx < cubeArch->Chunks.size(); ++chunkIdx)
		{
			Chunk* chunk         = cubeArch->Chunks[chunkIdx];
			uint32_t entityCount = cubeArch->GetChunkCount(chunkIdx);

			// Build field array table
			void* fieldArrayTable[MAX_FIELD_ARRAYS];
			cubeArch->BuildFieldArrayTable(chunk, fieldArrayTable, 0, 0);
			Cube.Hydrate(fieldArrayTable, reinterpret_cast<uint8_t*>(Reg->DirtyBitsFrame(0)->data())
						 + (chunk->Header.GlobalIndexStart / 8));

			// Initialize all entities in this chunk
			for (uint32_t i = 0; i < entityCount; ++i, Cube.Advance(1))
			{
				Cube.Flags.Flags    = static_cast<int32_t>(TemporalFlagBits::Active);
				Cube.transform.PosX = posX(gen);
				Cube.transform.PosY = posY(gen);
				Cube.transform.PosZ = posZ(gen);
				Cube.transform.Rotation.SetIdentity();
				Cube.scale.ScaleX = 1.0f;
				Cube.scale.ScaleY = 1.0f;
				Cube.scale.ScaleZ = 1.0f;

				Cube.velocity.vX = velX(gen);
				Cube.velocity.vY = velY(gen);
				Cube.velocity.vZ = 0.0f;

				Cube.color.R = color(gen);
				Cube.color.G = color(gen);
				Cube.color.B = color(gen);
				Cube.color.A = 1.0f;
			}
		}
	}

	// Intentionally no Reset call. We want to continue testing with these entities for now.
}

// ---------------------------------------------------------------------------
// Testbed GameManager — runs the test suite, then enters the engine loop
// ---------------------------------------------------------------------------
class TestbedGame : public GameManager<TestbedGame>
{
public:
	const char* GetWindowTitle() const { return "Trinyx Testbed"; }

	bool PostInitialize(TrinyxEngine& engine)
	{
		if (TestRegistry::Instance().RunAll(engine) != 0) return false;
		return true;
	}
};

TNX_IMPLEMENT_GAME(TestbedGame)
