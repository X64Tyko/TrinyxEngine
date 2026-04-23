#pragma once

#include "Archetype.h"
#include "CTransform.h"
#include "Construct.h"
#include "ConstructView.h"
#include "EPlayer.h"
#include "EPoint.h"
#include "GameMode.h"
#include "Input.h"
#include "Logger.h"
#include "ReflectionRegistry.h"
#include "Registry.h"
#include "ReplicationSystem.h"
#include "SchemaReflector.h"
#include "Soul.h"
#include "Types.h"
#include "World.h"

// ---------------------------------------------------------------------------
// KinematicCubeConstruct — dead-simple kinematic mover with no physics.
//
// Owns a single EPlayer entity (transform + mesh + color) so it's visible.
// Each PrePhysics tick it reads MoveForward from the player 1 input buffer:
//   - Client/Standalone: GetInputForPlayer(1) falls back to SimInput (local kbd)
//   - Server:            GetInputForPlayer(1) returns the network-injected buffer
// This exercises the full input-recording/replay path through rollback.
// ---------------------------------------------------------------------------
class KinematicCubeConstruct : public Construct<KinematicCubeConstruct>
{
	TNX_REGISTER_CONSTRUCT(KinematicCubeConstruct)

public:
	TNX_CONSTRUCT_WORLD

	ConstructView<EPlayer> Body;

	float SpawnX = 0.0f;
	float SpawnY = 2.0f;
	float SpawnZ = -10.0f;
	float Speed  = 3.0f; // units/s in +X when MoveForward is held

	void InitializeViews()
	{
		if (bIsClientSide)
		{
			Body.Attach(this, ReplicationEntityHandle);
			Body.SetFlags(TemporalFlagBits::Active | TemporalFlagBits::Alive | TemporalFlagBits::Replicated);
			return;
		}

		Body.Initialize(this, [this](EPlayer<FieldWidth::Scalar>& e)
		{
			e.Transform.PosX = SpawnX;
			e.Transform.PosY = SpawnY;
			e.Transform.PosZ = SpawnZ;
			e.Transform.Rotation.SetIdentity();

			e.Scale.ScaleX = 3.0f;
			e.Scale.ScaleY = 3.0f;
			e.Scale.ScaleZ = 3.0f;

			e.Color.R = 0.2f;
			e.Color.G = 0.4f;
			e.Color.B = 0.9f;
			e.Color.A = 1.0f;

			e.Mesh.MeshID = 1u; // Cube (slot 0=invalid, slot 1=Cube)
		});

		Body.SetFlags(TemporalFlagBits::Alive | TemporalFlagBits::Replicated | TemporalFlagBits::Active);
	}

	/// Replication entry point — called by ConstructRegistry::CreateForReplication on the client.
	void InitializeForReplication(WorldBase* world, EntityHandle* viewHandles, uint8_t viewCount)
	{
		bIsClientSide = true;
		if (viewCount > 0) ReplicationEntityHandle = viewHandles[0];
		Initialize(world);
	}

	void PrePhysics(SimFloat dt)
	{
		Soul* soul            = GetOwnerSoul();
		InputBuffer* simInput = soul
									? soul->GetSimInput(GetWorld())
									: GetWorld()->GetSimInput(); // standalone fallback

		const uint32_t localF = GetWorld()->GetLogicThread()->GetLastCompletedFrame() + 1;
		const int32_t srvF    = static_cast<int32_t>(localF)
			+ static_cast<int32_t>(GetWorld()->GetServerFrameOffset());
		const bool bMoving = simInput && simInput->IsActionDown(Action::MoveForward);

		if (srvF >= 1020 && srvF <= 1060)
		{
			LOG_NET_INFO_F(soul,
						   "[Cube] localF=%u srvF=%d moving=%d resim=%d pos=%.6f",
						   localF, srvF, bMoving ? 1 : 0,
						   GetWorld()->GetLogicThread()->IsResimulating() ? 1 : 0,
						   Body.Transform.PosX.Value());
		}

		if (!bMoving) return; // Echo souls have no input

		Body.Transform.PosX = Body.Transform.PosX.Value() + Speed * static_cast<float>(dt);
	}

private:
	bool bIsClientSide = false;
	EntityHandle ReplicationEntityHandle{};
};

