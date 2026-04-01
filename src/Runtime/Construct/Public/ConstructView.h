#pragma once

#include "Archetype.h"
#include "EntityView.h"
#include "Registry.h"

// ---------------------------------------------------------------------------
// ConstructView — Base helper for Construct Views.
//
// A ConstructView creates a single ECS entity of type ViewEntity and keeps a
// scalar EntityView permanently hydrated at that entity's slot. The Construct
// reads/writes SoA fields through the hydrated FieldProxy cursors.
//
// Template parameters:
//   Derived    — the concrete View class (CRTP)
//   ViewEntity — the internal ECS entity type (e.g., DefaultViewEntity)
//
// The ViewEntity is a regular ECS entity — it participates in wide sweeps,
// physics, rendering, and replication. The ConstructView just provides a
// persistent scalar lens into its fields.
// ---------------------------------------------------------------------------
template <typename Derived, template <FieldWidth> class ViewEntity>
class ConstructView
{
public:
	ConstructView() = default;
	~ConstructView() { Shutdown(); }

	ConstructView(const ConstructView&)            = delete;
	ConstructView& operator=(const ConstructView&) = delete;

	void Initialize(Registry* InRegistry)
	{
		Reg = InRegistry;

		// Create the backing ECS entity
		Handle = Reg->Create<ViewEntity<FieldWidth::Scalar>>();

		// Hydrate the scalar view at this entity's slot
		RehydrateCursors();

		// Mark entity active so GPU predicate includes it
		View.Flags.Flags = static_cast<int32_t>(TemporalFlagBits::Active);

		// Register for defrag — invalidate cursors when cache slot moves
		using Self = ConstructView<Derived, ViewEntity>;
		Reg->BindOnCacheSlotChange<Self, &Self::OnCacheSlotChanged>(Handle, this);
	}

	void Shutdown()
	{
		if (Reg && Handle.IsValid())
		{
			using Self = ConstructView<Derived, ViewEntity>;
			Reg->UnbindOnCacheSlotChange<Self, &Self::OnCacheSlotChanged>(Handle, this);

			Reg->Destroy(Handle);
			Handle = EntityHandle{};
			Reg    = nullptr;
		}
	}

	EntityHandle GetEntityHandle() const { return Handle; }
	Registry* GetRegistry() const { return Reg; }
	bool IsInitialized() const { return Reg != nullptr && Handle.IsValid(); }

protected:
	ViewEntity<FieldWidth::Scalar>& GetView()
	{
		EnsureHydrated();
		return View;
	}

	const ViewEntity<FieldWidth::Scalar>& GetView() const
	{
		const_cast<ConstructView*>(this)->EnsureHydrated();
		return View;
	}

	// Called by EntityRecord::OnCacheSlotChange when defrag relocates the entity.
	// Invalidates cached frame numbers so the next GetView() forces a rehydrate.
	void OnCacheSlotChanged([[maybe_unused]] uint32_t OldIndex, [[maybe_unused]] uint32_t NewIndex)
	{
		LastTemporalFrame = UINT32_MAX;
		LastVolatileFrame = UINT32_MAX;
	}

private:
	void EnsureHydrated()
	{
		uint32_t TemporalWrite = Reg->GetTemporalCache()->GetActiveWriteFrame();
		uint32_t VolatileWrite = Reg->GetVolatileCache()->GetActiveWriteFrame();
		if (TemporalWrite == LastTemporalFrame && VolatileWrite == LastVolatileFrame) return;
		LastTemporalFrame = TemporalWrite;
		LastVolatileFrame = VolatileWrite;
		RehydrateCursors(TemporalWrite, VolatileWrite);
	}

	void RehydrateCursors()
	{
		RehydrateCursors(
			Reg->GetTemporalCache()->GetActiveWriteFrame(),
			Reg->GetVolatileCache()->GetActiveWriteFrame());
	}

	void RehydrateCursors(uint32_t temporalWrite, uint32_t volatileWrite)
	{
		EntityRecord Record = Reg->GetRecord(Handle);
		if (!Record.IsValid()) return;

		void* FieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
		Record.Arch->BuildFieldArrayTable(Record.TargetChunk, FieldArrayTable, temporalWrite, volatileWrite);
		View.Hydrate(FieldArrayTable, FieldArrayTable[0], Record.LocalIndex);
	}

	Registry* Reg = nullptr;
	EntityHandle Handle{};
	ViewEntity<FieldWidth::Scalar> View;
	uint32_t LastTemporalFrame = UINT32_MAX;
	uint32_t LastVolatileFrame = UINT32_MAX;
};
