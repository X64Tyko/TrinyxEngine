#pragma once
#include <cstdint>
#include "Types.h"

/// @brief Per-entity status flags stored in the SoA slab alongside field data.
///
/// Each entity owns one int32_t slot in the flags array. Bits are read by the GPU
/// predicate pass (Active), the rollback system (DirtiedFrame), and the replication
/// system (Replicated, Tombstone, NetConfirmedDead). The low 16 bits are reserved
/// for game-layer use.
enum class TemporalFlagBits : int32_t
{
	Active           = static_cast<int32_t>(1u << 31), ///< Entity ticks, renders, and participates in simulation. Cleared on deactivate or destroy.
	Dirty            = static_cast<int32_t>(1u << 30), ///< Entity data changed — accumulates until render clears
	DirtiedFrame     = static_cast<int32_t>(1u << 29), ///< Entity dirtied THIS frame — cleared at frame start, used for per-frame logic reset
	Replicated       = static_cast<int32_t>(1u << 28), ///< This entity replicates
	Alive            = static_cast<int32_t>(1u << 27), ///< Entity data is valid and slot cannot be reclaimed. Cleared only on deferred destroy.
	Tombstone        = static_cast<int32_t>(1u << 16), ///< Entity is tombstoned (deferred destruction)
	NetConfirmedDead = static_cast<int32_t>(1u << 25), ///< Server confirmed death, waiting for client ack before freeing net handle.

	depthMask = 0xF << 21, ///< 4 bits for attachment depth (bits 24..21)

	// Used for ConstructView bound entities
	PrePhysSkip  = static_cast<int32_t>(1u << 20), ///< if 1 Disable PrePhysics sweep
	PostPhysSkip = static_cast<int32_t>(1u << 19), ///< if 1 Disable PostPhysics sweep
	ScalarSkip   = static_cast<int32_t>(1u << 18), ///< if 1 Disable ScalarUpdate sweep
	ASleep       = static_cast<int32_t>(1u << 17), ///< if 1 Disable in Jolt
												   // Bits 15..0 available for game-layer flags
};

FORCE_INLINE TemporalFlagBits operator|(TemporalFlagBits lhs, TemporalFlagBits rhs)
{
	return static_cast<TemporalFlagBits>(static_cast<int32_t>(lhs) | static_cast<int32_t>(rhs));
}
