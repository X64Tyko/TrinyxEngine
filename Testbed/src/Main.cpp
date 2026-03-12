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
	std::uniform_real_distribution<float> colorDist(0.2f, 1.0f);
	const int zOffset = -100;
	const int yOffset = -30;

	// --- Jolt Pyramid Test (matches Jorrit's PyramidScene.h) ---
	constexpr float cBoxSize       = 2.0f;
	constexpr float cHalfBoxSize   = 1.0f;
	constexpr float cBoxSeparation = 0.5f;
	constexpr int cPyramidHeight   = 25; // ~9,455 dynamic boxes

	// Pre-compute pyramid positions
	struct BoxSetup
	{
		float x, y, z, hx, hy, hz, mass, r, g, b;
		uint32_t motion;
	};
	std::vector<BoxSetup> setups;

	for (int i = 0; i < cPyramidHeight; ++i)
	{
		for (int j = i / 2; j < cPyramidHeight - (i + 1) / 2; ++j)
		{
			for (int k = i / 2; k < cPyramidHeight - (i + 1) / 2; ++k)
			{
				setups.push_back({
					static_cast<float>(-cPyramidHeight) + cBoxSize * j + ((i & 1) ? cHalfBoxSize : 0.0f),
					yOffset + 1.0f + (cBoxSize + cBoxSeparation) * i,
					zOffset + static_cast<float>(-cPyramidHeight) + cBoxSize * k + ((i & 1) ? cHalfBoxSize : 0.0f),
					cHalfBoxSize, cHalfBoxSize, cHalfBoxSize,
					1.0f,
					colorDist(gen), colorDist(gen), colorDist(gen),
					JoltMotion::Dynamic
				});
			}
		}
	}

	// Floor
	setups.push_back({
		0.0f, yOffset - 1.0f, zOffset,
		50.0f, 1.0f, 50.0f,
		0.0f,
		0.3f, 0.3f, 0.3f,
		JoltMotion::Static
	});

	int32_t totalCount = static_cast<int32_t>(setups.size());
	entityIDs.reserve(totalCount);
	std::vector<EntityID> newIds = Reg->Create<CubeEntity<>>(totalCount);
	entityIDs.insert(entityIDs.end(), newIds.begin(), newIds.end());

	LOG_ALWAYS_F("Pyramid test: %d dynamic boxes + 1 floor = %d entities",
				 totalCount - 1, totalCount);

	// Write into chunks
	uint32_t setupIdx = 0;
	std::vector<Archetype*> Arches = Reg->ClassQuery<CubeEntity<>>();
	for (Archetype* cubeArch : Arches)
	{
		CubeEntity<> Cube;
		for (size_t chunkIdx = 0; chunkIdx < cubeArch->Chunks.size(); ++chunkIdx)
		{
			Chunk* chunk         = cubeArch->Chunks[chunkIdx];
			uint32_t entityCount = cubeArch->GetChunkCount(chunkIdx);

			void* fieldArrayTable[MAX_FIELD_ARRAYS];
			cubeArch->BuildFieldArrayTable(chunk, fieldArrayTable, 0, 0);
			Cube.Hydrate(fieldArrayTable, reinterpret_cast<uint8_t*>(Reg->DirtyBitsFrame(0)->data())
						 + (chunk->Header.GlobalIndexStart / 8));

			for (uint32_t i = 0; i < entityCount; ++i, Cube.Advance(1))
			{
				if (setupIdx >= setups.size()) break;
				const auto& s = setups[setupIdx++];

				Cube.Flags.Flags    = static_cast<int32_t>(TemporalFlagBits::Active);
				Cube.transform.PosX = s.x;
				Cube.transform.PosY = s.y;
				Cube.transform.PosZ = s.z;
				Cube.transform.Rotation.SetIdentity();
				Cube.scale.ScaleX = s.hx * 2.0f;
				Cube.scale.ScaleY = s.hy * 2.0f;
				Cube.scale.ScaleZ = s.hz * 2.0f;

				Cube.velocity.vX = 0.0f;
				Cube.velocity.vY = 0.0f;
				Cube.velocity.vZ = 0.0f;

				Cube.color.R = s.r;
				Cube.color.G = s.g;
				Cube.color.B = s.b;
				Cube.color.A = 1.0f;

				Cube.physBody.Shape       = JoltShapeType::Box;
				Cube.physBody.HalfExtentX = s.hx;
				Cube.physBody.HalfExtentY = s.hy;
				Cube.physBody.HalfExtentZ = s.hz;
				Cube.physBody.Motion      = s.motion;
				Cube.physBody.Mass        = s.mass;
				Cube.physBody.Friction    = 0.5f;
				Cube.physBody.Restitution = 0.5f;
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
