# Vice City VR on the Valve Steam Frame

The Frame runs Android apps in **Lepton**, an Android 11 (API 30) container
with arm64, Vulkan 1.1 (Turnip, Adreno 750) and SteamVR's OpenXR runtime. This
kit's Quest build already uses the Khronos loader, Vulkan through
`XR_KHR_vulkan_enable2` and no Meta-only API, so the Frame build is that same
build with a Gradle build type, `frame`, on top. The Quest builds are
unchanged. What differs, and why, is in [VR_ARCHITECTURE.md](VR_ARCHITECTURE.md).

Status: it plays on the Frame (first wear, 2026-10-03). The cinema screen,
laser menu navigation, 3D cutscenes and the first/third-person quick swap have
been worn and liked (round 3, 2026-10-05). Round 3's fixes are host builds,
not worn yet. See sections 5 and 6.

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
Host build, not worn.

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

## 3a. Display: 120 Hz, about 2160 per eye

The Frame's panel is about 2160 x 2160 per eye. What SteamVR recommends depends
on its per-app resolution override: 1728 x 1728 by default, 2160 x 2160 once the
player sets the override to 2160 for ViceCityVR. So the Frame default is not a
fixed percentage but **AUTO**: at session start the scale is 2160 divided by the
recommended eye width, clamped to 100-175%. The eye swapchain is created at that
size and the game renders straight into it, with no larger render and no blit
down.

| SteamVR recommends | AUTO picks | Eye image |
| --- | --- | --- |
| 1728 x 1728 (no override) | 125% | 2160 x 2160 |
| 2160 x 2160 (override 2160) | 100% | 2160 x 2160 |

(A recommendation above 2160 is used as it is at 100%; a very small one is capped at 175%.)

The log shows the whole chain:

```
[vr] display defaults moved to AUTO render scale and 120 Hz: applied   (once, on an older settings file)
[vr] render scale AUTO: recommended 1728x1728, target 2160 per eye, scale 125% (about 2160x2160)
per-eye render target: 2160x2160 (requested 125%, selected 125%, effective 125.00%, fallback NONE)
stable native scene=2160x2160 output=2160x2160 temporal=off msaa=1x
```

`RenderScalePercent` in `vr_settings.ini` is 0 for AUTO (the default). Settings
files written by earlier Frame builds carry 100% (the Quest baseline) or 125% (the
first Frame default) and 72 Hz; they are moved to AUTO, and 120 Hz where the rate
was still the old one, once (`FrameDisplayProfileVersion` 2). After that the VR
menu's choices stick: GRAPHICS > OUTPUT cycles AUTO, 100, 125, 150 and 175% (restart
to apply), and LOCOMOTION > REFRESH RATE. Both pages show the real values now:
GRAPHICS prints the request (`AUTO 2160` or the percentage), the active scale and
the eye size, plus `DISPLAY n HZ`; LOCOMOTION prints `ACTIVE n`. The Quest keeps
its 100% and 72 Hz defaults.

**Refresh rate.** The app cannot change the Frame's refresh rate: SteamVR offers
only the title's per-app rate, and only on a launch from the Steam library (an
adb launch runs at 72). So the rate is set by the user, once, in the headset:

> **Wear checklist: set ViceCityVR to 120 Hz in SteamVR's per-app settings**
> (resolution 2160 there too). Done by the user, 2026-10-05. Editing
> `steamvr.vrsettings` while SteamVR runs does nothing.

The game does ask once. `RefreshRate` (default 120, the LOCOMOTION menu row) is
the requested rate. When the session gets focus it lists the rates
`XR_FB_display_refresh_rate` offers and calls `xrRequestDisplayRefreshRateFB` at
most once per session, for the highest offered rate not above the setting, and
never for less than the compositor already runs. No retries, no held frames:

```
[vr] refresh: setting 120, offered {90}, requested 90
[vr] refresh: setting 120, offered {120}, requested none (compositor already at 120 Hz)
```

The real rate is whatever SteamVR runs, measured from the frame period
(`predictedDisplayPeriod`); the menu's `ACTIVE` and `DISPLAY n HZ` show it. There
is no automatic step-down, because it could never take effect. Instead, while worn
(session FOCUSED), after two 10 s `[vr-perf]` windows in a row under 90% of the
display rate the game logs once per session:

```
[vr] refresh: not holding 120 Hz (fps 101.3, 99.8); set ViceCityVR to 90 Hz in SteamVR per-app settings
```

The number is the highest of 120 / 90 / 72 the measured frame rate would hold.
A window only counts if the headset was worn and rendering the whole time and the
rate did not change in it. Changing REFRESH RATE in the menu mid-session applies
next launch.

