# Unreal Gold VR - Meta Quest (OpenXR) beta packaging

The Android project (`Projects/Android`, app name "Unreal Gold VR", Gradle project
`UnrealGoldVR`; the package id stays `com.dpjudas.surrealengine` because the JNI entry points
are named after it) can produce a self-contained APK that carries the game data, so a headset
needs nothing sideloaded.

## What the APK contains

* `libsurrealengine.so` (Vulkan + OpenXR build of the engine, arm64-v8a) and the OpenXR loader.
* Assets from `Projects/Android/assets-game/` (git-ignored, staged from a local game install):
  * `SurrealEngine.pk3` - the engine's own UI resources.
  * `game/<System|Maps|Textures|Sounds|Music|SystemLocalized|Help>/...` - the game files.

On first launch (and again after an APK with a higher `versionCode` is installed) the activity
extracts the assets into `/sdcard/SurrealEngine` on a background thread, showing a progress
panel, then starts the engine. Files that are not part of the bundle (`commandline.txt`,
edited `.ini` files, extra maps) are never touched, so a previously sideloaded install keeps
working. Remove `/sdcard/SurrealEngine/.bundled-game-version` to force a re-extraction.

## Building

1. Stage the data (Unreal Tournament shown; any supported game works the same way):

       robocopy D:\UnrealTournament Projects\Android\assets-game\game /E /XD Installer NetGamesUSA.com Web Logs /XF *.lnk commandline.txt UnrealTournament.ini User.ini
       copy SurrealEngine.pk3 Projects\Android\assets-game\

   `UnrealTournament.ini`/`User.ini` are the Windows install's personal settings (display
   brightness, resolution, key bindings) and are deliberately left out - the engine falls back
   to `Default.ini`/`DefUser.ini` and keeps its own settings in the app's private storage.

2. Bump `android:versionCode` / `android:versionName` in `Projects/Android/AndroidManifest.xml`.
3. `gradle assembleRelease` in `Projects/Android` using **Gradle 8.x** (8.14.5 is known good). Gradle 9
   fails in `processReleaseResources` with "Cannot mutate the dependencies of configuration
   ':releaseCompileClasspath' after the configuration was resolved" because AGP 8.2.1 predates
   Gradle 9 support. The APK lands in
   `Projects/Android/build/outputs/apk/release/`. It is signed with the checked-in debug
   keystore unless `key.store`/`key.alias` (and their passwords) are passed as Gradle
   properties.
4. `adb install -r` the APK; on first start the Quest asks for "All files access" (needed for
   `/sdcard/SurrealEngine`), then the extraction runs.

Without `assets-game/game` (only `SurrealEngine.pk3` staged - this is the public release
build) the APK copies the pk3 to the game directory and, if no game files are there, opens
`GameSetupActivity`: a regular 2D panel (immersive activities can't show Android views on the
headset) that offers to download the original Unreal Gold disc image from OldUnreal's servers
(archive.org as fallback; URLs, size and SHA-256 as in OldUnreal's own installer,
github.com/OldUnreal/FullGameInstallers, configured per flavor in `build.gradle`). After
checking the hash, `GameIsoInstaller` copies the disc's 226b files into the game directory
(English sounds/text only, no default `Unreal.ini`/`User.ini`), deletes the image and starts
the game. The download resumes after an interruption. The UT flavor has no image configured
yet and stays sideload-only.

## Licensing

Unreal, Unreal Tournament and Deus Ex game data are copyrighted by their publishers. An APK
built with `assets-game` populated is for the builder's own devices only - do not distribute
it. Distributable builds must be made without the `game/` assets (the `SurrealEngine.pk3`
asset alone is fine) and rely on players sideloading their own legally owned game files.

## Supported games

Verified on a Quest 3: Unreal Tournament (436) and Unreal Gold (226b - stage its folder the same way,
excluding `Unreal.ini`/`User.ini`). The engine log is available on the headset via
`adb logcat -s SurrealEngine-Log:*`; per-frame VR state via `SurrealEngine-VRDiag:*`.

## VR controls (Unreal Tournament / Unreal Gold)

| Input | Action |
| --- | --- |
| Left stick | Move (full speed; direction follows snap-turns) |
| Right stick left/right | Snap-turn 45 degrees |
| Right stick up/down | Next / previous weapon |
| Right trigger | Fire (aimed where the crosshair on the gun's ray sits) |
| Right grip | Alt-fire |
| Left trigger | Jump |
| Left X | ] - select next inventory item |
| Right B | F2 - universal translator in Unreal Gold |
| Right A | Enter - activate selected inventory item |
| Left grip (hold) | Crouch (C / Duck) |
| Left Y | Scoreboard (toggle) |
| Meta button long-press | Recenter (current facing becomes the body direction) |
| Left menu button | Open / close the game menu (curved panel; right controller is the pointer, trigger clicks) |
| Sniper zoom | Scope screen appears above the gun; aim with the hand |
| Redeemer guided shell | Steered with the right controller |
