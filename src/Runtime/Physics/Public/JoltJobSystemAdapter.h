#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/FixedSizeFreeList.h>

#include "TrinyxJobs.h"

JPH_SUPPRESS_WARNINGS

// Bridges Jolt's job system onto TrinyxJobs::Physics queue.
//
// Jolt creates Jobs internally during PhysicsSystem::Update(). This adapter
// allocates them from a FixedSizeFreeList and dispatches each ready job to
// the engine's lock-free Physics queue. Workers (and Brain via work-stealing)
// execute the jobs. Barriers are handled by JobSystemWithBarrier.
//
// Thread safety: QueueJob/QueueJobs are called from any thread (Jolt internal).
// Job::Execute() uses a CAS, so double-dispatch from both TrinyxJobs workers
// and the barrier's inline execution path is safe — only one thread wins.
class JoltJobSystemAdapter final : public JPH::JobSystemWithBarrier
{
public:
	JoltJobSystemAdapter(JPH::uint inMaxJobs, JPH::uint inMaxBarriers)
		: JobSystemWithBarrier(inMaxBarriers)
	{
		mJobs.Init(inMaxJobs, inMaxJobs);
	}

	~JoltJobSystemAdapter() override = default;

	int GetMaxConcurrency() const override
	{
		// Workers + Brain (coordinator acts as worker during WaitForJobs)
		return static_cast<int>(TrinyxJobs::GetWorkerCount()) + 1;
	}

	JobHandle CreateJob(const char* inName, JPH::ColorArg inColor,
						const JobFunction& inJobFunction,
						JPH::uint32 inNumDependencies = 0) override
	{
		// Allocate from free list. If full, Jolt asserts internally.
		uint32_t idx = mJobs.ConstructObject(inName, inColor, this, inJobFunction, inNumDependencies);
		Job* job     = &mJobs.Get(idx);

		// If no dependencies, the job can run immediately
		JobHandle handle(job);
		if (inNumDependencies == 0) QueueJob(job);
		return handle;
	}

protected:
	void QueueJob(Job* inJob) override
	{
		// Take a reference so the job stays alive until our lambda completes.
		inJob->AddRef();

		// Dispatch to TrinyxJobs Physics queue. The lambda captures only the
		// raw pointer (8 bytes), well within the 48-byte payload limit.
		TrinyxJobs::JobCounter* noCounter = nullptr;
		(void)noCounter; // We don't use TrinyxJobs counters for Jolt jobs —
		// Jolt tracks completion via its own barrier.

		// We can't use Dispatch() with a counter because Jolt manages its own
		// completion tracking. Submit directly via a one-off counter we ignore.
		// Actually, we need a counter for SubmitJob. Use a thread-local dummy.
		static thread_local TrinyxJobs::JobCounter sDummyCounter;

		TrinyxJobs::Dispatch(
			[inJob](uint32_t)
			{
				inJob->Execute();
				inJob->Release();
			},
			&sDummyCounter, TrinyxJobs::Queue::Physics);
	}

	void QueueJobs(Job** inJobs, JPH::uint inNumJobs) override
	{
		for (JPH::uint i = 0; i < inNumJobs; ++i) QueueJob(inJobs[i]);
	}

	void FreeJob(Job* inJob) override
	{
		mJobs.DestructObject(inJob);
	}

private:
	JPH::FixedSizeFreeList<Job> mJobs;
};