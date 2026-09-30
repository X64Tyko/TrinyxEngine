#pragma once

#include <cstdint>

#include "Events.h"
#include "RegistryTypes.h"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4201) // nameless struct in union
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

/**
 * @brief Handle to an active streaming batch.
 *
 * Bit layout (32 bits):
 *   [NetOwnerID_Bits-1 .. 0] = OwnerID — PIE routing tag; 0 = server/solo (no routing).
 *   [31 .. NetOwnerID_Bits]  = Seq     — monotonic counter; 0 is reserved (invalid).
 *
 * Value == 0 is the invalid sentinel (InvalidStreamingRequest).
 * Pack OwnerID via BeginRequest(onComplete, ownerTag) so PIENetThread can route the
 * completion callback to the right handler without separate state.
 */
union StreamingRequestID
{
	uint32_t Value;

	struct
	{
		uint32_t OwnerID : NetOwnerID_Bits;
		uint32_t Seq : 32 - NetOwnerID_Bits;
	};

	uint8_t GetOwnerID() const { return static_cast<uint8_t>(OwnerID); }
	uint32_t GetSeq() const { return Seq; }
	bool IsValid() const { return Value != 0; }

	bool operator==(StreamingRequestID o) const { return Value == o.Value; }
	bool operator!=(StreamingRequestID o) const { return Value != o.Value; }
};

static_assert(sizeof(StreamingRequestID) == 4, "StreamingRequestID must be 4 bytes");

static constexpr StreamingRequestID InvalidStreamingRequest{};

/// @brief Callback fired on the Sentinel tick when all jobs in a batch complete.
DEFINE_CALLBACK(StreamingOnComplete, StreamingRequestID);

#ifdef _MSC_VER
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
