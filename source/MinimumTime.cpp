#include "PCH.h"

#include "MinimumTime.h"

#include "utils/Logger.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <mutex>

namespace mintime
{
	namespace
	{
		// Skyrim SE 1.5.97 (read out of the executable, 2026-09-18):
		//   id 38085 (0x63FA50)  the loading-screen loop: one iteration draws one frame of the loading screen and it
		//                        repeats while `IsLoadState(2)` is true;
		//   id 34550 (0x5763B0)  bool IsLoadState(int which): the load-state word (== which); called from the loop
		//                        only, at +0x39, as `call rel32` with which = 2.
		// The hook replaces that one call with ours: the original is asked first, and when it says "done" before the
		// minimum has passed we answer "still loading" instead, so the loop keeps drawing - backdrop, 3D model, hint
		// swipe - and nothing after the loop (cell attach, control return, menu hide) runs until the time is reached.
		constexpr std::uint64_t kID_Loop = 38085;
		constexpr std::uint64_t kID_IsLoadState = 34550;
		constexpr std::uintptr_t kCallOffset = 0x39;
		constexpr int kLoadingState = 2;
		// Cell transitions (doors, fast travel, coc) never enter that loop. Traced on 2026-09-18 (every coc's show and
		// hide came from id 39366): the transition function shows the loading screen, then BLOCKS the main thread in
		//     while (TasksPending()) { PumpTasks(); Sleep(10); }        (+0x7AC and +0x7DE: `call rel32` id 40306)
		// while the loading thread draws the Loading Menu, and only after the loop hides the screen and attaches the
		// player. `TasksPending` (id 40306) is a one-word read. Its two calls in that loop are redirected here: a
		// "nothing pending" before the minimum is answered "still pending", so the engine keeps sleeping in its own
		// wait - the very state every slow load sits in - and the loading screen stays up in full.
		constexpr std::uint64_t kID_Transition = 39366;
		constexpr std::uint64_t kID_TasksPending = 40306;
		constexpr std::uintptr_t kWaitCallOffsetA = 0x7AC;
		constexpr std::uintptr_t kWaitCallOffsetB = 0x7DE;

		using Fn_t = bool(int);
		using Pending_t = bool();

		Settings g_settings;
		REL::Relocation<Fn_t> g_orig;
		REL::Relocation<Pending_t> g_origPending;
		std::atomic<bool> g_installed{ false };
		std::atomic<bool> g_holding{ false };
		std::atomic<std::uint32_t> g_shows{ 0 }, g_holds{ 0 }, g_replays{ 0 };

		std::mutex g_lock;
		std::chrono::steady_clock::time_point g_shownAt;
		bool g_shownValid{ false };

		double ElapsedMs()
		{
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_shownAt).count();
		}

		void MarkShown()
		{
			g_shownAt = std::chrono::steady_clock::now();
			g_shownValid = true;
			g_holding = false;
			++g_shows;
		}

