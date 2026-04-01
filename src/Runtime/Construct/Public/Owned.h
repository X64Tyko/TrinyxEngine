#pragma once

#include <type_traits>

// ---------------------------------------------------------------------------
// Owned<T> — Compile-time composition handle for Construct members.
//
// Wraps a child Construct as a value member of a parent Construct.
// The parent's InitializeViews() must call Member.Initialize(GetWorld()).
// Destruction is automatic — C++ reverse-declaration-order guarantees
// children shut down before the parent.
//
// Usage:
//   class Turret : public Construct<Turret>
//   {
//       DefaultView Body;
//       Owned<BarrelAssembly>  Barrel;
//       Owned<TargetingSystem> Targeting;
//
//       void InitializeViews()
//       {
//           Body.Initialize(GetRegistry());
//           Barrel->Initialize(GetWorld());
//           Targeting->Initialize(GetWorld());
//       }
//   };
// ---------------------------------------------------------------------------
template <typename T>
class Owned
{
public:
	Owned()  = default;
	~Owned() = default;

	Owned(const Owned&)            = delete;
	Owned& operator=(const Owned&) = delete;

	T* operator->() { return &Value; }
	const T* operator->() const { return &Value; }

	T& operator*() { return Value; }
	const T& operator*() const { return Value; }

	T* Get() { return &Value; }
	const T* Get() const { return &Value; }

private:
	T Value;
};