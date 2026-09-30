#include "InputWindowCodec.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <type_traits>

namespace
{
enum class SectionKind : uint8_t
{
	Persistent, // held state: sent when it differs from the previous frame, otherwise inherited
	PerFrame,   // per-frame value: sent when non-zero, otherwise zero
};

struct Section
{
	uint16_t Offset; // within InputSnapshot
	uint16_t Size;
	SectionKind Kind;
};

// Wire order. A section's flag is its index bit; the bit after the last section flags events.
// ViewYaw/ViewPitch travel together, so they must stay adjacent.
static_assert(offsetof(InputSnapshot, ViewPitch) == offsetof(InputSnapshot, ViewYaw) + sizeof(InputSnapshot::ViewYaw));
constexpr Section kSections[] = {
	{ offsetof(InputSnapshot, KeyState), sizeof(InputSnapshot::KeyState), SectionKind::Persistent },
	{ offsetof(InputSnapshot, MouseDX), sizeof(InputSnapshot::MouseDX), SectionKind::PerFrame },
	{ offsetof(InputSnapshot, MouseDY), sizeof(InputSnapshot::MouseDY), SectionKind::PerFrame },
	{ offsetof(InputSnapshot, MouseButtons), sizeof(InputSnapshot::MouseButtons), SectionKind::Persistent },
	{ offsetof(InputSnapshot, ViewYaw), sizeof(InputSnapshot::ViewYaw) + sizeof(InputSnapshot::ViewPitch), SectionKind::Persistent },
};
constexpr size_t kSectionCount = std::size(kSections);
constexpr uint8_t kHasEvents   = 1u << kSectionCount;
static_assert(kSectionCount < 8, "delta flags are one byte: one bit per section plus one for events");

constexpr size_t kMaxEvents = std::extent_v<decltype(NetInputFrame::Events)>;

constexpr uint8_t kZeroes[sizeof(InputSnapshot)]{};

const uint8_t* Bytes(const InputSnapshot& s)
{
	return reinterpret_cast<const uint8_t*>(&s);
}
uint8_t* Bytes(InputSnapshot& s)
{
	return reinterpret_cast<uint8_t*>(&s);
}

uint8_t* EncodeDelta(const NetInputFrame& prev, const NetInputFrame& cur, uint8_t* out)
{
	std::memcpy(out, &cur.Frame, sizeof(cur.Frame));
	out += sizeof(cur.Frame);
	uint8_t& flags = *out++;
	flags          = 0;

	for (size_t i = 0; i < kSectionCount; ++i)
	{
		const Section& s         = kSections[i];
		const uint8_t* value     = Bytes(cur.State) + s.Offset;
		const uint8_t* reference = s.Kind == SectionKind::Persistent ? Bytes(prev.State) + s.Offset : kZeroes;
		if (std::memcmp(value, reference, s.Size) == 0) continue;

		flags |= static_cast<uint8_t>(1u << i);
		std::memcpy(out, value, s.Size);
		out += s.Size;
	}

	if (cur.EventCount > 0)
	{
		flags |= kHasEvents;
		const uint8_t count = static_cast<uint8_t>(std::min<size_t>(cur.EventCount, kMaxEvents));
		*out++              = count;
		std::memcpy(out, cur.Events, count * sizeof(NetInputEvent));
		out += count * sizeof(NetInputEvent);
	}
	return out;
}

/// Returns the advanced read pointer, or nullptr if the payload is truncated.
const uint8_t* DecodeDelta(const NetInputFrame& prev, NetInputFrame& cur, const uint8_t* p, const uint8_t* end)
{
	if (static_cast<size_t>(end - p) < sizeof(cur.Frame) + 1) return nullptr;

	cur.State      = prev.State;
	cur.EventCount = 0;
	for (const Section& s : kSections)
		if (s.Kind == SectionKind::PerFrame) std::memset(Bytes(cur.State) + s.Offset, 0, s.Size);

	std::memcpy(&cur.Frame, p, sizeof(cur.Frame));
	p += sizeof(cur.Frame);
	const uint8_t flags = *p++;

	for (size_t i = 0; i < kSectionCount; ++i)
	{
		if (!(flags & (1u << i))) continue;
		const Section& s = kSections[i];
		if (static_cast<size_t>(end - p) < s.Size) return nullptr;
		std::memcpy(Bytes(cur.State) + s.Offset, p, s.Size);
		p += s.Size;
	}

	if (flags & kHasEvents)
	{
		if (end - p < 1) return nullptr;
		cur.EventCount          = static_cast<uint8_t>(std::min<size_t>(*p++, kMaxEvents));
		const size_t eventBytes = cur.EventCount * sizeof(NetInputEvent);
		if (static_cast<size_t>(end - p) < eventBytes) return nullptr;
		std::memcpy(cur.Events, p, eventBytes);
		p += eventBytes;
	}
	return p;
}
} // namespace

InputWindowWriter::InputWindowWriter(uint8_t* buffer, const NetInputFrame& base)
	: Begin(buffer),
	  Cursor(buffer + sizeof(InputDeltaPacketHeader)),
	  Prev(base)
{
	const InputDeltaPacketHeader header{ base.Frame, 1 };
	std::memcpy(Begin, &header, sizeof(header));
	std::memcpy(Cursor, &base, sizeof(NetInputFrame));
	Cursor += sizeof(NetInputFrame);
}

void InputWindowWriter::Append(const NetInputFrame& frame)
{
	Cursor = EncodeDelta(Prev, frame, Cursor);
	Prev   = frame;
	++FrameCount;
}

size_t InputWindowWriter::Finish()
{
	std::memcpy(Begin + offsetof(InputDeltaPacketHeader, FrameCount), &FrameCount, sizeof(FrameCount));
	return static_cast<size_t>(Cursor - Begin);
}

bool InputWindowCodec::Decode(const uint8_t* data, size_t size, InputWindowPacket& out)
{
	if (size < sizeof(InputDeltaPacketHeader) + sizeof(NetInputFrame)) return false;
	const uint8_t* p   = data;
	const uint8_t* end = data + size;

	InputDeltaPacketHeader header;
	std::memcpy(&header, p, sizeof(header));
	p += sizeof(header);
	if (header.FrameCount == 0 || header.FrameCount > MaxWindowFrames) return false;

	out.FirstFrame = header.FirstFrame;
	out.FrameCount = header.FrameCount;

	std::memcpy(&out.Frames[0], p, sizeof(NetInputFrame));
	p += sizeof(NetInputFrame);
	out.Frames[0].EventCount = static_cast<uint8_t>(std::min<size_t>(out.Frames[0].EventCount, kMaxEvents));

	for (uint32_t i = 1; i < header.FrameCount; ++i)
	{
		p = DecodeDelta(out.Frames[i - 1], out.Frames[i], p, end);
		if (!p) return false;
	}
	return true;
}