		bool LoadingMenuOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
		}

		// The engine says the load is done (a_where names the path, for the log). True = keep it on the loading screen.
		bool HoldDone(const char* a_where)
		{
			std::scoped_lock l(g_lock);
			const float minimum = g_settings.enabled ? g_settings.seconds : 0.0f;
			if (minimum > 0.0f && g_shownValid && LoadingMenuOpen() && ElapsedMs() < minimum * 1000.0)
			{
				if (!g_holding.load())
				{
					g_holding = true;
					++g_holds;
					logger::info("minimum loading time: the {} finished after {:.1f} s; keeping the loading screen up until {:.0f} s", a_where, ElapsedMs() / 1000.0, minimum);
				}
				return true;
			}
			if (g_holding.load())
			{
				g_holding = false;
				++g_replays;
				logger::info("minimum loading time: {:.1f} s reached; letting the {} finish", ElapsedMs() / 1000.0, a_where);
			}
			return false;
		}

		bool Hook(int a_which)
		{
			const bool loading = g_orig(a_which);
			if (a_which != kLoadingState || loading) { return loading; }
			return HoldDone("game load");
		}

		bool HookPending()
		{
			const bool pending = g_origPending();
			if (pending) { return true; }
			return HoldDone("cell transition");
		}

		// Verifies that game function a_fn + a_offset is `call rel32` to a_target, then redirects it to a_hook.
		bool PatchCall(std::uint64_t a_fn, std::uintptr_t a_offset, std::uint64_t a_target, void* a_hook, const char* a_what)
		{
			std::uintptr_t fn = 0, target = 0;
			try { fn = REL::ID(a_fn).address(); target = REL::ID(a_target).address(); }
			catch (...) { logger::error("minimum loading time: Address Library did not resolve ids {} / {} ({}); off", a_fn, a_target, a_what); return false; }
			const auto base = REL::Module::get().base();
			const std::uintptr_t site = fn + a_offset;
			const auto* b = reinterpret_cast<const std::uint8_t*>(site);
			std::int32_t rel = 0;
			std::memcpy(&rel, b + 1, 4);
			const std::uintptr_t found = site + 5 + static_cast<std::intptr_t>(rel);
			if (b[0] != 0xE8 || found != target)
			{
				logger::error("minimum loading time: game offset 0x{:X} is not `call` to 0x{:X} ({:02X} {:02X} {:02X} {:02X} {:02X}, target 0x{:X}); {} not attached",
							  site - base, target - base, b[0], b[1], b[2], b[3], b[4], found - base, a_what);
				return false;
			}
			SKSE::GetTrampoline().write_call<5>(site, reinterpret_cast<std::uintptr_t>(a_hook));
			logger::info("minimum loading time: {} attached at game offset 0x{:X}", a_what, site - base);
			return true;
		}
	}

	Settings& GetSettings() { return g_settings; }

	void Install()
	{
		if (g_installed.load()) { return; }
		if (!REL::Module::IsSE())
		{
			logger::warn("minimum loading time: the loading-loop ids are known for Skyrim SE 1.5.97 only; the minimum time is off on this runtime");
			return;
		}
		try
		{
			g_orig = REL::Relocation<Fn_t>(REL::ID(kID_IsLoadState));
			g_origPending = REL::Relocation<Pending_t>(REL::ID(kID_TasksPending));
		}
		catch (...) { logger::error("minimum loading time: Address Library did not resolve ids {} / {}; off", kID_IsLoadState, kID_TasksPending); return; }
		const bool a = PatchCall(kID_Loop, kCallOffset, kID_IsLoadState, reinterpret_cast<void*>(&Hook), "the game-load loop's state check");
		const bool b1 = PatchCall(kID_Transition, kWaitCallOffsetA, kID_TasksPending, reinterpret_cast<void*>(&HookPending), "the cell transition's wait (entry)");
		const bool b2 = PatchCall(kID_Transition, kWaitCallOffsetB, kID_TasksPending, reinterpret_cast<void*>(&HookPending), "the cell transition's wait (loop)");
		const bool b = b1 && b2;
		g_installed = a || b;
		logger::info("minimum loading time: minimum {:.0f} s ({}); game loads {}, cell transitions {}",
					 g_settings.seconds, g_settings.enabled ? "on" : "off", a ? "covered" : "NOT covered", b ? "covered" : "NOT covered");
	}

	void Tick() {}

	void NoteLoadingMenu(bool a_open)
	{
		std::scoped_lock l(g_lock);
		if (a_open) { MarkShown(); return; }
		if (g_holding.load())
		{
			logger::warn("minimum loading time: the Loading Menu closed while the load was being held; the hold is dropped");
			g_holding = false;
		}
		g_shownValid = false;
	}

	std::string StateJson()
	{
		double sinceMs = 0.0;
		{
			std::scoped_lock l(g_lock);
			if (g_shownValid) { sinceMs = ElapsedMs(); }
		}
		return std::format("\"minimumInstalled\":{},\"minimumEnabled\":{},\"minimumSeconds\":{:.1f},\"holding\":{},\"sinceShowMs\":{:.0f},\"shows\":{},\"holds\":{},\"replays\":{}",
						   g_installed.load(), g_settings.enabled, g_settings.seconds, g_holding.load(), sinceMs, g_shows.load(), g_holds.load(), g_replays.load());
	}
}
