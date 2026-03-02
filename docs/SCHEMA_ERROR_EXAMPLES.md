# Schema Reflection - Error Message Examples

> **Navigation:** [← Back to README](../README.md) | [← Build Options](BUILD_OPTIONS.md)

This document shows the improved error messages for common schema reflection mistakes.

## 1. Forgetting TNX_REGISTER_ENTITY

**Code:**
```cpp
struct MyEntity {
    Ref<Transform> transform;

    void Update(double dt) { /* ... */ }

    static constexpr auto DefineSchema() {
        return Schema::Create(&MyEntity::transform, &MyEntity::Update);
    }
};

// MISSING: TNX_REGISTER_ENTITY(MyEntity);

// In main:
EntityID id = Registry::Create<MyEntity>();  // Runtime error!
```

**Error (Runtime - in log file):**
```
[ERROR] FATAL: Entity type 'struct MyEntity' not registered!
        Did you forget TNX_REGISTER_ENTITY(MyEntity)?
```

**Debug build:** Triggers assertion
**Release build:** Returns invalid EntityID

---

## 2. Missing DefineSchema() Method

**Code:**
```cpp
struct BadEntity {
    Ref<Transform> transform;

    void Update(double dt) { /* ... */ }

    // MISSING: static constexpr auto DefineSchema() { ... }
};

TNX_REGISTER_ENTITY(BadEntity);  // Compile error!
```

**Error (Compile-time):**
```
================================================================
ERROR: Entity missing DefineSchema()!
================================================================

Add this to your entity class:

    static constexpr auto DefineSchema() {
        return Schema::Create(
            &YourEntity::component1,
            &YourEntity::Update
        );
    }

================================================================
```

---

## 3. Entity with Virtual Functions

**Code:**
```cpp
struct VirtualEntity {
    Ref<Transform> transform;

    virtual void Update(double dt) { /* ... */ }  // WRONG: virtual!

    static constexpr auto DefineSchema() {
        return Schema::Create(&VirtualEntity::transform, &VirtualEntity::Update);
    }
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
struct BadComponent {
    std::string name;  // WRONG: not POD!
    float value;
};

struct MyEntity {
    Ref<BadComponent> component;  // Compile error!

    static constexpr auto DefineSchema() {
        return Schema::Create(&MyEntity::component);
    }
};

TNX_REGISTER_ENTITY(MyEntity);
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

Use simple structs with raw data only:

    struct Transform {
        float PositionX, PositionY, PositionZ;
        float RotationX, RotationY, RotationZ;
    };

================================================================
```

---

## 5. Forgetting Replace() in Derived Entity

**Code:**
```cpp
struct CubeEntity {
    Ref<Transform> transform;

    void Update(double dt) {
        transform->RotationX += dt;
    }

    static constexpr auto DefineSchema() {
        return Schema::Create(&CubeEntity::transform, &CubeEntity::Update);
    }
};

class SuperCube : public CubeEntity {
    void Update(double dt) {  // Overrides parent
        transform->RotationX += dt * 2.0;  // Faster rotation
    }

    static constexpr auto DefineSchema() {
        // MISSING .Replace()!
        return CubeEntity::DefineSchema();
    }
};

TNX_REGISTER_ENTITY(CubeEntity);
TNX_REGISTER_ENTITY(SuperCube);
```

**Error (Runtime - silent!):**
- Compiles successfully
- SuperCube entities call **CubeEntity::Update()** instead of SuperCube::Update()
- Rotation speed is wrong, but no error message

**Solution:**
```cpp
static constexpr auto DefineSchema() {
    return CubeEntity::DefineSchema()
        .Replace(&CubeEntity::Update, &SuperCube::Update);  // ✓ Correct
}
```

**Note:** This is the main "gotcha" that doesn't have a good compile-time check yet.
We could add runtime validation comparing function pointers, but it's expensive.

---

## Best Practices

### ✅ Good Entity
```cpp
struct GoodEntity {
    // Simple Ref<> members
    Ref<Transform> transform;
    Ref<Velocity> velocity;
    Ref<ColorData> color;

    // Lifecycle methods (not virtual)
    void OnCreate() { /* initialization */ }
    void Update(double dt) { /* update logic */ }
    void OnDestroy() { /* cleanup */ }

    // Schema registration
    static constexpr auto DefineSchema() {
        return Schema::Create(
            &GoodEntity::transform,
            &GoodEntity::velocity,
            &GoodEntity::color,
            &GoodEntity::OnCreate,
            &GoodEntity::Update,
            &GoodEntity::OnDestroy
        );
    }
};

TNX_REGISTER_ENTITY(GoodEntity);
```

### ✅ Good Component
```cpp
struct GoodComponent {
    // Plain Old Data only
    float Value1;
    float Value2;
    int32_t Value3;
    bool Value4;

    // No constructors, destructors, or methods
    // Components are pure data
};
```

### ❌ Bad Entity
```cpp
struct BadEntity {
    Ref<Transform> transform;

    // WRONG: Virtual function
    virtual void Update(double dt) { }

    // WRONG: Constructor with logic
    BadEntity() : transform(nullptr) { DoSetup(); }

    // WRONG: Complex member
    std::vector<int> data;
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
