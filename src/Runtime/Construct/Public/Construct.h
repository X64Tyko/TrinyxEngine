#pragma once

#include <vector>

#include "EngineAnnotations.h"
#include "AssetRegistry.h"
#include "EntityMeta.h"
#include "EntityRecord.h"
#include "LogicThreadBase.h"
#include "PhysicsTypes.h"
#include "Schema.h"
#include "WorldBase.h"

/** @addtogroup gameplay
 *  @{
 */

class Soul;

struct ConstructViewRef
{
	void* View;
	void (*HydrateFn)(void*);
	EntityHandle (*GetHandleFn)(void*); // Returns this view's backing ECS entity handle

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
// ---------------------------------------------------------------------------
// ConstructBase — non-template core of Construct<T>.
//
// Holds everything that doesn't depend on the derived type: world/soul/ID state
// and the registered View list. Bodies live in Construct.cpp, so changes here
// don't recompile every Construct. Construct<Derived> adds only the concept-
// detected, type-specific wiring.
// ---------------------------------------------------------------------------
class ConstructBase
{
public:
	void RegisterView(ConstructViewRef ref);
	void DeregisterView(void* viewPtr);

	/// Collect the ECS EntityHandle for each registered View.
	/// Used by ReplicationSystem to build the ConstructSpawnPayload.
	void CollectViewHandles(std::vector<EntityHandle>& out) const;

	WorldBase* GetWorld() const { return OwnerWorld; }
	Registry* GetRegistry() const;
	bool IsInitialized() const { return bInitialized; }
	uint32_t GetConstructID() const { return ConstructID; }
	void SetConstructID(uint32_t id) { ConstructID = id; }

	Soul* GetOwnerSoul() const { return OwnerSoul; }
	void SetOwnerSoul(Soul* soul) { OwnerSoul = soul; }

protected:
	ConstructBase()  = default;
	~ConstructBase() = default;

	void HydrateAllViews();

	/// Binds or unbinds contact callbacks for every registered View's entity.
	/// @p self is the Derived* the callbacks were registered with (may differ from `this`
	/// under multiple inheritance).
	void UnbindAllViewContacts(void* self);

	/// Deregisters every scalar tick batch entry for @p self (the Derived* used at registration).
	void DeregisterTicks(void* self);

	WorldBase* OwnerWorld = nullptr;
	Soul* OwnerSoul       = nullptr;
	uint32_t ConstructID  = 0;
	bool bInitialized     = false;

	std::vector<ConstructViewRef> Views;
};

template <typename Derived>
class Construct : public ConstructBase
{
public:
	// Default lifetime tier. Derived classes override via TNX_CONSTRUCT_SESSION, etc.
	static constexpr ConstructLifetime Lifetime = ConstructLifetime::World;

	// Optional pre-init hook — runs after OwnerWorld is set but before InitializeViews,
	// so derived classes can read GetWorld() to configure spawn parameters.
	// Implement void PreInit() in a derived class to opt in.
	// ConstructRegistry::Create<T> also accepts a zero-cost template callable for
	// external configuration (e.g., GameMode setting spawn pos/soul from outside).
	void PreInitBase()
	{
		if constexpr (requires { static_cast<Derived*>(this)->PreInit(); })
		{
			static_cast<Derived*>(this)->PreInit();
		}
	}

	void Initialize(WorldBase* InWorld)
	{
		OwnerWorld = InWorld;

		// CRTP PreInit hook: runs after OwnerWorld is set, before InitializeViews.
		PreInitBase();

		// Let the derived class create and set up its Views
		if constexpr (requires { static_cast<Derived*>(this)->InitializeViews(); })
		{
			static_cast<Derived*>(this)->InitializeViews();
			AssetRegistry::Get().DrainPendingCheckouts();
		}

		// Auto-register tick methods via concept detection
		LogicThreadBase* Logic = OwnerWorld->GetLogicThread();

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

		// Auto-bind contact callbacks for all registered Views.
		// Implement OnHit / OnOverlapBegin / OnOverlapEnd → get the callback. Don't implement → pay nothing.
		if constexpr (HasOnHit<Derived> || HasOnOverlapBegin<Derived> || HasOnOverlapEnd<Derived>)
		{
			JoltPhysics* Phys = OwnerWorld->GetPhysics();
			Registry* Reg     = OwnerWorld->GetRegistry();
			Derived* Self     = static_cast<Derived*>(this);
			for (const auto& v : Views)
			{
				if (!v.GetHandleFn) continue;
				EntityHandle handle = v.GetHandleFn(v.View);
				if constexpr (HasOnHit<Derived>) Phys->BindOnHit<Derived, &Derived::OnHit>(handle, Reg, Self);
				if constexpr (HasOnOverlapBegin<Derived>) Phys->BindOnOverlapBegin<Derived, &Derived::OnOverlapBegin>(handle, Reg, Self);
				if constexpr (HasOnOverlapEnd<Derived>) Phys->BindOnOverlapEnd<Derived, &Derived::OnOverlapEnd>(handle, Reg, Self);
			}
		}

		bInitialized = true;

		// Called once after views are hydrated and ticks are registered.
		// Implement void OnSpawned() in a derived class to run post-init logic.
		if constexpr (requires { static_cast<Derived*>(this)->OnSpawned(); })
		{
			static_cast<Derived*>(this)->OnSpawned();
		}
	}

	void Shutdown()
	{
		if (!bInitialized) return;

		// Unbind contact callbacks before deregistering ticks
		if constexpr (HasOnHit<Derived> || HasOnOverlapBegin<Derived> || HasOnOverlapEnd<Derived>)
			UnbindAllViewContacts(static_cast<Derived*>(this));

		DeregisterTicks(static_cast<Derived*>(this));

		bInitialized = false;
		OwnerWorld   = nullptr;
	}

protected:
	Construct() = default;

	~Construct()
	{
		if (bInitialized) Shutdown();
	}

private:
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

/** @} */
