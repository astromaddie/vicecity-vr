# Vice City VR for Steam Frame

This is my Steam Frame fork of [Vice City VR for Quest](https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr-quest), based on [reVC](https://github.com/mrxenginner/reVC). It runs on the headset itself through Lepton, using OpenXR and Vulkan. No gaming PC needed to play.

**EXPERIMENTAL.** I made this for personal use, and it's still a work in progress. You need the original PC game's data from your own copy of GTA Vice City. This repository includes the source build kit; the prebuilt package is linked below.

**Want to play on Steam Frame?** Download the prebuilt standalone package from the [v0.1.0 prerelease](https://github.com/astromaddie/vicecity-vr/releases/tag/v0.1.0). No compiler is needed. You still need data from your own PC copy of Grand Theft Auto: Vice City; it is not included.

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

Build on x86-64 Linux, macOS, or x86-64 Ubuntu under Windows WSL2. The Frame is the ARM64 run target, not the build host. The NDK's Linux host tools are x86-64; Apple Silicon Macs need Rosetta for the pinned x86-64 tools.

On Windows, install Ubuntu with [WSL2](https://learn.microsoft.com/windows/wsl/install), then install a Linux JDK 17 or 21 and the Linux Android SDK Command-Line Tools inside Ubuntu. For example, install OpenJDK with `sudo apt update && sudo apt install -y openjdk-17-jdk`, and download/extract the Linux SDK Command-Line Tools under `$HOME/Android/Sdk`. Set `JAVA_HOME` and `ANDROID_HOME` to those Linux installations, not Windows paths, and keep the checkout under the Linux home (for example, `~/src/vicecity-vr`). See Android's [SDK command-line tools](https://developer.android.com/tools/sdkmanager) and [NDK downloads](https://developer.android.com/ndk/downloads). The [detailed build/install guide](docs/STEAM_FRAME.md) has the Frame setup and install steps.

You'll need Git, JDK 17 or 21, Android SDK command-line tools (`sdkmanager`), platform 35, NDK 27.2.12479018 and CMake 3.22.1. Set `JAVA_HOME` and `ANDROID_HOME` to your own installations. The SDK is commonly at `$HOME/Library/Android/sdk` on macOS and `$HOME/Android/Sdk` on Linux or WSL2. Run all commands in Ubuntu Bash under WSL2; do not build from PowerShell. After installing the packages, accept their licenses with `sdkmanager --licenses`.

```sh
mkdir -p "$HOME/src"
cd "$HOME/src"
git clone --branch steam-frame https://github.com/astromaddie/vicecity-vr.git
cd vicecity-vr
export JAVA_HOME="/path/to/jdk-17-or-21"
export ANDROID_HOME="/path/to/android-sdk"
export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$PATH"
sdkmanager --sdk_root="$ANDROID_HOME" \
  "platforms;android-35" "ndk;27.2.12479018" "cmake;3.22.1"
sdkmanager --licenses
tools/steam_frame/build.sh --work "$PWD/.frame-build"
```

The script fetches the public reVC source at its pinned revision, applies this kit's patches and builds your APK. Game data isn't needed to compile. The output is:

```text
.frame-build/tree/android/app/build/outputs/apk/frame/app-frame.apk
```

For the original Quest build and manual source assembly, see [BUILDING.md](BUILDING.md).

## Game data, install and play

Use the data from your own copy of the original PC version of GTA Vice City. The Frame build can read an existing Steam installation of the game on the headset, or use files copied from a PC installation.

The first Steam library entry is created with Valve's public [SteamOS Devkit Client Title Upload](https://partner.steamgames.com/doc/steamhardware/steamframe/loadgames). Pair the Frame with the client, then upload a folder containing the APK with **Name** `ViceCityVR`, **Local Folder** set to that folder, **Start Command** `ViceCity-VR.apk`, and **Runtime** `Android`. For example, stage the APK like this before selecting the folder in the client:

```sh
mkdir -p .frame-upload/vicecity
cp .frame-build/tree/android/app/build/outputs/apk/frame/app-frame.apk \
  .frame-upload/vicecity/ViceCity-VR.apk
```

On Windows, copy `.frame-upload/vicecity` to your Windows Downloads folder (available inside WSL at `/mnt/c/Users/YOUR_WINDOWS_USERNAME/Downloads/`), then select that copied folder in the Devkit Client.

To copy files, enable SSH access to the Frame and set its own address. The build computer needs OpenSSH and `rsync`. If Vice City is installed from Steam on the Frame, copy the port's build-matched files into Documents:

```sh
export FRAME_HOST="steamos@FRAME_HOST_OR_IP"
tools/steam_frame/deploy.sh --tree "$PWD/.frame-build/tree" \
  --port-assets --no-adb --no-install --no-launch
```

If you use a PC installation instead, supply its folder to copy the retail data and port files together:

```sh
tools/steam_frame/deploy.sh --tree "$PWD/.frame-build/tree" \
  --game-dir "/mnt/c/Program Files (x86)/Steam/steamapps/common/Grand Theft Auto Vice City" \
  --no-adb --no-install --no-launch
```

The SteamOS Devkit Client creates the first persistent title. Later builds can refresh its APK from SSH when the title directory exists; run `tools/steam_frame/deploy.sh --tree "$PWD/.frame-build/tree" --no-adb --no-launch`. If the client stores the title under a different directory, upload the new APK through the client again. The deploy script does not need Lepton Development for this SSH-only route. For optional ADB development installs, add Android SDK platform-tools (`adb`) to the build computer and start **Lepton Development**. Launch **ViceCityVR from the Steam library** so SteamVR supplies the controllers and per-app display settings.

Copied game data goes in `~/Documents/ViceCityVR/gamedata`. Saves are in `~/Documents/ViceCityVR/userfiles`, and VR settings are in `gamedata/vr_settings.ini`. Inside Lepton, the same root is `/sdcard/Documents/ViceCityVR`.

## Updating an existing installation

Pull the updated kit, then build into a new work directory so the current patches and overlays are assembled again. Rebuilding an old assembled tree alone won't apply kit updates.

```sh
git pull --ff-only
tools/steam_frame/build.sh --work "$PWD/.frame-build-update"
tools/steam_frame/deploy.sh --tree "$PWD/.frame-build-update/tree" --no-adb --no-launch
```

Use a new work-directory name for each source update. Keep the same Android signing key when rebuilding, and update the existing Steam title rather than creating a second entry. Your game data, settings and saves stay in Documents; the deploy script refreshes the port files and the Steam title's APK.

## Credits and licenses

The original VR port is [dubrovskiy-yevhen-stakelogic/vice-city-vr-quest](https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr-quest), built on [reVC](https://github.com/mrxenginner/reVC) and librw. This fork adds the Steam Frame support. Shared camera and VR helpers come from [sfvr](https://github.com/astromaddie/sfvr).

The project's [license](LICENSE) covers its original contributions. Upstream code and third-party files keep their own licenses; see [NOTICE.md](NOTICE.md). These notices don't grant permission to redistribute GTA Vice City's commercial game data, textures, music or audio. Bring your own game data.