// ---------------------------------------------------------------------------
// DeterminismDriver — Construct that owns the timed test harness.
//
// Lifecycle:
//   1. DeterminismMode::Initialize spawns a KinematicCubeConstruct, then
//      sets CubePtr on the driver.
//   2. PrePhysics counts frames, injects W key presses into SimInput for the
//      move window, then dumps the slab once the cooldown expires.
//      Because input is recorded in the frame header each tick and replayed
//      during resim, this exercises the full input-recording rollback path.
// ---------------------------------------------------------------------------
class DeterminismDriver : public Construct<DeterminismDriver>
{
public:
	TNX_CONSTRUCT_WORLD

	KinematicCubeConstruct* CubePtr = nullptr;

	ConstructView<EPoint> Self; // ECS presence required for replication to client world

	void InitializeViews()
	{
		Self.Initialize(this, [](EPoint<FieldWidth::Scalar>& e)
		{
			e.Transform.PosX = 0.f;
			e.Transform.PosY = 0.f;
			e.Transform.PosZ = 0.f;
			e.Transform.Rotation.SetIdentity();
		});
	}

	void PostPhysics(SimFloat /*dt*/)
	{
		if (!CubePtr) return;

		// During resim the input is replayed via InjectFrameInput — re-injecting
		// keys here would corrupt SimInput/NetInput and advance the phase state machine
		// against frames that aren't part of the authoritative forward pass.
		if (GetWorld()->GetLogicThread()->IsResimulating()) return;

		const uint32_t localFrame = GetCurrentLocalFrame();
		const int32_t srvFrame    = GetCurrentServerFrame();

		switch (Phase)
		{
			case TestPhase::Waiting: if (srvFrame >= MoveTriggerSrvFrame)
				{
					Phase               = TestPhase::Moving;
					MovingUntilSrvFrame = srvFrame + static_cast<int32_t>(MoveFrames);
					InjectKey(SDL_SCANCODE_W, true);
					LOG_NET_INFO_F(CubePtr->GetOwnerSoul(),
								   "[DeterminismDriver] >> Moving for %u frames  (localF=%u  srvF=%d  until srvF=%d)",
								   MoveFrames, localFrame, srvFrame, MovingUntilSrvFrame);
				}
				break;

			case TestPhase::Moving: if (srvFrame >= MovingUntilSrvFrame)
				{
					Phase                 = TestPhase::Cooling;
					CooldownUntilSrvFrame = srvFrame + static_cast<int32_t>(CooldownFrames);
					InjectKey(SDL_SCANCODE_W, false);
					LOG_NET_INFO_F(CubePtr->GetOwnerSoul(),
								   "[DeterminismDriver] >> Stopped — cooling down %u frames  (localF=%u  srvF=%d  until srvF=%d)",
								   CooldownFrames, localFrame, srvFrame, CooldownUntilSrvFrame);
				}
				break;

			case TestPhase::Cooling: if (srvFrame >= CooldownUntilSrvFrame)
				{
					Phase = TestPhase::Done;
					DumpSlabState();
				}
				break;

			case TestPhase::Done: break;
		}
	}

private:
	enum class TestPhase { Waiting, Moving, Cooling, Done };

	static constexpr uint32_t WaitFrames     = 1024;
	static constexpr uint32_t MoveFrames     = 30;
	static constexpr uint32_t CooldownFrames = 30;

	// Both sides fire at the same server frame regardless of InputLead or clock offset.
	static constexpr int32_t MoveTriggerSrvFrame = static_cast<int32_t>(WaitFrames);

