#include "Registry.h"
#include <iostream>
#include <random>
#include <thread>
#include <SDL3/SDL_timer.h>

#include "TrinyxEngine.h"
#include "GameManager.h"
#include "TestEntity.h"
#include "Public/CubeEntity.h"
#include "Public/Projectile.h"
#include "Archetype.h"
#include "Logger.h"
#include "TestFramework.h"
#include "GNSContext.h"
#include "NetConnectionManager.h"
#include "NetThread.h"
#include "World.h"
#include "Input.h"
#include "Public/PlayerConstruct.h"

using namespace tnx::Testing;

// ---------------------------------------------------------------------------
// Networking: GNS loopback ping/pong test
// Validates: GNS static linking, loopback connectivity, PacketHeader framing
// ---------------------------------------------------------------------------
TEST(Net_LoopbackPingPong)
{
	// Tests run in Standalone mode — GNS isn't initialized by the engine.
	// Create a local GNSContext for this test.
	(void)Engine;
	GNSContext gnsLocal;
	ASSERT(gnsLocal.Initialize());
	GNSContext* gns = &gnsLocal;

	// Single connection manager handles both server and client roles.
	// GNS supports listen + connect on the same interface — the poll group
	// collects messages from all connections regardless of direction.
	NetConnectionManager mgr;
	mgr.Initialize(gns);

	constexpr uint16_t kPort = 27015;
	ASSERT(mgr.Listen(kPort));

	HSteamNetConnection clientConn = mgr.Connect("127.0.0.1", kPort);
	ASSERT(clientConn != 0);

	// Pump callbacks until we have 2 connections (server-side accept + client-side connect)
	const uint64_t deadline = SDL_GetTicks() + 2000;
	while (mgr.GetConnectionCount() < 2 && SDL_GetTicks() < deadline)
	{
		mgr.RunCallbacks();
		SDL_Delay(10);
	}
	ASSERT(mgr.GetConnectionCount() >= 2);

	// Identify server-side and client-side connection handles
	HSteamNetConnection serverConn = 0;
	for (const auto& ci : mgr.GetConnections())
	{
		if (ci.Handle != clientConn)
		{
			serverConn = ci.Handle;
			break;
		}
	}
	ASSERT(serverConn != 0);

	// Client sends Ping
	PacketHeader pingHeader{};
	pingHeader.Type        = static_cast<uint8_t>(NetMessageType::Ping);
	pingHeader.Flags       = PacketFlag::DefaultFlags;
	pingHeader.SequenceNum = 1;
	pingHeader.FrameNumber = 0;
	pingHeader.SenderID    = 1;
	pingHeader.Timestamp   = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
	pingHeader.PayloadSize = 0;

	ASSERT(mgr.Send(clientConn, pingHeader, nullptr, true));

	// Poll until a Ping arrives on the server-side connection
	std::vector<ReceivedMessage> msgs;
	const uint64_t recvDeadline = SDL_GetTicks() + 1000;
	while (msgs.empty() && SDL_GetTicks() < recvDeadline)
	{
		mgr.RunCallbacks();
		mgr.PollIncoming(msgs);
		SDL_Delay(5);
	}

	ASSERT_EQ(static_cast<int>(msgs.size()), 1);
	ASSERT_EQ(msgs[0].Header.Type, static_cast<uint8_t>(NetMessageType::Ping));
	ASSERT_EQ(msgs[0].Header.SenderID, 1);
	ASSERT_EQ(msgs[0].Connection, serverConn); // Arrived on the server-accepted connection

	// Server responds with Pong on the connection it received from
	PacketHeader pongHeader{};
	pongHeader.Type        = static_cast<uint8_t>(NetMessageType::Pong);
	pongHeader.Flags       = PacketFlag::DefaultFlags;
	pongHeader.SequenceNum = 1;
	pongHeader.FrameNumber = 0;
	pongHeader.SenderID    = 0; // Server
	pongHeader.Timestamp   = static_cast<uint16_t>(SDL_GetTicks() & 0xFFFF);
	pongHeader.PayloadSize = 0;

	ASSERT(mgr.Send(serverConn, pongHeader, nullptr, true));

	// Poll until the Pong arrives on the client connection
	msgs.clear();
	const uint64_t pongDeadline = SDL_GetTicks() + 1000;
	while (msgs.empty() && SDL_GetTicks() < pongDeadline)
	{
		mgr.RunCallbacks();
		mgr.PollIncoming(msgs);
		SDL_Delay(5);
	}

	ASSERT_EQ(static_cast<int>(msgs.size()), 1);
	ASSERT_EQ(msgs[0].Header.Type, static_cast<uint8_t>(NetMessageType::Pong));
	ASSERT_EQ(msgs[0].Header.SenderID, 0);
	ASSERT_EQ(msgs[0].Connection, clientConn); // Arrived on the client connection

	LOG_ALWAYS("[Net_LoopbackPingPong] Loopback ping/pong successful — GNS connectivity verified");

	mgr.Shutdown();
	// gnsLocal destroyed by stack unwinding after mgr
}

