#include "TestFramework.h"
#include "NetDelta.h"
#include "NetTypes.h"
#include "SchemaReflector.h"
#include "CTransform.h"
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Test component — 3 float fields, no custom serializer.
// ---------------------------------------------------------------------------
struct PacketTestComp
{
	TNX_REGISTER_FIELDS(PacketTestComp, X, Y, Z)
	FieldProxy<float, FieldWidth::Scalar> X;
	FieldProxy<float, FieldWidth::Scalar> Y;
	FieldProxy<float, FieldWidth::Scalar> Z;
};

// ---------------------------------------------------------------------------
// EntityDelta packet builder.
//
// Mirrors the payload-building logic in DispatchDeltaCorrectionJobs so tests
// can produce wire-format packets without needing a live world. Keep this in
// sync with the production dispatch code if the wire format changes.
//
// Wire format (NetMessageType::EntityDelta):
//   uint16_t EntityCount
//   For each entity:
//     uint32_t NetHandle
//     uint8_t  ComponentCount
//     For each changed component:
//       uint8_t  CacheSlot    — StaticTemporalIndex() of the component
//       uint8_t  DeltaLen     — byte count of delta payload
//       [DeltaLen bytes]      — mask + changed field values from DeltaSerialize
// ---------------------------------------------------------------------------
struct CompDesc
{
	ComponentDeltaFns Fns; // Fns.CacheSlot used as wire ID; Fns.Serialize for delta
	uint32_t FieldCount;   // used for zero-mask detection
	void** CurPtrs;        // current-frame per-field arrays, Index=0
	void** BasePtrs;       // baseline-frame per-field arrays
};

struct EntityDesc
{
	uint32_t NetHandle;
	std::vector<CompDesc> Components;
};

static std::vector<uint8_t> BuildPacket(const std::vector<EntityDesc>& entities)
{
	std::vector<uint8_t> payload;
	payload.push_back(0);
	payload.push_back(0);
	uint16_t entityCount = 0;

	for (const auto& ent : entities)
	{
		const size_t entityPos = payload.size();
		payload.resize(entityPos + 5);
		uint8_t componentCount = 0;

		for (const auto& comp : ent.Components)
		{
			ComponentDeltaCtx ctx{ comp.CurPtrs, comp.BasePtrs, 0 };
			uint8_t compBuf[256];
			NetDeltaWriter w{ compBuf, 0, sizeof(compBuf) };
			comp.Fns.Serialize(ctx, w);

			if (!w.IsOk() || w.BytesWritten() == 0) continue;

			// Detect zero mask — skip components with no changed fields.
			const uint32_t maskBytes = (comp.FieldCount <= 8)    ? 1u
									   : (comp.FieldCount <= 16) ? 2u
																 : 4u;
			if (w.BytesWritten() <= maskBytes)
			{
				uint32_t maskVal = 0;
				std::memcpy(&maskVal, compBuf, w.BytesWritten());
				if (maskVal == 0) continue;
			}

			const uint8_t deltaLen = static_cast<uint8_t>(w.BytesWritten());
			payload.push_back(comp.Fns.CacheSlot);
			payload.push_back(deltaLen);
			for (uint8_t b = 0; b < deltaLen; ++b)
				payload.push_back(compBuf[b]);
			++componentCount;
		}

		if (componentCount == 0)
		{
			payload.resize(entityPos);
			continue;
		}

		std::memcpy(payload.data() + entityPos, &ent.NetHandle, 4);
		payload[entityPos + 4] = componentCount;
		++entityCount;
	}

	std::memcpy(payload.data(), &entityCount, 2);
	return payload;
}

// ---------------------------------------------------------------------------
// EntityDelta packet parser.
//
// Reference implementation for what OwnerNetThread will eventually contain.
// Reads an EntityDelta payload and returns the parsed entities and their
// per-component delta bytes. Caller applies deltas using DefaultDeltaDeserialize.
// ---------------------------------------------------------------------------
struct ParsedComp
{
	uint8_t CacheSlot;
	uint8_t DeltaLen;
	uint8_t Data[256];
};

struct ParsedEntity
{
	uint32_t NetHandle;
	std::vector<ParsedComp> Components;
};

