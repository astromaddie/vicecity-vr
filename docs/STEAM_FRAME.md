# Vice City VR on the Valve Steam Frame

The Frame runs Android apps in **Lepton**, an Android 11 (API 30) container
with arm64, Vulkan 1.1 (Turnip, Adreno 750) and SteamVR's OpenXR runtime. This
kit's Quest build already uses the Khronos loader, Vulkan through
`XR_KHR_vulkan_enable2` and no Meta-only API, so the Frame build is that same
build with a Gradle build type, `frame`, on top. The Quest builds are
unchanged. What differs, and why, is in [VR_ARCHITECTURE.md](VR_ARCHITECTURE.md).

The game runs on the Frame. The cinema screen, laser menu navigation, 3D cutscenes
and quick first/third-person switching have been played and confirmed. The current
fork also includes live camera previews, grip-and-stick camera adjustment, an
AUTO target of 2160×2160 per eye and a 120 Hz refresh request. See the
[README](../README.md) for the feature overview.

## 1. Build the APK on a computer

Use x86-64 Linux, macOS, or x86-64 Ubuntu under Windows WSL2. The Frame is the ARM64 run target, not the build host; the NDK's Linux host tools are x86-64. Apple Silicon Macs need Rosetta for the pinned x86-64 NDK tools.

