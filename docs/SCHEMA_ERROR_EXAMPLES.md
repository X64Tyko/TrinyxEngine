# Schema Reflection - Error Message Examples

> **Navigation:** [← Back to README](../README.md) | [← Build Options](BUILD_OPTIONS.md)

This document shows the improved error messages for common schema reflection mistakes.

## 1. Forgetting TNX_REGISTER_ENTITY

**Code:**
```cpp
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct MyEntity : EntityView<MyEntity, WIDTH> {
    CTransform<WIDTH> Transform;

    FORCE_INLINE void PrePhysics(SimFloat dt) { /* ... */ }

    TNX_REGISTER_SCHEMA(MyEntity, EntityView, Transform)
};

// MISSING: TNX_REGISTER_ENTITY(MyEntity);

// In main:
EntityID id = Registry::Create<MyEntity>();  // Runtime error!
```

**Error (Runtime - in log file):**
```
[ERROR] FATAL: Entity type 'MyEntity' not registered!
        Did you forget TNX_REGISTER_ENTITY(MyEntity)?
```

**Debug build:** Triggers assertion
**Release build:** Returns invalid EntityID

---

## 2. Missing TNX_REGISTER_SCHEMA

**Code:**
```cpp
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct BadEntity : EntityView<BadEntity, WIDTH> {
    CTransform<WIDTH> Transform;

    FORCE_INLINE void PrePhysics(SimFloat dt) { /* ... */ }

    // MISSING: TNX_REGISTER_SCHEMA(BadEntity, EntityView, Transform)
};

TNX_REGISTER_ENTITY(BadEntity);  // Compile error!
```

**Error (Compile-time):**
```
================================================================
ERROR: Entity missing schema registration!
================================================================

Add TNX_REGISTER_SCHEMA to your entity class:

    TNX_REGISTER_SCHEMA(YourEntity, EntityView, component1, component2, ...)

================================================================
```

---

## 3. Entity with Virtual Functions

**Code:**
```cpp
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct VirtualEntity : EntityView<VirtualEntity, WIDTH> {
    CTransform<WIDTH> Transform;

    virtual void PrePhysics(SimFloat dt) { /* ... */ }  // WRONG: virtual!

    TNX_REGISTER_SCHEMA(VirtualEntity, EntityView, Transform)
};

TNX_REGISTER_ENTITY(VirtualEntity);  // Compile error!
```

**Error (Compile-time):**
```
================================================================
ERROR: Entity must be standard layout!
================================================================

Entity types cannot have:
  - Virtual functions
  - Complex inheritance

Entities are lightweight data containers.
================================================================
```

---

## 4. Component with std::string or std::vector

**Code:**
```cpp
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct BadComponent {
    std::string name;  // WRONG: not POD! Components must use FieldProxy.
    FieldProxy<float, WIDTH> value;

    TNX_TEMPORAL_FIELDS(BadComponent, SystemGroup::None, value)
};
TNX_REGISTER_COMPONENT(BadComponent);  // Compile error from VALIDATE_COMPONENT_IS_POD!
```

**Error (Compile-time):**
```
================================================================
ERROR: Component must be POD (plain old data)!
================================================================

Components CANNOT have:
  - Virtual functions
  - Non-trivial constructors/destructors
  - std::string, std::vector, or complex types
  - Heap-allocated pointers

All component fields must be FieldProxy<T, WIDTH>:

    template <FieldWidth WIDTH = FieldWidth::Scalar>
    struct CMyTransform {
        FieldProxy<float, WIDTH> PosX, PosY, PosZ;
        TNX_TEMPORAL_FIELDS(CMyTransform, SystemGroup::None, PosX, PosY, PosZ)
    };

================================================================
```

---

## 5. Derived Entity Missing TNX_REGISTER_SUPER_SCHEMA on Base

