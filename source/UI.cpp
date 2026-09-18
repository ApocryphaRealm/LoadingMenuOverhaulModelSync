// Loading Menu Overhaul Model Sync: the settings page, drawn inside the Apocrypha Menu Framework (memory:
// every mod of ours keeps its settings inside AMF as its own mod menu). Two things live here - whether the model
// follows the swiped hint, and the minimum loading screen time (the owner, 2026-09-18, default 20 s) - plus Save,
// Reload, Restore defaults and the log level. Every string goes through strings::TR, so the eleven translation
// files carry the page's text.
#include "PCH.h"

#include "UI.h"

#include "SKSEMenuFramework.h"

#include "MinimumTime.h"
#include "ModelSync.h"
#include "utils/Logger.h"
#include "utils/Strings.h"
#include "utils/Toggle.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace UI
{
	namespace
	{
		std::string statusMessage;
		std::string selectedSlider;

		constexpr const char* kLogLevelKeys[] = { "LMS_LogLevel_Trace", "LMS_LogLevel_Debug", "LMS_LogLevel_Info" };
		constexpr const char* kLogLevelNames[] = { "Trace", "Debug", "Info" };

		void OnMainThread(std::function<void()> a_task)
		{
			if (auto* taskInterface = SKSE::GetTaskInterface()) { taskInterface->AddTask(std::move(a_task)); }
		}

		bool HasRequiredExports()
		{
			constexpr const char* required[] = { "AddSectionItem", "igTextV", "igTextDisabledV", "igTextWrappedV", "igSetTooltipV", "igSeparatorText",
												 "igCombo_Str_arr", "igSliderFloat", "igIsKeyPressed_Bool", "igIsItemClicked", "igIsItemActive", "igIsItemHovered",
												 "igButton", "igSameLine", "igSpacing", "igPushItemWidth", "igPopItemWidth", "igGetCursorScreenPos",
												 "igGetWindowDrawList", "igGetFrameHeight", "igInvisibleButton", "igPushID_Str", "igPopID",
												 "ImDrawList_AddRectFilled", "ImDrawList_AddCircleFilled" };
			for (const char* name : required)
			{
				if (!GetMenuFrameworkFunction<void*>(name))
				{
					logger::warn("The menu framework does not export \"{}\"", name);
					return false;
				}
			}
			return true;
		}

		void HelpMarker(const char* a_description)
		{
			ImGuiMCP::SameLine();
			ImGuiMCP::TextDisabled("%s", strings::TR("LMS_HelpMark", "(?)"));
			if (ImGuiMCP::IsItemHovered()) { ImGuiMCP::SetTooltip("%s", a_description); }
		}

		bool NudgeableSlider(const char* a_label, float* a_value, float a_min, float a_max, const char* a_format, float a_step)
		{
			bool changed = ImGuiMCP::SliderFloat(a_label, a_value, a_min, a_max, a_format);
			if (ImGuiMCP::IsItemClicked() || ImGuiMCP::IsItemActive()) { selectedSlider = a_label; }
			if (selectedSlider == a_label)
			{
				float nudge = 0.0F;
				if (ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_LeftArrow) || ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_DownArrow)) { nudge -= a_step; }
				if (ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_RightArrow) || ImGuiMCP::IsKeyPressed(ImGuiMCP::ImGuiKey_UpArrow)) { nudge += a_step; }
				if (nudge != 0.0F) { *a_value = std::clamp(*a_value + nudge, a_min, a_max); changed = true; }
				ImGuiMCP::SameLine();
				ImGuiMCP::TextDisabled("<-->");
			}
			return changed;
		}

		void RenderButtons()
		{
			ImGuiMCP::Spacing();
			if (ImGuiMCP::Button(strings::TR("LMS_SaveBtn", "Save")))
			{
				statusMessage = strings::TR("LMS_StatusSaving", "Saving...");
				OnMainThread([]() {
					statusMessage = modelsync::SaveSettings() ? strings::TR("LMS_StatusSaved", "Settings saved.")
															   : strings::TR("LMS_StatusSaveFail", "Could not write the INI. See the log for why.");
				});
			}
			HelpMarker(strings::TR("LMS_HelpSave", "Writes every setting on this page to the plugin's INI so it survives a restart."));
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button(strings::TR("LMS_ReloadBtn", "Reload from INI")))
			{
				statusMessage = strings::TR("LMS_StatusReloading", "Reloading...");
				OnMainThread([]() {
					modelsync::LoadSettings();
					statusMessage = strings::TR("LMS_StatusReloaded", "Settings reloaded from the INI.");
				});
			}
			HelpMarker(strings::TR("LMS_HelpReload", "Throws away any change made here since the last save and re-reads the INI from disk."));
			ImGuiMCP::SameLine();
			if (ImGuiMCP::Button(strings::TR("LMS_RestoreBtn", "Restore defaults")))
			{
				modelsync::RestoreDefaults();
				statusMessage = strings::TR("LMS_StatusRestored", "Defaults restored. Press Save to keep them.");
			}
			HelpMarker(strings::TR("LMS_HelpRestore", "Puts every setting back to its fresh-install value. Nothing is written until you press Save."));
			if (!statusMessage.empty()) { ImGuiMCP::TextDisabled("%s", statusMessage.c_str()); }
		}
	}

	void Register()
	{
		if (!SKSEMenuFramework::IsInstalled())
		{
			logger::info("No menu framework is installed; settings will be read from the INI only");
			return;
		}
		if (!HasRequiredExports())
		{
			logger::warn("The installed menu framework is older than this plugin's settings menu needs. Update it (Apocrypha Menu Framework, or SKSE Menu Framework version 3 or newer).");
			return;
		}
		SKSEMenuFramework::SetSection(modelsync::kDisplayName);
		SKSEMenuFramework::AddSectionItem("Settings", SettingsPanel::Render);
		logger::info("Registered the settings page with the menu framework");
	}

	void __stdcall SettingsPanel::Render()
	{
		strings::Tick();

		ImGuiMCP::TextWrapped("%s", strings::TR("LMS_Intro", "Changes apply as soon as you make them. Press Save to keep them for the next time you play."));
		ImGuiMCP::Spacing();

		ImGuiMCP::SeparatorText(strings::TR("LMS_SecSync", "Model follows the hint"));
		auto& s = modelsync::GetSettings();
		ImGuiMCP::Toggle(strings::TR("LMS_Enabled", "Swap the 3D model with the hint"), &s.enabled);
		HelpMarker(strings::TR("LMS_HelpEnabled", "On: when Loading Menu Overhaul swipes to another hint, the loading screen's 3D model changes to that load screen's model. Off: the model stays as the game picked it."));

		ImGuiMCP::SeparatorText(strings::TR("LMS_SecMinimum", "Minimum loading screen time"));
		auto& m = mintime::GetSettings();
		ImGuiMCP::Toggle(strings::TR("LMS_MinEnabled", "Keep the loading screen up for a minimum time"), &m.enabled);
		HelpMarker(strings::TR("LMS_HelpMinEnabled", "On: a loading screen that finishes early stays up - backdrop, model and hints - until the time below has passed, so there is time to read and swipe. The game is paused meanwhile. Off: loading screens end as soon as the game is ready."));
		if (m.enabled)
		{
			ImGuiMCP::PushItemWidth(260.0F);
			NudgeableSlider(strings::TR("LMS_MinSeconds", "Minimum seconds"), &m.seconds, 0.0F, 120.0F, "%.0f s", 1.0F);
			ImGuiMCP::PopItemWidth();
			HelpMarker(strings::TR("LMS_HelpMinSeconds", "Counted from the moment the loading screen appears. 20 by default; 0 turns the minimum off. A load that takes longer than this is never held."));
			ImGuiMCP::Toggle(strings::TR("LMS_MinCells", "Also hold door and fast-travel loads"), &m.cellTransitions);
			HelpMarker(strings::TR("LMS_HelpMinCells", "Off: only a load from the main menu is held; a door or fast travel ends when the game is ready. On: those are held too - but Loading Menu Overhaul's prompt art and hint cycling can stall during that hold until you press a bumper, so this stays off until that is solved."));
		}

		ImGuiMCP::SeparatorText(strings::TR("LMS_SecDebug", "Debug"));
		{
			static std::vector<std::string> store;
			static std::vector<const char*> labels;
			store.clear(); labels.clear();
			for (int i = 0; i < 3; ++i) { store.push_back(strings::TR(kLogLevelKeys[i], kLogLevelNames[i])); }
			for (const auto& l : store) { labels.push_back(l.c_str()); }
			int level = std::clamp(s.logLevel, 0, 2);
			if (ImGuiMCP::Combo(strings::TR("LMS_LogLevel", "Log level"), &level, labels.data(), 3))
			{
				s.logLevel = level;
				modelsync::ApplyLogLevel();
			}
			HelpMarker(strings::TR("LMS_HelpLogLevel", "Applies immediately. The log is at Documents\\My Games\\Skyrim Special Edition\\SKSE\\LoadingMenuOverhaulModelSync.log."));
		}

		RenderButtons();
	}
}
