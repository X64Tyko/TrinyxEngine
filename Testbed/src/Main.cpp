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

    static int32_t EntityCount = 100000;

    // Step 1: Create all entities first
    entityIDs.reserve(EntityCount);
    for (int i = 0; i < EntityCount; ++i)
    {
        EntityID id = Reg->Create<CubeEntity<>>();
        entityIDs.push_back(id);
    }

    LOG_ALWAYS_F("Created %i test entities", EntityCount);

    // Step 2: Initialize by iterating through archetypes/chunks
    Archetype* cubeArch = Reg->GetOrCreateArchetype(
        MetaRegistry::Get().ClassToArchetype[CubeEntity<>::StaticClassID()],
        CubeEntity<>::StaticClassID());
    if (cubeArch)
    {
        constexpr size_t MAX_FIELD_ARRAYS = 256;

        for (size_t chunkIdx = 0; chunkIdx < cubeArch->Chunks.size(); ++chunkIdx)
        {
            Chunk* chunk = cubeArch->Chunks[chunkIdx];
            uint32_t entityCount = cubeArch->GetChunkCount(chunkIdx);

            // Build field array table
            void* fieldArrayTable[MAX_FIELD_ARRAYS];
            cubeArch->BuildFieldArrayTable(chunk, fieldArrayTable, 0, 0);

            // Get field arrays for Transform (component ID 1)
            // Transform has 9 fields: PositionX, PositionY, PositionZ, RotX, RotY, RotZ, ScaleX, ScaleY, ScaleZ
            auto posXArray = static_cast<float*>(fieldArrayTable[0]);
            auto posYArray = static_cast<float*>(fieldArrayTable[2]);
            auto posZArray = static_cast<float*>(fieldArrayTable[4]);
            auto rotXArray = static_cast<float*>(fieldArrayTable[6]);
            auto rotYArray = static_cast<float*>(fieldArrayTable[8]);
            auto rotZArray = static_cast<float*>(fieldArrayTable[10]);
            auto scaleXArray = static_cast<float*>(fieldArrayTable[12]);
            auto scaleYArray = static_cast<float*>(fieldArrayTable[14]);
            auto scaleZArray = static_cast<float*>(fieldArrayTable[16]);

            // Velocity starts after Transform (12 fields), so index 12-14
            auto velXArray = static_cast<float*>(fieldArrayTable[18]);
            auto velYArray = static_cast<float*>(fieldArrayTable[20]);
            auto velZArray = static_cast<float*>(fieldArrayTable[22]);

            // ColorData starts after Velocity (4 fields), so index 16-19
            auto rArray = static_cast<float*>(fieldArrayTable[24]);
            auto gArray = static_cast<float*>(fieldArrayTable[26]);
            auto bArray = static_cast<float*>(fieldArrayTable[28]);
            auto aArray = static_cast<float*>(fieldArrayTable[30]);
/*
            auto rArray = static_cast<float*>(fieldArrayTable[18]);
            auto gArray = static_cast<float*>(fieldArrayTable[20]);
            auto bArray = static_cast<float*>(fieldArrayTable[22]);
            auto aArray = static_cast<float*>(fieldArrayTable[24]);
*/
            // Initialize all entities in this chunk
            for (uint32_t i = 0; i < entityCount; ++i)
            {
                posXArray[i] = posX(gen);
                posYArray[i] = posY(gen);
                posZArray[i] = posZ(gen);
                rotXArray[i] = 0.0f;
                rotYArray[i] = 0.0f;
                rotZArray[i] = 0.0f;
                scaleXArray[i] = 1.0f;
                scaleYArray[i] = 1.0f;
                scaleZArray[i] = 1.0f;

                velXArray[i] = velX(gen);
                velYArray[i] = velY(gen);
                velZArray[i] = 0.0f;

                rArray[i] = color(gen);
                gArray[i] = color(gen);
                bArray[i] = color(gen);
                aArray[i] = 1.0f;
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
