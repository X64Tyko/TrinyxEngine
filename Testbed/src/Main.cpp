#include "Registry.h"
#include <iostream>
#include <random>

#include "StrigidEngine.h"
#include "TestEntity.h"
#include "Public/CubeEntity.h"
#include "Archetype.h"
#include "Logger.h"
#include "TestFramework.h"

using namespace Strigid::Testing;

TEST(Registry_CreateEntities)
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

TEST(Registry_ValidEntityIDs)
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

TEST(Registry_DestroyAndReuse)
{
    Registry* Reg = Engine.GetRegistry();
    std::vector<EntityID> Entities;

    for (int i = 0; i < 10; ++i)
    {
        Entities.push_back(Reg->Create<TestEntity<>>());
    }

    uint32_t firstIndex = Entities[0].GetIndex();
    uint32_t firstGeneration = Entities[0].GetGeneration();

    Reg->Destroy(Entities[0]);
    Reg->ProcessDeferredDestructions();

    EntityID NewId = Reg->Create<TestEntity<>>();
    ASSERT_EQ(NewId.GetIndex(), firstIndex);
    ASSERT(NewId.GetGeneration() > firstGeneration);

    Reg->ResetRegistry();
}

TEST(InitializeTestEntities)
{
    Registry* Reg = Engine.GetRegistry();
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<float> posX(-30.0f, 30.0f);
    std::uniform_real_distribution<float> posY(-30.0f, 30.0f);
    std::uniform_real_distribution<float> posZ(-500.0f, -200.0f);
    std::uniform_real_distribution<float> velX(-10.0f, 10.0f);
    std::uniform_real_distribution<float> velY(-10.0f, 10.0f);
    std::uniform_real_distribution<float> color(0.2f, 1.0f);

    static int32_t CubeCount = 10000;
    static int32_t SuperCount = 90000;

    // Step 1: Create all entities first
    entityIDs.reserve(CubeCount + SuperCount);
    for (int i = 0; i < CubeCount; ++i)
    {
        EntityID id = Reg->Create<CubeEntity<>>();
        entityIDs.push_back(id);
    }
    for (int i = 0; i < SuperCount; ++i)
    {
        EntityID id = Reg->Create<SuperCube<>>();
        entityIDs.push_back(id);
    }

    LOG_ALWAYS_F("Created %i test entities", CubeCount + SuperCount);

    // Step 2: Initialize by iterating through archetypes/chunks
    std::vector<Archetype*> Arches = Reg->ClassQuery<CubeEntity<>, SuperCube<>>();
    for (Archetype* cubeArch : Arches)
    {
        CubeEntity<> Cube;
        for (size_t chunkIdx = 0; chunkIdx < cubeArch->Chunks.size(); ++chunkIdx)
        {
            Chunk* chunk = cubeArch->Chunks[chunkIdx];
            uint32_t entityCount = cubeArch->GetChunkCount(chunkIdx);

            // Build field array table
            void* fieldArrayTable[MAX_FIELD_ARRAYS];
            cubeArch->BuildFieldArrayTable(chunk, fieldArrayTable, 0, 0);
            Cube.Hydrate(fieldArrayTable);
            
            // Initialize all entities in this chunk
            for (uint32_t i = 0; i < entityCount; ++i, Cube.Advance(1))
            {
                Cube.Flags.Flags = static_cast<int32_t>(TemporalFlagBits::Active);
                Cube.transform.PositionX = posX(gen);
                Cube.transform.PositionY = posY(gen);
                Cube.transform.PositionZ = posZ(gen);
                Cube.transform.RotationX = 0.0f;
                Cube.transform.RotationY = 0.0f;
                Cube.transform.RotationZ = 0.0f;
                Cube.transform.ScaleX = 1.0f;
                Cube.transform.ScaleY = 1.0f;
                Cube.transform.ScaleZ = 1.0f;

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

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
    // Stack allocation is fine for the main engine object
    StrigidEngine& Engine = StrigidEngine::Get();

    if (Engine.Initialize("Strigid v0.1", 1920, 1080))
    {
        // Run unit tests
        if (TestRegistry::Instance().RunAll(Engine) != 0)
        {
            return 1; // Exit with error if tests failed
        }

        Engine.Run();
    }

    return 0;
}
