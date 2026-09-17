// Loading Menu Overhaul - Model Sync. Own code, GPL-3.0-or-later (2026-09-17).
//
// Makes the loading screen's 3D model follow the hint text when Loading Menu Overhaul (Nexus
// 149874) swipes between hints. See include/ModelSync.h for how the engine behaves and what this
// changes. No record is edited, no Flash file is replaced; one LoadingMenu virtual slot is hooked.
#include "PCH.h"

#include "DevBenchTool.h"
#include "ModelSync.h"
#include "utils/Logger.h"

namespace
{
	class MenuWatcher : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static MenuWatcher* GetSingleton()
		{
			static MenuWatcher instance;
			return &instance;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME)
			{
				logger::debug("Loading Menu {}", a_event->opening ? "opened" : "closed");
				modelsync::Reset();
				modelsync::NoteLoadingMenu(a_event->opening);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type)
		{
		case SKSE::MessagingInterface::kPostLoad:
			DevBenchTool::Init(false);
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			modelsync::Install();
			if (auto* ui = RE::UI::GetSingleton()) { ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuWatcher::GetSingleton()); }
			else { logger::error("RE::UI singleton is null at kDataLoaded; menu open/close will not reset the sync"); }
			DevBenchTool::Init(true);
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SKSE::log::init(modelsync::kLogName);

	modelsync::LoadSettings();
	SKSE::log::describe_level(modelsync::kIniName);

	logger::info("{} {} loading", modelsync::kDisplayName,
				 SKSE::PluginDeclaration::GetSingleton()->GetVersion().string("."));

	SKSE::GetMessagingInterface()->RegisterListener(MessageHandler);
	return true;
}