All of this is a host build, headset-unverified.

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
| Reading the game from the Steam install in place, falling back to the copy in Documents | device, nobody wearing it, on the fallback (new game into the intro, 2026-10-05). The Steam path has a host test only: Vice City isn't installed in Steam on the Frame |
| CINEMA 3D cutscenes (projection layer) | **worn** (user: "it works now", 2026-10-03) |
| Third-person view: both styles and the View toggle | **worn**, toggled repeatedly in the log (2026-10-03); per-control feedback not given |
| Freeze loading the first scene (seen 2026-10-03, caught again 2026-10-05) | The watchdog showed `CdStreamSync` waiting for ever on the stream thread: a lost wake-up in the stream hand-off. Fixed (ordered hand-off, timed wait). One clean unattended new game since; it was intermittent, so watch for it |
| Cinema screen, laser menu navigation, 3D cutscene depth (CINEMA 3D), first / third-person quick swap | **worn**, round 3 (2026-10-05): all good, "I like this a lot" |
| Laser dot clipped to a half circle with a dither where it meets the menu (round 3) | Fixed (see Menu pointer). Device dumps show beam and dot over the title menu and the VR menu, but a flat dump can't show the depth clipping, so this needs a wear |
| Right grip also the handbrake in vehicles (round 3) | Grips now Action (L1), handbrake on A. Host build and host test. **Not worn** |
| 120 Hz (per-app rate set by the user; one request per session, a log hint if it is not held), AUTO render scale (about 2160 per eye: 125% on a 1728 recommendation, 100% on 2160), menu shows both (round 3) | Device, nobody wearing it (2026-10-05): AUTO picked 100% for 2160 x 2160; the intro held 120.0 fps, 0 missed, about 1.2 ms of game CPU a frame; menus 117–120. Not worn yet; open play not measured |
| Individual controls, colours, performance | not yet reported |

## Menu pointer

The right controller's ray works as a mouse in the game's menus (main menu,
pause, options) and as a pointer in the VR menu. Point to highlight, pull the
trigger to click. On a VR menu row, clicking near the left or right edge steps
the value down or up. The D-pad and A still work. The beam and dot go to
SteamVR as the last two layers, so they sit over the menu, the cinema screen and
the world. Laser navigation was worn in round 3 and the user liked it. Checked
on the Frame unattended before that (title menu and VR menu dumps: the row under
the dot highlights, beam and dot on top).

**Round 3 fix: the dot was a half circle.** SteamVR depth-composites quad layers
that cross each other. The dot faced the head but sat exactly on the hit point,
so on a tilted panel half of it was behind the panel and hidden, with a dither
where the two met. The beam's last few centimetres crossed the plane the same
way. Now the dot takes the orientation of the surface it hit, so it lies flat on
the menu or the screen, and sits 12 mm in front of it (towards the head). The
beam stops short of the surface, pulled back along the ray by 2 cm divided by
how square-on the ray is (never less than a quarter, and the beam keeps at least
3 cm). A ray that hits nothing is as before, and the dot keeps its size rule.
`vr-pointer.bmp` is built from the submitted poses, so it shows the new layout.
Host build, not worn yet.

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

## Camera migration delivery (2026-10-07)

Source `4fd059d78f9b989ba363498cd3ee00cdefa0ad74` passed focused production camera-transform/modifier checks,
the native ARM64 build, package/dependency checks and Android v2 signature verification.

The shared camera adapter provides snap/smooth/off choices, signed native-collision
camera offsets, apparent world size, height, seated calibration, head movement,
the live frozen-gameplay settings preview and right/left/centre panel placement.
Physical left grip plus physical right stick adjusts size horizontally and
farther/higher or nearer/lower vertically, before turn/game readers; release or
focus loss requires a neutral stick. Occupied first-person grips and custom
bindings retain their game action. Fresh defaults are snap 30, with a persistent
personal smooth-120/third-person profile; explicit saved choices are preserved.
Third-person pickup is on D-pad Up, freeing left grip. First-person pickup and
existing vehicle controls remain available.

Installed `~/devkit-game/ViceCityVR/ViceCity-VR.apk` SHA-256:
`ebee41a762deae304d8f41d0d8b4121ac03c1270c890d9f9e05059d8f1e7e9e3`. The booked, idle Steam-title launch (raw app ID 3436746357) booted the
frontend through SteamVR/OpenXR, accepted Frame/Index/Touch profile suggestions
(26/18/19 bindings), and reached READY/SYNCHRONIZED. Only the task's own Lepton
process was stopped. No new gameplay-preview capture or physical input test was
performed in this smoke check.

The APK records committed kit `4fd059d7` and actual assembled tree
`09e446dd` plus dirty overlays; the compiled adapter and renderer sources match
the kit. This is not labelled as a clean assembled-tree build. The settings file
remained exact. Startup truncated the existing transition log; its original
bytes, hash and mode 600 were restored after retaining the new log separately.

| Current increment | Evidence level |
| --- | --- |
| ARM64 package, Steam-title frontend boot and accepted profile suggestions | verified unattended |
| New live camera preview, physical modifier/remaps, tracked recentring and comfort | not worn; preview not captured |

Wear checks: grip plus all four right-stick directions; neutral release;
pause/settings/resume; right-side panel and pointer; recentre; on-foot pickup;
first-person actions and vehicle entry/driving/exit. Evidence: task output
`device-camera-migration/ViceCityVR/lepton-steamlaunch.log` and the cross-port report.
