# Changelog - Loading Menu Overhaul Model Sync

## 1.0.2 - 2026-09-18 - untested

### Fixed
- **The Address Library guard now runs before SKSE::Init.** CommonLibSSE-NG's Init opens the Address Library itself, so the guard added for a missing file sat after the very call that fails on it and never ran; oproso's log (Perfected Wheeler 1.3.2, 2026-09-18) showed the banner, then CommonLib's bare 'failed to open address library file', and no [AddressLibrary] line. The check is now the first thing after the logger, so a missing file is named - game version, file, folder - and the plugin loads inert.

## 1.0.1 - 2026-09-18 - working

### Added
- **A minimum loading-screen time** (the owner: a settings page with a minimum the player can set, default 20 seconds). The game-load loop's state check (id 38085) and the cell transition's wait (id 39366) are held until the time has passed, so the swapped model is on screen long enough to be seen on a fast machine. Settings page inside Apocrypha Menu Framework: swap on/off, minimum on/off and seconds, log level; INI `[MinimumTime] bMinimumLoadingTime`, `fMinimumLoadingSeconds`.

### Fixed
- **Every model swap now happens on the thread that draws the loading screen**, from the loading movie's Advance/Display hooks, before the engine's draw - never from LoadingMenu::AdvanceMovie, ProcessMessage or the RequestLoadingText callback, which the game runs on the main thread and on several JobList threads (measured 2026-09-18: AdvanceMovie on the game thread, ProcessMessage on four different threads across four loads, Display on a different thread per load). crash-2026-09-18-05-02-11 was a null node while the JobList thread drew the loading movie through this plugin's Display hook; a swap or movie call from another thread during that draw is the nearest cause. The hint and swipe paths only record the wanted screen now.
- **A screen whose model the game cannot open is never swapped to** (crash-2026-09-18-05-02-11: an access violation in the engine's NIF loader under DisplayLoadingScreen with this plugin's swap on the stack). A screen is picked by hint text, which can be one the engine's own conditions would never show, and its NIF can be missing or broken; every screen's model is now probed through the game's resource layer before it is requested, the answer cached, and a hint whose screens all fail leaves the model as it is.
- The previous run's log is kept as `.prev`, so a crash's context survives the next launch.
- Carries the Address Library guard: with the file for the game missing, the plugin loads inert and says so.

## 1.0.0

* First version. When Loading Menu Overhaul swipes to another loading-screen hint, the 3D model on
  screen changes to the one that belongs to that hint - forward and back.
* How: the engine picks one load screen when the Loading Menu first updates, hands its model to the
  loading-menu scene, and never touches the model again; the hint text is fetched separately by the
  Flash movie through the RequestLoadingText callback, which returns a different screen's text on
  every later call and erases it from the remaining list. During a cell load nothing advances that
  movie through a path a plugin can watch per frame, so this plugin hooks the menu's Accept() and
  wraps the RequestLoadingText registration: every swipe passes through it, the erased screen is the
  pick, and its model is handed to the same engine function the setup used - after checking the
  scene is not still loading the previous one. The menu's update messages, when they arrive, also
  re-read the hint the movie shows, which is how a replayed "previous" hint is followed.
* Skyrim SE 1.5.97 only: the engine functions were read out of that executable and mapped to
  Address Library ids 51454 (the model setter) and 519827/519829/519830 (the scene and its
  requested/loaded model paths). On any other runtime the plugin loads, logs why, and does nothing.
* Ships a DevBench tool, `lmo.modelsync` (state / next / apply / set), so the sync can be driven and
  read without touching the keyboard.
