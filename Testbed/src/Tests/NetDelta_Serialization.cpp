#include "TestFramework.h"
#include "NetDelta.h"
#include "SchemaReflector.h"
#include "CTransform.h"
#include "CVelocity.h"
#include "CTranslation.h"
#include "CRotation.h"
#include "CRigidBody.h"

// ---------------------------------------------------------------------------
// Minimal test-only components.
// Uses plain float (not SimFloat) so expected byte counts are unambiguous.
// ---------------------------------------------------------------------------

// Standard 3-field component — no custom serializer (exercises default path).
struct DeltaTestComp
{
	TNX_REGISTER_FIELDS(DeltaTestComp, X, Y, Z)
	FieldProxy<float, FieldWidth::Scalar> X;
	FieldProxy<float, FieldWidth::Scalar> Y;
	FieldProxy<float, FieldWidth::Scalar> Z;
};

// Component that provides its own DeltaSerialize — exercises the concept dispatch path.
// Writes/reads a fixed sentinel byte so we can verify the custom impl was called.
struct CustomDeltaComp
{
	TNX_REGISTER_FIELDS(CustomDeltaComp, A)
	FieldProxy<float, FieldWidth::Scalar> A;

	static void DeltaSerialize(const ComponentDeltaCtx&, NetDeltaWriter& w)
	{
		w.WriteU8(0xCC);
	}
	static bool DeltaDeserialize(const ComponentDeltaCtx&, NetDeltaReader& r)
	{
		return r.ReadU8() == 0xCC && r.IsOk();
	}
};

// ---------------------------------------------------------------------------
// NetDeltaWriter / NetDeltaReader
// ---------------------------------------------------------------------------

TEST(NetDelta_Writer_RoundTrip)
{
	uint8_t buf[32] = {};
	NetDeltaWriter w{ buf, 0, sizeof(buf) };

	w.WriteU8(0xAB);
	w.WriteU16(0x1234);
	w.WriteU32(0xDEADBEEF);
	const float fval = 3.14f;
	w.WriteBytes(&fval, sizeof(float));

	ASSERT(w.IsOk());
	ASSERT_EQ(w.BytesWritten(), 11u); // 1 + 2 + 4 + 4

	NetDeltaReader r{ buf, 0, w.BytesWritten() };
	ASSERT_EQ(r.ReadU8(), uint8_t(0xAB));
	ASSERT_EQ(r.ReadU16(), uint16_t(0x1234));
	ASSERT_EQ(r.ReadU32(), uint32_t(0xDEADBEEF));
	float rval = 0.0f;
	r.ReadBytes(&rval, sizeof(float));

	ASSERT(r.IsOk());
	ASSERT_EQ(rval, 3.14f);
}

TEST(NetDelta_Writer_OverflowSetsFlag)
{
	uint8_t buf[2] = {};
	NetDeltaWriter w{ buf, 0, 2 };
	w.WriteU32(0x12345678); // needs 4 bytes
	ASSERT(!w.IsOk());
}

TEST(NetDelta_Reader_OverreadSetsFlag)
{
	uint8_t buf[2] = { 0xAA, 0xBB };
	NetDeltaReader r{ buf, 0, 2 };
	r.ReadU32(); // needs 4 bytes
	ASSERT(!r.IsOk());
}

// ---------------------------------------------------------------------------
// DefaultDeltaSerialize — wire format and mask encoding
// ---------------------------------------------------------------------------

// Helper: build per-field "array" pointers where each pointer addresses one
// stack float. Index 0 is the only entity — matches ctx.Index = 0.
TEST(NetDelta_DefaultSerialize_NoChanges_ZeroMask)
{
	// current == baseline for all fields: only the uint8 mask should be written.
	float x = 1.f, y = 2.f, z = 3.f;
	float bx = 1.f, by = 2.f, bz = 3.f;

	void* cur[3]  = { &x, &y, &z };
	void* base[3] = { &bx, &by, &bz };

	uint8_t buf[16] = {};
	NetDeltaWriter w{ buf, 0, sizeof(buf) };
	DefaultDeltaSerialize<DeltaTestComp>({ cur, base, 0 }, w);

	ASSERT(w.IsOk());
	ASSERT_EQ(w.BytesWritten(), 1u); // mask only
	ASSERT_EQ(buf[0], uint8_t(0));   // mask = 0 — nothing changed
}

TEST(NetDelta_DefaultSerialize_SingleFieldChanged)
{
	// Only Y (field index 1) differs → bit 1 set in mask.
	float curX = 1.f, curY = 2.f, curZ = 3.f;
	float basX = 1.f, basY = 0.f, basZ = 3.f;

	void* cur[3]  = { &curX, &curY, &curZ };
	void* base[3] = { &basX, &basY, &basZ };

	uint8_t buf[16] = {};
	NetDeltaWriter w{ buf, 0, sizeof(buf) };
	DefaultDeltaSerialize<DeltaTestComp>({ cur, base, 0 }, w);

	ASSERT(w.IsOk());
	ASSERT_EQ(w.BytesWritten(), 5u);        // mask (1) + Y float (4)
	ASSERT_EQ(buf[0], uint8_t(0b00000010)); // bit 1 = field Y
}

