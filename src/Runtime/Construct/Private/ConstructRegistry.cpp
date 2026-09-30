#include "ConstructRegistry.h"

ConstructRef ConstructRegistry::WireNetRef(void* ptr, ConstructNetHandle serverHandle,
	ConstructNetManifest manifest, uint32_t typeHash, uint32_t spawnFrame)
{
	const uint32_t netIndex = serverHandle.NetIndex;
	const uint8_t ownerID   = serverHandle.NetOwnerID;

	if (netIndex >= NextNetIndex) NextNetIndex = netIndex + 1; // keep client counter ahead of server's

	ConstructRecord rec;
	rec.NetworkID    = serverHandle;
	rec.ConstructPtr = ptr;
	rec.TypeHash     = typeHash;
	rec.PrefabIDRaw  = 0;
	rec.SpawnFrame   = spawnFrame;
	rec.Generation   = 1;
	rec.OwnerID      = ownerID;

	GlobalConstructHandle gHandle(netIndex, 1, manifest.PrefabIndex);
	Records.set(netIndex, rec);
	NetToRecord.set(netIndex, gHandle);

	ConstructRef ref;
	ref.Handle     = serverHandle;
	ref.Generation = 1;
	return ref;
}

ConstructRef ConstructRegistry::AllocateNetRef(void* ptr, uint8_t ownerID, ConstructNetManifest manifest,
	uint32_t typeHash, int64_t prefabIDRaw, uint32_t spawnFrame)
{
	const uint32_t netIndex = NextNetIndex++;

	ConstructNetHandle netHandle;
	netHandle.NetOwnerID = ownerID;
	netHandle.NetIndex   = netIndex;

	ConstructRecord rec;
	rec.NetworkID    = netHandle;
	rec.ConstructPtr = ptr;
	rec.TypeHash     = typeHash;
	rec.PrefabIDRaw  = prefabIDRaw;
	rec.SpawnFrame   = spawnFrame;
	rec.Generation   = 1;
	rec.OwnerID      = ownerID;

	GlobalConstructHandle gHandle(netIndex, 1, manifest.PrefabIndex);
	Records.set(netIndex, rec);
	NetToRecord.set(netIndex, gHandle);

	ConstructRef ref;
	ref.Handle     = netHandle;
	ref.Generation = 1;
	return ref;
}

void ConstructRegistry::DestroyByNetHandle(ConstructNetHandle handle)
{
	const GlobalConstructHandle& gH = LookupGlobalHandle(handle);
	if (gH.GetIndex() == 0) return;
	const ConstructRecord* rec = Records.try_get_ptr(gH.GetIndex());
	if (!rec || !rec->ConstructPtr) return;
	void* target = rec->ConstructPtr;
	for (auto& bucket : Buckets)
	{
		for (auto& entry : bucket)
		{
			if (entry.Ptr == target)
			{
				Destroy(entry.ID);
				return;
			}
		}
	}
}

void ConstructRegistry::SetNetDestroyHook(void* ptr, ConstructNetHandle handle,
	void (*fn)(void*, ConstructNetHandle), void* ctx)
{
	for (auto& bucket : Buckets)
	{
		for (auto& entry : bucket)
		{
			if (entry.Ptr == ptr)
			{
				entry.NetHandle     = handle;
				entry.NetDestroyFn  = fn;
				entry.NetDestroyCtx = ctx;
				return;
			}
		}
	}
}

void ConstructRegistry::ClearNetDestroyHooks()
{
	for (auto& bucket : Buckets)
	{
		for (auto& entry : bucket)
		{
			entry.NetDestroyFn  = nullptr;
			entry.NetDestroyCtx = nullptr;
		}
	}
}

