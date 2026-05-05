#include "DefragSystem.h"
#include "Registry.h"
#include <algorithm>

void DefragSystem::Tick(const FlatMap<Archetype::ArchetypeKey, Archetype*>& archetypes,
                        Registry& registry,
                        TrinyxJobs::WorldQueueHandle wq)
{
    if (++FrameCounter < AnalysisCadence) return;
    FrameCounter = 0;

    for (const auto& [key, arch] : archetypes)
    {
        if (arch->TotalEntityCount < MinLiveEntities)   continue;
        if (arch->AllocatedEntityCount == 0)             continue;
        if (arch->InactiveEntitySlots.empty())           continue;

        const float holeRatio = 1.0f - static_cast<float>(arch->TotalEntityCount)
                                     / static_cast<float>(arch->AllocatedEntityCount);
        if (holeRatio < HoleThreshold) continue;

        const uint32_t estimatedMoves = arch->AllocatedEntityCount - arch->TotalEntityCount;
        PostMoveJob(registry, arch, estimatedMoves, wq);
    }
}

void DefragSystem::PostMoveJob(Registry& registry,
                                Archetype* arch,
                                uint32_t   movesRemaining,
                                TrinyxJobs::WorldQueueHandle wq)
{
    if (wq == TrinyxJobs::InvalidWorldQueue) return;
    DefragSystem* self = this;
    Registry*     reg  = &registry;
    TrinyxJobs::Post([self, reg, arch, movesRemaining, wq](uint32_t)
    {
        self->ProcessMoves(*reg, arch, movesRemaining, wq);
    }, wq);
}

void DefragSystem::ProcessMoves(Registry& registry,
                                 Archetype* arch,
                                 uint32_t   movesRemaining,
                                 TrinyxJobs::WorldQueueHandle wq)
{
    if (arch->InactiveEntitySlots.empty() || arch->TotalEntityCount == 0) return;

    // Sort InactiveEntitySlots ascending so:
    //   (a) binary search works for the "is this slot inactive?" check, and
    //   (b) the lowest-index holes are naturally at the front for pair building.
    std::sort(arch->InactiveEntitySlots.begin(), arch->InactiveEntitySlots.end(),
              [](const Archetype::EntitySlot& a, const Archetype::EntitySlot& b)
              { return a.ArchIndex < b.ArchIndex; });

    // Pre-compute up to MaxMovesPerTick (src, dst) pairs before touching any
    // bookkeeping, so the sorted InactiveEntitySlots stays consistent throughout.
    //
    // dst — lowest-ArchIndex inactive slot  (index into InactiveEntitySlots[dstIdx])
    // src — highest-ArchIndex live slot     (walk AllocatedEntityCount-1 backwards)
    std::array<MovePair, MaxMovesPerTick> pairs{};
    uint32_t moveCount = 0;

    size_t  dstIdx = 0;
    int32_t srcIdx = static_cast<int32_t>(arch->AllocatedEntityCount) - 1;

    while (moveCount < MaxMovesPerTick && movesRemaining > 0
           && dstIdx < arch->InactiveEntitySlots.size())
    {
        const Archetype::EntitySlot& dst = arch->InactiveEntitySlots[dstIdx];

        bool foundSrc = false;
        while (srcIdx > static_cast<int32_t>(dst.ArchIndex))
        {
            const Archetype::EntitySlot& candidate = arch->ActiveEntitySlots[srcIdx];

            const bool isInactive = std::binary_search(
                arch->InactiveEntitySlots.begin(), arch->InactiveEntitySlots.end(),
                candidate,
                [](const Archetype::EntitySlot& a, const Archetype::EntitySlot& b)
                { return a.ArchIndex < b.ArchIndex; });

            if (!isInactive)
            {
                pairs[moveCount++] = {candidate, dst};
                ++dstIdx;
                --srcIdx;
                --movesRemaining;
                foundSrc = true;
                break;
            }
            --srcIdx;
        }

        if (!foundSrc) break; // no live slots remain above any remaining hole
    }

    // Execute all moves — each updates EntityRecord, CacheToRecord, and ChunkLiveCounts.
    for (uint32_t i = 0; i < moveCount; ++i)
        registry.ExecuteDefragMove(arch, pairs[i].Src, pairs[i].Dst);

    // Replace the filled dst entries (first moveCount positions in the ascending-sorted
    // InactiveEntitySlots) with the now-empty src slots, then re-sort descending so
    // PushEntities (which pops back()) gets the lowest-index hole first, keeping
    // future allocations compact.
    for (uint32_t i = 0; i < moveCount; ++i)
        arch->InactiveEntitySlots[i] = pairs[i].Src;

    std::sort(arch->InactiveEntitySlots.begin(), arch->InactiveEntitySlots.end(),
              [](const Archetype::EntitySlot& a, const Archetype::EntitySlot& b)
              { return a.ArchIndex > b.ArchIndex; });

    // Free trailing chunks that are now fully empty (ChunkLiveCounts.back() == 0).
    registry.TrimTailChunks(arch);

    // Re-post if the budget was exhausted before fragmentation was resolved.
    if (movesRemaining > 0 && !arch->InactiveEntitySlots.empty()
        && arch->AllocatedEntityCount > 0)
    {
        const float holeRatio = 1.0f - static_cast<float>(arch->TotalEntityCount)
                                     / static_cast<float>(arch->AllocatedEntityCount);
        if (holeRatio >= HoleThreshold)
            PostMoveJob(registry, arch, movesRemaining, wq);
    }
}