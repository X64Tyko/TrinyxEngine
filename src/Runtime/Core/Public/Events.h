#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

#include "Types.h"
#include "Logger.h"

/** @addtogroup core
 *  @{
 */

/// @brief Declares a fixed-capacity MultiCallback type with void return.
/// @param FuncName  Name of the resulting type alias.
/// @param Size      Maximum number of simultaneous bindings (compile-time, stored in-place).
#define DEFINE_FIXED_MULTICALLBACK(FuncName, Size, ...) \
	using FuncName = MultiCallback<void, true, Size, __VA_ARGS__>;

/// @brief Declares a dynamically-growing MultiCallback type with void return and default capacity 16.
/// @param FuncName  Name of the resulting type alias.
#define DEFINE_MULTICAST_CALLBACK(FuncName, ...) \
	using FuncName = MultiCallback<void, false, 16, __VA_ARGS__>;

/// @brief Declares a single-binding Callback type with a custom return type.
/// @param RetVal    Return type of the callback.
/// @param FuncName  Name of the resulting type alias.
#define DEFINE_CALLBACK_RET(RetVal, FuncName, ...) \
	using FuncName = Callback<RetVal, __VA_ARGS__>;

/// @brief Declares a single-binding Callback type with void return.
/// @param FuncName  Name of the resulting type alias.
#define DEFINE_CALLBACK(FuncName, ...) \
	using FuncName = Callback<void, __VA_ARGS__>;

#define DEFINE_CALLBACK_VOID(FuncName) \
	using FuncName = Callback<void>;


/**
 * @brief Type-erased single-binding delegate — stores one member function or free function.
 *
 * Internally holds a context pointer (@c bindObj) and a stateless thunk (@c stub) that
 * performs the cast and dispatch. No heap allocation; trivially copyable.
 *
 * @tparam Ret  Return type.
 * @tparam Args Argument types.
 */
template <typename Ret, typename... Args>
struct Callback
{
	using Fn      = Ret (*)(void*, Args...); ///< Thunk signature: context pointer followed by call args.
	void* bindObj = nullptr;                 ///< Context pointer passed as first argument to the thunk.
	Fn stub       = nullptr;                 ///< Stateless dispatch thunk; null when unbound.

	/// @brief Invoke the bound function. Undefined behaviour if @c IsBound() is false.
	FORCE_INLINE Ret operator()(Args... args) const { return stub(bindObj, args...); }

	/// @brief Bind a member function. Stores the object pointer and synthesizes a thunk.
	/// @tparam T     Object type that owns @p MemFn.
	/// @tparam MemFn Pointer-to-member to dispatch to.
	template <typename T, Ret (T::*MemFn)(Args...)>
	void Bind(T* obj)
	{
		bindObj = obj;
		stub    = [](void* ptr, Args... args) -> Ret
		{
			return (static_cast<T*>(ptr)->*MemFn)(args...);
		};
	}

	/// @brief Bind a free function or capturing-context thunk.
	/// @note  @p fn receives @p ctx as its first argument, matching the internal @c Fn signature.
	void BindStatic(Fn fn, void* ctx = nullptr)
	{
		bindObj = ctx;
		stub    = fn;
	}

	/// @brief Clear the binding; @c IsBound() returns false afterward.
	void Reset()
	{
		bindObj = nullptr;
		stub    = nullptr;
	}

	/// @brief Returns true if a function is currently bound.
	bool IsBound() const { return stub != nullptr; }
};

/// @brief Fixed-capacity storage backend for MultiCallback — `MaxBinds` slots in a `std::array`.
/// @tparam CB       Callback element type.
/// @tparam MaxBinds Number of pre-allocated slots.
template <typename CB, bool Fixed, size_t MaxBinds>
struct MultiCallbackStorage;

template <typename CB, size_t MaxBinds>
struct MultiCallbackStorage<CB, true, MaxBinds>
{
	std::array<CB, MaxBinds> Data{};
	static constexpr size_t size() { return MaxBinds; }
	CB* begin() { return Data.data(); }
	CB* end() { return Data.data() + MaxBinds; }
	const CB* begin() const { return Data.data(); }
	const CB* end() const { return Data.data() + MaxBinds; }
	bool empty() const { return false; } // slots may be unbound, but the array is always full
};

/// @brief Dynamic-capacity storage backend for MultiCallback — bindings live in a `std::vector`.
/// @tparam CB       Callback element type.
/// @tparam MaxBinds Growth increment per resize.
template <typename CB, size_t MaxBinds>
struct MultiCallbackStorage<CB, false, MaxBinds>
{
	std::vector<CB> Data{};
	size_t size() const { return Data.size(); }
	auto begin() { return Data.begin(); }
	auto end() { return Data.end(); }
	auto begin() const { return Data.begin(); }
	auto end() const { return Data.end(); }
	bool empty() const { return Data.empty(); }
};