// ---------------------------------------------------------------------------
// Networking: InputFrame routing through NetThread to World's InputBuffer
// Validates: NetThread inline Tick(), InputFrame deserialization, InputBuffer injection
// ---------------------------------------------------------------------------
TEST(Net_InputFrameRouting)
{
	(void)Engine;

	GNSContext gnsLocal;
	ASSERT(gnsLocal.Initialize());

	// Create a NetThread in inline mode (no thread spawned)
	EngineConfig config{};
	config.NetworkUpdateHz = 30;

	NetThread net;
	net.Initialize(&gnsLocal, &config);

	// We need a World to route input to. Use an InputBuffer directly
	// since we can't easily create a full World in the test context.
	// Instead, use the engine's DefaultWorld and map OwnerID 1 to it.
	World* world = Engine.GetDefaultWorld();
	ASSERT(world != nullptr);
	net.MapConnectionToWorld(1, world);

	// Use the NetThread's own connection manager for both roles.
	// GNS supports listen + connect on the same interface, and the static
	// s_Instance callback only routes to one manager at a time.
	NetConnectionManager* mgr = net.GetConnectionManager();

	constexpr uint16_t kPort = 27016;
	ASSERT(mgr->Listen(kPort));

	HSteamNetConnection clientConn = mgr->Connect("127.0.0.1", kPort);
	ASSERT(clientConn != 0);

	// Pump until both sides are connected (server accept + client connect = 2)
	const uint64_t deadline = SDL_GetTicks() + 2000;
	while (mgr->GetConnectionCount() < 2 && SDL_GetTicks() < deadline)
	{
		mgr->RunCallbacks();
		SDL_Delay(10);
	}
	ASSERT(mgr->GetConnectionCount() >= 2);

	// Find the server-side connection (the one that isn't the client's outgoing)
	HSteamNetConnection serverSideConn = 0;
	for (const auto& ci : mgr->GetConnections())
	{
		if (ci.Handle != clientConn)
		{
			serverSideConn = ci.Handle;
			break;
		}
	}
	ASSERT(serverSideConn != 0);
	mgr->AssignOwnerID(serverSideConn, 1);

	// Build an InputFrame payload with 'W' key pressed (SDL_SCANCODE_W = 26)
	InputFramePayload payload{};
	memset(payload.KeyState, 0, 64);
	constexpr uint8_t wScancode      = 26; // SDL_SCANCODE_W
	payload.KeyState[wScancode >> 3] |= (1u << (wScancode & 7));
	payload.MouseDX                  = 1.5f;
	payload.MouseDY                  = -0.5f;

	// Send InputFrame from client
	PacketHeader header{};
	header.Type        = static_cast<uint8_t>(NetMessageType::InputFrame);
	header.Flags       = PacketFlag::DefaultFlags;
	header.PayloadSize = sizeof(InputFramePayload);
	header.SequenceNum = 1;
	header.FrameNumber = 0;
	header.SenderID    = 1;

	ASSERT(mgr->Send(clientConn, header,
		reinterpret_cast<const uint8_t*>(&payload), true));

	// Pump until the message arrives and is routed
	const uint64_t deadline2 = SDL_GetTicks() + 2000;
	bool received            = false;
	while (!received && SDL_GetTicks() < deadline2)
	{
		net.Tick(); // inline mode — polls server-side + routes messages

		// Check if the key was injected into the World's SimInput
		InputBuffer* simInput = world->GetSimInput();
		simInput->Swap(); // advance read slot to see injected state
		if (simInput->IsKeyDown(static_cast<SDL_Scancode>(wScancode)))
		{
			received = true;
		}
		else
		{
			SDL_Delay(10);
		}
	}
	ASSERT(received);

	LOG_ALWAYS("[Net_InputFrameRouting] InputFrame routed through NetThread to World's InputBuffer — verified");

	mgr->StopListening();
	// gnsLocal and net destroyed in correct order by stack unwinding
	// (net first, then gnsLocal — so NetConnectionManager shuts down before GNS)
}