TEST(NetDelta_DefaultSerialize_AllFieldsChanged)
{
	float curX = 10.f, curY = 20.f, curZ = 30.f;
	float basX = 0.f, basY = 0.f, basZ = 0.f;

	void* cur[3]  = { &curX, &curY, &curZ };
	void* base[3] = { &basX, &basY, &basZ };

	uint8_t buf[16] = {};
	NetDeltaWriter w{ buf, 0, sizeof(buf) };
	DefaultDeltaSerialize<DeltaTestComp>({ cur, base, 0 }, w);

	ASSERT(w.IsOk());
	ASSERT_EQ(w.BytesWritten(), 13u);       // mask (1) + 3 floats (12)
	ASSERT_EQ(buf[0], uint8_t(0b00000111)); // all 3 bits set
}

// ---------------------------------------------------------------------------
// DefaultDeltaDeserialize — only flagged fields are written into the dest
// ---------------------------------------------------------------------------

TEST(NetDelta_DefaultDeserialize_PartialRoundTrip)
{
	// Serialise: Y changed (0 → 2), X and Z unchanged.
	float curX = 1.f, curY = 2.f, curZ = 3.f;
	float basX = 1.f, basY = 0.f, basZ = 3.f;

	void* cur[3]  = { &curX, &curY, &curZ };
	void* base[3] = { &basX, &basY, &basZ };

	uint8_t buf[16] = {};
	NetDeltaWriter w{ buf, 0, sizeof(buf) };
	DefaultDeltaSerialize<DeltaTestComp>({ cur, base, 0 }, w);
	ASSERT(w.IsOk());

	// Deserialise into an apply buffer starting at baseline values.
	float apX = 1.f, apY = 0.f, apZ = 3.f;
	void* ap[3] = { &apX, &apY, &apZ };

	NetDeltaReader r{ buf, 0, w.BytesWritten() };
	const bool ok = DefaultDeltaDeserialize<DeltaTestComp>({ ap, nullptr, 0 }, r);

	ASSERT(ok);
	ASSERT(r.IsOk());
	ASSERT_EQ(apX, 1.f); // unchanged
	ASSERT_EQ(apY, 2.f); // updated
	ASSERT_EQ(apZ, 3.f); // unchanged
}

TEST(NetDelta_DefaultDeserialize_AllFieldsRoundTrip)
{
	float curX = 10.f, curY = 20.f, curZ = 30.f;
	float basX = 0.f, basY = 0.f, basZ = 0.f;

	void* cur[3]  = { &curX, &curY, &curZ };
	void* base[3] = { &basX, &basY, &basZ };

	uint8_t buf[16] = {};
	NetDeltaWriter w{ buf, 0, sizeof(buf) };
	DefaultDeltaSerialize<DeltaTestComp>({ cur, base, 0 }, w);

	float apX = 0.f, apY = 0.f, apZ = 0.f;
	void* ap[3] = { &apX, &apY, &apZ };

	NetDeltaReader r{ buf, 0, w.BytesWritten() };
	ASSERT(DefaultDeltaDeserialize<DeltaTestComp>({ ap, nullptr, 0 }, r));
	ASSERT_EQ(apX, 10.f);
	ASSERT_EQ(apY, 20.f);
	ASSERT_EQ(apZ, 30.f);
}

TEST(NetDelta_Deserialize_TruncatedPacket_ReturnsFalse)
{
	// Provide a buffer that's too short to hold the field payload.
	uint8_t buf[1] = { 0b00000111 }; // mask says all 3 fields present, but no data follows
	NetDeltaReader r{ buf, 0, sizeof(buf) };

	float apX = 0.f, apY = 0.f, apZ = 0.f;
	void* ap[3] = { &apX, &apY, &apZ };

	const bool ok = DefaultDeltaDeserialize<DeltaTestComp>({ ap, nullptr, 0 }, r);
	ASSERT(!ok); // reader hit truncation
}

// ---------------------------------------------------------------------------
// InvokeDeltaSerialize — concept dispatch (default vs. custom)
// ---------------------------------------------------------------------------

