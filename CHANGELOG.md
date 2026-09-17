# Changelog - Loading Menu Overhaul - Model Sync

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
