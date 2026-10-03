# Vice City VR for Meta Quest — source build kit

> **NO APK IS PROVIDED. THIS IS A SOURCE-BUILD RELEASE.**
>
> On Windows, connect the Quest and double-click **`BUILD_AND_INSTALL.bat`**.
> On Linux, connect the Quest and run **`./BUILD_AND_INSTALL.sh`**.
> The wizard obtains portable Git, JDK 21, the Android command-line SDK,
> the exact reVC source, assembles the port, builds your personal APK, installs
> it without clearing existing data, and can copy only the required folders
> from your legally owned Vice City installation.
> Missing build tools and SDK packages are downloaded from their official
> sources and verified against pinned SHA256 hashes automatically. Google SDK
> licenses are still shown for the user to accept.
> If an older installation has an incompatible signing key, the wizard warns
> that replacing it can erase saves/game data and uninstalls it only after an
> explicit `Y` confirmation. It validates the supplied Vice City PC folder
> before removal so the required game data can be restored afterward.
> You may also download only **`BUILD_AND_INSTALL.bat`** (Windows) or
> **`BUILD_AND_INSTALL.sh`** (Linux). If the rest of the
> source kit is not beside it, the wrapper downloads and extracts the complete
> public repository automatically.
>
> Run the wrapper (`.bat` on Windows, `.sh` on Linux), not the internal
> wizard file. The window remains open on
> errors and writes `%TEMP%\ViceCityVR-Build-And-Install.log` (Windows) or
> `$TMPDIR/ViceCityVR-Build-And-Install.log` (Linux); send that log
> when asking for help.