TEST(NetDelta_InvokeDefault_MatchesDirectCall)
{
	// InvokeDeltaSerialize with no custom impl should produce identical output
	// to calling DefaultDeltaSerialize directly.
	float cX = 5.f, cY = 0.f, cZ = 5.f;
	float bX = 0.f, bY = 0.f, bZ = 0.f;

	void* cur[3]  = { &cX, &cY, &cZ };
	void* base[3] = { &bX, &bY, &bZ };

	uint8_t buf1[16] = {}, buf2[16] = {};
	NetDeltaWriter w1{ buf1, 0, sizeof(buf1) };
	NetDeltaWriter w2{ buf2, 0, sizeof(buf2) };

	DefaultDeltaSerialize<DeltaTestComp>({ cur, base, 0 }, w1);
	InvokeDeltaSerialize<DeltaTestComp>({ cur, base, 0 }, w2);

	ASSERT_EQ(w1.BytesWritten(), w2.BytesWritten());
	for (uint32_t i = 0; i < w1.BytesWritten(); ++i)
		ASSERT_EQ(buf1[i], buf2[i]);
}

TEST(NetDelta_InvokeCustom_CallsCustomImpl)
{
	// InvokeDeltaSerialize with HasCustomDeltaSerialize<T> true must route
	// to the component's own DeltaSerialize, not the default.
	float a      = 0.f;
	void* cur[1] = { &a };
	ComponentDeltaCtx ctx{ cur, cur, 0 };

	uint8_t buf[8] = {};
	NetDeltaWriter w{ buf, 0, sizeof(buf) };
	InvokeDeltaSerialize<CustomDeltaComp>(ctx, w);

	ASSERT(w.IsOk());
	ASSERT_EQ(w.BytesWritten(), 1u);
	ASSERT_EQ(buf[0], uint8_t(0xCC)); // sentinel confirms custom impl ran

	NetDeltaReader r{ buf, 0, 1 };
	ASSERT(InvokeDeltaDeserialize<CustomDeltaComp>(ctx, r));
}

// ---------------------------------------------------------------------------
// ComponentDeltaRegistry — TNX_NET_TEMPORAL registration
// ---------------------------------------------------------------------------

TEST(NetDelta_Registry_TemporalComponentsPresent)
{
	// All components tagged TNX_NET_TEMPORAL must be findable by type ID.
	const auto check = [](ComponentTypeID id, [[maybe_unused]] const char* name)
	{
		const ComponentDeltaFns* fns = ComponentDeltaRegistry::Get().Find(id);
		if (!fns || !fns->IsValid())
		{
			// Manual assertion message so we can identify the failing component.
			ASSERT(false);
		}
	};

	check(CTransform<>::StaticTypeID(), "CTransform");
	check(CVelocity<>::StaticTypeID(), "CVelocity");
	check(CTranslation<>::StaticTypeID(), "CTranslation");
	check(CRotation<>::StaticTypeID(), "CRotation");
	check(CRigidBody<>::StaticTypeID(), "CRigidBody");
}

TEST(NetDelta_Registry_FnPtrsProduceValidOutput)
{
	// Run a full round-trip through the type-erased fn ptrs for CTransform.
	// CTransform has 7 SimFloat fields — allocate 7 independent float-sized slots.
	// We route through ComponentDeltaFns to exercise the runtime dispatch path.
	using Comp      = CTransform<>;
	constexpr int N = 7;

	SimFloat cur[N]  = { 1, 0, 0, 0, 0, 0, 1 }; // PosX=1, RotQw=1
	SimFloat base[N] = { 0, 0, 0, 0, 0, 0, 1 }; // PosX differs

	void *curPtrs[N], *basePtrs[N];
	for (int i = 0; i < N; ++i)
	{
		curPtrs[i]  = &cur[i];
		basePtrs[i] = &base[i];
	}

	const ComponentDeltaFns* fns = ComponentDeltaRegistry::Get().Find(Comp::StaticTypeID());
	ASSERT(fns != nullptr);

	uint8_t buf[64] = {};
	NetDeltaWriter w{ buf, 0, sizeof(buf) };
	fns->Serialize({ curPtrs, basePtrs, 0 }, w);
	ASSERT(w.IsOk());

	// mask (uint8) + 1 changed field (SimFloat) must have been written.
	ASSERT_EQ(w.BytesWritten(), uint32_t(1 + sizeof(SimFloat)));
	ASSERT_EQ(buf[0], uint8_t(0b00000001)); // bit 0 = PosX

	SimFloat apply[N] = { 0, 0, 0, 0, 0, 0, 1 };
	void* apPtrs[N];
	for (int i = 0; i < N; ++i)
		apPtrs[i] = &apply[i];

	NetDeltaReader r{ buf, 0, w.BytesWritten() };
	ASSERT(fns->Deserialize({ apPtrs, nullptr, 0 }, r));
	ASSERT(r.IsOk());

	// PosX should be updated; all other fields untouched.
	ASSERT_EQ(apply[0], SimFloat(1.f)); // PosX updated
	ASSERT_EQ(apply[1], SimFloat(0.f)); // PosY unchanged
	ASSERT_EQ(apply[6], SimFloat(1.f)); // RotQw unchanged
}
