#include "StreamingManager.h"
#include "Logger.h"

StreamingManager::Request* StreamingManager::Find(StreamingRequestID id)
{
	if (id == InvalidStreamingRequest) return nullptr;
	for (Request& r : Pool)
		if (r.bActive && r.ID == id) return &r;
	return nullptr;
}

StreamingManager::Request* StreamingManager::Alloc(StreamingRequestID id, StreamingOnComplete onComplete)
{
	for (Request& r : Pool)
	{
		if (r.bActive) continue;
		r.ID         = id;
		r.bActive    = true;
		r.OnComplete = onComplete;
		r.Counter.Value.store(1, std::memory_order_relaxed); // gate ref
		r.PathBuf[0] = '\0';
		return &r;
	}
	return nullptr;
}

void StreamingManager::Release(Request& r)
{
	r.bActive = false;
	r.ID      = InvalidStreamingRequest;
	r.OnComplete.Reset();
}

StreamingRequestID StreamingManager::BeginRequest(StreamingOnComplete onComplete, uint8_t ownerTag)
{
	// Retry until the 24-bit Seq field is non-zero (wraps every ~16M requests).
	// ownerTag is packed into the low NetOwnerID_Bits bits for PIE routing.
	StreamingRequestID id{};
	while (!id.IsValid())
	{
		id.Seq     = NextID.fetch_add(1, std::memory_order_relaxed);
		id.OwnerID = ownerTag;
	}

	if (!Alloc(id, onComplete))
	{
		LOG_ENG_ERROR("[StreamingManager] Pool exhausted — increase MaxRequests");
		return InvalidStreamingRequest;
	}
	return id;
}

const char* StreamingManager::CopyPath(StreamingRequestID id, const char* path)
{
	Request* r = Find(id);
	if (!r || !path) return nullptr;
	std::strncpy(r->PathBuf, path, MaxPathLength - 1);
	r->PathBuf[MaxPathLength - 1] = '\0';
	return r->PathBuf;
}

void StreamingManager::SubmitRequest(StreamingRequestID id)
{
	Request* r = Find(id);
	if (!r) return;
	r->Counter.Value.fetch_sub(1, std::memory_order_acq_rel);
}

void StreamingManager::Cancel(StreamingRequestID id)
{
	Request* r = Find(id);
	if (!r) return;
	// Mark inactive now; if jobs are in-flight the counter still drains.
	// Tick will not fire the callback since bActive will be false on drain.
	Release(*r);
}

void StreamingManager::Tick()
{
	for (Request& r : Pool)
	{
		if (!r.bActive) continue;
		if (r.Counter.Value.load(std::memory_order_acquire) != 0) continue;

		// Capture before Release clears the slot.
		const StreamingRequestID id  = r.ID;
		const StreamingOnComplete cb = r.OnComplete;
		Release(r);
		if (cb.IsBound()) cb(id);
	}
}

bool StreamingManager::IsComplete(StreamingRequestID id) const
{
	for (const Request& r : Pool)
		if (r.bActive && r.ID == id)
			return r.Counter.Value.load(std::memory_order_acquire) == 0;
	return true; // not found = completed or cancelled
}
