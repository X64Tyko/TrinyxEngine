#include "Registry.h"
#include <iostream>
#include <random>
#include <thread>
#include <SDL3/SDL_timer.h>

#include "TrinyxEngine.h"
#include "GameManager.h"
#include "TestEntity.h"
#include "Public/CubeEntity.h"
#include "Public/Projectile.h"
#include "Archetype.h"
#include "Logger.h"
#include "TestFramework.h"

using namespace tnx::Testing;

TEST(Registry_CreateEntities)
{
	Registry* Reg = Engine.GetRegistry();
	std::vector<EntityHandle> Entities;

	for (int i = 0; i < 5; ++i)
	{
		EntityHandle Id = Reg->Create<TestEntity<>>();
		Entities.push_back(Id);
	}

	ASSERT_EQ(Entities.size(), 5);

	Engine.ResetRegistry();
}

TEST(Registry_ValidEntityIDs)
{
	Registry* Reg = Engine.GetRegistry();
	std::vector<EntityHandle> Entities;

	for (int i = 0; i < 100; ++i)
	{
		Entities.push_back(Reg->Create<TestEntity<>>());
	}

	for (EntityHandle Id : Entities)
	{
		ASSERT(Id.IsValid());
	}
	Engine.ResetRegistry();
}

TEST(Registry_DestroyAndReuse)
{
	Registry* Reg = Engine.GetRegistry();
	std::vector<EntityHandle> Entities;

	for (int i = 0; i < 10; ++i)
	{
		Entities.push_back(Reg->Create<TestEntity<>>());
	}

	uint32_t firstHandleIndex = Entities[0].GetHandleIndex();

	Reg->Destroy(Entities[0]);
	Reg->ProcessDeferredDestructions();
	Engine.ConfirmLocalRecycles();

	EntityHandle NewId = Reg->Create<TestEntity<>>();
	ASSERT_EQ(NewId.GetHandleIndex(), firstHandleIndex);

	Engine.ResetRegistry();
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

	size_t gsi = chunk->Header.CacheIndexStart;
	Reg->InvokePrePhys(1.0 / 128.0, 0);

	uint8_t* dirtyBytes = reinterpret_cast<uint8_t*>(Reg->DirtyBitsFrame(0)->data()) + (gsi / 8);

	LOG_INFO_F("[DirtyBits] CacheIndexStart=%zu  byteOffset=%zu", gsi, gsi / 8);
	for (int b = 0; b < (kCount + 7) / 8; ++b)
		LOG_INFO_F("[DirtyBits]   byte[%d] = 0x%02X  (expected 0xFF)", b, dirtyBytes[b]);

	for (int b = 0; b < (kCount + 7) / 8; ++b)
		ASSERT_EQ((int)dirtyBytes[b], 0xFF);

	Engine.ResetRegistry();
}
*/

// ---------------------------------------------------------------------------
// Helper: writes common cube fields into a hydrated entity view.
// Avoids duplicating the Hydrate → field-write loop across every spawn test.
// ---------------------------------------------------------------------------
struct CubeSetup
{
	float x, y, z;
	float halfX, halfY, halfZ;
	float mass;
	float r, g, b;
	uint32_t motion;
};

