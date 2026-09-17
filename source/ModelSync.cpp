#include "PCH.h"

#include "ModelSync.h"
#include "utils/Logger.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
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
		std::mutex g_stateLock;   // guards State::lastText and State::textPathUsed across threads

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
			REL::Relocation<decltype(&RE::IMenu::Accept)> originalAccept;
			REL::Relocation<decltype(&RE::IMenu::ProcessMessage)> originalProcessMessage;

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

		bool ReadHint(RE::GFxMovieView* a_movie, std::string& a_out)
		{
			auto* movie = a_movie;
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
					std::scoped_lock l(g_stateLock);
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

		// Is this Scaleform movie the loading menu's? Decided by asking it (rule 30): the loading
		// menu's root clip has a LoadingText member; nothing else does. Cached per movie pointer
		// while a Loading Menu is up, cleared when it opens or closes. This is how the movie is
		// recognised during a cell load, where the pointer the menu object holds turned out NOT to be
		// the one the loading code advances (2026-09-17: 3235 Advance calls on other movies, none on
		// the noted pointer, while the swipe visibly worked).
		std::mutex g_probeLock;
		std::unordered_map<RE::GFxMovieView*, bool> g_probe;

		bool IsLoadingMovie(RE::GFxMovieView* a_movie)
		{
			if (!a_movie) { return false; }
			{
				std::scoped_lock l(g_probeLock);
				auto it = g_probe.find(a_movie);
				if (it != g_probe.end()) { return it->second; }
			}
			// Several spellings, and the answer types, logged - so a run says exactly what each movie
			// advanced during a load answers, instead of leaving a silent miss to be guessed at.
			RE::GFxValue v1, v2, v3;
			const bool hasText = a_movie->GetVariable(&v1, "_root.Menu_mc.LoadingText");
			const bool hasMenu = a_movie->GetVariable(&v2, "_root.Menu_mc");
			const bool hasRoot = a_movie->GetVariable(&v3, "_root");
			const bool is = (hasText && (v1.IsObject() || v1.IsDisplayObject())) || (hasMenu && (v2.IsObject() || v2.IsDisplayObject()) && hasText);
			std::scoped_lock l(g_probeLock);
			g_probe[a_movie] = is;
			if (g_probe.size() <= 24)
			{
				logger::debug("probe movie {:#x}: _root={} ({}), _root.Menu_mc={} ({}), LoadingText={} ({}) -> {}",
							  reinterpret_cast<std::uintptr_t>(a_movie), hasRoot, static_cast<int>(v3.GetType()), hasMenu, static_cast<int>(v2.GetType()),
							  hasText, static_cast<int>(v1.GetType()), is ? "LOADING MENU" : "other");
			}
			return is;
		}

		void ClearProbe()
		{
			std::scoped_lock l(g_probeLock);
			g_probe.clear();
		}

		void Tick(RE::GFxMovieView* a_movie)
		{
			if (!g_settings.enabled || !g.resolved) { return; }

			std::string raw;
			if (!ReadHint(a_movie, raw)) { return; }
			const std::string text = Normalise(raw);

			bool changed = false;
			{
				std::scoped_lock l(g_stateLock);
				changed = text != g.lastText;
				if (changed) { g.lastText = text; }
			}
			if (changed)
			{
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

		// The per-frame tick. IMenu::AdvanceMovie on the LoadingMenu is only called while the game's
		// main loop runs the menus - the load from the main menu, not a cell transition, whose loading
		// screen is advanced by the loading code calling the Scaleform movie's own Advance directly
		// (seen 2026-09-17: the vtable-5 hook logged the first hint on the save load and nothing at all
		// during four coc loads). So the movie's GFxMovieView::Advance (slot 0x25 of the concrete
		// movie's vtable, patched at runtime the first time the Loading Menu is seen) is hooked too, and
		// filtered to the Loading Menu's own movie. Both paths call Tick; whichever runs, runs.
		using MovieAdvanceFn = float (*)(RE::GFxMovieView*, float, std::uint32_t);
		using MovieDisplayFn = void (*)(RE::GFxMovieView*);
		MovieAdvanceFn g_originalMovieAdvance = nullptr;
		MovieDisplayFn g_originalMovieDisplay = nullptr;
		std::atomic<RE::GFxMovieView*> g_loadingMovie{ nullptr };
		std::atomic<RE::LoadingMenu*> g_loadingMenu{ nullptr };
		bool g_moviePatched = false;
		inline constexpr std::size_t kMovieAdvanceSlot = 0x25;
		inline constexpr std::size_t kMovieDisplaySlot = 0x26;
		// Which path is actually ticking, per Loading Menu - so a run says which hook the engine uses
		// during a cell load rather than leaving it to be inferred from silence.
		std::atomic<std::uint32_t> g_ticksMenu{ 0 }, g_ticksAdvance{ 0 }, g_ticksDisplay{ 0 }, g_advanceOther{ 0 };

		// ---- the swipe itself. During a cell load nothing advances the loading movie through a
		// path we can hook per frame (run 4, 2026-09-17: the movie answers the probe exactly once, at
		// the end of the load). But every swipe Loading Menu Overhaul makes is a GameDelegate call to
		// "RequestLoadingText", the callback the menu registers in Accept(). So Accept is hooked, the
		// registration is wrapped, and every request passes through here: the engine's handler runs
		// (it returns the text and erases the picked screen from loadScreens), the erased screen is
		// the pick, and its model is applied on the spot - on the thread that runs the Flash call.
		using CallbackFn = RE::FxDelegateHandler::CallbackFn;
		CallbackFn* g_origRequestLoadingText = nullptr;
		std::atomic<std::uint32_t> g_requests{ 0 }, g_picked{ 0 }, g_msgUpdate{ 0 }, g_msgOther{ 0 };
		// Swipes the DevBench tool asked for, performed on the game's thread when the menu next
		// processes a message. Run 7 (2026-09-17): invoking the movie from the DevBench thread reached
		// the engine's model loader on that thread and crashed it (56661 <- 51454 <- our Apply).
		std::atomic<std::uint32_t> g_pendingNext{ 0 };
		std::atomic<std::uint32_t> g_servedNext{ 0 };

		// A pick deferred because the previous model was still loading is applied at the next
		// opportunity the game thread gives us - any menu message or advance - not only on kUpdate.
		void RetryDeferred(const char* a_why)
		{
			if (!g.resolved || !g.wanted || g.wanted == g.current || LoadPending()) { return; }
			if (Apply(g.wanted, a_why))
			{
				g.current = g.wanted;
				++g.applied;
				g.pendingFrames = 0;
			}
			else { ++g.retries; }
		}

		void ServePendingNext(RE::LoadingMenu* a_menu)
		{
			RetryDeferred("deferred");
			if (!a_menu || !a_menu->uiMovie || g_pendingNext.load() == 0) { return; }
			--g_pendingNext;
			++g_servedNext;
			const bool ok = a_menu->uiMovie->Invoke("_root.Menu_mc.refreshLoadingText", nullptr, nullptr, 0);
			logger::debug("served a queued swipe on the game thread (refreshLoadingText -> {})", ok);
		}

		void RequestLoadingText(const RE::FxDelegateArgs& a_args)
		{
			auto* menu = static_cast<RE::LoadingMenu*>(static_cast<RE::IMenu*>(a_args.GetHandler()));
			++g_requests;
			std::vector<RE::TESLoadScreen*> before;
			RE::TESLoadScreen* prePick = nullptr;
			if (menu)
			{
				auto& rd = menu->GetRuntimeData();
				// unk78 in this CommonLib line; 7.x names it loadScreen - the pre-picked TESLoadScreen*
				prePick = reinterpret_cast<RE::TESLoadScreen*>(rd.unk78);
				for (auto* s : rd.loadScreens) { before.push_back(s); }
			}
			if (g_origRequestLoadingText) { g_origRequestLoadingText(a_args); }
			if (!menu || !g_settings.enabled || !g.resolved) { return; }

			if (prePick)
			{
				// the first request after the menu opens returns the pre-picked screen's text; its
				// model is the one the engine already put up
				g.current = prePick;
				g.wanted = prePick;
				g.firstTextSeen = true;
				logger::debug("swipe: first request answered with the pre-picked screen {} (model already up)", ScreenName(prePick));
				return;
			}
			auto& rd = menu->GetRuntimeData();
			RE::TESLoadScreen* chosen = nullptr;
			for (auto* s : before)
			{
				bool still = false;
				for (auto* t : rd.loadScreens) { if (t == s) { still = true; break; } }
				if (!still) { chosen = s; break; }
			}
			if (!chosen)
			{
				logger::debug("swipe: request {} erased nothing from loadScreens ({} left) - no pick to follow", g_requests.load(), rd.loadScreens.size());
				return;
			}
			++g_picked;
			g.wanted = chosen;
			g.firstTextSeen = true;
			if (LoadPending())
			{
				++g.retries;
				logger::debug("swipe: picked {} but a model load is still pending; deferred", ScreenName(chosen));
				return;
			}
			if (Apply(chosen, "swipe"))
			{
				g.current = chosen;
				++g.applied;
			}
			else { ++g.retries; }
		}

		struct Processor : RE::FxDelegateHandler::CallbackProcessor
		{
			RE::FxDelegateHandler::CallbackProcessor* inner{ nullptr };
			void Process(const RE::GString& a_name, CallbackFn* a_fn) override
			{
				logger::debug("Accept registers \"{}\" -> +{:#x}", a_name.c_str() ? a_name.c_str() : "(null)", reinterpret_cast<std::uintptr_t>(a_fn) - REL::Module::get().base());
				if (a_name.c_str() && std::strcmp(a_name.c_str(), "RequestLoadingText") == 0)
				{
					g_origRequestLoadingText = a_fn;
					inner->Process(a_name, &RequestLoadingText);
					logger::debug("RequestLoadingText registration wrapped (engine handler +{:#x})", reinterpret_cast<std::uintptr_t>(a_fn) - REL::Module::get().base());
					return;
				}
				inner->Process(a_name, a_fn);
			}
		};

		// The Loading Menu is an always-open menu: one instance, created at start-up, before any
		// plugin's kDataLoaded, and its Accept() ran once then (runs 5 and 6, 2026-09-17: the Accept
		// hook was installed and never called). So the registration is rewritten where it already
		// sits - the CallbackDefn for "RequestLoadingText" in the menu's own FxDelegate table.
		bool WrapDelegate(RE::LoadingMenu* a_menu)
		{
			if (!a_menu) { return false; }
			auto* del = a_menu->fxDelegate.get();
			if (!del)
			{
				logger::warn("the Loading Menu has no FxDelegate; the swipe cannot be followed");
				return false;
			}
			RE::GString key("RequestLoadingText");
			auto* defn = del->callbacks.Get(key);
			if (!defn)
			{
				std::string names;
				for (auto it = del->callbacks.begin(); it != del->callbacks.end(); ++it) { names += it->first.c_str() ? it->first.c_str() : "?"; names += ' '; }
				logger::warn("the Loading Menu's delegate table has no RequestLoadingText; it holds: {}", names);
				return false;
			}
			if (defn->callback == &RequestLoadingText) { return true; }
			g_origRequestLoadingText = defn->callback;
			defn->callback = &RequestLoadingText;
			logger::info("RequestLoadingText rewired in the Loading Menu's delegate table (engine handler +{:#x})",
						 reinterpret_cast<std::uintptr_t>(g_origRequestLoadingText) - REL::Module::get().base());
			return true;
		}

		struct Hook
		{
			static void Accept(RE::LoadingMenu* a_this, RE::FxDelegateHandler::CallbackProcessor* a_processor)
			{
				logger::debug("LoadingMenu::Accept called (menu {:#x})", reinterpret_cast<std::uintptr_t>(a_this));
				Processor p;
				p.inner = a_processor;
				g.originalAccept(a_this, &p);
			}

			static RE::UI_MESSAGE_RESULTS ProcessMessage(RE::LoadingMenu* a_this, RE::UIMessage& a_message)
			{
				if (a_message.type.get() == RE::UI_MESSAGE_TYPE::kUpdate)
				{
					++g_msgUpdate;
					if (a_this && a_this->uiMovie) { Tick(a_this->uiMovie.get()); }
				}
				else { ++g_msgOther; }
				ServePendingNext(a_this);
				return g.originalProcessMessage(a_this, a_message);
			}

			static void AdvanceMovie(RE::LoadingMenu* a_this, float a_interval, std::uint32_t a_currentTime)
			{
				g.originalAdvanceMovie(a_this, a_interval, a_currentTime);
				++g_ticksMenu;
				if (a_this && a_this->uiMovie) { Tick(a_this->uiMovie.get()); }
				ServePendingNext(a_this);
			}

			static float MovieAdvance(RE::GFxMovieView* a_this, float a_deltaT, std::uint32_t a_frameCatchUpCount)
			{
				const float r = g_originalMovieAdvance ? g_originalMovieAdvance(a_this, a_deltaT, a_frameCatchUpCount) : 0.0f;
				if (g_loadingMenu.load() && IsLoadingMovie(a_this))
				{
					++g_ticksAdvance;
					Tick(a_this);
				}
				else { ++g_advanceOther; }
				return r;
			}

			static void MovieDisplay(RE::GFxMovieView* a_this)
			{
				if (g_originalMovieDisplay) { g_originalMovieDisplay(a_this); }
				if (g_loadingMenu.load() && IsLoadingMovie(a_this))
				{
					++g_ticksDisplay;
					Tick(a_this);
				}
			}
		};

		void PatchMovieVtable(RE::GFxMovieView* a_movie)
		{
			if (g_moviePatched || !a_movie) { return; }
			auto** vtable = *reinterpret_cast<void***>(a_movie);
			if (!vtable) { return; }
			g_originalMovieAdvance = reinterpret_cast<MovieAdvanceFn>(vtable[kMovieAdvanceSlot]);
			g_originalMovieDisplay = reinterpret_cast<MovieDisplayFn>(vtable[kMovieDisplaySlot]);
			REL::safe_write(reinterpret_cast<std::uintptr_t>(&vtable[kMovieAdvanceSlot]), reinterpret_cast<std::uintptr_t>(&Hook::MovieAdvance));
			REL::safe_write(reinterpret_cast<std::uintptr_t>(&vtable[kMovieDisplaySlot]), reinterpret_cast<std::uintptr_t>(&Hook::MovieDisplay));
			g_moviePatched = true;
			logger::info("GFxMovieView::Advance and Display hooked on the loading movie's vtable (slots {:#x}/{:#x}, originals +{:#x}/+{:#x}, vtable +{:#x})",
						 kMovieAdvanceSlot, kMovieDisplaySlot,
						 reinterpret_cast<std::uintptr_t>(g_originalMovieAdvance) - REL::Module::get().base(),
						 reinterpret_cast<std::uintptr_t>(g_originalMovieDisplay) - REL::Module::get().base(),
						 reinterpret_cast<std::uintptr_t>(vtable) - REL::Module::get().base());
		}
	}

	void NoteLoadingMenu(bool a_open)
	{
		ClearProbe();
		if (!a_open)
		{
			g_loadingMovie.store(nullptr);
			g_loadingMenu.store(nullptr);
			return;
		}
		auto* ui = RE::UI::GetSingleton();
		auto menu = ui ? ui->GetMenu<RE::LoadingMenu>() : nullptr;
		if (!menu || !menu->uiMovie)
		{
			logger::warn("Loading Menu opened but its menu or movie could not be fetched; this load will not be synced");
			return;
		}
		g_loadingMenu.store(menu.get());
		g_loadingMovie.store(menu->uiMovie.get());
		g_ticksMenu = g_ticksAdvance = g_ticksDisplay = 0;
		if (g.resolved) { WrapDelegate(menu.get()); }
		logger::debug("Loading Menu {:#x} movie {:#x} noted", reinterpret_cast<std::uintptr_t>(menu.get()), reinterpret_cast<std::uintptr_t>(menu->uiMovie.get()));
		if (g.resolved) { PatchMovieVtable(menu->uiMovie.get()); }
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
		{ std::scoped_lock l(g_stateLock); g.lastText.clear(); }
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
		g.originalAccept = vtable.write_vfunc(0x1, &Hook::Accept);
		g.originalProcessMessage = vtable.write_vfunc(0x4, &Hook::ProcessMessage);
		logger::info("LoadingMenu::Accept (slot 1) and ProcessMessage (slot 4) hooked");
		if (auto* ui = RE::UI::GetSingleton())
		{
			auto menu = ui->GetMenu<RE::LoadingMenu>();
			if (menu) { WrapDelegate(menu.get()); }
			else { logger::debug("the Loading Menu instance does not exist yet; its delegate will be rewired when it first opens"); }
		}
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
		// Queued, never invoked from the caller's thread: the menu performs it when it next processes
		// a message or advances, on the game's own thread (see g_pendingNext).
		if (!g_loadingMenu.load())
		{
			logger::info("RequestNextHint: the Loading Menu is not open; nothing queued");
			return false;
		}
		++g_pendingNext;
		logger::debug("RequestNextHint: queued ({} pending)", g_pendingNext.load());
		return true;
	}

	std::string StateJson()
	{
		std::string textPath, lastText;
		{
			std::scoped_lock l(g_stateLock);
			textPath = g.textPathUsed;
			lastText = g.lastText.substr(0, 80);
		}
		auto esc = [](std::string_view s) {
			std::string o;
			for (const char c : s) { if (c == '"' || c == '\\') { o += '\\'; } if (c != '\r' && c != '\n') { o += c; } }
			return o;
		};
		return std::format(
			"\"installed\":{},\"runtimeSupported\":{},\"resolved\":{},\"enabled\":{},\"screens\":{},\"screensWithText\":{},"
			"\"distinctTexts\":{},\"textPath\":\"{}\",\"lastText\":\"{}\",\"current\":\"{}\",\"wanted\":\"{}\","
			"\"applied\":{},\"retries\":{},\"pendingFrames\":{},\"unmatched\":{},\"loadPending\":{},\"requestedPath\":\"{}\","
			"\"ticksMenu\":{},\"ticksAdvance\":{},\"ticksDisplay\":{},\"advanceOther\":{},\"movieNoted\":{},"
			"\"requests\":{},\"picked\":{},\"msgUpdate\":{},\"msgOther\":{},\"pendingNext\":{},\"servedNext\":{}",
			g.installed, g.runtimeSupported, g.resolved, g_settings.enabled, g.screens, g.screensWithText, g.byText.size(),
			esc(textPath), esc(lastText), ScreenName(g.current), ScreenName(g.wanted),
			g.applied, g.retries, g.pendingFrames, g.unmatched, g.resolved ? LoadPending() : false, esc(RequestedPath()),
			g_ticksMenu.load(), g_ticksAdvance.load(), g_ticksDisplay.load(), g_advanceOther.load(), g_loadingMovie.load() != nullptr,
			g_requests.load(), g_picked.load(), g_msgUpdate.load(), g_msgOther.load(), g_pendingNext.load(), g_servedNext.load());
	}
}
