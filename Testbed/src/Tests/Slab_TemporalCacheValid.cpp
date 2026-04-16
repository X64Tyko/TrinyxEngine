#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "TemporalComponentCache.h"

// Validates the Temporal SoA slab behavior based on build configuration:
//   - TNX_ENABLE_ROLLBACK ON  → dedicated Temporal slab, tier = Temporal, frames >= 8
//   - TNX_ENABLE_ROLLBACK OFF → aliases Volatile slab, tier = Volatile, frames >= 3
//
// This test captures the contract both paths must honor: callers using GetTemporalCache()
// always get a valid, usable cache regardless of rollback build flag.
RUNTIME_TEST(Slab_TemporalCacheValid)
{
	Registry* Reg               = Engine.GetRegistry();
	ComponentCacheBase* Temporal = Reg->GetTemporalCache();

	ASSERT(Temporal != nullptr);

#ifdef TNX_ENABLE_ROLLBACK
	ASSERT(Temporal->GetTier() == CacheTier::Temporal);
	ASSERT(Temporal->GetTotalFrameCount() >= 8);
#else
	// Without rollback, GetTemporalCache() aliases the Volatile slab
	ASSERT(Temporal->GetTier() == CacheTier::Volatile);
	ASSERT(Temporal->GetTotalFrameCount() >= 3);
	ASSERT(Temporal == static_cast<ComponentCacheBase*>(Reg->GetVolatileCache()));
#endif
}