static bool ParsePacket(const std::vector<uint8_t>& payload,
	std::vector<ParsedEntity>& out)
{
	if (payload.size() < 2) return false;
	NetDeltaReader r{ payload.data(), 0, static_cast<uint32_t>(payload.size()) };

	const uint16_t entityCount = r.ReadU16();
	for (uint16_t e = 0; e < entityCount; ++e)
	{
		ParsedEntity ent;
		ent.NetHandle           = r.ReadU32();
		const uint8_t compCount = r.ReadU8();
		for (uint8_t c = 0; c < compCount; ++c)
		{
			ParsedComp pc{};
			pc.CacheSlot = r.ReadU8();
			pc.DeltaLen  = r.ReadU8();
			r.ReadBytes(pc.Data, pc.DeltaLen);
			if (!r.IsOk()) return false;
			ent.Components.push_back(pc);
		}
		out.push_back(std::move(ent));
	}
	return r.IsOk();
}

// ---------------------------------------------------------------------------
// Size tests — confirm delta packets are smaller than StateCorrectionEntry
// ---------------------------------------------------------------------------

TEST(NetDelta_Packet_PartialChange_SmallerThanFullState)
{
	// Only Y differs. Expected wire layout:
	//   2 (EntityCount) + 4 (NetHandle) + 1 (ComponentCount)
	//   + 1 (CacheSlot) + 1 (DeltaLen) + 1 (mask=0b010) + 4 (Y float) = 14 bytes
	float curX = 1.f, curY = 5.f, curZ = 3.f;
	float basX = 1.f, basY = 0.f, basZ = 3.f;
	void* cur[3] = { &curX, &curY, &curZ };
	void* bas[3] = { &basX, &basY, &basZ };

	ComponentDeltaFns fns = MakeComponentDeltaFns<PacketTestComp>();

	const auto payload = BuildPacket({ { 0xABCDu, { CompDesc{ fns, 3, cur, bas } } } });

	ASSERT_EQ(payload.size(), 14u);
	ASSERT(payload.size() < sizeof(StateCorrectionEntry)); // 14 < 64
}

TEST(NetDelta_Packet_AllFieldsChanged_SmallerThanStateCorrectionEntry)
{
	// All 3 fields changed. Expected layout:
	//   2 + 4 + 1 + 1 + 1 + 1 (mask=0b111) + 12 (3 floats) = 22 bytes
	float curX = 10.f, curY = 20.f, curZ = 30.f;
	float basX = 0.f, basY = 0.f, basZ = 0.f;
	void* cur[3] = { &curX, &curY, &curZ };
	void* bas[3] = { &basX, &basY, &basZ };

	ComponentDeltaFns fns = MakeComponentDeltaFns<PacketTestComp>();

	const auto payload = BuildPacket({ { 1u, { CompDesc{ fns, 3, cur, bas } } } });

	ASSERT_EQ(payload.size(), 22u);
	ASSERT(payload.size() < sizeof(StateCorrectionEntry)); // 22 < 64
}

// ---------------------------------------------------------------------------
// Omission tests — unchanged entities/components must not appear in packet
// ---------------------------------------------------------------------------

TEST(NetDelta_Packet_NoChange_EntityOmitted)
{
	// All fields identical between current and baseline.
	float x = 1.f, y = 2.f, z = 3.f;
	void* cur[3] = { &x, &y, &z };
	void* bas[3] = { &x, &y, &z }; // same values

	ComponentDeltaFns fns = MakeComponentDeltaFns<PacketTestComp>();

	const auto payload = BuildPacket({ { 1u, { CompDesc{ fns, 3, cur, bas } } } });

	uint16_t entityCount = 0;
	std::memcpy(&entityCount, payload.data(), 2);
	ASSERT_EQ(entityCount, uint16_t(0));
	ASSERT_EQ(payload.size(), 2u); // only the count header
}

