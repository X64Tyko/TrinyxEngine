#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "Registry.h"
#include "TemporalComponentCache.h"

// Validates the Volatile SoA slab: non-null, at least 3 frames (triple-buffer minimum),
// and reports the correct CacheTier. Volatile is always present regardless of build flags.
RUNTIME_TEST(Slab_VolatileCacheValid)
{
	Registry* Reg                = Engine.GetRegistry();
	ComponentCacheBase* Volatile = Reg->GetVolatileCache();

	ASSERT(Volatile != nullptr);
	ASSERT(Volatile->GetTotalFrameCount() >= 3);
	ASSERT(Volatile->GetTier() == CacheTier::Volatile);
}