TEST(Registry_CreateEntities)
{
	Registry* Reg = Engine.GetRegistry();
	std::vector<EntityHandle> Entities;

	for (int i = 0; i < 5; ++i)
	{
		EntityHandle Id = Reg->Create<TestEntity<>>();
		Entities.push_back(Id);
	}

	ASSERT_EQ(Entities.size(), 5);

	Engine.ResetRegistry();
}

TEST(Registry_ValidEntityIDs)
{
	Registry* Reg = Engine.GetRegistry();
	std::vector<EntityHandle> Entities;

	for (int i = 0; i < 100; ++i)
	{
		Entities.push_back(Reg->Create<TestEntity<>>());
	}

	for (EntityHandle Id : Entities)
	{
		ASSERT(Id.IsValid());
	}
	Engine.ResetRegistry();
}

TEST(Registry_DestroyAndReuse)
{
	Registry* Reg = Engine.GetRegistry();
	std::vector<EntityHandle> Entities;

	for (int i = 0; i < 10; ++i)
	{
		Entities.push_back(Reg->Create<TestEntity<>>());
	}

	uint32_t firstHandleIndex = Entities[0].GetHandleIndex();

	Reg->Destroy(Entities[0]);
	Reg->ProcessDeferredDestructions();
	Engine.ConfirmLocalRecycles();

	EntityHandle NewId = Reg->Create<TestEntity<>>();
	ASSERT_EQ(NewId.GetHandleIndex(), firstHandleIndex);

	Engine.ResetRegistry();
}

/* Temporarily disabled, jobs aren't properly initialized until the Run loop is active.
TEST(DirtyBits_SetAfterPrePhys)
{
	Registry* Reg = Engine.GetRegistry();

	TrinyxJobs::Initialize(Reg->GetConfig);

	constexpr int Count = 16; // exactly 2 dirty bytes
	Reg->Create<CubeEntity<>>(Count);

	std::vector<Archetype*> arches = Reg->ClassQuery<CubeEntity<>>();
	ASSERT(!arches.empty());
	Chunk* chunk = arches[0]->Chunks[0];

	size_t gsi = chunk->Header.CacheIndexStart;
	Reg->InvokePrePhys(1.0 / 128.0, 0);

	uint8_t* dirtyBytes = reinterpret_cast<uint8_t*>(Reg->DirtyBitsFrame(0)->data()) + (gsi / 8);

	LOG_INFO_F("[DirtyBits] CacheIndexStart=%zu  byteOffset=%zu", gsi, gsi / 8);
	for (int b = 0; b < (Count + 7) / 8; ++b)
		LOG_INFO_F("[DirtyBits]   byte[%d] = 0x%02X  (expected 0xFF)", b, dirtyBytes[b]);

	for (int b = 0; b < (Count + 7) / 8; ++b)
		ASSERT_EQ((int)dirtyBytes[b], 0xFF);

	Engine.ResetRegistry();
}
*/

