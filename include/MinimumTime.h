#pragma once

// Minimum loading screen time (the owner, 2026-09-18: "an INI settings page ... that allows the user to set a minimum
// loading screen time and have it default to 20 seconds minimum", and "prevent the game from loading a cell by
// intercepting whatever it does to say that the cell is ready to load into until the time period that we set").
//
// How the engine runs a loading screen (read out of the 1.5.97 code, 2026-09-18): the main thread sits in one loop -
// Address Library id 38085 - that draws one frame of the loading screen per iteration (backdrop, the 3D model scene,
// the hint text and its swipe) and repeats while `IsLoadState(2)` (id 34550) answers "still loading". The loading
// thread clears that state when the cell is attached; the next check ends the loop, and only then does the engine
// hide the menu, hand the controls back and let the world render. So "the cell is ready" is intercepted at that
// check: the loop's one call to it is redirected here, the real answer is taken first, and a "done" that arrives
// before the minimum has passed since the screen appeared is answered "still loading" instead. The game keeps drawing
// its own loading screen meanwhile - nothing is faked, held or replayed.
//
// Cell transitions (doors, fast travel, coc) never enter that loop. There the main thread keeps running its normal
// frame with the Loading Menu up, and the transition function (id 39366) asks a readiness predicate (id 39628) once
// per frame; only "ready" lets it tear the loading screen down and attach the player. Its one call is redirected the
// same way: a "ready" before the minimum is answered "not yet", which is exactly the path every slow load takes.
// Both holds apply only while the Loading Menu is actually open, so the start-up load is untouched.
//
// Two other routes were tried the same day and rejected: swallowing the Loading Menu's hide message kept the text
// but let the world render behind it, and holding the show/hide manager's (id 13214) hide call let the game go on
// underneath and crashed the half-torn-down menu ten seconds later - that call is the visual teardown, not the
// decision.

#include <string>

namespace mintime
{
	struct Settings
	{
		bool enabled{ true };
		float seconds{ 20.0f };   // fMinimumLoadingSeconds:MinimumTime - 0 = off
		bool cellTransitions{ false };   // bHoldCellTransitions:MinimumTime - hold door/fast-travel loads too (off: Loading Menu Overhaul's UI stalls there, 2026-09-18)
	};
	Settings& GetSettings();

	// kDataLoaded, SE 1.5.97 only (the ids are SE's). Verifies the call site's bytes and target before writing.
	void Install();

	// Kept for the menu hooks that call it; the loop polls the check itself, so there is nothing to tick.
	void Tick();

	// From the MenuOpenCloseEvent: an opening Loading Menu starts the clock; a closing one drops any hold.
	void NoteLoadingMenu(bool a_open);

	// For the DevBench tool: "minimumInstalled", "minimumEnabled", "minimumSeconds", "holding", "sinceShowMs",
	// "shows", "holds", "replays".
	std::string StateJson();
}