**Code:**
```cpp
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct BaseCube : EntityView<BaseCube, WIDTH> {
    CTransform<WIDTH> Transform;

    FORCE_INLINE void PrePhysics(SimFloat dt) {
        Transform.RotationQw += static_cast<float>(dt);  // simplified example
    }

    // WRONG: Using TNX_REGISTER_SCHEMA instead of TNX_REGISTER_SUPER_SCHEMA!
    TNX_REGISTER_SCHEMA(BaseCube, EntityView, Transform)
};

template <FieldWidth WIDTH = FieldWidth::Scalar>
struct SuperCube : BaseCube<SuperCube, WIDTH> {
    CVelocity<WIDTH> Velocity;

    FORCE_INLINE void PrePhysics(SimFloat dt) {
        BaseCube<SuperCube, WIDTH>::PrePhysics(dt);
        Transform.PosX += Velocity.VelX * static_cast<float>(dt);
    }

    TNX_REGISTER_SCHEMA(SuperCube, BaseCube, Velocity)
};
TNX_REGISTER_ENTITY(SuperCube);
```

**Solution:** Use `TNX_REGISTER_SUPER_SCHEMA` for non-leaf base entities:
```cpp
// In BaseCube (intermediate base, not instantiated directly):
TNX_REGISTER_SUPER_SCHEMA(BaseCube, EntityView, transform)
```

**Note:** `TNX_REGISTER_SUPER_SCHEMA` is required for base classes in the CRTP hierarchy that
pass `Derived` through to `EntityView`. Using `TNX_REGISTER_SCHEMA` on a base will cause
incorrect schema generation for derived types.

---

## Best Practices

### ✅ Good Entity
```cpp
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct GoodEntity : EntityView<GoodEntity, WIDTH> {
    CTransform<WIDTH> Transform;
    CVelocity<WIDTH>  Velocity;
    CColor<WIDTH>     Color;

    // Lifecycle methods (not virtual, FORCE_INLINE)
    FORCE_INLINE void PrePhysics(SimFloat dt) {
        Transform.PosX += Velocity.VelX * static_cast<float>(dt);
    }

    TNX_REGISTER_SCHEMA(GoodEntity, EntityView, Transform, Velocity, Color)
};
TNX_REGISTER_ENTITY(GoodEntity)
```

### ✅ Good Component (Temporal)
```cpp
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct GoodComponent {
    FieldProxy<float, WIDTH> Value1, Value2;
    FieldProxy<int32_t, WIDTH> Value3;

    TNX_TEMPORAL_FIELDS(GoodComponent, SystemGroup::None, Value1, Value2, Value3)
};
TNX_REGISTER_COMPONENT(GoodComponent)
```

### ✅ Good Component (Cold)

```cpp
struct ColdComponent {
    uint32_t ShapeType;
    float Mass;
    float Friction;

    TNX_REGISTER_FIELDS(ColdComponent, ShapeType, Mass, Friction)
};
TNX_REGISTER_COMPONENT(ColdComponent)
```

### ❌ Bad Entity
```cpp
template <FieldWidth WIDTH = FieldWidth::Scalar>
struct BadEntity : EntityView<BadEntity, WIDTH> {
    CTransform<WIDTH> Transform;

    // WRONG: Virtual function
    virtual void PrePhysics(SimFloat dt) { }

    // WRONG: Complex member (not FieldProxy)
    std::vector<int> data;

    TNX_REGISTER_SCHEMA(BadEntity, EntityView, Transform)
};
```

---

## Summary

**Compile-time checks (fail fast):**
- ✅ Missing DefineSchema()
- ✅ Virtual functions in entities
- ✅ Non-POD components

**Runtime checks:**
- ✅ Forgetting TNX_REGISTER_ENTITY (asserts in debug, logs error in release)

**Not checked (gotcha):**
- ⚠️ Forgetting Replace() in derived entities (silent wrong behavior)

**Future improvements:**
- Add static analyzer to detect missing Replace()
- Add runtime function pointer comparison (expensive)
- Generate better template error messages with concepts (C++20)