void ConstructRegistry::ProcessDeferredDestructions()
{
	if (PendingDestructions.empty()) return;

	for (uint32_t id : PendingDestructions)
	{
		for (auto& bucket : Buckets)
		{
			bool found = false;
			for (size_t i = 0; i < bucket.size(); ++i)
			{
				if (bucket[i].ID == id)
				{
					if (bucket[i].NetDestroyFn) bucket[i].NetDestroyFn(bucket[i].NetDestroyCtx, bucket[i].NetHandle);
					if (i != bucket.size() - 1) bucket[i] = std::move(bucket.back());
					bucket.pop_back();
					found = true;
					break;
				}
			}
			if (found) break;
		}
	}
	PendingDestructions.clear();
}

void ConstructRegistry::DestroyByLifetime(ConstructLifetime minSurviving)
{
	for (uint8_t i = 0; i < static_cast<uint8_t>(minSurviving); ++i)
	{
		for (auto& entry : Buckets[i])
			if (entry.NetDestroyFn) entry.NetDestroyFn(entry.NetDestroyCtx, entry.NetHandle);
		Buckets[i].clear();
	}
}

void ConstructRegistry::NotifyWorldTeardown(ConstructLifetime minSurviving)
{
	for (uint8_t i = static_cast<uint8_t>(minSurviving); i < BucketCount; ++i)
	{
		for (auto& entry : Buckets[i])
		{
			if (entry.OnTeardown) entry.OnTeardown(entry.Ptr);
			if (entry.ShutdownPtr) entry.ShutdownPtr(entry.Ptr);
		}
	}
}

void ConstructRegistry::NotifyWorldInitialized(ConstructLifetime minSurviving, WorldBase* newWorld)
{
	for (uint8_t i = static_cast<uint8_t>(minSurviving); i < BucketCount; ++i)
	{
		for (auto& entry : Buckets[i])
		{
			if (entry.ReinitPtr) entry.ReinitPtr(entry.Ptr, newWorld);
			if (entry.OnInitialized) entry.OnInitialized(entry.Ptr, newWorld);
		}
	}
}

void ConstructRegistry::DestroyAll()
{
	for (auto& bucket : Buckets)
	{
		for (auto& entry : bucket)
			if (entry.NetDestroyFn) entry.NetDestroyFn(entry.NetDestroyCtx, entry.NetHandle);
		bucket.clear();
	}
	PendingDestructions.clear();
}

uint32_t ConstructRegistry::GetCount() const
{
	uint32_t total = 0;
	for (const auto& bucket : Buckets)
		total += static_cast<uint32_t>(bucket.size());
	return total;
}

uint32_t ConstructRegistry::GetEarliestSpawnFrame() const
{
	uint32_t earliest = UINT32_MAX;
	for (uint32_t i = 1; i < NextNetIndex; ++i)
	{
		const ConstructRecord* rec = Records.try_get_ptr(i);
		if (rec && rec->IsValid() && rec->SpawnFrame < earliest) earliest = rec->SpawnFrame;
	}
	return earliest;
}

ConstructRecord ConstructRegistry::GetRecord(ConstructNetHandle handle) const
{
	const GlobalConstructHandle& gHandle = LookupGlobalHandle(handle);
	return Records.get(gHandle.GetIndex());
}

ConstructRecord* ConstructRegistry::GetRecordPtr(ConstructNetHandle handle)
{
	if (!IsHandleValid(handle)) return nullptr;
	const GlobalConstructHandle gHandle = LookupGlobalHandle(handle);
	return Records[gHandle.GetIndex()];
}

bool ConstructRegistry::IsHandleValid(ConstructNetHandle handle) const
{
	const GlobalConstructHandle gHandle = LookupGlobalHandle(handle);
	const ConstructRecord* rec          = Records.try_get_ptr(gHandle.GetIndex());
	return rec && gHandle.GetGeneration() == rec->Generation;
}

bool ConstructRegistry::IsHandleValid(const ConstructRef& ref) const
{
	const ConstructRecord* rec = Records.try_get_ptr(LookupGlobalHandle(ref.Handle).GetIndex());
	return rec && ref.Generation == rec->Generation;
}

void ConstructRegistry::AddEntry(uint8_t lifetimeTier, Entry&& entry)
{
	Buckets[lifetimeTier].push_back(std::move(entry));
}
