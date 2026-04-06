#pragma once

#include "LogicThread.h"
#include "Schema.h"
#include "World.h"

struct ConstructViewRef
{
	void* View;
	void (*HydrateFn)(void*);

	void EnsureHydrated() { HydrateFn(View); }
};

// ---------------------------------------------------------------------------
// Construct lifetime declaration macros.
// Place one of these in the public section of a Construct-derived class.
// They expand to the static constexpr Lifetime member and will be extended
// with additional wiring (registry tags, concept constraints, etc.) later.
// ---------------------------------------------------------------------------
#define TNX_CONSTRUCT_LEVEL      static constexpr ConstructLifetime Lifetime = ConstructLifetime::Level;
#define TNX_CONSTRUCT_WORLD      static constexpr ConstructLifetime Lifetime = ConstructLifetime::World;
#define TNX_CONSTRUCT_SESSION    static constexpr ConstructLifetime Lifetime = ConstructLifetime::Session;
#define TNX_CONSTRUCT_PERSISTENT static constexpr ConstructLifetime Lifetime = ConstructLifetime::Persistent;

// ---------------------------------------------------------------------------
// Construct<T> — CRTP lifecycle base for singular OOP gameplay objects.
//
// Constructs are the "things that think" — Player, GameMode, AIDirector.
// They own Views (scalar lenses into ECS data) and hold bespoke C++ state.
// Tick methods are auto-registered via concept detection: implement the
// method, get the tick. Don't implement it, pay nothing.
//
// Lifecycle:
//   1. Heap-allocate (or Owned<T> in a parent Construct)
//   2. Call Initialize(World*) — hydrates Views, registers ticks
//   3. Engine ticks via ConstructBatch
//   4. Call Shutdown() or destroy — deregisters ticks, destroys View entities
//
// Thread safety:
//   Initialize/Shutdown must be called within a Spawn() callback or during
//   PostStart (before threads run). Same contract as entity creation.
//
// Derived class hooks (all optional, detected via concepts):
//   void InitializeViews()       — create and set up Views
//   void PrePhysics(SimFloat dt) — runs after wide entity PrePhysics sweep
//   void PostPhysics(SimFloat dt)— runs after wide entity PostPhysics sweep
//   void ScalarUpdate(SimFloat dt)— runs after entity ScalarUpdate
// ---------------------------------------------------------------------------
template <typename Derived>
class Construct
{
public:
	// Default lifetime tier. Derived classes override via TNX_CONSTRUCT_SESSION, etc.
	static constexpr ConstructLifetime Lifetime = ConstructLifetime::World;

	void Initialize(World* InWorld)
	{
		OwnerWorld = InWorld;

		// Let the derived class create and set up its Views
		if constexpr (requires { static_cast<Derived*>(this)->InitializeViews(); })
		{
			static_cast<Derived*>(this)->InitializeViews();
		}

		// Auto-register tick methods via concept detection
		LogicThread* Logic = OwnerWorld->GetLogicThread();

		if constexpr (HasPrePhysics<Derived>)
		{
			Logic->ScalarPrePhysicsBatch.Register<Construct, &Construct::PrePhysBase>(
				static_cast<Derived*>(this));
		}

		if constexpr (HasPostPhysics<Derived>)
		{
			Logic->ScalarPostPhysicsBatch.Register<Construct, &Construct::PostPhysBase>(
				static_cast<Derived*>(this));
		}

		if constexpr (HasScalarUpdate<Derived>)
		{
			Logic->ScalarUpdateBatch.Register<Construct, &Construct::ScalarUpdateBase>(
				static_cast<Derived*>(this));
		}

		if constexpr (HasPhysicsStep<Derived>)
		{
			Logic->ScalarPhysicsStepBatch.Register<Construct, &Construct::PhysicsStepBase>(
				static_cast<Derived*>(this));
		}

		bInitialized = true;
	}

	void Shutdown()
	{
		if (!bInitialized) return;

		LogicThread* Logic = OwnerWorld->GetLogicThread();
		Logic->ScalarPrePhysicsBatch.Deregister(static_cast<Derived*>(this));
		Logic->ScalarPostPhysicsBatch.Deregister(static_cast<Derived*>(this));
		Logic->ScalarPhysicsStepBatch.Deregister(static_cast<Derived*>(this));
		Logic->ScalarUpdateBatch.Deregister(static_cast<Derived*>(this));

		bInitialized = false;
		OwnerWorld   = nullptr;
	}

	void RegisterView(ConstructViewRef ref)
	{
		Views[ViewCount++] = ref;
	}

	void DeregisterView(void* viewPtr)
	{
		for (uint32_t i = 0; i < ViewCount; ++i)
		{
			if (Views[i].View == viewPtr)
			{
				Views[i] = Views[--ViewCount]; // swap-remove
				return;
			}
		}
	}

	World* GetWorld() const { return OwnerWorld; }
	Registry* GetRegistry() const { return OwnerWorld ? OwnerWorld->GetRegistry() : nullptr; }
	bool IsInitialized() const { return bInitialized; }
	uint32_t GetConstructID() const { return ConstructID; }
	void SetConstructID(uint32_t id) { ConstructID = id; }

protected:
	Construct() = default;

	~Construct()
	{
		if (bInitialized) Shutdown();
	}

private:
	World* OwnerWorld    = nullptr;
	uint32_t ConstructID = 0;
	bool bInitialized    = false;

	static constexpr uint32_t MaxViews = 8;
	ConstructViewRef Views[MaxViews];
	uint32_t ViewCount = 0;

	void HydrateAllViews()
	{
		for (uint32_t i = 0; i < ViewCount; ++i) Views[i].EnsureHydrated();
	}

	void PrePhysBase(SimFloat dt)
	{
		HydrateAllViews();
		static_cast<Derived*>(this)->PrePhysics(dt);
	}

	void PostPhysBase(SimFloat dt)
	{
		HydrateAllViews();
		static_cast<Derived*>(this)->PostPhysics(dt);
	}

	void ScalarUpdateBase(SimFloat dt)
	{
		HydrateAllViews();
		static_cast<Derived*>(this)->ScalarUpdate(dt);
	}

	void PhysicsStepBase(SimFloat dt)
	{
		HydrateAllViews();
		static_cast<Derived*>(this)->PhysicsStep(dt);
	}
};
