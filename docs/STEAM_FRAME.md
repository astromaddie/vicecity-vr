# Vice City VR on the Valve Steam Frame

The Frame runs Android apps in **Lepton**, an Android 11 (API 30) container
with arm64, Vulkan 1.1 (Turnip, Adreno 750) and SteamVR's OpenXR runtime. This
kit's Quest build already uses the Khronos loader, Vulkan through
`XR_KHR_vulkan_enable2` and no Meta-only API, so the Frame build is that same
build with a Gradle build type, `frame`, on top. The Quest builds are
unchanged. What differs, and why, is in [VR_ARCHITECTURE.md](VR_ARCHITECTURE.md).

Status: it plays on the Frame (first wear, 2026-10-03). See section 5.

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

`FRAME_HOST` sets the SSH target. The default is `steamos@192.168.1.31`, the
Frame on Wi-Fi; over USB networking use `steamos@10.86.200.233`.
`FRAME_ADB_HOST` sets the adb host. `deploy.sh --help` lists the rest.

## 3. Controls (Frame)

| Input | Action |
| --- | --- |
| R Menu | Pause (Start) |
| L View, short press | Back in the VR menu and frontend; during play, switch first / third person |
| L View, hold 1 s | Recentre (buzz on both hands) |
| L D-pad | The game's D-pad |
| Both grips + Menu | VR menu (settings, controls, cheats, graphics) |
| A B X Y, sticks, triggers, grips | As on the Quest. X and Y are now on the right controller |

The old both-stick-clicks recentre is off on the Frame. The stick clicks stay
game actions.

**Third person.** Press View briefly during play to switch between first person and third person. Pick the third-person style in the VR menu under LOCOMOTION > VIEW:

- **THIRD: 3D SCREEN** (the default): all gameplay, on foot and in vehicles, plays on the cinema screen from the game's own camera. It uses the same 3D depth as the cutscenes. Your view never moves on its own.
- **THIRD: OVER SHOULDER**: you stand where the game's chase camera is, with full head tracking and a level horizon. The camera turns only with your own snap or smooth turns. Vehicles use the vehicle third-person view. Entering or leaving a car holds the view still.

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

## 4. Unattended testing (nothing worn)

A headset on a desk reports `shouldRender=false`. In that state nothing draws
and the game does not step. Frame-only switches live in
`~/Documents/ViceCityVR/vr-diag.ini`. Set them with `deploy.sh --diag KEY=VAL`
and remove them with `--diag-reset` before playing:

| Key | Default | Effect |
| --- | --- | --- |
| `force_render` | 0 | Render and step the game even with `shouldRender=false` or before focus. A synthetic head stands at 1.6 m, with eyes at ±32 mm |
| `diag_yaw` | 0 | Yaw of the synthetic head, in degrees (positive turns left) |
| `dump_frame` | 0 | On the Nth rendered frame, write `vr-dumps/vr-eye0.bmp`, `vr-eye1.bmp`, `vr-hud.bmp` (when the menu quad is visible) and `vr-state.txt`. Runs once and stalls once |
| `timing` | 0 | Recognised; `[vr-perf]` is logged every 10 s regardless |
| `diag_new_game` | 0 | Start a new game from the title menu after about 5 s, with no input (use with `force_render`) |

```sh
tools/steam_frame/deploy.sh --clear-dumps --diag force_render=1 --diag dump_frame=600 \
    --diag diag_yaw=90 --logcat 40 --pull-dumps scratchpad/dumps/yaw90
tools/steam_frame/tests/check_bindings.py        # host test: Frame/Index/Touch bindings
```

Log tags are `MiamiVR`, `MiamiVR-game` and `librw-vk`. The Frame's own lines
start with `[vr]`, `[vr-perf]` and `[vr-dump]`.

Run nothing unattended while someone is wearing the headset. A launch replaces
their game. Check first, for example with
`ssh steamos@<frame> 'ps -eo etime,comm | grep -i activity'`.

## 5. Verified, and at which level

| Claim | Level |
| --- | --- |
| Kit assembles on macOS (all 21 patches apply); Quest debug APK builds with Gradle 8.13 / NDK 27.2 | host build |
| Frame APK builds: `com.miamivr.quest.frame`, minSdk 30, headtracking not required, `focusaware` removed | host build (`aapt dump`) |
| Frame bindings use only real component paths on the right hands, with unique action names | host test |
| Lepton is API 30, Vulkan 1.1, no headtracking feature; SteamVR runtime manifest present | device, read-only |
| Installs and plays from the ViceCityVR Steam title; data and settings in `~/Documents/ViceCityVR` | **worn** (user report, 2026-10-03), before the Steam-in-place change below |
| Reading the game from the Steam install in place, falling back to the copy in Documents | host build and a host test of the lookup. The Steam path has not been tried on the headset. The fallback is what runs today, because Vice City isn't installed in Steam on the Frame right now, and it hasn't been re-run on the device since the change |
| CINEMA 3D cutscenes (projection layer) | **worn** (user: "it works now", 2026-10-03) |
| Third-person view: both styles and the View toggle | **worn**, toggled repeatedly in the log (2026-10-03); per-control feedback not given |
| Freeze at the third intro scene load (17:32, 2026-10-03) | seen once; the next new game loaded in 0.07 s. **Open**: the watchdog now logs `[vr-stall]` traces |
| Individual controls, colours, performance | not yet reported |

## Menu pointer

The right controller's ray works as a mouse in the game's menus (main menu,
pause, options) and as a pointer in the VR menu. Point to highlight, pull the
trigger to click. On a VR menu row, clicking near the left or right edge steps
the value down or up. The D-pad and A still work. The beam and dot go to
SteamVR as the last two layers, so they always sit on top of the menu, the
cinema screen and the world. Checked on the Frame unattended (title menu and
VR menu dumps: the row under the dot highlights, beam and dot on top), not
worn yet.

Test switches in `vr-diag.ini`: `diag_pointer=frontend|vrmenu` with
`diag_pointer_u` / `diag_pointer_v` parks a ray on that surface,
`diag_vr_menu=N` opens VR menu page N, and `dump_frame` also writes
`vr-pointer.bmp`, a flat composite of the layers in order.

## Steam-in-place data, first boot (2026-10-05)

The resolver build booted on the Frame unattended into a new game and the
opening cutscene. Vice City isn't installed from Steam on that headset, so it
took the fallback and logged `[vr] game data: Documents copy ...`. The Steam
path itself still needs an install of app 12110 on the Frame to test.

(An earlier "failed" run turned out to be the test script: it ran `deploy.sh`
without `--tree`, deploy stopped before copying the APK, and the old build ran.)
