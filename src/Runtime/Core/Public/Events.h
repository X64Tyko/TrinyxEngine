#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

#include "Types.h"
#include "Logger.h"

#define DEFINE_FIXED_MULTICALLBACK(FuncName, Size, ...) \
using FuncName = MultiCallback<void, true, Size, __VA_ARGS__>;

#define DEFINE_MULTICAST_CALLBACK(FuncName, ...) \
using FuncName = MultiCallback<void, false, 16, __VA_ARGS__>;

#define DEFINE_CALLBACK_RET(RetVal, FuncName, ...) \
using FuncName = Callback<RetVal, __VA_ARGS__>;

#define DEFINE_CALLBACK(FuncName, ...) \
using FuncName = Callback<void, __VA_ARGS__>;


template <typename Ret, typename... Args>
struct Callback
{
	using Fn      = Ret(*)(void*, Args...);
	void* bindObj = nullptr;
	Fn stub       = nullptr;

	FORCE_INLINE Ret operator()(Args... args) const { return stub(bindObj, args...); }

	template <typename T, Ret(T::*MemFn)(Args...)>
	void Bind(T* obj)
	{
		bindObj = obj;
		stub    = [](void* ptr, Args... args) -> Ret { return (static_cast<T*>(ptr)->*MemFn)(args...); };
	}

	// Bind a free function (or a capturing-context thunk) with an optional context pointer.
	// The function receives bindObj as its first argument, matching the internal Fn signature.
	void BindStatic(Fn fn, void* ctx = nullptr)
	{
		bindObj = ctx;
		stub    = fn;
	}

	void Reset()
	{
		bindObj = nullptr;
		stub    = nullptr;
	}

	bool IsBound() const { return stub != nullptr; }
};

// Storage selector: std::array for Fixed (trivially copyable), std::vector for dynamic.
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
	bool empty() const { return false; } // always full (slots may be unbound)
};

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

template <typename Ret, bool Fixed, size_t MaxBinds = 16, typename... Args>
struct MultiCallback
{
	using CB = Callback<Ret, Args...>;
	MultiCallbackStorage<CB, Fixed, MaxBinds> Bindings{};

	template <typename T, Ret(T::*MemFn)(Args...)>
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

	FORCE_INLINE void operator()(Args... args) const requires std::is_void_v<Ret>
	{
		for (auto& cb : Bindings)
		{
			if (cb.IsBound()) cb(args...);
		}
	}

	template <typename T, Ret(T::*MemFn)(Args...)>
	void Unbind(T* obj)
	{
		auto tempStub = [](void* ptr, Args... args) -> Ret { return (static_cast<T*>(ptr)->*MemFn)(args...); };
		for (size_t i = 0; i < Bindings.size(); ++i)
		{
			auto& cb = Bindings.Data[i];
			if (cb.IsBound() && cb.bindObj == obj && tempStub == cb.stub)
			{
				if constexpr (Fixed) cb.Reset();
				else
				{
					std::swap(cb, Bindings.Data.back());
					Bindings.Data.pop_back();
				}
				return;
			}
		}
	}

	void UnbindStatic(typename CB::Fn fn, void* ctx = nullptr)
	{
		for (size_t i = 0; i < Bindings.size(); ++i)
		{
			auto& cb = Bindings.Data[i];
			if (cb.IsBound() && cb.stub == fn && cb.bindObj == ctx)
			{
				if constexpr (Fixed) cb.Reset();
				else
				{
					std::swap(cb, Bindings.Data.back());
					Bindings.Data.pop_back();
				}
				return;
			}
		}
	}

	// Unbind all bindings whose context object matches — used by Checkin
	// so callers don't need to know whether they used Bind or BindStatic.
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
			else ++i;
		}
	}

	void Reset()
	{
		if constexpr (Fixed)
		{
			for (auto& cb : Bindings) cb.Reset();
		}
		else
		{
			Bindings.Data.clear();
		}
	}

	bool IsBound() const
	{
		if constexpr (Fixed)
		{
			for (auto& cb : Bindings) { if (cb.IsBound()) return true; }
			return false;
		}
		else return !Bindings.empty();
	}
};