	TestPhase Phase               = TestPhase::Waiting;
	int32_t MovingUntilSrvFrame   = 0;
	int32_t CooldownUntilSrvFrame = 0;

	// GetLastCompletedFrame() returns N-1 during PostPhysics of frame N (PublishCompletedFrame
	// hasn't run yet). Adding 1 gives the correct in-flight frame number.
	// Reading the temporal cache header would give the stale value from 128 frames ago because
	// the header for the current slot is only stamped by PublishCompletedFrame (post-PostPhysics).
	uint32_t GetCurrentLocalFrame() const
	{
		return GetWorld()->GetLogicThread()->GetLastCompletedFrame() + 1;
	}

	int32_t GetCurrentServerFrame() const
	{
		return static_cast<int32_t>(GetCurrentLocalFrame())
			+ static_cast<int32_t>(GetWorld()->GetServerFrameOffset());
	}

	// ---------------------------------------------------------------------------
	// InjectKey — pushes a key state into SimInput for this world.
	//
	// Client world: SimInput only. The MPSC accumulator ring picks it up at
	//               ProcessSimInput time and the net thread sends it to the server.
	// Server world: nothing — the server reads whatever the client sent.
	// ---------------------------------------------------------------------------
	void InjectKey(SDL_Scancode key, bool down)
	{
#if defined(TNX_NET_MODEL_CLIENT)
		GetWorld()->GetSimInput()->PushKey(key, down);
#else
		// In PIE the model is TNX_NET_MODEL_PIE — use runtime role instead.
		// A PIE client world has LocalOwnerID != 0; solo/server worlds have 0.
		if (GetWorld() && GetWorld()->GetLocalOwnerID() != 0) GetWorld()->GetSimInput()->PushKey(key, down);
#endif
	}

	// ---------------------------------------------------------------------------
	// DumpSlabState — walk the temporal ring for the cube entity and log the
	// CTransform position at every stored frame slot.
	//
	// Printed oldest→newest so you can visually scan for monotone +X movement
	// during the move window, a clean plateau during the cooldown, and zero
	// corruption outside those regions.
	// ---------------------------------------------------------------------------
	void DumpSlabState()
	{
		Registry* reg = GetWorld()->GetRegistry();

		EntityHandle handle = CubePtr->Body.GetEntityHandle();
		if (!handle.IsValid())
		{
			LOG_WARN("[DeterminismDriver] DumpSlabState: cube EntityHandle invalid");
			return;
		}

		EntityRecord record = reg->GetRecord(handle);
		if (!record.IsValid())
		{
			LOG_WARN("[DeterminismDriver] DumpSlabState: cube EntityRecord invalid");
			return;
		}

		Archetype* arch   = record.Arch;
		Chunk* chunk      = record.TargetChunk;
		uint32_t localIdx = record.LocalIndex;

		auto* temporal               = reg->GetTemporalCache();
		auto* volatile_              = reg->GetVolatileCache();
		const uint32_t totalFrames   = temporal->GetTotalFrameCount();
		const uint32_t currentSlot   = temporal->GetActiveWriteFrame();
		const uint32_t volatileWrite = volatile_->GetActiveWriteFrame();

		const int32_t offset           = static_cast<int32_t>(GetWorld()->GetServerFrameOffset());
		const uint32_t currentAbsFrame = GetCurrentLocalFrame();
		const int32_t currentSrvFrame  = static_cast<int32_t>(currentAbsFrame) + offset;

		LOG_NET_INFO(CubePtr->GetOwnerSoul(), "[DeterminismDriver] ==================== SLAB DUMP ====================");
		LOG_INFO_F("[DeterminismDriver] localFrame=%u  serverFrame=%d  offset=%d  slot=%u  ringSize=%u",
				   currentAbsFrame, currentSrvFrame, offset, currentSlot, totalFrames);
		LOG_INFO_F("[DeterminismDriver] localIndex=%u  archetype=%u  moveFrames=%u  cooldown=%u",
				   localIdx, arch->ArchClassID, MoveFrames, CooldownFrames);
		LOG_INFO("[DeterminismDriver] ---------------------------------------------------");

		void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];

