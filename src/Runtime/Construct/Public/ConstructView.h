#pragma once

#include "Archetype.h"
#include "EntityBuilder.h"
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
template <template <FieldWidth> class TEntity>
class ConstructView : public TEntity<FieldWidth::Scalar>
{
	using TEntityView = TEntity<FieldWidth::Scalar>;

public:
	ConstructView()
		: TEntityView()
	{
	}

	~ConstructView() { Shutdown(); }

	ConstructView(const ConstructView&)            = delete;
	ConstructView& operator=(const ConstructView&) = delete;

	template <typename TConstruct>
	void Initialize(TConstruct* owner)
	{
		Reg         = owner->GetRegistry();
		Handle      = Reg->Create<TEntity<FieldWidth::Scalar>>();
		bOwnsEntity = true;
		RehydrateCursors();
		this->SetFlags(TemporalFlagBits::Active | TemporalFlagBits::Alive);

		using Self = ConstructView;
		Reg->BindOnCacheSlotChange<Self, &Self::OnCacheSlotChanged>(Handle, this);

		// Auto-register with owning Construct
		owner->RegisterView({
			this,
			[](void* ptr) { static_cast<Self*>(ptr)->EnsureHydrated(); },
			[](void* ptr) -> EntityHandle { return static_cast<Self*>(ptr)->GetEntityHandle(); }
		});
	}

	// Initialize with an init lambda — asset checkouts are drained after fn returns.
	template <typename TConstruct, std::invocable<TEntityView&> Fn>
	void Initialize(TConstruct* owner, Fn&& fn)
	{
		Reg         = owner->GetRegistry();
		Handle      = Reg->Create<TEntity<FieldWidth::Scalar>>(std::forward < Fn > (fn));
		bOwnsEntity = true;
		RehydrateCursors();
		this->SetFlags(TemporalFlagBits::Active | TemporalFlagBits::Alive);

		using Self = ConstructView;
		Reg->BindOnCacheSlotChange<Self, &Self::OnCacheSlotChanged>(Handle, this);

		owner->RegisterView({
			this,
			[](void* ptr) { static_cast<Self*>(ptr)->EnsureHydrated(); },
			[](void* ptr) -> EntityHandle { return static_cast<Self*>(ptr)->GetEntityHandle(); }
		});
	}

	// Initialize from a prefab AssetID — loads JSON, applies fields, wires asset checkouts.
	template <typename TConstruct>
	void Initialize(TConstruct* owner, AssetID prefabID)
	{
		Reg         = owner->GetRegistry();
		Handle      = EntityBuilder::SpawnTyped<TEntity>(Reg, prefabID);
		bOwnsEntity = true;
		RehydrateCursors();
		this->SetFlags(TemporalFlagBits::Active | TemporalFlagBits::Alive);

		using Self = ConstructView;
		Reg->BindOnCacheSlotChange<Self, &Self::OnCacheSlotChanged>(Handle, this);

		owner->RegisterView({
			this,
			[](void* ptr) { static_cast<Self*>(ptr)->EnsureHydrated(); },
			[](void* ptr) -> EntityHandle { return static_cast<Self*>(ptr)->GetEntityHandle(); }
		});
	}

	// Attach binds to an existing entity rather than creating a new one.
	// bOwnsEntity is false — the entity outlives this view.
	template <typename TConstruct>
	void Attach(TConstruct* owner, EntityHandle existing)
	{
		Reg         = owner->GetRegistry();
		Handle      = existing;
		bOwnsEntity = false;
		RehydrateCursors();

		using Self = ConstructView;
		Reg->BindOnCacheSlotChange<Self, &Self::OnCacheSlotChanged>(Handle, this);

		owner->RegisterView({
			this,
			[](void* ptr) { static_cast<Self*>(ptr)->EnsureHydrated(); },
			[](void* ptr) -> EntityHandle { return static_cast<Self*>(ptr)->GetEntityHandle(); }
		});
	}

	template <typename TConstruct>
	void Detach(TConstruct* owner)
	{
		Handle      = {};
		bOwnsEntity = false;
		owner->DeregisterView(this);
	}

	void Shutdown()
	{
		if (Reg && Handle.IsValid())
		{
			using Self = ConstructView;
			Reg->UnbindOnCacheSlotChange<Self, &Self::OnCacheSlotChanged>(Handle, this);

			if (bOwnsEntity) Reg->Destroy(Handle);
			Handle = EntityHandle{};
			Reg    = nullptr;
		}
	}

	EntityHandle GetEntityHandle() const { return Handle; }
	Registry* GetRegistry() const { return Reg; }
	bool IsInitialized() const { return Reg != nullptr && Handle.IsValid(); }

protected:
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
		this->Hydrate(FieldArrayTable, FieldArrayTable[0], Record.LocalIndex);
	}

	Registry* Reg = nullptr;
	EntityHandle Handle{};
	uint32_t LastTemporalFrame = UINT32_MAX;
	uint32_t LastVolatileFrame = UINT32_MAX;
	bool bOwnsEntity;
};
