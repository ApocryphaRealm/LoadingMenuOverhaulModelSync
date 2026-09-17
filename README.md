Loading Menu Overhaul - Model Sync
Version 1.0.0

WHAT THIS IS
    A companion plugin for Loading Menu Overhaul (jpsteel, Nexus 149874). That mod lets
    you swipe through the loading-screen hints while a load runs; the 3D model on
    screen, though, stays on whatever the game picked first. With this plugin the
    model follows the hint - swipe forward or back, and the object the hint is about
    is the one turning on screen.

HOW IT WORKS
    The game picks one load screen when the loading menu first updates, hands its
    model to the loading-menu scene, and never touches the model again; the hint text
    is fetched separately by the menu's Flash movie, and each later request returns a
    different screen's text. This plugin watches the hint the movie is showing (one
    read per frame while the loading menu is up), finds the load screen that owns
    that text, and hands ITS model - NIF, scale, rotation, translation, camera path -
    to the same engine function the game used for the first one. If the scene is still
    loading the previous model it waits a frame and tries again.

    Nothing is edited: no record, no Flash file, no INI of another mod. One virtual
    function of the loading menu is hooked, and the original runs first.

REQUIREMENTS
    Skyrim SE 1.5.97 (the engine addresses were read out of that executable; on AE or
    VR the plugin loads, says so in its log, and does nothing)
    SKSE64                     https://skse.silverlock.org
    Address Library for SKSE   https://www.nexusmods.com/skyrimspecialedition/mods/32444
    Loading Menu Overhaul      https://www.nexusmods.com/skyrimspecialedition/mods/149874
                               (without it there is nothing to swipe, and this plugin
                               simply never sees the hint change)

INSTALLATION
    Install with a mod manager. Files:
        SKSE\Plugins\LoadingMenuOverhaulModelSync.dll
        SKSE\Plugins\LoadingMenuOverhaulModelSync.pdb
        SKSE\Plugins\LoadingMenuOverhaulModelSync.ini

SETTINGS
    SKSE\Plugins\LoadingMenuOverhaulModelSync.ini
        bEnabled=1     the sync on (1) or off (0)
        uLogLevel=1    0 trace, 1 debug, 2 info

DEBUGGING
    Documents\My Games\Skyrim Special Edition\SKSE\LoadingMenuOverhaulModelSync.log
    records the load screens it indexed, which movie variable answers with the hint,
    every hint change and every model swap with the model path and whether the engine
    accepted it. With DevBench installed, the tool "lmo.modelsync" reports the same
    state and can request the next hint or force a screen's model.

CREDITS
    jpsteel        Loading Menu Overhaul, whose swipe this plugin completes.
    meh321         Address Library for SKSE Plugins.
    CharmedBaryon and the CommonLibSSE-NG contributors - the library this is built on.

LICENCE
    GPL-3.0-or-later. See LICENSE, NOTICE.md and THIRD_PARTY_NOTICES.md.
    Source: https://github.com/ApocryphaRealm/LoadingMenuOverhaulModelSync