On Windows, install Ubuntu with [WSL2](https://learn.microsoft.com/windows/wsl/install), then install a Linux JDK 17 or 21 and the Linux Android SDK Command-Line Tools inside Ubuntu. For example, install OpenJDK with `sudo apt update && sudo apt install -y openjdk-17-jdk`, and download/extract the Linux SDK Command-Line Tools under `$HOME/Android/Sdk`. Keep `JAVA_HOME`, `ANDROID_HOME` and the checkout in the Linux filesystem (for example, `~/src/vicecity-vr`), not on a Windows-mounted path. The SDK is commonly `$HOME/Android/Sdk` on Linux or WSL2. See Android's [SDK command-line tools](https://developer.android.com/tools/sdkmanager) and [NDK downloads](https://developer.android.com/ndk/downloads).

Install Git, JDK 17 or 21, Android SDK command-line tools (`sdkmanager`), platform 35, NDK 27.2.12479018 and CMake 3.22.1. The SDK is commonly `$HOME/Library/Android/sdk` on macOS and `$HOME/Android/Sdk` on Linux or WSL2.

```sh
export JAVA_HOME="/path/to/jdk-17-or-21"
export ANDROID_HOME="/path/to/android-sdk"
export PATH="$ANDROID_HOME/cmdline-tools/latest/bin:$PATH"
sdkmanager --sdk_root="$ANDROID_HOME" \
  "platforms;android-35" "ndk;27.2.12479018" "cmake;3.22.1"
sdkmanager --licenses
tools/steam_frame/build.sh --work "$PWD/.frame-build"
```

The first run fetches reVC at the pinned revision, assembles it with this kit's patches and builds the Frame APK. Later runs rebuild the assembled tree in place; delete the tree only when you need to assemble it again from the current kit.

The APK is `.frame-build/tree/android/app/build/outputs/apk/frame/app-frame.apk`. This compiles without GTA Vice City data. Historical build evidence and its exact checked revision are in [PR0 readiness](PR0_READINESS.md).

## 2. Create the Steam title, copy game files and play

The first persistent library entry is created with Valve's public [SteamOS Devkit Client Title Upload](https://partner.steamgames.com/doc/steamhardware/steamframe/loadgames). Pair the Frame with the Devkit Client, then upload a folder containing the APK with **Name** `ViceCityVR`, **Local Folder** set to that folder, **Start Command** `ViceCity-VR.apk`, and **Runtime** `Android`:

```sh
mkdir -p .frame-upload/vicecity
cp .frame-build/tree/android/app/build/outputs/apk/frame/app-frame.apk \
  .frame-upload/vicecity/ViceCity-VR.apk
```

On Windows, copy `.frame-upload/vicecity` to your Windows Downloads folder (available inside WSL at `/mnt/c/Users/YOUR_WINDOWS_USERNAME/Downloads/`), then select that copied folder in the Devkit Client.

Install the original PC game through Steam on the Frame if you want the app to read it directly (Steam app 12110). The game searches Steam libraries and reads the install in place. The port's own files still need to be copied into Documents. Enable SSH to the Frame, set its address, and run the helper from the build computer; it uses SSH and `rsync`, so this route does not need Lepton Development or ADB:

```sh
export FRAME_HOST="steamos@FRAME_HOST_OR_IP"
tools/steam_frame/deploy.sh --tree "$PWD/.frame-build/tree" \
  --port-assets --no-adb --no-install --no-launch
```

If you use a PC game installation instead, copy its retail folders and the matching port files into Documents:

```sh
tools/steam_frame/deploy.sh --tree "$PWD/.frame-build/tree" \
  --game-dir "/mnt/c/Program Files (x86)/Steam/steamapps/common/Grand Theft Auto Vice City" \
  --no-adb --no-install --no-launch
```

With no Steam install, the copied files go in `~/Documents/ViceCityVR/gamedata`. The game reads retail folders `data TEXT anim txd skins mp3 movies models Audio`; the port's replacement text, frontend art, VR hands, `neo/` and scripts are installed there too. `--from-steam` can copy the Frame's Steam installation to Documents as a fallback. Saves and settings are in `~/Documents/ViceCityVR/userfiles` and `gamedata/vr_settings.ini`, which survive a Lepton reset. The log line `[vr] game data:` identifies whether the live game used a Steam path or the Documents copy.

The build computer needs OpenSSH and `rsync` for SSH transfers. Set `FRAME_HOST` to your own Frame SSH target. After creating the title, the helper can refresh its APK when the title directory exists, along with the port files. For SSH-only updating, run `tools/steam_frame/deploy.sh --tree "$PWD/.frame-build/tree" --no-adb --no-launch`; if the Devkit Client stored the title elsewhere, upload the new APK through that client. For optional ADB development installs, add Android SDK platform-tools (`adb`), start **Lepton Development**, and run the helper without `--no-adb`; `FRAME_ADB_HOST` can override the ADB host. ADB-started games do not receive the Steam title's controller focus. To play, launch **ViceCityVR** from the Frame's Steam library so SteamVR supplies controller input and per-app display settings.

## 3. Controls (Frame)

| Input | Action |
| --- | --- |
| R Menu | Pause (Start) |
| L View, short press | Back in the VR menu and frontend; during play, switch first / third person |
| L View, hold 1 s | Recentre (buzz on both hands) |
| L D-pad | The game's D-pad; Up is pickup/action in third person |
| Both grips + Menu | VR menu (settings, controls, cheats, graphics) |
| Left grip + right stick | Adjust world size horizontally, camera distance and height vertically |
| A B X Y, sticks, triggers, grips | As on the Quest. X and Y are now on the right controller |
| Grips in a vehicle | The game's Action control (L1, "pick up / radio"), same as the left grip. Not the handbrake |
| A in a vehicle | Handbrake (it still is) |

The old both-stick-clicks recentre is off on the Frame. The stick clicks stay
game actions.

**Grips in a vehicle.** The right grip used to be the handbrake as well as A,
which is not what a grip is for here. In a car or on a bike both grips now press
L1, the game's Action control, in pad modes 0 and 1. In pad modes 2 and 3 L1
leaves the vehicle or is the handbrake, so the grips do nothing there. On foot
nothing changed: the grips go through your CONTROLS bindings. The VR driving
gestures (hands on the wheel, grips with B for the drive-by window) are as before.

**Third person.** Press View briefly during play to switch between first person and third person. Pick the third-person style in the VR menu under LOCOMOTION > VIEW:

- **THIRD: 3D SCREEN**: all gameplay, on foot and in vehicles, plays on the cinema screen from the game's own camera. It uses the same 3D depth as the cutscenes. Your view never moves on its own.
- **THIRD: OVER SHOULDER** (the default): you stand where the game's chase camera is, with full head tracking and a level horizon. The camera turns only with your own snap or smooth turns. Vehicles use the vehicle third-person view. Entering or leaving a car holds the view still.

On foot in third person:

| Input | Action |
| --- | --- |
| Right trigger | Fire |
| Left trigger | Aim / target |
| Bumpers | Previous / next weapon |
| Face buttons, grips | Your CONTROLS bindings |

Tommy is visible, the VR hands and wrist panels are off, and the normal HUD shows. Movement follows the camera, not your head.

**Cutscenes** default to **CINEMA 3D**: the cinema screen keeps its frame, and
the scene has depth behind it, like a 3D film. To change it, open the VR menu
and go to LOCOMOTION > CUTSCENES. The row steps through CINEMA SCREEN (flat),
CINEMA 3D SUBTLE, CINEMA 3D, CINEMA 3D DEEP, and STEREO (immersive, inside the
shot).

During a cutscene with a ready flyby camera, hold physical **A** for half a
second to skip. A white ring fills clockwise around the A label beside “Hold to
skip” at the bottom right. A tap does not skip. The prompt appears only when
the native cutscene camera can safely finish; “Finale” and scenes without a
loaded flyby camera remain unskippable. Menu, View, either grip, pause, loss of
focus, loading, or leaving that camera cancels the hold and requires A release
before trying again. Completion uses the native `FinishCutscene()` path, which
preserves the mission script handoff.

## 3a. Display: native 2160×2160 per eye and 120 Hz

The Frame build defaults to **AUTO** render scale, targeting the Frame's native
2160×2160 per eye. AUTO adapts to OpenXR's recommended eye size:

| SteamVR recommends | AUTO scale | Eye image |
| --- | --- | --- |
| 1728×1728 | 125% | 2160×2160 |
| 2160×2160 | 100% | 2160×2160 |

The percentage is relative to the runtime's recommendation. The game renders
directly into those eye images. AUTO is clamped to 100–175%; a recommendation
above 2160 is kept at 100%.

Set **ViceCityVR to 120 Hz** in SteamVR's per-app settings and launch it from the
Steam library. SteamVR controls the actual display rate. The app's default
refresh request is 120 Hz; the graphics menu shows `DISPLAY`, and the locomotion
menu shows `ACTIVE`, for the rate currently in use.

GRAPHICS > OUTPUT offers AUTO, 100, 125, 150 and 175%. Changes to output resolution
and MSAA apply after restarting. Saved choices are preserved.

## 3b. Live camera settings

Open the VR settings panel to adjust apparent world size, camera distance and
height, turning, seated calibration and head movement. The paused game remains
visible as a live camera preview, with the settings panel beside it. The panel
can be placed on the right, left or centre.

For a quick adjustment during play, hold the physical left grip and move the
physical right stick: left/right changes world size; up/down changes camera
distance and height. Release the grip and centre the stick before resuming normal
turning. Existing first-person grip interactions and custom bindings retain their
game action when in use.

## 4. Developer diagnostics

Frame-only diagnostic settings live in `~/Documents/ViceCityVR/vr-diag.ini`.
They are useful for capturing eye images and checking the renderer without
playing through a scene. Remove the overrides with `--diag-reset` before normal
play. Don't launch a diagnostic while someone else is using the headset.

| Key | Default | Effect |
| --- | --- | --- |
| `force_render` | 0 | Render and step using a synthetic head pose even without normal session focus |
| `diag_yaw` | 0 | Yaw of the synthetic head, in degrees |
| `dump_frame` | 0 | Capture the two eyes, menu image and state on the selected frame |
| `diag_new_game` | 0 | Start a new game from the title menu after about 5 seconds |

```sh
tools/steam_frame/deploy.sh --clear-dumps --diag force_render=1 --diag dump_frame=600 \
    --diag diag_yaw=90 --logcat 40 --pull-dumps scratchpad/dumps/yaw90
python3 tools/steam_frame/tests/check_bindings.py
tools/steam_frame/deploy.sh --diag-reset --no-install --no-launch
```

Log tags are `MiamiVR`, `MiamiVR-game` and `librw-vk`. The Frame's own lines start
with `[vr]`, `[vr-perf]` and `[vr-dump]`. Renderer, menu-pointer and file-resolver
details are in [VR_ARCHITECTURE.md](VR_ARCHITECTURE.md).
