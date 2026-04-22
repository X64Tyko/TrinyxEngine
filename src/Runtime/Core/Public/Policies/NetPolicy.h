#pragma once

// ---------------------------------------------------------------------------
// Net-role policies for LogicThread<TNetMode, TRollbackMode, TFrameMode>.
//
// TNetMode controls input injection and replication dispatch for this world.
//
//   SoloSim       — offline, no networking. Both hooks are no-ops.
//   AuthoritySim  — server-side. Defined in src/Runtime/Net/Public/AuthoritySim.h.
//   OwnerSim      — client-side. Defined in src/Runtime/Net/Public/OwnerSim.h.
//
// Each policy exposes:
//   template <typename TLogic> bool OnSimInput(uint32_t frame, TLogic& logic)
//   template <typename TLogic> void OnFramePublished(uint32_t frame, TLogic& logic)
// ---------------------------------------------------------------------------

struct SoloSim
{
template <typename TLogic>
bool OnSimInput(uint32_t /*frame*/, TLogic& /*logic*/) { return false; }

template <typename TLogic>
void OnFramePublished(uint32_t /*frame*/, TLogic& /*logic*/) {}
};
