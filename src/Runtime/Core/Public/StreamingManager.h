#pragma once

#include <atomic>

#include "StreamingTypes.h"
#include "TrinyxJobs.h"

/**
 * @brief Async job-batch manager for level loads, chunk activations, and future GPU asset transfers.
 *
 * Batch lifecycle:
 *   1. BeginRequest()  — opens a batch; internal counter initialised to 1 (gate ref).
 *   2. AddJob() × N   — each call increments the counter and dispatches to TrinyxJobs.
 *   3. SubmitRequest() — releases the gate ref; callback fires on Tick() when counter hits 0.
 *
 * All callbacks are fired from Tick(), which TrinyxEngine calls on the Sentinel thread.
 * CopyPath() provides stable path storage inside the request (valid until the batch is released).
 *
 * TODO: GPU push/pull helpers (mesh, material, shader, texture uploads / downloads).
 */
class StreamingManager
{
public:
	// --- Batch API ---

	/// Open a new batch. Counter is held open at 1 until SubmitRequest.
	/// Pass ownerTag = GetLocalOwnerID() for OwnerSim worlds so PIENetThread can route
	/// the completion callback to the correct child handler.
	StreamingRequestID BeginRequest(StreamingOnComplete onComplete, uint8_t ownerTag = 0);

	/// Dispatch a job into the batch. Counter incremented once per call.
	/// All dispatched jobs must finish before the callback fires.
	template <TrinyxJobs::ValidJobLambda LAMBDA>
	bool AddJob(StreamingRequestID id, LAMBDA lambda,
		TrinyxJobs::Queue queue = TrinyxJobs::Queue::General);

	/// Close the batch — releases the gate ref. Callback fires on the next Tick() once counter is 0.
	void SubmitRequest(StreamingRequestID id);

	/// Cancel an open batch without firing the callback.
	/// NOTE: if jobs are already in-flight the counter still drains; the slot is recycled
	/// once it reaches 0 on the next Tick(). Do not cancel after SubmitRequest.
	void Cancel(StreamingRequestID id);

	/// Returns true once all jobs for this request have completed (or the request is unknown).
	bool IsComplete(StreamingRequestID id) const;

	/// Copy path into stable internal storage for use in job lambda captures.
	/// Call before AddJob. Pointer is valid until the request is released.
	const char* CopyPath(StreamingRequestID id, const char* path);

	// --- Engine tick ---

	/// Drain completed batches and fire their callbacks. Call from the Sentinel tick.
	void Tick();

private:
	static constexpr uint32_t MaxRequests   = 32;
	static constexpr uint32_t MaxPathLength = 512;

	struct Request
	{
		TrinyxJobs::JobCounter Counter{};
		StreamingOnComplete OnComplete{};
		StreamingRequestID ID = InvalidStreamingRequest;
		bool bActive          = false;
		char PathBuf[MaxPathLength]{};
	};

	std::atomic<uint32_t> NextID{ 1 };
	Request Pool[MaxRequests]{};

	Request* Find(StreamingRequestID id);
	Request* Alloc(StreamingRequestID id, StreamingOnComplete onComplete);
	void Release(Request& r);
};

// ---------------------------------------------------------------------------
// AddJob — inline template so the lambda trampoline is visible in all TUs
// ---------------------------------------------------------------------------

template <TrinyxJobs::ValidJobLambda LAMBDA>
bool StreamingManager::AddJob(StreamingRequestID id, LAMBDA lambda, TrinyxJobs::Queue queue)
{
	Request* r = Find(id);
	if (!r)
	{
		LOG_ENG_WARN_F("[StreamingManager] AddJob: unknown request %u", id.Value);
		return false;
	}
	TrinyxJobs::Dispatch(lambda, &r->Counter, queue);
	return true;
}