/**
 * @brief Multi-binding delegate — dispatches to @c MaxBinds simultaneous listeners.
 *
 * Two storage strategies selected at compile time:
 * - @c Fixed=true  — bindings stored in a `std::array`; trivially copyable, no heap.
 * - @c Fixed=false — bindings stored in a `std::vector`; grows dynamically.
 *
 * @tparam Ret      Return type (typically @c void for multicast use).
 * @tparam Fixed    @c true for fixed-size in-place storage, @c false for dynamic.
 * @tparam MaxBinds Capacity (Fixed) or growth increment (dynamic).
 * @tparam Args     Argument types forwarded to all bindings on invocation.
 */
template <typename Ret, bool Fixed, size_t MaxBinds = 16, typename... Args>
struct MultiCallback
{
	using CB = Callback<Ret, Args...>;
	MultiCallbackStorage<CB, Fixed, MaxBinds> Bindings{};

	/// @brief Bind a member function to the next available slot.
	/// @note Asserts (via LOG_ENG_ERROR) if all slots are occupied.
	template <typename T, Ret (T::*MemFn)(Args...)>
	void Bind(T* obj)
	{
		if constexpr (Fixed)
		{
			for (auto& cb : Bindings)
			{
				if (!cb.IsBound())
				{
					cb.template Bind<T, MemFn>(obj);
					return;
				}
			}
		}
		else
		{
			size_t oldSize = Bindings.Data.size();
			Bindings.Data.resize(oldSize + MaxBinds);
			Bindings.Data[oldSize].template Bind<T, MemFn>(obj);
			return;
		}

		LOG_ENG_ERROR("MultiCallback::Bind - No free slots available for binding");
	}

	/// @brief Bind a free function or thunk to the next available slot.
	/// @note Asserts (via LOG_ENG_ERROR) if all slots are occupied.
	void BindStatic(typename CB::Fn fn, void* ctx = nullptr)
	{
		if constexpr (Fixed)
		{
			for (auto& cb : Bindings)
			{
				if (!cb.IsBound())
				{
					cb.BindStatic(fn, ctx);
					return;
				}
			}
		}
		else
		{
			size_t oldSize = Bindings.Data.size();
			Bindings.Data.resize(oldSize + MaxBinds);
			Bindings.Data[oldSize].BindStatic(fn, ctx);
			return;
		}

		LOG_ENG_ERROR("MultiCallback::BindStatic - No free slots available for binding");
	}

	/// @brief Invoke all bound listeners in registration order.
	FORCE_INLINE void operator()(Args... args) const
		requires std::is_void_v<Ret>
	{
		for (auto& cb : Bindings)
		{
			if (cb.IsBound()) cb(args...);
		}
	}

	/// @brief Remove the first binding that matches @p obj and @p MemFn.
	template <typename T, Ret (T::*MemFn)(Args...)>
	void Unbind(T* obj)
	{
		auto tempStub = [](void* ptr, Args... args) -> Ret
		{
			return (static_cast<T*>(ptr)->*MemFn)(args...);
		};
		for (size_t i = 0; i < Bindings.size(); ++i)
		{
			auto& cb = Bindings.Data[i];
			if (cb.IsBound() && cb.bindObj == obj && tempStub == cb.stub)
			{
				if constexpr (Fixed)
					cb.Reset();
				else
				{
					std::swap(cb, Bindings.Data.back());
					Bindings.Data.pop_back();
				}
				return;
			}
		}
	}

	/// @brief Remove the first static binding that matches @p fn and @p ctx.
	void UnbindStatic(typename CB::Fn fn, void* ctx = nullptr)
	{
		for (size_t i = 0; i < Bindings.size(); ++i)
		{
			auto& cb = Bindings.Data[i];
			if (cb.IsBound() && cb.stub == fn && cb.bindObj == ctx)
			{
				if constexpr (Fixed)
					cb.Reset();
				else
				{
					std::swap(cb, Bindings.Data.back());
					Bindings.Data.pop_back();
				}
				return;
			}
		}
	}

	/// @brief Remove all bindings whose context pointer equals @p ctx.
	/// @note Used by Checkin so callers need not track whether they used Bind or BindStatic.
	void UnbindByContext(void* ctx)
	{
		for (size_t i = 0; i < Bindings.size();)
		{
			auto& cb = Bindings.Data[i];
			if (cb.IsBound() && cb.bindObj == ctx)
			{
				if constexpr (Fixed)
				{
					cb.Reset();
					++i;
				}
				else
				{
					std::swap(cb, Bindings.Data.back());
					Bindings.Data.pop_back();
				}
			}
			else
				++i;
		}
	}

	/// @brief Clear all bindings.
	void Reset()
	{
		if constexpr (Fixed)
		{
			for (auto& cb : Bindings)
				cb.Reset();
		}
		else
		{
			Bindings.Data.clear();
		}
	}

	/// @brief Returns true if at least one slot is currently bound.
	bool IsBound() const
	{
		if constexpr (Fixed)
		{
			for (auto& cb : Bindings)
			{
				if (cb.IsBound()) return true;
			}
			return false;
		}
		else
			return !Bindings.empty();
	}
};

/** @} */
