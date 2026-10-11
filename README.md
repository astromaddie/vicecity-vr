# Vice City VR for Steam Frame

This is my Steam Frame fork of [Vice City VR for Quest](https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr-quest), based on [reVC](https://github.com/mrxenginner/reVC). It runs on the headset itself through Lepton, using OpenXR and Vulkan. No gaming PC needed to play.

**EXPERIMENTAL.** I made this for personal use, and it's still a work in progress. You need the original PC game's data from your own copy of GTA Vice City. This is a source build kit; no APK or original game data is included.

I've played it on my Frame. The cinema screen, laser menus, 3D cutscenes and quick first/third-person switching are working.

## What this fork adds

### Views, cutscenes and camera settings

- First-person play, with two third-person options: the whole game on a 3D cinema screen, or an over-the-shoulder view with head tracking and a level horizon.
- A quick first/third-person toggle on View. It returns to whichever third-person style you last selected.
- 3D cinema cutscenes with subtle, normal and deep settings. You can also choose a flat cinema screen or an immersive stereo view inside the shot.
- Live camera previews in the settings menu. Adjust world size, camera distance and height while seeing the effect on the paused game, with the settings panel beside the view.
- Quick camera adjustment with left grip + right stick: left/right changes apparent world size; up/down changes camera distance and height. The stick returns to gameplay after you release the grip and bring it back to centre.
- Snap, smooth or no stick turning, movement direction options, seated calibration and head-movement settings.

### Frame controls and menus

- Frame controller bindings, with A/B/X/Y on the right controller and View and the D-pad on the left. Touch and Index fallback bindings are also included.
- Point-and-click navigation for the game's menus and VR settings, using the right controller's laser and trigger. Buttons still work too.
- Remappable buttons, triggers, grips and stick clicks. Third-person pickup/action is on D-pad Up, leaving the left grip free for camera adjustment.
- View for back, switching views and recentring; Menu for pause; both grips + Menu for the VR settings panel.
- In the standard vehicle layout, grips use the game's Action control and A is the handbrake.

### Display and storage

- An AUTO render scale that targets the Frame's native **2160×2160 per eye**, adapting to SteamVR's recommended eye size. The game renders directly into those eye images.
- A **120 Hz** default refresh request, with the active rate and eye resolution shown in the graphics and locomotion menus. Set ViceCityVR's refresh rate in SteamVR's per-app settings and launch it from the Steam library.
- Game data lookup from an existing Steam installation on the Frame, with a copied PC installation in Documents as the fallback.
- Saves, settings and the port's own files in `Documents/ViceCityVR`, outside Lepton's disposable app storage.

## What comes from the original VR port

The Quest port already supplies the core VR game: stereo rendering, tracked weapons and hands, two-handed aiming, holsters, scopes, room-scale movement, and physical steering for cars and bikes.

It also includes wrist HUD panels, per-weapon and per-seat calibration, articulated ragdolls, optional vehicle dents, traffic and physics settings, cheats, and graphics options such as mipmaps, MSAA and spatial antialiasing. Those features are retained here. The Quest build remains available too.

The [architecture notes](docs/VR_ARCHITECTURE.md) explain the Frame changes. The [Steam Frame guide](docs/STEAM_FRAME.md) covers controls, display settings and development tools in more detail. Upstream changes are in the [changelog](CHANGELOG.md).

## Default Frame controls

| Input | Action |
| --- | --- |
| Right Menu | Pause |
| Both grips + Menu | Open or close VR settings |
| Left View, short press | Switch first/third person during play; Back in menus |
| Left View, held 1 second | Recentre |
| Left grip + right stick, left/right | Adjust apparent world size |
| Left grip + right stick, up/down | Adjust camera distance and height |
| D-pad Up in third person | Pickup / action |
| Bumpers in third person, on foot | Previous / next weapon |
| A in a vehicle | Handbrake |
| Grips in the standard vehicle layout | Action, including radio |

The other gameplay controls come from the original VR port and can be changed under CONTROLS. First-person interactions keep their existing grip actions when those are in use.

## Build the Frame version

Build on macOS with Apple Silicon and Rosetta, or Linux with x86-64 Android tools. You'll need Git, JDK 17 or 21, and the Android SDK packages below. Set `JAVA_HOME` and `ANDROID_HOME` to your own tool installations.

```sh
git clone --branch steam-frame https://github.com/astromaddie/vicecity-vr.git
cd vicecity-vr
export JAVA_HOME="/path/to/jdk"
export ANDROID_HOME="/path/to/android-sdk"
sdkmanager "platforms;android-35" "ndk;27.2.12479018" "cmake;3.22.1"
tools/steam_frame/build.sh --work "$PWD/.frame-build"
```

The script fetches the public reVC source at its pinned revision, applies this kit's patches and builds your APK. Game data isn't needed to compile. The output is:

```text
.frame-build/tree/android/app/build/outputs/apk/frame/app-frame.apk
```

For the original Quest build and manual source assembly, see [BUILDING.md](BUILDING.md).

## Game data and installation

Use the data from your own copy of the original PC version of GTA Vice City. The Frame build can read an existing Steam installation of the game on the headset, or use files copied from a PC installation.

Connect the Frame, enable SSH access and start **Lepton Development** from its Steam library. The deploy script needs `adb`, SSH and `rsync`. Set your own Frame address, then install:

```sh
export FRAME_HOST="steamos@YOUR_FRAME_ADDRESS"
tools/steam_frame/deploy.sh --tree "$PWD/.frame-build/tree" \
    --game-dir "/path/to/Grand Theft Auto Vice City" --register-title --no-launch
```

If the original game is already installed through Steam on the Frame, leave out `--game-dir`. The script still installs the port's own files and registers the title. Launch **ViceCityVR from the Steam library** so SteamVR supplies the controllers and per-app display settings.

Copied game data goes in `~/Documents/ViceCityVR/gamedata`. Saves are in `~/Documents/ViceCityVR/userfiles`, and VR settings are in `gamedata/vr_settings.ini`. Inside Lepton, the same root is `/sdcard/Documents/ViceCityVR`.

## Updating an existing installation

Pull the updated kit, then build into a new work directory so the current patches and overlays are assembled again. Rebuilding an old assembled tree alone won't apply kit updates.

```sh
git pull --ff-only
tools/steam_frame/build.sh --work "$PWD/.frame-build-update"
tools/steam_frame/deploy.sh --tree "$PWD/.frame-build-update/tree" --no-launch
```

Use a new work-directory name for each source update. Keep the same Android signing key when rebuilding, and update the existing installation rather than uninstalling it. Your game data, settings and saves stay in Documents; the deploy script refreshes the port files and the Steam title's APK.

## Credits and licenses

The original VR port is [dubrovskiy-yevhen-stakelogic/vice-city-vr-quest](https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr-quest), built on [reVC](https://github.com/mrxenginner/reVC) and librw. This fork adds the Steam Frame support. Shared camera and VR helpers come from [sfvr](https://github.com/astromaddie/sfvr).

The project's [license](LICENSE) covers its original contributions. Upstream code and third-party files keep their own licenses; see [NOTICE.md](NOTICE.md). These notices don't grant permission to redistribute GTA Vice City's commercial game data, textures, music or audio. Bring your own game data.
