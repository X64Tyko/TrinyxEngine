#include "TestFramework.h"
#include "TrinyxEngine.h"
#include "EngineConfig.h"

// Validates EngineConfig::FillFrom priority: fields already set must not be overwritten;
// Unset fields must be filled from the source. Also validates ApplyDefaults gap-filling.
TEST(Config_FillFrom)
{
	(void)Engine;

	// --- FillFrom: set field survives, Unset field gets filled ---
	{
		EngineConfig dst;
		dst.FixedUpdateHz = 512;        // explicitly set
		dst.NetworkUpdateHz = EngineConfig::Unset; // unset — should be filled

		EngineConfig src;
		src.FixedUpdateHz   = 64;   // lower value — must NOT overwrite dst
		src.NetworkUpdateHz = 20;   // should fill dst

		dst.FillFrom(src);

		ASSERT_EQ(dst.FixedUpdateHz, 512);  // dst's value preserved
		ASSERT_EQ(dst.NetworkUpdateHz, 20); // filled from src
	}

	// --- FillFrom: Unset source field does NOT fill destination ---
	{
		EngineConfig dst;
		dst.TargetFPS = EngineConfig::Unset;

		EngineConfig src;
		src.TargetFPS = EngineConfig::Unset; // also unset

		dst.FillFrom(src);
		ASSERT_EQ(dst.TargetFPS, EngineConfig::Unset); // still unset
	}

	// --- ApplyDefaults: fills remaining Unset fields with known compiled defaults ---
	{
		EngineConfig cfg;
		// Everything starts at Unset
		cfg.ApplyDefaults();

		ASSERT(cfg.FixedUpdateHz != EngineConfig::Unset);
		ASSERT(cfg.NetworkUpdateHz != EngineConfig::Unset);
		ASSERT(cfg.MAX_CACHED_ENTITIES != EngineConfig::Unset);
		ASSERT(cfg.MAX_RENDERABLE_ENTITIES != EngineConfig::Unset);
		ASSERT(cfg.MAX_JOLT_BODIES != EngineConfig::Unset);
		ASSERT(cfg.TemporalFrameCount != EngineConfig::Unset);
		ASSERT(cfg.JobCacheSize != EngineConfig::Unset);

		// Known compiled defaults from EngineConfig.cpp
		ASSERT_EQ(cfg.FixedUpdateHz, 128);
		ASSERT_EQ(cfg.NetworkUpdateHz, 30);
		ASSERT_EQ(cfg.PhysicsUpdateInterval, 8);
	}

	// --- ApplyDefaults: pre-set field not clobbered ---
	{
		EngineConfig cfg;
		cfg.FixedUpdateHz = 256;
		cfg.ApplyDefaults();
		ASSERT_EQ(cfg.FixedUpdateHz, 256); // not overwritten by default (128)
	}

	// --- FillFrom + ApplyDefaults cascade: simulates the LoadProjectConfig chain ---
	{
		// Project-specific config (most specific)
		EngineConfig project;
		project.FixedUpdateHz = 512;

		// Engine defaults (least specific)
		EngineConfig engine;
		engine.FixedUpdateHz   = 128;
		engine.NetworkUpdateHz = 30;
		engine.MAX_CACHED_ENTITIES = 50000;

		// Cascade: project fills from engine for anything still Unset
		project.FillFrom(engine);
		project.ApplyDefaults();

		ASSERT_EQ(project.FixedUpdateHz, 512);          // project wins
		ASSERT_EQ(project.NetworkUpdateHz, 30);          // engine filled it
		ASSERT_EQ(project.MAX_CACHED_ENTITIES, 50000);   // engine filled it
	}
}
