#include "PCH.h"

#include "ModelSync.h"
#include "utils/Logger.h"

#include <cmath>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace modelsync
{
	namespace
	{
		// ---- engine addresses, Skyrim SE 1.5.97 (Address Library ids). Read out of the running
		// executable on 2026-09-17; see ModelSync.h for what each one is and how it was found.
		//   51454  void SetLoadingMenuModel(TESModel* model, float scale, const NiPoint3* rotation,
		//                                   const NiPoint3* translation, const char* cameraPath,
		//                                   float rotationOffset0, float rotationOffset1)
		//          - model may be null (clears). rotation is RNAM degrees * pi/180; translation is XNAM;
		//            the offsets are ONAM[0], ONAM[1] as floats. Defaults when a screen has no NIF data:
		//            scale 1, zero vectors, "" camera path, -180 / 180.
		//   519827 pointer to the loading-menu 3D scene object; byte 0x135 of it = "a model is set"
		//   519830 BSFixedString: the model path last REQUESTED from the setter
		//   519829 BSFixedString: the model path the scene has LOADED
		//          The setter ignores a call while (0x135 && requested != loaded), i.e. while a load
		//          is still pending - so the sync checks that first and retries on a later frame.
		inline constexpr std::uint64_t kID_SetModel = 51454;
		inline constexpr std::uint64_t kID_Scene = 519827;
		inline constexpr std::uint64_t kID_RequestedPath = 519830;
		inline constexpr std::uint64_t kID_LoadedPath = 519829;
		inline constexpr float kDegToRad = 0.01745329238474369f;

		using SetModelFn = void (*)(RE::TESModel*, float, const RE::NiPoint3*, const RE::NiPoint3*, const char*, float, float);

		Settings g_settings;

		struct State
		{
			bool installed{ false };
			bool resolved{ false };
			bool runtimeSupported{ false };
			SetModelFn setModel{ nullptr };
			std::uintptr_t scenePtrAddr{ 0 };
			std::uintptr_t requestedPathAddr{ 0 };
			std::uintptr_t loadedPathAddr{ 0 };
			REL::Relocation<decltype(&RE::IMenu::AdvanceMovie)> originalAdvanceMovie;

			std::unordered_map<std::string, std::vector<RE::TESLoadScreen*>> byText;
			std::size_t screens{ 0 };
			std::size_t screensWithText{ 0 };

			std::string lastText;       // normalised hint last seen in the movie
			bool firstTextSeen{ false };
			RE::TESLoadScreen* current{ nullptr };  // whose model is (believed to be) up
			RE::TESLoadScreen* wanted{ nullptr };   // whose model should be up
			std::uint32_t applied{ 0 };
			std::uint32_t retries{ 0 };
			std::uint32_t pendingFrames{ 0 };
			std::uint32_t unmatched{ 0 };
			std::string textPathUsed;   // which movie path answered
		} g;

		std::string Normalise(std::string_view a_in)
		{
			std::string out;
			out.reserve(a_in.size());
			bool space = false;
			for (const unsigned char c : a_in)
			{
				if (c == '\r') { continue; }
				if (c == ' ' || c == '\t' || c == '\n')
				{
					space = true;
					continue;
				}
				if (space && !out.empty()) { out += ' '; }
				space = false;
				out += static_cast<char>(c);
			}
			return out;
		}

		const char* ScreenName(RE::TESLoadScreen* a_screen)
		{
			if (!a_screen) { return "(none)"; }
			const char* edid = a_screen->GetFormEditorID();
			return edid && *edid ? edid : "(no editor id)";
		}

		void BuildTable()
		{
			g.byText.clear();
			g.screens = g.screensWithText = 0;
			auto* handler = RE::TESDataHandler::GetSingleton();
			if (!handler)
			{
				logger::error("TESDataHandler is null; no load screens indexed");
				return;
			}
			for (auto* screen : handler->GetFormArray<RE::TESLoadScreen>())
			{
				if (!screen) { continue; }
				++g.screens;
				const char* text = screen->loadingText.c_str();
				if (!text || !*text) { continue; }
				const std::string key = Normalise(text);
				if (key.empty()) { continue; }
				++g.screensWithText;
				g.byText[key].push_back(screen);
			}
			std::size_t dupes = 0;
			for (const auto& [k, v] : g.byText) { if (v.size() > 1) { ++dupes; } }
			logger::info("indexed {} load screens, {} with text, {} distinct texts ({} shared by more than one screen)",
						 g.screens, g.screensWithText, g.byText.size(), dupes);
		}

		bool LoadPending()
		{
			if (!g.scenePtrAddr) { return false; }
			const auto scene = *reinterpret_cast<std::uint8_t**>(g.scenePtrAddr);
			if (!scene) { return false; }
			if (scene[0x135] == 0) { return false; }
			const auto requested = *reinterpret_cast<const char**>(g.requestedPathAddr);
			const auto loaded = *reinterpret_cast<const char**>(g.loadedPathAddr);
			return requested != loaded;
		}

		const char* RequestedPath()
		{
			if (!g.requestedPathAddr) { return ""; }
			const auto p = *reinterpret_cast<const char**>(g.requestedPathAddr);
			return p ? p : "";
		}

		// The model, scale, rotation, translation, camera path and offsets of a screen, exactly as
		// the engine's own setup derives them (defaults for a screen with no NIF data).
		bool Apply(RE::TESLoadScreen* a_screen, const char* a_why)
		{
			if (!g.setModel) { return false; }
			RE::TESModel* model = nullptr;
			float scale = 1.0f;
			RE::NiPoint3 rotation{ 0.0f, 0.0f, 0.0f };
			RE::NiPoint3 translation{ 0.0f, 0.0f, 0.0f };
			const char* camera = "";
			float offset0 = -180.0f;
			float offset1 = 180.0f;
			const char* modelPath = "";

			if (auto* nif = a_screen->loadNIFData)
			{
				if (nif->loadNIF)
				{
					model = nif->loadNIF->As<RE::TESModel>();
					if (!model)
					{
						logger::warn("{}: load NIF form {:08X} is not a model-bearing object; clearing the model instead",
									 ScreenName(a_screen), nif->loadNIF->GetFormID());
					}
					else { modelPath = model->GetModel() ? model->GetModel() : ""; }
				}
				scale = nif->initialScale;
				rotation = { nif->rotationConstraints[0] * kDegToRad, nif->rotationConstraints[1] * kDegToRad, nif->rotationConstraints[2] * kDegToRad };
				translation = { nif->initialTranslationOffset[0], nif->initialTranslationOffset[1], nif->initialTranslationOffset[2] };
				camera = nif->cameraPath.GetModel() ? nif->cameraPath.GetModel() : "";
				offset0 = static_cast<float>(nif->rotationOffsetConstraints[0]);
				offset1 = static_cast<float>(nif->rotationOffsetConstraints[1]);
			}

			g.setModel(model, scale, &rotation, &translation, camera, offset0, offset1);
			const std::string after = RequestedPath();
			const bool accepted = model ? (after == modelPath) : true;
			logger::debug("{}: set model \"{}\" scale {:.2f} rot ({:.2f},{:.2f},{:.2f}) trans ({:.1f},{:.1f},{:.1f}) cam \"{}\" off {}/{} -> {}",
						  a_why, modelPath, scale, rotation.x, rotation.y, rotation.z, translation.x, translation.y, translation.z,
						  camera, offset0, offset1, accepted ? "accepted" : "NOT accepted (load pending)");
			return accepted;
		}

		bool ReadHint(RE::LoadingMenu* a_menu, std::string& a_out)
		{
			auto movie = a_menu->uiMovie;
			if (!movie) { return false; }
			// Loading Menu Overhaul's class keeps the text field in a member (LoadingText); the vanilla
			// movie names the instance path. Ask for both, remember which one answered (rule 30).
			static constexpr const char* kPaths[] = {
				"_root.Menu_mc.LoadingText.text",
				"_root.Menu_mc.LoadingTextFader.LoadingText.textField.text",
			};
			for (const char* path : kPaths)
			{
				RE::GFxValue v;
				if (movie->GetVariable(&v, path) && v.IsString())
				{
					const char* s = v.GetString();
					a_out = s ? s : "";
					if (g.textPathUsed != path)
					{
						g.textPathUsed = path;
						logger::info("hint text is read from {}", path);
					}
					return true;
				}
			}
			return false;
		}

		void Tick(RE::LoadingMenu* a_menu)
		{
			if (!g_settings.enabled || !g.resolved) { return; }

			std::string raw;
			if (!ReadHint(a_menu, raw)) { return; }
			const std::string text = Normalise(raw);

			if (text != g.lastText)
			{
				g.lastText = text;
				if (!g.firstTextSeen)
				{
					// The first hint after the menu opens belongs to the screen the engine already
					// put up (RequestLoadingText returns the pre-picked screen's text first).
					g.firstTextSeen = true;
					auto it = g.byText.find(text);
					g.current = (it != g.byText.end() && !it->second.empty()) ? it->second.front() : nullptr;
					g.wanted = g.current;
					logger::debug("first hint: \"{}\" -> {} (model already up)", text.substr(0, 60), ScreenName(g.current));
				}
				else if (!text.empty())
				{
					auto it = g.byText.find(text);
					if (it == g.byText.end())
					{
						++g.unmatched;
						logger::warn("hint \"{}\" matches no load screen's text; model left as is", text.substr(0, 80));
					}
					else
					{
						RE::TESLoadScreen* pick = nullptr;
						for (auto* s : it->second) { if (s == g.current) { pick = s; break; } }
						if (!pick) { for (auto* s : it->second) { if (s->loadNIFData) { pick = s; break; } } }
						if (!pick) { pick = it->second.front(); }
						g.wanted = pick;
						logger::debug("hint changed: \"{}\" -> {}{}", text.substr(0, 60), ScreenName(pick),
									  it->second.size() > 1 ? std::format(" (one of {} screens with this text)", it->second.size()) : "");
					}
				}
			}

			if (g.wanted && g.wanted != g.current)
			{
				if (LoadPending())
				{
					++g.pendingFrames;
					if (g.pendingFrames == 600) { logger::warn("a model load has been pending for 600 frames; still waiting to swap to {}", ScreenName(g.wanted)); }
					return;
				}
				if (Apply(g.wanted, "hint"))
				{
					g.current = g.wanted;
					++g.applied;
					g.pendingFrames = 0;
				}
				else { ++g.retries; }
			}
		}

		struct Hook
		{
			static void AdvanceMovie(RE::LoadingMenu* a_this, float a_interval, std::uint32_t a_currentTime)
			{
				g.originalAdvanceMovie(a_this, a_interval, a_currentTime);
				if (a_this) { Tick(a_this); }
			}
		};
	}

	Settings& GetSettings() { return g_settings; }

	void LoadSettings()
	{
		g_settings = Settings{};
		std::filesystem::path path = "Data/SKSE/Plugins";
		path /= kIniName;
		std::ifstream in(path);
		if (!in)
		{
			logger::info("no INI at {}; using the compiled defaults", path.string());
			return;
		}
		std::string line;
		while (std::getline(in, line))
		{
			const auto eq = line.find('=');
			if (eq == std::string::npos || line.empty() || line[0] == ';' || line[0] == '[') { continue; }
			const std::string key = Normalise(line.substr(0, eq));
			const std::string val = Normalise(line.substr(eq + 1));
			if (key == "bEnabled") { g_settings.enabled = val == "1" || val == "true"; }
			else if (key == "uLogLevel") { try { g_settings.logLevel = std::stoi(val); } catch (...) {} }
		}
		const auto level = g_settings.logLevel <= 0 ? spdlog::level::trace : g_settings.logLevel == 1 ? spdlog::level::debug : spdlog::level::info;
		SKSE::log::set_level(level, level);
	}

	void Reset()
	{
		g.lastText.clear();
		g.firstTextSeen = false;
		g.current = nullptr;
		g.wanted = nullptr;
		g.pendingFrames = 0;
	}

	void Install()
	{
		if (g.installed) { return; }
		g.installed = true;

		BuildTable();

		if (!REL::Module::IsSE())
		{
			logger::warn("this build maps the engine's loading-menu functions for Skyrim SE 1.5.97 only; on this runtime the model sync stays off");
			g.runtimeSupported = false;
			return;
		}
		g.runtimeSupported = true;

		try
		{
			g.setModel = reinterpret_cast<SetModelFn>(REL::ID(kID_SetModel).address());
			g.scenePtrAddr = REL::ID(kID_Scene).address();
			g.requestedPathAddr = REL::ID(kID_RequestedPath).address();
			g.loadedPathAddr = REL::ID(kID_LoadedPath).address();
		}
		catch (...)
		{
			logger::error("Address Library did not resolve one of ids {}, {}, {}, {}; model sync off", kID_SetModel, kID_Scene, kID_RequestedPath, kID_LoadedPath);
			return;
		}
		if (!g.setModel || !g.scenePtrAddr || !g.requestedPathAddr || !g.loadedPathAddr)
		{
			logger::error("an engine address resolved to null; model sync off");
			return;
		}
		g.resolved = true;
		logger::info("engine: SetLoadingMenuModel at +{:#x}, scene ptr at +{:#x}, requested/loaded paths at +{:#x}/+{:#x}",
					 reinterpret_cast<std::uintptr_t>(g.setModel) - REL::Module::get().base(), g.scenePtrAddr - REL::Module::get().base(),
					 g.requestedPathAddr - REL::Module::get().base(), g.loadedPathAddr - REL::Module::get().base());

		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_LoadingMenu[0] };
		g.originalAdvanceMovie = vtable.write_vfunc(0x5, &Hook::AdvanceMovie);
		logger::info("LoadingMenu::AdvanceMovie hooked (vtable slot 5)");
	}

	bool ApplyScreen(RE::TESLoadScreen* a_screen, const char* a_why)
	{
		if (!a_screen || !g.resolved) { return false; }
		const bool ok = Apply(a_screen, a_why);
		if (ok)
		{
			g.current = a_screen;
			g.wanted = a_screen;
			++g.applied;
		}
		return ok;
	}

	bool RequestNextHint()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) { return false; }
		auto menu = ui->GetMenu(RE::LoadingMenu::MENU_NAME);
		if (!menu || !menu->uiMovie)
		{
			logger::info("RequestNextHint: the Loading Menu is not open");
			return false;
		}
		// Loading Menu Overhaul's own refresh: GameDelegate RequestLoadingText -> SetLoadingText.
		const bool ok = menu->uiMovie->Invoke("_root.Menu_mc.refreshLoadingText", nullptr, nullptr, 0);
		logger::debug("RequestNextHint: Invoke refreshLoadingText -> {}", ok);
		return ok;
	}

	std::string StateJson()
	{
		auto esc = [](std::string_view s) {
			std::string o;
			for (const char c : s) { if (c == '"' || c == '\\') { o += '\\'; } if (c != '\r' && c != '\n') { o += c; } }
			return o;
		};
		return std::format(
			"\"installed\":{},\"runtimeSupported\":{},\"resolved\":{},\"enabled\":{},\"screens\":{},\"screensWithText\":{},"
			"\"distinctTexts\":{},\"textPath\":\"{}\",\"lastText\":\"{}\",\"current\":\"{}\",\"wanted\":\"{}\","
			"\"applied\":{},\"retries\":{},\"pendingFrames\":{},\"unmatched\":{},\"loadPending\":{},\"requestedPath\":\"{}\"",
			g.installed, g.runtimeSupported, g.resolved, g_settings.enabled, g.screens, g.screensWithText, g.byText.size(),
			esc(g.textPathUsed), esc(g.lastText.substr(0, 80)), ScreenName(g.current), ScreenName(g.wanted),
			g.applied, g.retries, g.pendingFrames, g.unmatched, g.resolved ? LoadPending() : false, esc(RequestedPath()));
	}
}
