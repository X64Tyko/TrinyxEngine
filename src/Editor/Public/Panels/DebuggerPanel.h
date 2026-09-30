#pragma once
#include "EditorPanel.h"
#include <array>
#include <cstdint>

class DebuggerPanel : public EditorPanel
{
public:
	DebuggerPanel()
		: EditorPanel("Debugger")
	{
	}

	void Draw(EditorState& state) override;

private:
	static constexpr int HistorySize = 128;

	std::array<float, HistorySize> StatCorrectionHistory{};
	std::array<float, HistorySize> EntityDeltaHistory{};
	std::array<float, HistorySize> DirtyEntityHistory{};
	std::array<float, HistorySize> LogicMsHistory{};
	std::array<float, HistorySize> FixedMsHistory{};

	int HistoryOffset        = 0;
	uint32_t LastFrameNumber = 0; // Last FrameNumber we sampled from ReplicationSystem::Stats
};
