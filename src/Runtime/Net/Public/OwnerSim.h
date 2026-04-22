#pragma once
#include <atomic>
#include <cstdint>

#include "Input.h"
#include "NetTypes.h"
#include "TrinyxMPSCRing.h"

// ---------------------------------------------------------------------------
// OwnerSim — client-side net policy for LogicThread<OwnerSim, ...>.
//
// OnSimInput: snapshots the current frame's input into InputAccumRing
//             immediately after SimInput->Swap(). Never stalls — returns false.
// OnFramePublished: no-op — clients do not drive CommittedFrameHorizon.
//
// Initialized by OwnerNet::WireNetMode(World*) after handshake completes.
// ---------------------------------------------------------------------------

struct OwnerSim
{
template <typename TLogic>
bool OnSimInput(uint32_t frameNumber, TLogic& logic);

template <typename TLogic>
void OnFramePublished(uint32_t /*frameNumber*/, TLogic& /*logic*/) {}

// Called by World::Initialize when running as Client, or by OwnerNet::WireNetMode.
void Initialize(TrinyxMPSCRing<NetInputFrame>* ring,
                const std::atomic<bool>*        enabled,
                InputBuffer*                    simInput);

private:
TrinyxMPSCRing<NetInputFrame>* InputAccumRing    = nullptr;
const std::atomic<bool>*       InputAccumEnabled = nullptr;
InputBuffer*                   SimInput          = nullptr;
};

// ---------------------------------------------------------------------------
// Inline template method body
// ---------------------------------------------------------------------------

template <typename TLogic>
bool OwnerSim::OnSimInput(uint32_t frameNumber, TLogic& /*logic*/)
{
if (!InputAccumRing || !InputAccumEnabled || !InputAccumEnabled->load(std::memory_order_acquire))
return false;

NetInputFrame snap{};
snap.Frame = frameNumber;
SimInput->SnapshotKeyState(snap.State.KeyState, sizeof(snap.State.KeyState));
snap.State.MouseDX      = SimInput->GetMouseDX();
snap.State.MouseDY      = SimInput->GetMouseDY();
snap.State.MouseButtons = SimInput->GetMouseButtonMask();

const uint16_t evCount = SimInput->GetEventCount();
snap.EventCount = static_cast<uint8_t>(evCount < 8 ? evCount : 8);
for (uint8_t i = 0; i < snap.EventCount; ++i)
{
InputData e = SimInput->ReadEvent();
snap.Events[i].Key           = static_cast<uint32_t>(e.Key);
snap.Events[i].FrameUSOffset = e.FrameUSOffset;
snap.Events[i].Pressed       = e.Pressed;
snap.Events[i]._Pad          = 0;
}

// Overwrite the oldest pre-connection frame if the ring is full so the consumer
// always sees the most recent Capacity frames when it drains on connect.
InputAccumRing->OverwritePush(snap);
return false;
}