// ---------------------------------------------------------------------------
// Helper: writes common cube fields into a hydrated entity view.
// Avoids duplicating the Hydrate → field-write loop across every spawn test.
// ---------------------------------------------------------------------------
struct CubeSetup
{
	float x, y, z;
	float halfX, halfY, halfZ;
	float mass;
	float r, g, b;
	uint32_t motion;
};

static void WriteCubeSetups(Registry* reg, const std::vector<CubeSetup>& setups,
							std::vector<EntityHandle>& outIds)
{
	int32_t totalCount               = static_cast<int32_t>(setups.size());
	std::vector<EntityHandle> newIds = reg->Create<CubeEntity<>>(totalCount);
	outIds.insert(outIds.end(), newIds.begin(), newIds.end());

	uint32_t setupIdx              = 0;
	std::vector<Archetype*> arches = reg->ClassQuery<CubeEntity<>>();
	for (Archetype* arch : arches)
	{
		CubeEntity<> cube;
		for (size_t chunkIdx = 0; chunkIdx < arch->Chunks.size(); ++chunkIdx)
		{
			Chunk* chunk              = arch->Chunks[chunkIdx];
			uint32_t chunkEntityCount = arch->GetAllocatedChunkCount(chunkIdx);

			void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
			arch->BuildFieldArrayTable(chunk, fieldArrayTable, reg->GetTemporalCache()->GetActiveWriteFrame(), reg->GetVolatileCache()->GetActiveWriteFrame());
			cube.Hydrate(fieldArrayTable, fieldArrayTable[0]);

			for (uint32_t i = 0; i < chunkEntityCount; ++i, cube.Advance(1))
			{
				if (setupIdx >= setups.size()) break;
				const auto& s = setups[setupIdx++];

				cube.Flags.Flags    = static_cast<int32_t>(TemporalFlagBits::Active);
				cube.transform.PosX = s.x;
				cube.transform.PosY = s.y;
				cube.transform.PosZ = s.z;
				cube.transform.Rotation.SetIdentity();
				cube.scale.ScaleX = s.halfX * 2.0f;
				cube.scale.ScaleY = s.halfY * 2.0f;
				cube.scale.ScaleZ = s.halfZ * 2.0f;

				cube.velocity.vX = 0.0f;
				cube.velocity.vY = 0.0f;
				cube.velocity.vZ = 0.0f;

				cube.color.R = s.r;
				cube.color.G = s.g;
				cube.color.B = s.b;
				cube.color.A = 1.0f;

				cube.physBody.Shape       = JoltShapeType::Box;
				cube.physBody.HalfExtentX = s.halfX;
				cube.physBody.HalfExtentY = s.halfY;
				cube.physBody.HalfExtentZ = s.halfZ;
				cube.physBody.Motion      = s.motion;
				cube.physBody.Mass        = s.mass;
				cube.physBody.Friction    = 0.5f;
				cube.physBody.Restitution = 0.5f;
			}
		}
	}
}

struct ProjectileSetup
{
	float x, y, z;
	float velX, velY, velZ;
	float r, g, b, a;
};

static void WriteProjectileSetups(Registry* reg, const std::vector<ProjectileSetup>& setups,
								  std::vector<EntityHandle>& outIds)
{
	int32_t totalCount               = static_cast<int32_t>(setups.size());
	std::vector<EntityHandle> newIds = reg->Create<Projectile<>>(totalCount);
	outIds.insert(outIds.end(), newIds.begin(), newIds.end());

	uint32_t setupIdx              = 0;
	std::vector<Archetype*> arches = reg->ClassQuery<Projectile<>>();
	for (Archetype* arch : arches)
	{
		Projectile<> proj;
		for (size_t chunkIdx = 0; chunkIdx < arch->Chunks.size(); ++chunkIdx)
		{
			Chunk* chunk              = arch->Chunks[chunkIdx];
			uint32_t chunkEntityCount = arch->GetAllocatedChunkCount(chunkIdx);

			void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
			arch->BuildFieldArrayTable(chunk, fieldArrayTable, reg->GetTemporalCache()->GetActiveWriteFrame(), reg->GetVolatileCache()->GetActiveWriteFrame());
			proj.Hydrate(fieldArrayTable, fieldArrayTable[0]);

			for (uint32_t i = 0; i < chunkEntityCount; ++i, proj.Advance(1))
			{
				if (setupIdx >= setups.size()) break;
				const auto& s = setups[setupIdx++];

				proj.Flags.Flags    = static_cast<int32_t>(TemporalFlagBits::Active);
				proj.transform.PosX = s.x;
				proj.transform.PosY = s.y;
				proj.transform.PosZ = s.z;
				proj.transform.Rotation.SetIdentity();

				proj.body.VelX = s.velX;
				proj.body.VelY = s.velY;
				proj.body.VelZ = s.velZ;

				proj.color.R = s.r;
				proj.color.G = s.g;
				proj.color.B = s.b;
				proj.color.A = s.a;
			}
		}
	}
}

