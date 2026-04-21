#pragma once
#include <cassert>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Curve System
//
// Three types serve distinct roles:
//
//   CurveHandle  — 32-bit wire-safe handle. Embeds the curve type in the
//                  high 4 bits so evaluation dispatch is free at call time.
//                  Value == 0 is the "linear, no table lookup" default —
//                  CurveRef::Evaluate() returns t directly, zero overhead.
//
//   CurveRef<T>  — Runtime read-only field. Lives in component and layer
//                  structs. Wraps a CurveHandle and exposes Evaluate(t) → T.
//                  T must have scalar lerp semantics (float, Vector3, etc.).
//
//   CurveEditor<T> — Editor / pipeline only. Never placed in runtime structs.
//                  Owns key storage, supports key editing and type switching.
//                  Writes a baked CurveHandle into the global CurveTable for
//                  runtime consumption.
//
// CurveTable — global singleton. All non-trivial curves (CurveType != Linear)
//              are registered here. Curves are pure data: sorted time-value
//              key arrays. Evaluation method is driven by the handle's type
//              bits — no virtual dispatch.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// CurveType — evaluation method, packed into CurveHandle bits[31:28].
// ---------------------------------------------------------------------------
enum class CurveType : uint8_t
{
    Linear   = 0,  // Lerp — no table lookup required (CurveHandle.Value == 0)
    Step     = 1,  // Constant: value of the key at or before t
    Bezier   = 2,  // Cubic Bezier — keys carry in/out tangent control points
    Hermite  = 3,  // Cubic Hermite spline — keys carry in/out tangent slopes
};

// ---------------------------------------------------------------------------
// CurveHandle — 32-bit packed handle.
//
//   bits[31:28] = CurveType  (4 bits — 16 types max)
//   bits[27:0]  = table index (28 bits — 268M entries max)
//
//   Value == 0  → CurveType::Linear, no table entry (free default).
//                 CurveRef::Evaluate() returns t unchanged for scalar float,
//                 or linearly interpolates endpoints for other T.
// ---------------------------------------------------------------------------
struct CurveHandle
{
    uint32_t Value = 0;

