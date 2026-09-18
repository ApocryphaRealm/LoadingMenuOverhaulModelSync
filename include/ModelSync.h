#pragma once

// Loading Menu Overhaul - Model Sync. Own code, GPL-3.0-or-later (2026-09-17).
//
// WHAT THE GAME DOES. When the Loading Menu first updates, the engine filters every TESLoadScreen by
// its conditions, picks one at random, hands its model (the load NIF, scale, rotation, translation
// and camera path) to the loading-menu 3D scene, and removes it from the list of remaining screens.
// The hint text is fetched separately by the menu's Flash movie through the "RequestLoadingText"
// callback; that callback returns the picked screen's text ONCE (then clears the pick), and every
// later call returns the text of another random remaining screen and removes it from the list. The
// model is never touched again. Loading Menu Overhaul's swipe buttons call that callback repeatedly,
// so the text moves on while the model stands still. (Read out of the 1.5.97 executable's
// LoadingMenu::ProcessMessage, its once-only setup at Address Library 51048 and the
// RequestLoadingText handler at 51046, 2026-09-17.)
//
// WHAT THIS DOES. Every frame the Loading Menu advances, read the hint the movie is showing
// (_root.Menu_mc.LoadingText.text). When it changes, find the load screen that owns that text and
// hand ITS model to the same engine function the setup used (Address Library 51454). The first hint
// after the menu opens belongs to the screen whose model is already up, so it is only recorded.
// Loading Menu Overhaul replays remembered text for "previous", which goes through the same path.
//
// Nothing here edits a record, patches a Flash file, or hooks the text handler; the only write into
// the engine is one virtual-table slot on LoadingMenu (AdvanceMovie), restored to the original's
// behaviour by calling it first.

namespace modelsync
{
	inline constexpr const char* kLogName = "LoadingMenuOverhaulModelSync";
	inline constexpr const char* kDisplayName = "Loading Menu Overhaul - Model Sync";
	inline constexpr const char* kIniName = "LoadingMenuOverhaulModelSync.ini";

	// Installs the AdvanceMovie hook and builds the text -> load screen table. kDataLoaded.
	void Install();

	// Forget the last seen text; called when the Loading Menu opens or closes.
	void Reset();

	// Remember (or forget) the Loading Menu and its movie, and hook the movie's Advance the first
	// time it is seen. Called from the menu open/close event.
	void NoteLoadingMenu(bool a_open);

	// Apply a load screen's model now, by hand (DevBench). Returns false if it has no model or the
	// engine function is not resolved.
	bool ApplyScreen(RE::TESLoadScreen* a_screen, const char* a_why);

	// Ask the movie for the next hint the way Loading Menu Overhaul's button does (DevBench).
	bool RequestNextHint();

	// One JSON object describing the state, for DevBench and the log.
	std::string StateJson();

	struct Settings
	{
		bool enabled{ true };
		int logLevel{ 1 };  // spdlog: 0 trace, 1 debug, 2 info
	};
	Settings& GetSettings();
	void LoadSettings();
	// 1.0.0 (settings page): write every setting back to the INI (keeps comments and order), restore the compiled
	// defaults, and apply the log level.
	bool SaveSettings();
	void RestoreDefaults();
	void ApplyLogLevel();
}