static void WriteSuperCubeSetups(Registry* reg, const std::vector<CubeSetup>& setups,
								 std::vector<EntityHandle>& outIds)
{
	int32_t totalCount               = static_cast<int32_t>(setups.size());
	std::vector<EntityHandle> newIds = reg->Create<SuperCube<>>(totalCount);
	outIds.insert(outIds.end(), newIds.begin(), newIds.end());

	uint32_t setupIdx              = 0;
	std::vector<Archetype*> arches = reg->ClassQuery<SuperCube<>>();
	for (Archetype* arch : arches)
	{
		SuperCube<> cube;
		for (size_t chunkIdx = 0; chunkIdx < arch->Chunks.size(); ++chunkIdx)
		{
			Chunk* chunk              = arch->Chunks[chunkIdx];
			uint32_t chunkEntityCount = arch->GetAllocatedChunkCount(chunkIdx);

			void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
			arch->BuildFieldArrayTable(chunk, fieldArrayTable, reg->GetTemporalCache()->GetActiveWriteFrame(), reg->GetVolatileCache()->GetActiveWriteFrame());
			cube.Hydrate(fieldArrayTable, fieldArrayTable[0]);

			for (uint32_t i = 0; i < chunkEntityCount; ++i, cube.Advance(1))
			{
				if (setupIdx >= setups.size()) break;
				const auto& s = setups[setupIdx++];

				cube.Flags.Flags    = static_cast<int32_t>(TemporalFlagBits::Active);
				cube.transform.PosX = s.x;
				cube.transform.PosY = s.y;
				cube.transform.PosZ = s.z;
				cube.transform.Rotation.SetIdentity();
				cube.scale.ScaleX = s.halfX * 2.0f;
				cube.scale.ScaleY = s.halfY * 2.0f;
				cube.scale.ScaleZ = s.halfZ * 2.0f;

				cube.velocity.vX = 0.0f;
				cube.velocity.vY = 0.0f;
				cube.velocity.vZ = 0.0f;

				cube.color.R = s.r;
				cube.color.G = s.g;
				cube.color.B = s.b;
				cube.color.A = 1.0f;
			}
		}
	}
}

// ===========================================================================
// Runtime tests — execute after the engine loop is active (threads + jobs up)
// ===========================================================================

// File-scope entity ID storage for runtime spawn/cleanup
static std::vector<EntityHandle> gPyramidIds;
static std::vector<EntityHandle> gSuperCubeIds;
static std::vector<EntityHandle> gProjectileIds;

RUNTIME_TEST(Runtime_JobsInitialized)
{
	ASSERT(Engine.GetJobsInitialized());
}

