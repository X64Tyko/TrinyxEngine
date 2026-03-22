#pragma once

#include <cstdint>

#include "Registry.h"
#include "RegistryTypes.h"
#include "Logger.h"

#define DEFINE_FIXED_MULTICALLBACK_RET(RetVal, FuncName, Size, ...) \
	using FuncName = FixedMultiCallback<RetVal, Size, __VA_ARGS__>;

#define DEFINE_FIXED_MULTICALLBACK(FuncName, Size, ...) \
using FuncName = FixedMultiCallback<void, Size, __VA_ARGS__>;

#define DEFINE_MULTICAST_CALLBACK(FuncName, CLASS, ...) \
	using FuncName = EntityCallback<void, CLASS, __VA_ARGS__>;

#define DEFINE_CALLBACK_RET(RetVal, FuncName, ...) \
using FuncName = Callback<RetVal, __VA_ARGS__>;

#define DEFINE_CALLBACK(FuncName, ...) \
using FuncName = Callback<void, __VA_ARGS__>;


template <typename Ret, typename... Args>
struct Callback
{
	using Fn = Ret(*)(void*, Args...);
	void* bindObj = nullptr;
	Fn stub = nullptr;
	
	FORCE_INLINE Ret operator()(Args... args) const { return stub(bindObj, args...);}
	
	template <typename T, Ret(T::*MemFn)(Args...)>
	void Bind(T* obj)
	{
		bindObj = obj;
		stub = [](void* ptr, Args... args) -> Ret { return (static_cast<T*>(ptr)->*MemFn)(args...); };
	}
	
	void Reset()
	{
		bindObj = nullptr;
		stub = nullptr;
	}
	
	bool IsBound() const { return stub != nullptr && bindObj != nullptr; }
};

template <typename Ret, size_t MaxBinds, typename... Args>
struct FixedMultiCallback
{
	std::array<Callback<Ret, Args...>, MaxBinds> Bindings;

	template <typename T, Ret(T::*MemFn)(Args...)>
	void Bind(T* obj)
	{
		for (auto& CB : Bindings)
		{
			if (!CB.IsBound())
			{
				CB.template Bind<T, MemFn>(obj);
				return;
			}
		}
		
		LOG_ERROR("FixedMultiCallback::Bind - No free slots available for binding");
	}
	
	FORCE_INLINE Ret operator()(Args... args) const
	{
		for (auto& CB : Bindings)
		{
			if (CB.IsBound())
			{
				return CB(args...);
			}
		}
		
		LOG_ERROR("FixedMultiCallback::operator() - No bound callbacks to invoke");
		return Ret();
	}
	
	template <typename T, Ret(T::*MemFn)(Args...)>
	void Unbind(T* obj)
	{
		auto tempStub = [](void* ptr, Args... args) -> Ret { return (static_cast<T*>(ptr)->*MemFn)(args...); };
		for (auto& CB : Bindings)
		{
			if (CB.IsBound() && CB.bindObj == obj && tempStub == CB.stub)
			{
				CB.Reset();
				return;
			}
		}
	}
	
	void Reset()
	{
		for (auto& CB : Bindings)
		{
			CB.Reset();
		}
	}
};