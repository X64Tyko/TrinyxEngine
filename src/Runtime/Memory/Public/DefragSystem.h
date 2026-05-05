#pragma once
#include <array>
#include <cstdint>
#include "Archetype.h"
#include "FlatMap.h"
#include "TrinyxJobs.h"

class Registry;

// Archetype entity-slot compactor (phase 1 of 2).
//
// Analyzes archetypes for fragmentation — holes left by entity removals that
// inflate AllocatedEntityCount and force the bitplane scan to touch empty
// slots on every InvokePrePhys / InvokePostPhys / InvokeScalarUpdate call.
//
// When an archetype's hole ratio exceeds HoleThreshold, batches of up to
// MaxMovesPerTick entity moves are posted to the WorldQueue so the Logic
// thread executes them at the top of subsequent frames before any fixed-update
// work begins.  If the budget is exhausted before compaction is complete, the
// job re-posts itself and continues next frame.
//
// Move algorithm (compacts from the tail):
//   dst — lowest-ArchIndex inactive slot  (fill the earliest hole)
//   src — highest-ArchIndex live entity   (evacuate the tail)
// After each batch, trailing chunks whose every slot is now inactive are freed
// and AllocatedEntityCount is decremented, directly shrinking the scan range.
//
// Phase 2 (slab-chunk defrag) will compact the slab allocations left behind by
// freed chunks; that is a separate system.
//
// Thread safety: analysis and all moves run exclusively on the Logic thread
// (via ThreadMain → TickDefrag after DrainWorldQueue + ProcessDeferredDestructions).
class DefragSystem
{
public:
    static constexpr float    HoleThreshold   = 0.20f; // 20% holes triggers work
    static constexpr uint32_t MinLiveEntities = 32;    // skip tiny archetypes
    static constexpr uint32_t AnalysisCadence = 1024;  // re-analyze every N Logic frames (~2 s at 512 Hz)
    static constexpr uint32_t MaxMovesPerTick = 64;    // entity moves per WorldQueue drain

    // Called from the Logic thread each frame (after ProcessDeferredDestructions).
    // Increments the internal frame counter; when the cadence fires, scans all
    // archetypes and posts WorldQueue move jobs for those that need compaction.
    void Tick(const FlatMap<Archetype::ArchetypeKey, Archetype*>& archetypes,
              Registry& registry,
              TrinyxJobs::WorldQueueHandle wq);

    // Executes up to MaxMovesPerTick entity moves for arch, then re-posts itself
    // to wq if moves remain and the archetype is still fragmented.
    // Called only on the Logic thread (via WorldQueue drain).
    void ProcessMoves(Registry& registry,
                      Archetype* arch,
                      uint32_t   movesRemaining,
                      TrinyxJobs::WorldQueueHandle wq);

private:
    struct MovePair
    {
        Archetype::EntitySlot Src;
        Archetype::EntitySlot Dst;
    };

    uint32_t FrameCounter = 0;

    void PostMoveJob(Registry& registry,
                     Archetype* arch,
                     uint32_t   movesRemaining,
                     TrinyxJobs::WorldQueueHandle wq);
};