TEST(NetDelta_Packet_MultiComponent_UnchangedComponentOmitted)
{
	// Component 0: X changed. Component 1: all same. Only comp 0 must appear.
	float c0X = 5.f, c0Y = 0.f, c0Z = 0.f;
	float b0X = 0.f, b0Y = 0.f, b0Z = 0.f;
	void* cur0[3] = { &c0X, &c0Y, &c0Z };
	void* bas0[3] = { &b0X, &b0Y, &b0Z };

	float same    = 7.f;
	void* cur1[3] = { &same, &same, &same };
	void* bas1[3] = { &same, &same, &same };

	ComponentDeltaFns fns0 = MakeComponentDeltaFns<PacketTestComp>();
	fns0.CacheSlot         = 0;
	ComponentDeltaFns fns1 = MakeComponentDeltaFns<PacketTestComp>();
	fns1.CacheSlot         = 1;

	const auto payload = BuildPacket({ { 1u, {
												 CompDesc{ fns0, 3, cur0, bas0 }, // changed
												 CompDesc{ fns1, 3, cur1, bas1 }, // unchanged
											 } } });

	std::vector<ParsedEntity> parsed;
	ASSERT(ParsePacket(payload, parsed));
	ASSERT_EQ(parsed.size(), 1u);
	ASSERT_EQ(parsed[0].Components.size(), 1u);               // only 1 component in packet
	ASSERT_EQ(parsed[0].Components[0].CacheSlot, uint8_t(0)); // the changed one
}

// ---------------------------------------------------------------------------
// Round-trip accuracy tests — encode then decode, verify correct field values
// ---------------------------------------------------------------------------

TEST(NetDelta_Packet_RoundTrip_PartialChange)
{
	// X and Y changed; Z unchanged. Deserialise into baseline copy, verify result.
	float curX = 10.f, curY = 20.f, curZ = 3.f;
	float basX = 0.f, basY = 0.f, basZ = 3.f;
	void* cur[3] = { &curX, &curY, &curZ };
	void* bas[3] = { &basX, &basY, &basZ };

	ComponentDeltaFns fns = MakeComponentDeltaFns<PacketTestComp>();

	const auto payload = BuildPacket({ { 42u, { CompDesc{ fns, 3, cur, bas } } } });

	std::vector<ParsedEntity> parsed;
	ASSERT(ParsePacket(payload, parsed));
	ASSERT_EQ(parsed.size(), 1u);
	ASSERT_EQ(parsed[0].NetHandle, 42u);
	ASSERT_EQ(parsed[0].Components.size(), 1u);

	// Apply delta to an apply buffer seeded with baseline values.
	float apX = 0.f, apY = 0.f, apZ = 3.f;
	void* ap[3] = { &apX, &apY, &apZ };

	const auto& pc = parsed[0].Components[0];
	NetDeltaReader r{ pc.Data, 0, pc.DeltaLen };
	ASSERT(DefaultDeltaDeserialize<PacketTestComp>({ ap, nullptr, 0 }, r));

	ASSERT_EQ(apX, 10.f); // updated from delta
	ASSERT_EQ(apY, 20.f); // updated from delta
	ASSERT_EQ(apZ, 3.f);  // untouched — baseline value preserved
}

TEST(NetDelta_Packet_RoundTrip_AllFieldsChanged)
{
	float curX = 1.f, curY = 2.f, curZ = 3.f;
	float basX = 0.f, basY = 0.f, basZ = 0.f;
	void* cur[3] = { &curX, &curY, &curZ };
	void* bas[3] = { &basX, &basY, &basZ };

	ComponentDeltaFns fns = MakeComponentDeltaFns<PacketTestComp>();

	const auto payload = BuildPacket({ { 7u, { CompDesc{ fns, 3, cur, bas } } } });

	std::vector<ParsedEntity> parsed;
	ASSERT(ParsePacket(payload, parsed));
	ASSERT_EQ(parsed.size(), 1u);

	float apX = 0.f, apY = 0.f, apZ = 0.f;
	void* ap[3] = { &apX, &apY, &apZ };

	const auto& pc = parsed[0].Components[0];
	NetDeltaReader r{ pc.Data, 0, pc.DeltaLen };
	ASSERT(DefaultDeltaDeserialize<PacketTestComp>({ ap, nullptr, 0 }, r));

	ASSERT_EQ(apX, 1.f);
	ASSERT_EQ(apY, 2.f);
	ASSERT_EQ(apZ, 3.f);
}

// ---------------------------------------------------------------------------
// Multi-entity tests — verify correct entities are included/excluded
// ---------------------------------------------------------------------------