// ---------------------------------------------------------------------------
// Jolt Pyramid — N-tier pyramid of dynamic physics cubes on a static floor.
// Exercises: batch entity creation, Jolt body flush, collision response,
//            transform pull-back, GPU predicate/scatter with Active flags.
// Persistent — stays up for the duration of the session.
// ---------------------------------------------------------------------------
RUNTIME_TEST(Spawn_JoltPyramid)
{
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_real_distribution<float> colorDist(0.2f, 1.0f);

	constexpr float cBoxSize       = 2.0f;
	constexpr float cHalfBoxSize   = 1.0f;
	constexpr float cBoxSeparation = 0.5f;
#ifdef TNX_ENABLE_ROLLBACK
	constexpr int cPyramidHeight = 5; // Reduced for rollback determinism testing
#else
	constexpr int cPyramidHeight   = 25;
#endif
	constexpr float xOffset        = 0.0f;
	constexpr float yOffset        = -30.0f;
	constexpr float zOffset        = -100.0f;

	static std::vector<CubeSetup> setups;
	setups.clear();

	for (int layer = 0; layer < cPyramidHeight; ++layer)
	{
		for (int j = layer / 2; j < cPyramidHeight - (layer + 1) / 2; ++j)
		{
			for (int k = layer / 2; k < cPyramidHeight - (layer + 1) / 2; ++k)
			{
				setups.push_back({
					xOffset + static_cast<float>(-cPyramidHeight) + cBoxSize * static_cast<float>(j) + ((layer & 1) ? cHalfBoxSize : 0.0f),
					yOffset + 1.0f + (cBoxSize + cBoxSeparation) * static_cast<float>(layer),
					zOffset + static_cast<float>(-cPyramidHeight) + cBoxSize * static_cast<float>(k) + ((layer & 1) ? cHalfBoxSize : 0.0f),
					cHalfBoxSize, cHalfBoxSize, cHalfBoxSize,
					1.0f,
					colorDist(gen), colorDist(gen), colorDist(gen),
					JoltMotion::Dynamic
				});
			}
		}
	}

	// Floor
	setups.push_back({
		xOffset, yOffset - 1.0f, zOffset,
		50.0f, 1.0f, 50.0f,
		0.0f,
		0.3f, 0.3f, 0.3f,
		JoltMotion::Static
	});

	Engine.Spawn([](Registry* reg)
	{
		WriteCubeSetups(reg, setups, gPyramidIds);
	});

	LOG_ALWAYS_F("Jolt Pyramid: %d-tier, %d dynamic + 1 floor = %zu entities (persistent)",
				 cPyramidHeight, static_cast<int>(setups.size()) - 1, setups.size());
}

// ---------------------------------------------------------------------------
// SuperCube Grid — spawns a requested number of non-physics cubes in a grid.
// Exercises: high entity count, ScalarUpdate color animation, no Jolt overhead.
// Self-destructs after 30 seconds.
// ---------------------------------------------------------------------------
RUNTIME_TEST(Spawn_SuperCubeGrid)
{
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_real_distribution<float> colorDist(0.2f, 1.0f);

	constexpr int Count      = 100000;
	constexpr float Spacing  = 3.0f;
	constexpr float CubeHalf = 0.5f;
	constexpr float YBase    = 10.0f;
	constexpr float ZOffset  = -200.0f;

	int gridSide   = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(Count))));
	float gridHalf = static_cast<float>(gridSide) * Spacing * 0.5f;

	static std::vector<CubeSetup> setups;
	setups.clear();
	setups.reserve(Count);

	for (int i = 0; i < Count; ++i)
	{
		int row = i / gridSide;
		int col = i % gridSide;

		setups.push_back({
			static_cast<float>(col) * Spacing - gridHalf,
			YBase + static_cast<float>(row) * Spacing,
			ZOffset,
			CubeHalf, CubeHalf, CubeHalf,
			0.0f,
			colorDist(gen), colorDist(gen), colorDist(gen),
			JoltMotion::Static
		});
	}

	Engine.Spawn([](Registry* reg)
	{
		WriteSuperCubeSetups(reg, setups, gSuperCubeIds);
	});

	LOG_ALWAYS_F("SuperCube Grid: %d entities in %dx%d grid (30s lifetime)",
				 Count, gridSide, gridSide);

	// Self-destruct after 30 seconds
	auto* idsPtr = &gSuperCubeIds;
	std::thread([idsPtr]()
	{
		SDL_Delay(30000);
		TrinyxEngine::Get().Spawn([idsPtr](Registry* reg)
		{
			for (EntityHandle id : *idsPtr) reg->Destroy(id);
			LOG_ALWAYS_F("[RuntimeTest] SuperCube Grid: destroyed %zu entities after 30s",
						 idsPtr->size());
			idsPtr->clear();
		});
	}).detach();
}

