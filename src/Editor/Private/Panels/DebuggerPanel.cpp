#include "Panels/DebuggerPanel.h"
#include "EditorState.h"
#include "EngineConfig.h"
#include "LogicThreadBase.h"
#include "ReplicationSystem.h"
#include "TnxStyle.h"
#include "TnxWidgets.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>

void DebuggerPanel::Draw(EditorState& state)
{
	if (!BeginPadded())
	{
		ImGui::End();
		return;
	}
	TnxWidgets::PanelHeader(nullptr, "Debugger");

	// --- Sample net stats (only on new frames to avoid duplicate ring entries) ---
	float corrBytes         = 0.f;
	float deltaBytes        = 0.f;
	float dirtyEnts         = 0.f;
	float logicMs           = 0.f;
	float fixedMs           = 0.f;
	uint32_t activeChannels = 0;
	uint32_t dirty          = 0;

	const ReplicationSystem* rep = state.ReplicatorPtr;
	if (rep)
	{
		const auto& s        = rep->GetStats();
		const uint32_t frame = s.FrameNumber.load(std::memory_order_acquire);
		if (frame != LastFrameNumber)
		{
			LastFrameNumber = frame;
			corrBytes       = static_cast<float>(s.StateCorrectionBytes.load(std::memory_order_relaxed));
			deltaBytes      = static_cast<float>(s.EntityDeltaBytes.load(std::memory_order_relaxed));
			dirty           = s.DirtyEntityCount;
			dirtyEnts       = static_cast<float>(dirty);
			activeChannels  = s.ActiveChannelCount;

			// Push new samples into ring buffer only when the dispatch frame advances.
			if (state.LogicPtr)
			{
				logicMs = state.LogicPtr->GetLogicFrameMs();
				fixedMs = state.LogicPtr->GetFixedFrameMs();
			}
			StatCorrectionHistory[HistoryOffset] = corrBytes;
			EntityDeltaHistory[HistoryOffset]    = deltaBytes;
			DirtyEntityHistory[HistoryOffset]    = dirtyEnts;
			LogicMsHistory[HistoryOffset]        = logicMs;
			FixedMsHistory[HistoryOffset]        = fixedMs;
			HistoryOffset                        = (HistoryOffset + 1) % HistorySize;
		}
		else
		{
			// No new dispatch — read cached last values from the history ring for display.
			const int prev = (HistoryOffset - 1 + HistorySize) % HistorySize;
			corrBytes      = StatCorrectionHistory[prev];
			deltaBytes     = EntityDeltaHistory[prev];
			dirtyEnts      = DirtyEntityHistory[prev];
			logicMs        = LogicMsHistory[prev];
			fixedMs        = FixedMsHistory[prev];
			dirty          = static_cast<uint32_t>(dirtyEnts);
		}
	}
	else
	{
		// PIE not active — read frozen last-session values without advancing the ring.
		const int prev = (HistoryOffset - 1 + HistorySize) % HistorySize;
		corrBytes      = StatCorrectionHistory[prev];
		deltaBytes     = EntityDeltaHistory[prev];
		dirtyEnts      = DirtyEntityHistory[prev];
		dirty          = static_cast<uint32_t>(dirtyEnts);
		logicMs        = LogicMsHistory[prev];
		fixedMs        = FixedMsHistory[prev];
	}

	if (ImGui::BeginTabBar("DebuggerTabs"))
	{
		// -----------------------------------------------------------------------
		// Network tab
		// -----------------------------------------------------------------------
		if (ImGui::BeginTabItem("Network"))
		{
			if (!state.ReplicatorPtr)
				ImGui::TextDisabled("no active session — last capture");
			else
			{
				ImGui::Text("Active Channels: %u", activeChannels);
				ImGui::Text("Dirty Entities:  %u", dirty);
			}
			ImGui::Separator();

			ImGui::Text("StateCorrection  bytes/dispatch: %.0f", corrBytes);
			ImGui::Text("EntityDelta      bytes/dispatch: %.0f", deltaBytes);

			if (corrBytes > 0.f)
			{
				const float ratio = deltaBytes / corrBytes;
				ImGui::Text("Ratio (delta/full):  %.2f  (%.0f%%)", ratio, ratio * 100.f);
			}
			ImGui::Separator();

			const float maxBytes = std::max({ corrBytes, deltaBytes, 1.f });

			char corrLabel[40], deltaLabel[40];
			snprintf(corrLabel, sizeof(corrLabel), "Corr %.0f B", corrBytes);
			snprintf(deltaLabel, sizeof(deltaLabel), "Delta %.0f B", deltaBytes);

			ImGui::TextUnformatted("StateCorrection history");
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, TnxStyle::Color::Warn);
			ImGui::PlotHistogram("##corr", StatCorrectionHistory.data(), HistorySize, HistoryOffset,
				corrLabel, 0.f, maxBytes * 1.5f, ImVec2(-1.f, 55.f));
			ImGui::PopStyleColor();

			ImGui::TextUnformatted("EntityDelta history");
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, TnxStyle::Color::Good);
			ImGui::PlotHistogram("##delta", EntityDeltaHistory.data(), HistorySize, HistoryOffset,
				deltaLabel, 0.f, maxBytes * 1.5f, ImVec2(-1.f, 55.f));
			ImGui::PopStyleColor();

			ImGui::Separator();
			ImGui::Text("Dirty entity count history");
			ImGui::PlotLines("##dirtyents", DirtyEntityHistory.data(), HistorySize, HistoryOffset,
				nullptr, 0.f, 256.f, ImVec2(-1.f, 40.f));

			ImGui::EndTabItem();
		}

		// -----------------------------------------------------------------------
		// Profiler tab
		// -----------------------------------------------------------------------
		if (ImGui::BeginTabItem("Profiler"))
		{
			const float fixedHz  = state.ConfigPtr
									   ? static_cast<float>(state.ConfigPtr->FixedUpdateHz)
									   : 512.f;
			const float budgetMs = 1000.f / fixedHz;

			char hzLabel[32];
			snprintf(hzLabel, sizeof(hzLabel), "%.0f Hz", fixedHz);
			TnxWidgets::FrameBudgetBar("Brain", hzLabel, fixedMs, budgetMs,
				TnxStyle::Color::ThBrain,
				"Fixed-step logic thread");
			ImGui::Spacing();

			char logicLabel[40], fixedLabel[40];
			snprintf(logicLabel, sizeof(logicLabel), "Logic %.2f ms", logicMs);
			snprintf(fixedLabel, sizeof(fixedLabel), "Fixed %.2f ms", fixedMs);

			const float plotMax = budgetMs * 2.f;

			ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);
			ImGui::TextUnformatted("Logic thread frame time");
			ImGui::PopStyleColor();
			ImGui::PushStyleColor(ImGuiCol_PlotLines, TnxStyle::Color::ThBrain);
			ImGui::PlotLines("##logicms", LogicMsHistory.data(), HistorySize, HistoryOffset,
				logicLabel, 0.f, plotMax, ImVec2(-1.f, 60.f));
			ImGui::PopStyleColor();

			ImGui::PushStyleColor(ImGuiCol_Text, TnxStyle::Color::FgMuted);
			ImGui::TextUnformatted("Fixed step frame time");
			ImGui::PopStyleColor();
			ImGui::PushStyleColor(ImGuiCol_PlotLines, TnxStyle::Color::ThEncoder);
			ImGui::PlotLines("##fixedms", FixedMsHistory.data(), HistorySize, HistoryOffset,
				fixedLabel, 0.f, plotMax, ImVec2(-1.f, 60.f));
			ImGui::PopStyleColor();

			ImGui::EndTabItem();
		}

		// -----------------------------------------------------------------------
		// Render Debug tab
		// -----------------------------------------------------------------------
#ifdef TNX_DEBUG_RENDERING
		if (ImGui::BeginTabItem("Render Debug"))
		{
			static const char* ModeNames[] = { "Off", "Skin Path" };
			int current                    = static_cast<int>(state.DebugDrawMode);
			if (ImGui::Combo("Debug Mode", &current, ModeNames, IM_ARRAYSIZE(ModeNames)))
				state.DebugDrawMode = static_cast<uint8_t>(current);

			if (state.DebugDrawMode == 1)
			{
				ImGui::Spacing();
				ImGui::TextColored(ImVec4(0.f, 1.f, 1.f, 1.f), "  Cyan  = skeletal (LBS path)");
				ImGui::TextColored(ImVec4(0.f, 1.f, 0.f, 1.f), "  Green = static   (TRS path)");
			}
			ImGui::EndTabItem();
		}
#endif

		ImGui::EndTabBar();
	}

	ImGui::End();
}