TEST(NetDelta_Packet_MultiEntity_ChangedAndUnchanged)
{
	// Entity A: Y changed. Entity B: no change. Entity C: all changed.
	// Expect: only A and C in packet, in order.
	float cAX = 1.f, cAY = 5.f, cAZ = 1.f;
	float bAX = 1.f, bAY = 0.f, bAZ = 1.f;
	void* curA[3] = { &cAX, &cAY, &cAZ };
	void* basA[3] = { &bAX, &bAY, &bAZ };

	float same    = 7.f;
	void* curB[3] = { &same, &same, &same };
	void* basB[3] = { &same, &same, &same };

	float cCX = 9.f, cCY = 8.f, cCZ = 7.f;
	float bCX = 0.f, bCY = 0.f, bCZ = 0.f;
	void* curC[3] = { &cCX, &cCY, &cCZ };
	void* basC[3] = { &bCX, &bCY, &bCZ };

	const ComponentDeltaFns fns = MakeComponentDeltaFns<PacketTestComp>();

	const auto payload = BuildPacket({
		{ 0xAu, { CompDesc{ fns, 3, curA, basA } } },
		{ 0xBu, { CompDesc{ fns, 3, curB, basB } } }, // no change — must be omitted
		{ 0xCu, { CompDesc{ fns, 3, curC, basC } } },
	});

	std::vector<ParsedEntity> parsed;
	ASSERT(ParsePacket(payload, parsed));
	ASSERT_EQ(parsed.size(), 2u);
	ASSERT_EQ(parsed[0].NetHandle, 0xAu);
	ASSERT_EQ(parsed[1].NetHandle, 0xCu);
}

TEST(NetDelta_Packet_MultiEntity_RoundTripAccuracy)
{
	// Two entities with different changes; verify both decode correctly.
	float c0X = 10.f, c0Y = 0.f, c0Z = 0.f; // only X
	float b0X = 0.f, b0Y = 0.f, b0Z = 0.f;
	void* cur0[3] = { &c0X, &c0Y, &c0Z };
	void* bas0[3] = { &b0X, &b0Y, &b0Z };

	float c1X = 0.f, c1Y = 0.f, c1Z = 99.f; // only Z
	float b1X = 0.f, b1Y = 0.f, b1Z = 0.f;
	void* cur1[3] = { &c1X, &c1Y, &c1Z };
	void* bas1[3] = { &b1X, &b1Y, &b1Z };

	const ComponentDeltaFns fns = MakeComponentDeltaFns<PacketTestComp>();

	const auto payload = BuildPacket({
		{ 100u, { CompDesc{ fns, 3, cur0, bas0 } } },
		{ 200u, { CompDesc{ fns, 3, cur1, bas1 } } },
	});

	std::vector<ParsedEntity> parsed;
	ASSERT(ParsePacket(payload, parsed));
	ASSERT_EQ(parsed.size(), 2u);

	// Entity 100 — apply into baseline copy
	float apX = 0.f, apY = 0.f, apZ = 0.f;
	void* ap0[3] = { &apX, &apY, &apZ };
	{
		const auto& pc = parsed[0].Components[0];
		NetDeltaReader r{ pc.Data, 0, pc.DeltaLen };
		ASSERT(DefaultDeltaDeserialize<PacketTestComp>({ ap0, nullptr, 0 }, r));
	}
	ASSERT_EQ(apX, 10.f);
	ASSERT_EQ(apY, 0.f);
	ASSERT_EQ(apZ, 0.f);

	// Entity 200 — apply into baseline copy
	float apX2 = 0.f, apY2 = 0.f, apZ2 = 0.f;
	void* ap1[3] = { &apX2, &apY2, &apZ2 };
	{
		const auto& pc = parsed[1].Components[0];
		NetDeltaReader r{ pc.Data, 0, pc.DeltaLen };
		ASSERT(DefaultDeltaDeserialize<PacketTestComp>({ ap1, nullptr, 0 }, r));
	}
	ASSERT_EQ(apX2, 0.f);
	ASSERT_EQ(apY2, 0.f);
	ASSERT_EQ(apZ2, 99.f);
}

// ---------------------------------------------------------------------------
// Registry wiring test — CacheSlot must be populated by TNX_NET_TEMPORAL
// ---------------------------------------------------------------------------

TEST(NetDelta_Packet_CacheSlot_PopulatedByMacro)
{
	// TNX_NET_TEMPORAL(CTransform) must set CacheSlot to StaticTemporalIndex().
	// If this fails, DispatchDeltaCorrectionJobs would query the wrong slab slot.
	const ComponentDeltaFns* fns = ComponentDeltaRegistry::Get().Find(CTransform<>::StaticTypeID());
	ASSERT(fns != nullptr);
	ASSERT_EQ(fns->CacheSlot, CTransform<>::StaticTemporalIndex());
}