static void WriteCubeSetups(Registry* reg, const std::vector<CubeSetup>& setups,
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
			arch->BuildFieldArrayTable(chunk, fieldArrayTable, reg->GetTemporalCache()->GetActiveWriteFrame(), reg->GetVolatileCache()->GetActiveWriteFrame());
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

struct ProjectileSetup
{
	float x, y, z;
	float velX, velY, velZ;
	float r, g, b, a;
};

static void WriteProjectileSetups(Registry* reg, const std::vector<ProjectileSetup>& setups,
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
			arch->BuildFieldArrayTable(chunk, fieldArrayTable, reg->GetTemporalCache()->GetActiveWriteFrame(), reg->GetVolatileCache()->GetActiveWriteFrame());
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

static void WriteSuperCubeSetups(Registry* reg, const std::vector<CubeSetup>& setups,
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
			arch->BuildFieldArrayTable(chunk, fieldArrayTable, reg->GetTemporalCache()->GetActiveWriteFrame(), reg->GetVolatileCache()->GetActiveWriteFrame());
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

// ===========================================================================
// Runtime tests — execute after the engine loop is active (threads + jobs up)
// ===========================================================================

// File-scope entity ID storage for runtime spawn/cleanup
static std::vector<EntityHandle> gPyramidIds;
static std::vector<EntityHandle> gSuperCubeIds;
static std::vector<EntityHandle> gProjectileIds;

RUNTIME_TEST(Runtime_JobsInitialized)
{
	ASSERT(Engine.GetJobsInitialized());
}

// ---------------------------------------------------------------------------
// Jolt Pyramid — N-tier pyramid of dynamic physics cubes on a static floor.
// Exercises: batch entity creation, Jolt body flush, collision response,
//            transform pull-back, GPU predicate/scatter with Active flags.
// Persistent — stays up for the duration of the session.
// ---------------------------------------------------------------------------
RUNTIME_TEST(Spawn_JoltPyramid)
{
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_real_distribution<float> colorDist(0.2f, 1.0f);

	constexpr float cBoxSize       = 2.0f;
	constexpr float cHalfBoxSize   = 1.0f;
	constexpr float cBoxSeparation = 0.5f;
	constexpr int cPyramidHeight   = 25;
	constexpr float xOffset        = 0.0f;
	constexpr float yOffset        = -30.0f;
	constexpr float zOffset        = -100.0f;

	static std::vector<CubeSetup> setups;
	setups.clear();

	for (int layer = 0; layer < cPyramidHeight; ++layer)
	{
		for (int j = layer / 2; j < cPyramidHeight - (layer + 1) / 2; ++j)
		{
			for (int k = layer / 2; k < cPyramidHeight - (layer + 1) / 2; ++k)
			{
				setups.push_back({
					xOffset + static_cast<float>(-cPyramidHeight) + cBoxSize * static_cast<float>(j) + ((layer & 1) ? cHalfBoxSize : 0.0f),
					yOffset + 1.0f + (cBoxSize + cBoxSeparation) * static_cast<float>(layer),
					zOffset + static_cast<float>(-cPyramidHeight) + cBoxSize * static_cast<float>(k) + ((layer & 1) ? cHalfBoxSize : 0.0f),
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
		xOffset, yOffset - 1.0f, zOffset,
		50.0f, 1.0f, 50.0f,
		0.0f,
		0.3f, 0.3f, 0.3f,
		JoltMotion::Static
	});

	Engine.Spawn([](Registry* reg)
	{
		WriteCubeSetups(reg, setups, gPyramidIds);
	});

	LOG_ALWAYS_F("Jolt Pyramid: %d-tier, %d dynamic + 1 floor = %zu entities (persistent)",
				 cPyramidHeight, static_cast<int>(setups.size()) - 1, setups.size());
}

// ---------------------------------------------------------------------------
// SuperCube Grid — spawns a requested number of non-physics cubes in a grid.
// Exercises: high entity count, ScalarUpdate color animation, no Jolt overhead.
// Self-destructs after 30 seconds.
// ---------------------------------------------------------------------------
RUNTIME_TEST(Spawn_SuperCubeGrid)
{
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_real_distribution<float> colorDist(0.2f, 1.0f);

	constexpr int kCount      = 100000;
	constexpr float kSpacing  = 3.0f;
	constexpr float kCubeHalf = 0.5f;
	constexpr float kYBase    = 10.0f;
	constexpr float kZOffset  = -200.0f;

	int gridSide   = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(kCount))));
	float gridHalf = static_cast<float>(gridSide) * kSpacing * 0.5f;

	static std::vector<CubeSetup> setups;
	setups.clear();
	setups.reserve(kCount);

	for (int i = 0; i < kCount; ++i)
	{
		int row = i / gridSide;
		int col = i % gridSide;

		setups.push_back({
			static_cast<float>(col) * kSpacing - gridHalf,
			kYBase + static_cast<float>(row) * kSpacing,
			kZOffset,
			kCubeHalf, kCubeHalf, kCubeHalf,
			0.0f,
			colorDist(gen), colorDist(gen), colorDist(gen),
			JoltMotion::Static
		});
	}

	Engine.Spawn([](Registry* reg)
	{
		WriteSuperCubeSetups(reg, setups, gSuperCubeIds);
	});

	LOG_ALWAYS_F("SuperCube Grid: %d entities in %dx%d grid (30s lifetime)",
				 kCount, gridSide, gridSide);

	// Self-destruct after 30 seconds
	auto* idsPtr = &gSuperCubeIds;
	std::thread([idsPtr]()
	{
		SDL_Delay(30000);
		TrinyxEngine::Get().Spawn([idsPtr](Registry* reg)
		{
			for (EntityHandle id : *idsPtr) reg->Destroy(id);
			LOG_ALWAYS_F("[RuntimeTest] SuperCube Grid: destroyed %zu entities after 30s",
						 idsPtr->size());
			idsPtr->clear();
		});
	}).detach();
}

// ---------------------------------------------------------------------------
// Projectile Burst — spawns a fan of projectiles from a single origin.
// Exercises: high-count minimal-field entities, velocity integration,
//            alpha fade, AVX2 wide-path throughput.
// Self-destructs after 30 seconds.
// ---------------------------------------------------------------------------
RUNTIME_TEST(Spawn_ProjectileBurst)
{
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_real_distribution<float> spreadDist(-20.0f, 20.0f);
	std::uniform_real_distribution<float> speedDist(30.0f, 80.0f);
	std::uniform_real_distribution<float> colorDist(0.4f, 1.0f);

	constexpr int kCount     = 0;
	constexpr float kOriginY = 20.0f;
	constexpr float kOriginZ = -50.0f;

	static std::vector<ProjectileSetup> setups;
	setups.clear();
	setups.reserve(kCount);

	for (int i = 0; i < kCount; ++i)
	{
		float speed = speedDist(gen);
		setups.push_back({
			0.0f, kOriginY, kOriginZ,
			spreadDist(gen), spreadDist(gen) + 10.0f, -speed,
			colorDist(gen), colorDist(gen) * 0.5f, 0.1f, 1.0f
		});
	}

	Engine.Spawn([](Registry* reg)
	{
		WriteProjectileSetups(reg, setups, gProjectileIds);
	});

	LOG_ALWAYS_F("Projectile Burst: %d projectiles from origin (0, %.0f, %.0f) (30s lifetime)",
				 kCount, kOriginY, kOriginZ);

	// Self-destruct after 30 seconds
	auto* idsPtr = &gProjectileIds;
	std::thread([idsPtr]()
	{
		SDL_Delay(30000);
		TrinyxEngine::Get().Spawn([idsPtr](Registry* reg)
		{
			for (EntityHandle id : *idsPtr) reg->Destroy(id);
			LOG_ALWAYS_F("[RuntimeTest] Projectile Burst: destroyed %zu entities after 30s",
						 idsPtr->size());
			idsPtr->clear();
		});
	}).detach();
}

RUNTIME_TEST(Runtime_EntityCountValid)
{
	Registry* Reg        = Engine.GetRegistry();
	size_t totalEntities = Reg->GetTotalEntityCount();
	LOG_ALWAYS_F("[RuntimeTest] Total entities alive: %zu", totalEntities);
	ASSERT(totalEntities > 0);
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

	void PostStart(TrinyxEngine& engine)
	{
		RuntimeTestRegistry::Instance().RunAll(engine);
	}
};

TNX_IMPLEMENT_GAME(TestbedGame)
