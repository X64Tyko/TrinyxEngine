#pragma once

#include "Transform.h"
#include "ColorData.h"
#include "EntityView.h"
#include "Schema.h"
#include "SchemaReflector.h"
#include "FieldProxy.h"

template < template <bool> class T, bool MASK = false>
class BaseCube : public EntityView<T, MASK>
{
    STRIGID_REGISTER_SUPER_SCHEMA(BaseCube, EntityView, transform, velocity, color)

    // We may still want a base class for these, just so it's less easy to use a non-SoA compliant component
    Transform<MASK> transform;
    Velocity<MASK> velocity;
    ColorData<MASK> color;

    // Lifecycle hooks
    __forceinline void PrePhysics([[maybe_unused]] double dt)
    {
        constexpr float TWO_PI = 6.283185307179586f;

        // Now we emulate less than ideal assignment and operations in a fixed update.
        transform.PositionX += static_cast<float>(dt) * velocity.vX;
        transform.PositionX.Select(50.f, transform.PositionX, -50.f);

        velocity.vX *= static_cast<float>(pow(0.98f, dt));
        velocity.vY *= static_cast<float>(pow(0.99f, dt));

        //float random = velocity.vX + velocity.vY; //static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
        transform.RotationY += static_cast<float>(dt) * 0.2f;
        //if (transform.RotationY > TWO_PI)[[unlikely]] transform.RotationY -= TWO_PI;

        //random = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
        transform.RotationZ += static_cast<float>(dt) * 0.4f;
        //if (transform.RotationZ > TWO_PI)[[unlikely]] transform.RotationZ -= TWO_PI;
    }
};

template<bool MASK = false>
class CubeEntity : public BaseCube<CubeEntity, MASK>
{
    STRIGID_REGISTER_SCHEMA(CubeEntity, BaseCube)
};

template<bool MASK = false>
class SuperCube : public BaseCube<SuperCube, MASK>
{
    STRIGID_REGISTER_SCHEMA(SuperCube, BaseCube)
    using Base::transform;
    
public:
    // Logic
    __forceinline void PrePhysics([[maybe_unused]] double dt)
    {
        constexpr float TWO_PI = 6.283185307179586f;

        transform.RotationX += static_cast<float>(dt);
        if (transform.RotationX > TWO_PI)[[unlikely]] transform.RotationX -= TWO_PI;

        transform.RotationY += static_cast<float>(dt) * 0.7f;
        if (transform.RotationY > TWO_PI)[[unlikely]] transform.RotationY -= TWO_PI;

        transform.RotationZ += static_cast<float>(dt) * 0.5f;
        if (transform.RotationZ > TWO_PI)[[unlikely]] transform.RotationZ -= TWO_PI;
    }
};