// ---------------------------------------------------------------------------
// Projectile Burst — spawns a fan of projectiles from a single origin.
// Exercises: high-count minimal-field entities, velocity integration,
//            alpha fade, AVX2 wide-path throughput.
// Self-destructs after 30 seconds.
// ---------------------------------------------------------------------------
RUNTIME_TEST(Spawn_ProjectileBurst)
{
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_real_distribution<float> spreadDist(-20.0f, 20.0f);
	std::uniform_real_distribution<float> speedDist(30.0f, 80.0f);
	std::uniform_real_distribution<float> colorDist(0.4f, 1.0f);

	constexpr int Count     = 0;
	constexpr float OriginY = 20.0f;
	constexpr float OriginZ = -50.0f;

	static std::vector<ProjectileSetup> setups;
	setups.clear();
	setups.reserve(Count);

	for (int i = 0; i < Count; ++i)
	{
		float speed = speedDist(gen);
		setups.push_back({
			0.0f, OriginY, OriginZ,
			spreadDist(gen), spreadDist(gen) + 10.0f, -speed,
			colorDist(gen), colorDist(gen) * 0.5f, 0.1f, 1.0f
		});
	}

	Engine.Spawn([](Registry* reg)
	{
		WriteProjectileSetups(reg, setups, gProjectileIds);
	});

	LOG_ALWAYS_F("Projectile Burst: %d projectiles from origin (0, %.0f, %.0f) (30s lifetime)",
				 Count, OriginY, OriginZ);

	// Self-destruct after 30 seconds
	auto* idsPtr = &gProjectileIds;
	std::thread([idsPtr]()
	{
		SDL_Delay(30000);
		TrinyxEngine::Get().Spawn([idsPtr](Registry* reg)
		{
			for (EntityHandle id : *idsPtr) reg->Destroy(id);
			LOG_ALWAYS_F("[RuntimeTest] Projectile Burst: destroyed %zu entities after 30s",
						 idsPtr->size());
			idsPtr->clear();
		});
	}).detach();
}

RUNTIME_TEST(Runtime_EntityCountValid)
{
	Registry* Reg        = Engine.GetRegistry();
	size_t totalEntities = Reg->GetTotalEntityCount();
	LOG_ALWAYS_F("[RuntimeTest] Total entities alive: %zu", totalEntities);
	ASSERT(totalEntities > 0);
}

// ---------------------------------------------------------------------------
// PlayerConstruct — Proves the full Construct/View stack end-to-end.
// Creates a PlayerConstruct with a DefaultView capsule entity.
// ScalarUpdate oscillates position and pulses color.
// ---------------------------------------------------------------------------
RUNTIME_TEST(Spawn_PlayerConstruct)
{
	Engine.Spawn([&Engine](Registry*)
	{
		World* world = Engine.GetDefaultWorld();
		world->GetConstructRegistry()->Create<PlayerConstruct>(world);
	});

	LOG_ALWAYS("[RuntimeTest] PlayerConstruct spawned — oscillating capsule with color pulse");
}

// ---------------------------------------------------------------------------
// Testbed GameManager — runs the test suite, then enters the engine loop
// ---------------------------------------------------------------------------
class TestbedGame : public GameManager<TestbedGame>
{
public:
	const char* GetWindowTitle() const { return "Trinyx Testbed"; }

	bool PostInitialize(TrinyxEngine& engine)
	{
		if (TestRegistry::Instance().RunAll(engine) != 0) return false;
		return true;
	}

	void PostStart(TrinyxEngine& engine)
	{
		RuntimeTestRegistry::Instance().RunAll(engine);
	}
};

TNX_IMPLEMENT_GAME(TestbedGame)
