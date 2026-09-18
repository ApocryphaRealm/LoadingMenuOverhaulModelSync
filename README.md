Loading Menu Overhaul Model Sync
Version 1.0.1

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
    to the same engine function the game used for the first one. The swap is made on
    the thread that draws the loading screen, between two draws, and a refused swap is
    tried again a few passes later. Load screens without a model, or whose model file
    the game cannot open, are taken out of the candidate list when the screen opens, so
    every hint you swipe to has something to show.

MINIMUM LOADING SCREEN TIME
    A load from the main menu that finishes early stays up - backdrop, model and hints -
    until a minimum time has passed (20 seconds by default), so there is time to read
    and swipe. Door and fast-travel loads end when the game is ready unless you turn
    'Also hold door and fast-travel loads' on; while such a load is held, Loading Menu
    Overhaul's prompt art and hint cycling can stall until a bumper is pressed, which is
    why that switch ships off.

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
        Interface\Translations\LoadingMenuOverhaulModelSync_<language>.txt (eleven languages)

SETTINGS
    A settings page inside Apocrypha Menu Framework (or SKSE Menu Framework): the sync
    on or off, the minimum time on or off and its seconds, the door / fast-travel switch,
    the log level, with Save, Reload from INI and Restore defaults. The same keys in
    SKSE\Plugins\LoadingMenuOverhaulModelSync.ini:
        bEnabled=1                 the sync on (1) or off (0)
        bMinimumLoadingTime=1      hold a load from the main menu for the minimum time
        fMinimumLoadingSeconds=20  the minimum, in seconds; 0 turns it off
        bHoldCellTransitions=0     hold door and fast-travel loads too (see above)
        uLogLevel=1                0 trace, 1 debug, 2 info (1 is the shipped level)

DEBUGGING
    Documents\My Games\Skyrim Special Edition\SKSE\LoadingMenuOverhaulModelSync.log (the previous run's log is kept as .prev)
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