> [!TIP]
> **Join the Flat2VR Discord!** Development updates, player feedback, testing,
> and discussion of the mod take place in the
> [Vice City VR discussion channel](https://discord.com/channels/747967102895390741/1543691482861408276).
> Join the Flat2VR server first if the channel link does not open for you.

This repository contains the original Quest/OpenXR port layer, Vulkan backend
changes, build scripts and reVC patch files needed to build Vice City VR for a
Meta Quest headset.

Current source-kit version: **v0.5.6**.

This release adds articulated ragdolls with car and wall contact, optional
vehicle body deformation, clearer menus and fixes for tracked weapon aiming,
melee, vehicle entry and seat centering. Existing graphics and calibration
preferences are preserved. See [the changelog](CHANGELOG.md) for the complete
0.5.6 changes and [performance notes](PERFORMANCE.md) for the retained rendering
optimizations.

It intentionally contains **no APK, complete reVC source tree, original game
files, saves, logs or third-party Modern model packs**. Every user builds their
own APK and supplies data from a legally owned PC copy of GTA Vice City.

## Updating an existing installation

After the initial setup, keep this folder and run **`UPDATE.bat`** on Windows
or **`./UPDATE.sh`** on Linux. There is no need to download the source ZIP again
for each release. Connect and authorize the Quest before installing the update.

The updater fetches newer source and reuses the existing tool and dependency
cache. From a ZIP installation, its first run creates a managed Git checkout
under the build work directory; later runs update that checkout. Each APK is
assembled in a new directory, preserving earlier builds and local files.

Updates replace only the APK. Saves, settings, game data and optional model
overlays stay in place. An incompatible signing key stops the update instead
of uninstalling the game. If a future release requires new game assets, the
updater asks you to use the ordinary installer for that separate step.

The default builds a personal debug APK using the same Android debug key as
the original wizard. To update an app signed with your release key, use
`-Release` / `--release` and set `MIAMIVR_RELEASE_SIGNING_PROPERTIES` to your
private signing configuration. It must use the installed app's signing key;
no signing keys are included in this source kit.

Use the same work directory as the original installation to reuse its cache:

```powershell
.\UPDATE.bat -WorkDir C:\VCVRBuild
```

```sh
./UPDATE.sh --work-dir "$HOME/VCVRBuild"
```

The Git update is fast-forward only. Local changes or a diverged checkout stop
it; it does not reset, stash or clean your files. `-BuildOnly` / `--build-only`
builds without installing, and `-DryRun` / `--dry-run` checks the update plan
without fetching, downloading, building or accessing the headset.

## What is included

| Path | Contents |
|---|---|
| `overlay/` | New Android, OpenXR, Quest VR and tooling files |
| `patches/` | Changes applied to a user-supplied reVC checkout |
| `librw/` | MIT-licensed librw fork with the Quest Vulkan backend |
| `tools/` | Windows and Unix source-assembly scripts |

Modified upstream reVC files are distributed as patches, not as a second copy
of the reVC repository. The only bundled runtime art is the credited MIT-licensed VR
hand asset under `overlay/gamefiles/models/vrhands/`.

## Required external source

The exact patch base comes from the public upstream reVC project:

<https://github.com/mrxenginner/reVC>

The tested public `miami` commit is
`026cd10f3fdbd92c089830e5067c4457c53c1b51`. The assembly scripts never modify
that checkout: they copy it to a new directory, apply the public-base
compatibility patch followed by this project's patches, and add Quest-only
files. No private repository is required or referenced.

Complete prerequisites and copy-paste commands are in
[BUILDING.md](BUILDING.md).

For most Windows users, the recommended route is the guided
[`BUILD_AND_INSTALL.bat`](BUILD_AND_INSTALL.bat) wizard (Linux:
[`BUILD_AND_INSTALL.sh`](BUILD_AND_INSTALL.sh)) instead of entering
those commands manually.

If the OPTIONS menus show entries like `FET_GFX missing` or `FED_AAS missing`
instead of labels, the installation predates the wizard copying the port's
replacement text and frontend files.
[`REPAIR_GAME_FILES.bat`](REPAIR_GAME_FILES.bat) (Windows) or
[`REPAIR_GAME_FILES.sh`](REPAIR_GAME_FILES.sh) (Linux) installs just those onto an
existing headset; saves, settings and models are left alone.

The source-only maintainer checks are in [RELEASING.md](RELEASING.md).

## Valve Steam Frame

A Frame build of the same port runs in the Frame's Android container
(Lepton). It adds Frame controller bindings, a View-button recentre, and data
kept in the headset's `~/Documents/ViceCityVR`. Build it on a Mac or Linux
with `tools/steam_frame/build.sh`, then install it with
`tools/steam_frame/deploy.sh`. See [docs/STEAM_FRAME.md](docs/STEAM_FRAME.md).
The Frame build is new and has not yet been played in the headset.

## Current Quest build

- Native arm64 Android application using OpenXR and Vulkan multiview.
- Room-scale movement, physical weapons, two-hand support, holsters, scoped
  aiming and physical steering for cars and motorcycles.
- Remappable face buttons, grips and stick clicks; a swapped-hands layout;
  per-weapon and per-seat calibration.
- In-headset controls, calibration, categorized cheats, traffic, ragdoll,
  graphics and model-set menus.
- Articulated ragdolls respond to bullets, cars and nearby world geometry.
  Body weight, car braking, grip and bullet response can be tuned in the menu.
- Optional vehicle deformation affects opaque body panels, bumpers and grilles.
  It is a bounded visual effect; severe repeated impacts can open component seams.
- Stereo cutscenes with selectable staged cameras, with CINEMA also available.
- Generated texture mipmaps without modifying the player's TXD files;
  OFF/2X/4X multisampling and spatial antialiasing.
- Optional dynamic lights, vehicle reflections, modern water and PS2-style
  two-pass alpha. These increase GPU work and remain off by default.
- Stereo-aware and authored building culling, with an OFF fallback. Authored
  culling is experimental; select STEREO SAFE or OFF if geometry disappears.
- Physics Director V2 with an ORIGINAL/OFF fallback; separate pedestrian and
  vehicle density controls from OFF to 300%. OFF prevents new spawns rather
  than deleting existing traffic.
- Classic/Modern asset categories. Classic vehicles remain the default;
  generated Modern overlays are optional and are not distributed here.

Defaults retain 100% render scale and sustained CPU/GPU performance hints.
Normal frontend and save loading are used on startup. Experimental temporal AA,
SGSR and runtime-only foveation remain disabled.

Runtime/data layout is documented in
[QUEST_PORT.md](overlay/docs/QUEST_PORT.md). See
[QUEST_GAMEPLAY.md](overlay/docs/QUEST_GAMEPLAY.md) for gameplay changes,
[QUEST_RAGDOLL.md](overlay/docs/QUEST_RAGDOLL.md) for ragdoll behavior and limits,
[QUEST_VEHICLE_DEFORMATION.md](overlay/docs/QUEST_VEHICLE_DEFORMATION.md) for
body deformation, and [QUEST_MODELSETS.md](overlay/docs/QUEST_MODELSETS.md)
for optional model mixing.

## Install the optional Modern models after the APK

After building/updating the APK to 0.5.6, no additional APK rebuild is needed
for asset selection. Connect and authorize the Quest, then double-click
**`INSTALL_MODERN_MODELS.bat`** (Windows) or run **`./INSTALL_MODERN_MODELS.sh`**
(Linux). Select the folder containing a legal original
GTA Vice City PC installation when asked. That is the only asset input the
player must supply. On Linux, build `tools/modelsets/txdcompress` from source
first: `g++ -O2 -o txdcompress txdcompress.cpp`.

The wizard downloads and verifies the two Modern packs and the Xbox vehicle
pack, builds separate `modelsets/modern` and `modelsets/xbox` overlays, and
installs them on the connected Quest. Expect about 4 GB of downloads and keep
at least 24 GB free for downloads, extraction and the staged build. Interrupted
downloads are retained and resumed. If installation fails after the local build
finishes, run the same BAT again: it detects the completed overlay and retries
only the Quest transfer. Verified downloads and extractions are also reused
instead of being downloaded again. These are the exact external inputs:

- [GTA VC HD + Weapons](https://drive.google.com/file/d/1Swe1dVWDnKz8ad51y8L0ihPWVCxmFRYj/view)
- [Mods / Atmosphere](https://drive.google.com/file/d/1y9KpKjLSna76bjz1Lf2DzP0G4AnkN_2d/view)
- [Fixed Xbox Vehicles 1.3](https://drive.google.com/file/d/1wqUggAT4VJGW6RHJiXhft8Ni8uxe3N5v/view) (35 MB; separate vehicle profile)

Xbox vehicles are installed alongside Modern in `modelsets/xbox`; they do not
replace the Modern car pack. Select **XBOX** for **VEHICLES** under
`VR MENU > SETTINGS > MODEL ASSETS`, then fully restart the game. The installer leaves
your current selections unchanged. Modern world textures and weapons can be
combined with Xbox vehicles.

To download and prepare only the compact Xbox profile, avoiding the 4 GB Modern
downloads, run `INSTALL_MODERN_MODELS.bat -XboxOnly` on Windows or
`./INSTALL_MODERN_MODELS.sh --xbox-only` on Linux. Add `-BuildOnly` /
`--build-only` to prepare files without touching the Quest. `-SkipXbox` /
`--skip-xbox` retains the Modern-only workflow. Linux Xbox preparation needs
Python 3, a C++ compiler, and a RAR reader such as `bsdtar` (`libarchive-tools`),
7-Zip, or `unrar`; Windows uses its built-in `tar.exe`.

The HD vegetation/LibertyCity pack is deliberately **not** downloaded or used.
The builder also removes matching palm/tree geometry found in the HD base and
generates a manifest that routes those models to the original Classic assets.
No original game data or third-party pack is bundled in this repository, and
the generated overlay must not be redistributed.

Before replacing an existing overlay, the installer validates the complete
build, uploads it through a temporary directory, and verifies the required
files on the headset, including SHA256 checks for the loose wheel model and
texture dictionary. If the bundled builder version changes, the BAT safely
rebuilds the local overlay and atomically replaces the old Quest folder only
after all checks pass. Cached downloads/extractions are reused. A failed
download, build or transfer does not silently replace a working Modern folder.

The following manual PowerShell method remains available for troubleshooting.
Change `$modern` to the actual generated folder on the PC. The `$adb` path is
the default created by `BUILD_AND_INSTALL.bat`; use the matching path if
`-WorkDir` was changed.

```powershell
$adb = "C:\VCVRBuild\.android-sdk\platform-tools\adb.exe"
$modern = "C:\Games\Vice City VR\modelsets\modern"

& $adb devices
& $adb shell am force-stop com.miamivr.quest
& $adb shell mkdir -p /sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets
& $adb push $modern /sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets
& $adb shell ls /sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets/modern/models
& $adb shell ls /sdcard/Android/data/com.miamivr.quest/files/gamedata/modelsets/modern/vegetation_models.txt
```

For troubleshooting, an already generated folder can still be transferred by
running `tools\install-modern-models.ps1 -ModernDir "C:\path\to\modern"`
(Windows) or `tools/install-modern-models.sh --modern-dir /path/to/modern`
(Linux).
The final Quest path must be exactly
`gamedata/modelsets/modern`, never `modern/modern`. Fully restart the game after
copying. The default/recommended Quest mix is Modern world textures and
weapons, with Classic vehicles, pedestrians and vegetation. Other categories
can be changed under `VR MENU > SETTINGS > MODEL ASSETS`; every change requires a full
process restart. Modern vehicles are GPU-heavy, particularly with high traffic,
and vegetation is intentionally kept Classic.

## Distribution boundary

Do not upload assembled source trees, APKs, original game files, saves, built
Modern model overlays or signing keys to this repository. Publish only this
build kit. Each player must obtain reVC and the original game separately and
perform the documented build locally.

## No affiliation

This is an unofficial fan project. It is not affiliated with or endorsed by
the publishers or developers of GTA Vice City, Meta, Khronos, Qualcomm or
NVIDIA. All trademarks belong to their owners.
