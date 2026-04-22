#include "OwnerSim.h"

void OwnerSim::Initialize(TrinyxMPSCRing<NetInputFrame>* ring,
                           const std::atomic<bool>*        enabled,
                           InputBuffer*                    simInput)
{
	InputAccumRing    = ring;
	InputAccumEnabled = enabled;
	SimInput          = simInput;
}