    CurveType  GetType()  const { return static_cast<CurveType>((Value >> 28) & 0xF); }
    uint32_t   GetIndex() const { return Value & 0x0FFF'FFFFu; }
    bool       IsLinear() const { return Value == 0; }

    static CurveHandle Make(CurveType type, uint32_t index)
    {
        assert(index <= 0x0FFF'FFFFu && "CurveTable index out of range");
        return { (static_cast<uint32_t>(type) << 28) | (index & 0x0FFF'FFFFu) };
    }

    static CurveHandle Linear() { return {}; }

    bool operator==(CurveHandle o) const { return Value == o.Value; }
    bool operator!=(CurveHandle o) const { return Value != o.Value; }
};
static_assert(sizeof(CurveHandle) == 4, "CurveHandle must be 4 bytes");

// ---------------------------------------------------------------------------
// CurveKey<T> — one key in a curve.
//
// For Linear / Step: only Time and Value are used.
// For Bezier:        TangentIn / TangentOut are the off-curve control points
//                    (same type as Value — e.g. float or Vector3).
// For Hermite:       TangentIn / TangentOut are slope values (same type).
//
// Keys must be stored sorted by Time in ascending order.
// ---------------------------------------------------------------------------
template <typename T>
struct CurveKey
{
    float Time       = 0.0f;
    T     Value      = {};
    T     TangentIn  = {};
    T     TangentOut = {};
};

// ---------------------------------------------------------------------------
// CurveData<T> — the raw key array for one curve, stored in CurveTable.
// Pure data, no virtual functions.
// ---------------------------------------------------------------------------
template <typename T>
struct CurveData
{
    std::vector<CurveKey<T>> Keys;
    CurveType                Type = CurveType::Linear;
};

// ---------------------------------------------------------------------------
// CurveTable — global singleton.
//
// All non-Linear curves are registered here at startup (editor bake) or on
// first use. Curves are never removed — handles remain valid for process lifetime.
//
// Thread safety: registration happens before runtime (editor/build pipeline).
// Evaluate() is read-only and safe to call from any thread during play.
// ---------------------------------------------------------------------------
class CurveTable
{
public:
    static CurveTable& Get()
    {
        static CurveTable instance;
        return instance;
    }

    // Register a pre-built float curve and return its handle.
    // Called by CurveEditor<float>::Bake() — not called at runtime.
    CurveHandle RegisterFloat(CurveData<float>&& data)
    {
        const uint32_t index = static_cast<uint32_t>(FloatCurves.size());
        CurveType type = data.Type;
        FloatCurves.push_back(std::move(data));
        return CurveHandle::Make(type, index);
    }

    // Evaluate a float curve at normalized t ∈ [0, 1].
    // CurveHandle::Value == 0 → returns t directly (linear pass-through).
    float EvaluateFloat(CurveHandle handle, float t) const
    {
        if (handle.IsLinear()) return t;
        const uint32_t index = handle.GetIndex();
        if (index >= FloatCurves.size()) return t;
        return EvaluateCurve(FloatCurves[index], handle.GetType(), t);
    }

private:
    CurveTable() = default;

    // Evaluate a typed curve — dispatches by CurveType without virtual calls.
    template <typename T>
    static T EvaluateCurve(const CurveData<T>& curve, CurveType type, float t)
    {
        const auto& keys = curve.Keys;
        if (keys.empty()) return T{};
        if (keys.size() == 1) return keys[0].Value;

        // Clamp t to key range.
        if (t <= keys.front().Time) return keys.front().Value;
        if (t >= keys.back().Time)  return keys.back().Value;

        // Binary search for the segment containing t.
        uint32_t lo = 0;
        uint32_t hi = static_cast<uint32_t>(keys.size()) - 1;
        while (hi - lo > 1)
        {
            const uint32_t mid = (lo + hi) / 2;
            if (keys[mid].Time <= t) lo = mid;
            else                     hi = mid;
        }

        const CurveKey<T>& k0 = keys[lo];
        const CurveKey<T>& k1 = keys[hi];
        const float segLen = k1.Time - k0.Time;
        const float s      = segLen > 0.0f ? (t - k0.Time) / segLen : 0.0f;

        switch (type)
        {
        case CurveType::Step:
            return k0.Value;

        case CurveType::Hermite:
            return EvalHermite(k0.Value, k0.TangentOut, k1.Value, k1.TangentIn, s, segLen);

        case CurveType::Bezier:
            return EvalBezier(k0.Value, k0.TangentOut, k1.TangentIn, k1.Value, s);

        default: // Linear
            return Lerp(k0.Value, k1.Value, s);
        }
    }

    // Lerp — requires T supports operator+ and operator* with float.
    template <typename T>
    static T Lerp(const T& a, const T& b, float t)
    {
        return a + (b - a) * t;
    }

    // Cubic Hermite: p(s) = h00*p0 + h10*m0*d + h01*p1 + h11*m1*d
    // m0/m1 are tangent slopes (rise over run); d is segment duration.
    template <typename T>
    static T EvalHermite(const T& p0, const T& m0, const T& p1, const T& m1, float s, float d)
    {
        const float s2  = s  * s;
        const float s3  = s2 * s;
        const float h00 =  2*s3 - 3*s2 + 1;
        const float h10 =    s3 - 2*s2 + s;
        const float h01 = -2*s3 + 3*s2;
        const float h11 =    s3 -   s2;
        return p0 * h00 + m0 * (h10 * d) + p1 * h01 + m1 * (h11 * d);
    }

    // Cubic Bezier: B(s) = (1-s)³p0 + 3(1-s)²s·c0 + 3(1-s)s²·c1 + s³p1
    template <typename T>
    static T EvalBezier(const T& p0, const T& c0, const T& c1, const T& p1, float s)
    {
        const float t1  = 1.0f - s;
        const float b0  = t1 * t1 * t1;
        const float b1  = 3.0f * t1 * t1 * s;
        const float b2  = 3.0f * t1 * s  * s;
        const float b3  = s  * s  * s;
        return p0 * b0 + c0 * b1 + c1 * b2 + p1 * b3;
    }

    std::vector<CurveData<float>> FloatCurves;
};

// ---------------------------------------------------------------------------
// CurveRef<T> — runtime field handle. Lives in component and layer structs.
//
// Evaluate(t) → T:
//   - handle.IsLinear(): returns t for float, or Lerp(T{}, T{1}, t) for others.
//   - Otherwise: delegates to CurveTable::Get().EvaluateFloat() (float only for now).
//
// T == float is the primary use case. Additional specializations can be added
// as CurveTable gains RegisterVector3 / RegisterColor etc.
// ---------------------------------------------------------------------------
template <typename T>
struct CurveRef
{
    CurveHandle Handle;

    CurveRef() = default;
    explicit CurveRef(CurveHandle h) : Handle(h) {}

    T Evaluate(float t) const;

    bool IsLinear() const { return Handle.IsLinear(); }
};

// float specialization — fully inline, zero overhead for the linear case.
template <>
inline float CurveRef<float>::Evaluate(float t) const
{
    if (Handle.IsLinear()) return t;
    return CurveTable::Get().EvaluateFloat(Handle, t);
}

// ---------------------------------------------------------------------------
// CurveEditor<T> — editor / pipeline type. Never placed in runtime structs.
//
// Owns mutable key storage. Bake() writes the final CurveData into CurveTable
// and returns the CurveHandle for embedding in asset data.
// ---------------------------------------------------------------------------
template <typename T>
struct CurveEditor
{
    CurveType            Type = CurveType::Linear;
    std::vector<CurveKey<T>> Keys;

    void AddKey(float time, T value, T tangentIn = {}, T tangentOut = {})
    {
        CurveKey<T> k{ time, value, tangentIn, tangentOut };
        // Insert sorted by time.
        auto it = Keys.begin();
        while (it != Keys.end() && it->Time < time) ++it;
        Keys.insert(it, k);
    }

    void RemoveKey(uint32_t index)
    {
        if (index < Keys.size()) Keys.erase(Keys.begin() + index);
    }

    void SetType(CurveType type) { Type = type; }

    // Bake into CurveTable and return the runtime handle.
    // Only valid for T == float until CurveTable gains more typed stores.
    CurveHandle Bake();
};

// float specialization.
template <>
inline CurveHandle CurveEditor<float>::Bake()
{
    if (Keys.empty() || Type == CurveType::Linear) return CurveHandle::Linear();
    CurveData<float> data;
    data.Type = Type;
    data.Keys = Keys;
    return CurveTable::Get().RegisterFloat(std::move(data));
}
