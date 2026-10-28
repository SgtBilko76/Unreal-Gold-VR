# Unreal Gold VR — a Meta Quest & PICO port of Unreal Gold

[![Sponsor](https://img.shields.io/badge/Sponsor-SgtBilko76-ea4aaa?logo=githubsponsors&logoColor=white)](https://github.com/sponsors/SgtBilko76)


A standalone VR port of **Unreal Gold** for Meta Quest 2 / Pro / 3 / 3S (developed and tested
on Quest 3) and, experimentally, PICO 4 / Neo 3 — built on [Surreal Engine](https://github.com/dpjudas/SurrealEngine), a
from-scratch reimplementation of Unreal Engine 1, with an OpenXR/Vulkan stereo renderer.

The same engine also runs Unreal Tournament in VR — see the sister repository
[Unreal-Tournament-QuestVR](https://github.com/SgtBilko76/Unreal-Tournament-VR); both apps
install side by side. More PortRoyale ports at [portroyale.online](https://portroyale.online).

This repository contains **no copyrighted game content** — only engine code. You provide your
own copy of the game (see below for a free, legal source).

## Features

* Native OpenXR rendering (72 Hz, per-eye asymmetric projection), snap turning, stick locomotion
* Weapons held in and aimed with the right controller, with a controller-ray crosshair
* World-anchored curved menu panel operated by pointing and clicking with the controller
* VR-tuned controls (translator, inventory, crouch on controller buttons), per-weapon hand sizes
* VR comfort: no view bob, no double-tap dodge; recenter on the Meta / Home button
* Save games persist on the headset
* Installs side by side with [Unreal Tournament VR](https://github.com/SgtBilko76/Unreal-Tournament-VR) (separate app and data folder)
* Experimental PICO 4 / Neo 3 support — one APK for both makes, **untested on PICO hardware**;
  reports are very welcome

## Getting the game (legal, free)

Unreal Gold is available for free from OldUnreal:

**https://www.oldunreal.com/downloads/unreal/full-game-installers/**

Run the `Unreal_Gold.exe` installer on a PC (Linux/macOS installers are on OldUnreal's GitHub
releases, linked from that page).

> **Important — game version:** this port targets Unreal Gold **226b**, the version on the
> original discs and on GOG/Steam. The OldUnreal installer applies their 227 patch at the end
> of installation; the 227 **System** files do not work with Surreal Engine yet. Use the
> original (unpatched) 226b `System` files — e.g. from a GOG/Steam install, or the disc
> contents the OldUnreal installer extracts before patching. The `Maps`, `Textures`, `Sounds`,
> `Music` and `Help` folders are the same either way.

## Installing on the headset

1. Install the APK from the [Releases](../../releases) page (sideload with `adb install -r` or
   [SideQuest](https://sidequestvr.com/setup-howto); developer mode required). SideQuest is a
   Quest tool — on PICO, sideload with adb.
2. **Easiest:** just launch the app. If no game files are found, a panel offers to download
   the free original Unreal Gold disc (645 MB) from OldUnreal's servers — the source Epic Games
   approved — and installs the 226b files for you (Wi-Fi needed, takes about a minute or two).
   The download is checked against OldUnreal's published SHA-256.

   **Or by hand:** copy the game folders to `/sdcard/SurrealEngine/` on the headset so you have:

       /sdcard/SurrealEngine/System   Maps   Textures   Sounds   Music   Help

   e.g. `adb push "C:\Games\Unreal Gold\System" /sdcard/SurrealEngine/System` — and so on for
   each folder. Leave out your PC's `Unreal.ini` / `User.ini`; the app keeps its own settings.
3. Launch **Unreal Gold VR** from the Unknown Sources section of the library and grant
   "All files access" when asked (needed to read `/sdcard/SurrealEngine`).

## Controls

![Unreal Gold VR controller mapping](docs/controls.webp)

| Input | Action |
| --- | --- |
| Left stick | Move (direction follows snap-turns) |
| Right stick left/right | Snap-turn 45° |
| Right stick up/down | Next / previous weapon |
| Right trigger | Fire (aimed where the crosshair on the gun's ray sits) |
| Right grip | Alt-fire |
| Right A | Enter — activate selected inventory item |
| Right B | F2 — universal translator |
| Left trigger | Jump |
| Left X | ] — select next inventory item |
| Left grip (hold) | Crouch |
| Left Y | Scoreboard |
| Left menu button | Open / close the game menu |
| Meta / Home button long-press | Recenter |

## Known limitations (engine)

* Enemy AI is only partially implemented in Surreal Engine — monsters often just stand around
  or retaliate weakly.
* Inventory does not carry over between maps.
* No dynamic lighting; some movers/semisolid brushes behave oddly.
* PICO support is brand new and untested on hardware.

## Building

See [Docs/AndroidBeta.md](Docs/AndroidBeta.md) for the Quest APK packaging, and
[Docs/Building.md](Docs/Building.md) for desktop builds. In short: Android SDK + NDK 27,
Gradle 8.x, `gradle assembleRelease` in `Projects/Android`.

## Credits and license

* [Surreal Engine](https://github.com/dpjudas/SurrealEngine) by dpjudas and contributors — the
  Unreal Engine 1 reimplementation this port is built on. See [LICENSE.md](LICENSE.md).
* VR/OpenXR approach inspired by [Team Beef](https://www.patreon.com/teambeef)'s Quest ports.
* Unreal and Unreal Gold are trademarks of Epic Games, Inc. This project is not affiliated
  with Epic Games. No game content is distributed here.
