#pragma once
#include <cstdint>

enum class BusID : uint8_t
{
	Master = 0
};

struct PlayParams
{
	float Volume     = 1.f;
	float Pitch      = 1.f; // > 1 = faster/higher
	bool Loop        = false;
	BusID Bus        = BusID::Master;
	uint8_t Priority = 128; // 0 = lowest; voice stealing picks smallest value
};