		for (uint32_t i = 0; i < totalFrames; ++i)
		{
			const uint32_t slot     = (currentSlot + 1 + i) % totalFrames;
			const uint32_t absFrame = temporal->GetFrameHeader(slot)->FrameNumber;

			arch->BuildFieldArrayTable(chunk, fieldArrayTable, absFrame, volatileWrite);

			float posX = 0.f, posY = 0.f, posZ = 0.f;
			for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
			{
				if (fdesc.componentID != CTransform<>::StaticTypeID()) continue;
				void* base = fieldArrayTable[fdesc.fieldSlotIndex];
				if (!base) continue;
				auto* fa = static_cast<float*>(base);
				switch (fdesc.componentSlotIndex)
				{
					case 0: posX = fa[localIdx];
						break;
					case 1: posY = fa[localIdx];
						break;
					case 2: posZ = fa[localIdx];
						break;
					default: break;
				}
			}

			LOG_INFO_F("[DeterminismDriver]  slot=%2u  localF=%5u  srvF=%5d  pos=(%8.4f, %8.4f, %8.4f)",
					   slot, absFrame, static_cast<int32_t>(absFrame) + offset, posX, posY, posZ);
		}

		LOG_INFO("[DeterminismDriver] ==================== END DUMP =====================");
	}
};

// ---------------------------------------------------------------------------
// DeterminismMode — standalone GameMode for rollback determinism testing.
//
// Creates a KinematicCubeConstruct and a DeterminismDriver in Initialize().
// No physics — pure temporal slab verification via SimInput injection.
// ---------------------------------------------------------------------------
class DeterminismMode : public GameMode
{
public:
	void Initialize(WorldBase* world) override
	{
		GameMode::Initialize(world);

		ConstructRegistry* reg  = world->GetConstructRegistry();
		ReplicationSystem* repl = world->GetReplicationSystem();

		const uint16_t cubeHash   = ReflectionRegistry::ConstructTypeHashFromName("KinematicCubeConstruct");
		const uint16_t driverHash = ReflectionRegistry::ConstructTypeHashFromName("DeterminismDriver");

		Cube = reg->Create<KinematicCubeConstruct>(world);

		if (repl) CubeRef = repl->RegisterConstruct<KinematicCubeConstruct>(reg, Cube, /*ownerID=*/1, cubeHash, /*prefabID=*/0);
		LOG_INFO("[DeterminismMode] KinematicCubeConstruct created at (0,2,0)");

		Driver          = reg->Create<DeterminismDriver>(world);
		Driver->CubePtr = Cube;
		if (repl) repl->RegisterConstruct<DeterminismDriver>(reg, Driver, /*ownerID=*/1, driverHash, /*prefabID=*/0);
		LOG_INFO("[DeterminismMode] DeterminismDriver armed — waiting to move cube");
	}

	const char* GetModeName() const override { return "DeterminismMode"; }

	PlayerBeginResult OnPlayerBeginRequest(Soul& soul, const PlayerBeginRequestPayload& req) override
	{
		(void)req;
		if (GetWorld()->GetReplicationSystem() && soul.GetOwnerID() != 1) return PlayerBeginResult{};

		Cube->SetOwnerSoul(&soul);
		soul.ClaimBody(CubeRef);

		PlayerBeginResult result;
		result.Accepted = true;
		result.PosX     = 0.0f;
		result.PosY     = 2.0f;
		result.PosZ     = 0.0f;
		result.Body     = soul.GetBodyHandle();
		return result;
	}

private:
	ConstructRef CubeRef{};
	KinematicCubeConstruct* Cube = nullptr;
	DeterminismDriver* Driver    = nullptr;
};
