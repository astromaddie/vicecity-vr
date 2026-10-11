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

## 1. Build on a Mac (Apple Silicon) or Linux

You need:

- Git and a JDK 17 or 21 (`brew install openjdk@17`).
- An Android SDK with platform 35, NDK 27.2.12479018 and CMake 3.22.1:

  ```sh
  sdkmanager "platforms;android-35" "ndk;27.2.12479018" "cmake;3.22.1"
  ```

  The NDK ships x86_64 host tools, so a Mac needs Rosetta.

Then run:

```sh
tools/steam_frame/build.sh            # add --quest to also build the Quest debug APK
# -> ~/VCVRFrameBuild/tree/android/app/build/outputs/apk/frame/app-frame.apk
```

The first run fetches reVC at the tested commit and assembles
`~/VCVRFrameBuild/tree` with `tools/assemble.sh`. It then commits that tree as
a local git baseline. Later runs rebuild the tree in place; a clean build takes
about a minute.

To develop, edit inside the tree, rebuild, and copy the work back into the kit:

```sh
tools/steam_frame/sync-from-tree.sh ~/VCVRFrameBuild/tree
```

The sync script refuses to copy a changed reVC file that `overlay/` does not
carry; that change needs a patch. Delete the tree to reassemble it from the kit.

## 2. Install and play

Setup is done once. Install Grand Theft Auto: Vice City from Steam on the Frame
if you haven't (app 12110). The game reads it from the Steam library in place,
so there is nothing to copy. Start **Lepton Development** from the Frame's Steam
library (adb runs over it), then run:

```sh
tools/steam_frame/deploy.sh --register-title
```

When the game starts it looks for Vice City in the Steam library (every library
listed in `libraryfolders.vdf`, the install folder from the appmanifest). Lepton
can read Steam's folder but not write to it, which is fine: saves, settings and
the port's own files all live in Documents.

If Vice City isn't installed in Steam on the Frame, the game uses a copy of the
data in `~/Documents/ViceCityVR/gamedata` instead. Make one from a PC install:

```sh
tools/steam_frame/deploy.sh --game-dir "/path/to/Grand Theft Auto Vice City" --register-title
```

`--from-steam` still works too. It copies the Steam install into Documents on
the headset as a fallback; you don't need it any more. A copy you already made
keeps working: when both exist the game prefers Steam, and uses the copy for
anything Steam doesn't have.

A deploy does these things:

- Installs the port's own files in `~/Documents/ViceCityVR/gamedata`: the
  replacement text, frontend art, VR hands, `neo/` and scripts (the tree's
  `gamefiles/`). The game always reads these from Documents, never from Steam, so
  every deploy that installs an APK refreshes them (they are tied to the
  build). `--port-assets` does only that, `--no-port-assets` skips it.
- Installs the APK in Lepton Development.
- Registers the persistent **ViceCityVR** title in the Steam library (the
  `--register-title` part, only the first time).
- With `--game-dir` or `--from-steam`, first copies the retail folders the game
  reads into Documents over SSH: `data TEXT anim txd skins mp3 movies models
  Audio`.

To see which one the game picked, look for `[vr] game data:` in the log. It says
either `steam <folder>` or `Documents copy <folder> (no Steam install of app
12110 found)`, followed by the Documents folder that holds the port's files,
settings and saves.

Saves and settings go to `~/Documents/ViceCityVR/userfiles` and
`gamedata/vr_settings.ini`. All of it survives a Lepton reset.

After the first setup, `tools/steam_frame/deploy.sh` installs the new APK and
refreshes the title's copy. To play, **launch ViceCityVR from the Steam
library**. A game started by adb gets no controllers and no focus.

`FRAME_HOST` is required and must name the user's own Frame SSH target, for
example `steamos@frame.local`. Do not copy a host value from another setup.
`FRAME_ADB_HOST` sets the adb host. `deploy.sh --help` lists the rest.

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
