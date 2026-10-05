# Vice City VR on the Steam Frame: architecture (current state)

This is the Steam Frame retarget of the Quest port in this kit. The Quest port
already renders in stereo, tracks the hands and has an in-headset menu. The
Frame work moves that build onto SteamVR inside Lepton without changing what
the Quest build does. How to build, deploy and test is in
[STEAM_FRAME.md](STEAM_FRAME.md). Quest-side rendering detail is in
[RENDERING_NOTES.md](../RENDERING_NOTES.md) and
[overlay/docs/QUEST_PORT.md](../overlay/docs/QUEST_PORT.md).

## 1. Target and probe facts

| Item | Value | Source |
| --- | --- | --- |
| Package route | Android APK in Lepton (compat tool `lepton`), devkit title `ViceCityVR` | as GoldenEye VR and Halo CE VR |
| Container | Android 11, API 30, arm64-v8a | `getprop ro.build.version.sdk` = 30, 2026-10-03 |
| Android features | `vulkan.level=1`, `vulkan.version=0x401000` (1.1), `vulkan.compute`; **no** `android.hardware.vr.headtracking` | `pm list features`, 2026-10-03 |
| OpenXR runtime | SteamVR OpenXR, `/vendor/etc/openxr/1/active_runtime.json`; no loader broker | `ls`, 2026-10-03 |
| Loader | Khronos `openxr_loader_for_android` 1.1.43 (Maven, the Quest build's own) | `build.gradle.kts` |
| Graphics binding | `XR_KHR_vulkan_enable2`; the runtime creates the Vulkan instance and device | Quest code; **unproven on the Frame** |
| Swapchain | one image, `arraySize = 2`, multiview; RGBA8/BGRA8 UNORM preferred, sRGB accepted. On the Frame an sRGB image is created mutable-format and rendered through a UNORM view, because the post-FX output is already gamma-encoded | Quest code + Frame build; the format SteamVR offers in Lepton is unknown until the first run |
| API version | OpenXR 1.0 requested in the Frame build (1.1 retried if refused) | every Frame-proven port asks for 1.0 |

The headtracking feature is why the Frame manifest exists: the Quest manifest
requires it, so the Quest APK would not install in Lepton.

## 2. Build flag

Everything Frame-only sits behind `MIAMIVR_STEAM_FRAME` (CMake option, set by
the Gradle build type `frame`, defined 0/1 for every target through librw's
public definitions). With it off, the Quest debug and release builds are what
they were. The Gradle file lowers `minSdk` from 32 to 30 only when a task name
contains `Frame`, because `minSdk` cannot vary per build type.

| Area | Quest | Frame (`assembleFrame`) |
| --- | --- | --- |
| Application id | `com.miamivr.quest` | `com.miamivr.quest.frame` |
| minSdk | 32 | 30 |
| Manifest | `src/main` | plus `src/frame`: headtracking not required, no `focusaware`, all-files access declared |
| Signing | debug, or the release keystore | debug key |
| Data root | app external files dir | `/sdcard/Documents/ViceCityVR` (the headset's `~/Documents/ViceCityVR`), app dir as fallback |
| Retail data | the copy in the data root | the Steam install in place when there is one, else the copy (section 2a) |
| OpenXR API | `XR_CURRENT_API_VERSION` | 1.0 |
| Controller profiles | Touch | Frame, Index, Touch |
| Recentre | both stick clicks | View hold 1 s (and the system recentre event) |
| Shared library | none | `third_party/sfvr` 0.1.0 (View timing, haptic table, `[vr-perf]`) |

## 2a. Game data sources (Frame)

Inside Lepton the host's Steam library is visible at the same paths
(`/home/steamos/.local/share/Steam` and its `steamapps`): world-readable, not
writable by the app (device check on another port, 2026-10-04).
`/sdcard/Documents` is a symlink to `/home/steamos/Documents`.

At start-up (`gamedata::init`, `src/skel/android/GameData.cpp`) the game parses
`steamapps/libraryfolders.vdf` for the library paths, reads
`appmanifest_12110.acf` for `installdir` ("Grand Theft Auto Vice City") and its
`StateFlags` (it must say installed, bit 4), and checks that `models/gta3.img`,
`models/gta3.dir` and `data/gta_vc.dat` are readable, matching names without
regard to case. The log says which one won, once:

```
[vr] game data: steam <folder>
[vr] game data: Documents copy <folder> (no Steam install of app 12110 found)
[vr] game data overlay: <folder> (port files, settings, saves, caches)
```

Reads then resolve in this order:

1. **Port files** from Documents: everything in the tree's `gamefiles/` (reVC's
   text, frontend art, VR hands, `neo/`, scripts). CMake lists them into the
   build, so the APK always knows which files it expects there. `deploy.sh`
   installs them.
2. **Everything else in the retail folders** (`data TEXT anim txd skins mp3
   movies models Audio`) from the Steam install.
3. **The Documents copy**, for anything the above did not find. This is the
   fallback, and it is the old behaviour: with no Steam install nothing
   changes.

Anything outside those folders (`vr_settings.ini`, `re3.ini`, `modelsets/`, caches) is
read from Documents first, so the Modern and Xbox model sets, which the user
installs into Documents, are untouched. **All writes land in Documents**:
the process's working directory stays there, saves go to `userfiles/`, and the
temporary files for the radio and mission-dialogue streams go to the storage
root. The Steam folder is never written.

The game changes directory constantly (`CFileMgr::SetDir("DATA")` and the
like), so `mychdir` no longer really changes it on the Frame; it records the
game's current folder and every read is resolved against that. Every route to
a file goes through `gamedata::resolve`:

| Reader | Where |
| --- | --- |
| `CFileMgr` (`.dat`, `.ini`, `.gxt`, `.scm`, `.dir`, saves, the VR hand meshes) | `fcaseopen` in `crossplatform.cpp` |
| `.img` archives (including the model-set fallback), and the block size from `statvfs` | `CdStreamAddImagePath`, `GetGTA3ImgSize`, `CdStreamInit` |
| librw streams (`.txd`, `.dff`, `.col`, `.ifp`) and BMP/PNG images (the hand albedo) | `RwStreamOpen`, `RtPNGImageRead` and friends in `fakerw/fake.cpp` |
| Folder listing (`skins/*.bmp`) | `FindFirstFile`: Documents' folder if it has one, else Steam's |
| `sfx.SDT`, `sfx.RAW`, radio and mission streams | `platform::resolveGamePathCaseInsensitive` |

The radio and mission streams are handed to Android's `MediaPlayer` by path, so
if it cannot open a file in the Steam folder the app copies that one stream
under the storage root and tries again (`[vr] MediaPlayer could not open ...
in place; using a copy`). The IMA ADPCM mission dialogue is decoded to a
temporary file under the storage root as before.

The engine edits sit behind `MIAMIVR_GAMEDATA`, which `GameData.h` defines as
`defined(RW_VULKAN) && defined(__ANDROID__) && MIAMIVR_STEAM_FRAME` (and not
`MIAMIVR_BRINGUP`). On the Quest build it is 0, and `GameData.cpp` compiles to
nothing.

Verified: the resolver, library discovery and the order above by a host test
(`tools/steam_frame/tests/run_gamedata_test.sh` in the assembled tree, a fake
library in a temp folder), and both APKs build. The Steam-in-place path is not
device-verified. The fallback path is what runs today, because Vice City is
not installed in Steam on the Frame, and it has not been re-run on the device
since this change.

## 3. Frame protocol (inherited from the Quest port)

- A game thread owns the session. It runs `xrWaitFrame`, `xrBeginFrame`,
  `syncInput`, `xrLocateViews` and then the renderer callback.
  `renderGameFrame` steps the game once per XR frame and draws both eyes in one
  multiview pass. Then it calls `xrEndFrame`.
- Game time comes from `predictedDisplayTime` (`SetFrameTimeNs`). reVC runs a
  variable timestep on that clock and falls back to the wall clock after about
  50 ms of frozen time.
- When nothing renders, the sim does not step (`shouldRender=false`, standby).
  The Frame build's `force_render` diagnostic exists for that reason.
- Reference space is STAGE, falling back to LOCAL. A VIEW space carries the
  head-locked menu quad.

## 4. Draw inventory (all inherited; the Frame adds none)

| Draw | VR rule in this port |
| --- | --- |
| Head roll (Frame build) | The HUD plane, crosshair and sights follow head yaw and pitch only, never roll. World sprites (coronas, flares, particles) sit on a second plane that keeps the full head pose, so they still land on their lights, and each sprite is counter-rotated by the head roll every frame (`CSprite::UpdateVrRoll`). The VR menu is world-locked with a yaw-only anchor set when it opens and on recentre. The log prints `[vr] roll-free billboards/HUD` at startup. Host build only, not worn |
| World | Multiview stereo, per-eye view-projection from the runtime FOV. Culling is stereo-aware by default ("stereo safe"), with OFF as a fallback |
| HUD (Im2D) | Projected per eye onto a plane 2 m from the head inside the multiview pass |
| Map, status, clock, ammo | Wrist panels rendered to small textures and placed on the arm |
| Frontend menus, cutscenes | Theater mode: a 2.56 x 1.44 m quad layer, yaw-anchored at 2 m, reading array layer 0 |
| Cutscenes, CINEMA 3D (Frame default) | The screen is submitted as a **projection layer**. Each eye's view has the screen's orientation and a field of view that exactly fits the screen rectangle; per-eye quads (eyeVisibility) showed flat on SteamVR. The director camera is split into two parallel off-axis cameras converged 1.5 m ahead. The camera baseline is solved so the images at infinity are depth x IPD apart on the screen, with depth 0.35 / 0.55 / 0.75 for SUBTLE / default / DEEP. The background therefore sits at 3.1 / 4.4 / 8 m and never diverges. Subtitles and the letterbox sit on the zero-parallax plane. Menus, loading and the intro stay flat (`makeCinema3DMatrices` in `android_main.cpp`) |
| VR menu and debug overlay | 1024x768 head-locked quad layer in VIEW space, source-alpha blended |
| Menu laser (Frame) | Beam and dot are the last two quad layers. SteamVR depth-composites intersecting quads, so neither may touch the surface's plane: the dot takes the hit surface's orientation (`g.ptrHitOrientation`, set in `updatePointer` from the menu or theater pose) and sits 12 mm off it towards the head, flipped if the surface normal points away; the beam ends `0.02 / max(abs(dot(dir, n)), 0.25)` m short of the hit (at least 3 cm of beam remain). A missed ray is unchanged. Headset-unverified |
| Stereo cutscenes | Staged cameras selectable per scene (R3 cycles, L3 keeps) |

Every one of these is **headset-unverified on the Frame**.

## 5. Input

The Frame controller is a split gamepad. Its bindings:

| Physical | Action | Notes |
| --- | --- | --- |
| R Menu | Start (pause) | Quest: left Menu |
| L View short | Back: VR menu back, frontend back (Triangle pulse) | sfvr View timing |
| L View hold 1 s / 3 s | Recentre, with a 0.6 / 80 ms buzz on both hands | no standing-height calibration in this port, so 3 s recentres again |
| A B X Y (right) | A B X Y as on the Quest, through the CONTROLS remap table | Quest X and Y are on the left hand |
| L D-pad | The game's D-pad, in the frontend and in play | new on the Frame |
| Sticks, triggers, stick clicks | As on the Quest | |
| Grips | On foot: the CONTROLS bindings. In a vehicle: L1, the game's Action control (pick up / radio), in pad modes 0 and 1; nothing in modes 2 and 3, where L1 exits or is the handbrake | Frame only (`VrVehiclePadTarget` in `android.cpp`). Was R1, the handbrake |
| A in a vehicle | Handbrake | `MapHandbrakeToPad` in `QuestDrivingVR.cpp` |
| Bumpers | Third person on foot: previous / next weapon (L2 / R2). First person: unbound | Frame profile only |
| Both grips + Menu | VR menu (the Quest port's chord) | |

- The Frame profile is suggested only when `XR_VALVE_frame_controller_interaction` is advertised.
- Index fallback: left B is Menu and left A is View. X and Y are reachable only through remapping.
- Each profile's suggest result and the active profile are logged with `[vr]`.
- The bindings are checked on the host by `tools/steam_frame/tests/check_bindings.py`.

## 5a. Display

Standard section 9: 120 Hz at about 2160 x 2160 per eye.

- **Eye size.** The target is an absolute 2160 per eye. The Frame's default
  `RenderScalePercent` is 0, meaning AUTO: `createSwapchain` takes
  `2160 / recommendedImageRectWidth`, clamped to 100-175% (recommended 1728 gives
  125%, recommended 2160 gives 100%, as the user's SteamVR per-app
  `resolutionOverride` moves the recommendation). It logs
  `[vr] render scale AUTO: recommended WxH, target 2160 per eye, scale P%`, then the
  existing `per-eye render target` line. The eye swapchain is created at that size
  and the game's scene (`sceneWidth/Height`) and its post-FX output are the same
  size. There is no render-larger-then-blit step (the only blits in librw are the
  reflection copy, divisor 1 by default, and the panel raster copy). Older settings
  files (100% from the Quest baseline, 125% from the first Frame build, 72 Hz) are
  moved once, keyed by `FrameDisplayProfileVersion` 2; a percentage chosen in the
  menu afterwards stays. The Quest keeps 100% and 72 Hz. The menu's GRAPHICS row
  cycles AUTO / 100 / 125 / 150 / 175 and shows the request, active scale, eye size
  and `DISPLAY n HZ`.
- **Refresh request.** `RefreshRate` (default 120, key `refresh_rate` in sfvr's
  settings table, whose default moved from 90 to 120) feeds
  `setPreferredDisplayRefreshRate`. On FOCUSED, `requestPreferredDisplayRefreshRate`
  enumerates the offered rates and requests the highest not above the setting
  (the lowest offered if all are higher), logging
  `[vr] refresh: setting N, offered {...}, requested R`. The measured rate comes
  from `predictedDisplayPeriod`, which is the truth on the Frame (the FB list can
  disagree with the compositor, lesson 79): if it is already above the rate the
  list would give, nothing is requested. Layers are held until the display
  reaches the requested rate, at most about 8 s on the Frame.
- **Fallback.** `perfPoll` watches each `[vr-perf]` window. Two in a row below
  90% of the current rate, while FOCUSED, rendering and with no rate change in
  between, step the target 120 -> 90 -> 72 and re-request. It is per launch; the
  saved setting is untouched. It stops when the runtime offers nothing lower.
- **SteamVR decides.** The compositor only offers the per-app rate set in
  SteamVR's settings for the Steam title, so the user sets 120 there once (wear
  checklist in STEAM_FRAME.md section 3a).

Host build only, none of it worn.

## 6. Status

| Item | State |
| --- | --- |
| Quest debug APK builds on the Mac from the kit (Gradle 8.13, NDK 27.2) | host build |
| Frame APK builds; Frame strings only in the Frame library | host build |
| Bindings: component paths, hands, unique names | host test |
| Lepton facts in section 1 | device, read-only |
| Install from the Steam library title, game boots and plays, data in `~/Documents/ViceCityVR` | **worn**: the user reported "it works good" (2026-10-03, first session). The game wrote `vr_settings.ini` and its logs to the Documents folder. That was before the Steam-in-place change |
| Game data from the Steam install in place, Documents copy as the fallback (section 2a): library discovery, read order, writes in Documents | host build and host test. Not device-verified: the fallback is what runs today, Vice City is not installed in Steam on the Frame |
| CINEMA 3D cutscenes | host build, plus a numeric check of the perceived depths. **Not worn** |
| Third-person view (`ViewMode`: 3D SCREEN / OVER SHOULDER, View short-press toggle) | host build. **Not worn**. Risks: chase-camera collision jumps, crosshair against the head-locked HUD plane, screen-to-stereo switches on mode change |
| Diagnostics (`vr-diag.ini`: force_render, synthetic head, eye dumps, `[vr-perf]`) | host build |
| Fresh `build.sh` from the kit reproduces the dev tree byte-for-byte and builds both APKs | host build |
| Cinema screen, laser menu navigation, CINEMA 3D depth and the first / third-person swap | **worn**, round 3 (2026-10-05): all good, "I like this a lot" |
| Laser dot half-hidden at the menu (round 3): dot lies on the surface, beam stops short | host build. **Not worn** |
| Grips in vehicles are Action (L1), handbrake on A only (round 3) | host build and `check_bindings.py`. **Not worn** |
| 120 Hz request and fallback, AUTO render scale (about 2160 per eye), menu readouts (round 3) | host build. **Not worn**; needs the per-app 120 Hz set in SteamVR |
| Stereo eye dumps at 0, 90 and 180 degrees of yaw | **not run** |
| Individual controls (View recentre, D-pad, Index fallback), colour correctness, performance numbers | not reported on yet |

## 7. Open items

- Run phase 0/1 on the device. This needs the Steam download to finish and an idle headset.
- Rendering and the game step only after `APP_CMD_RESUME` (`AppState.resumed`). If Lepton never sends it, nothing draws. Check this on the first run.
- Comfort set, the standard's turn settings, the VR settings page and "Quit to Steam" all follow the Quest port's own menu for now. Map them in a later pass.
- Wear the display change: set 120 Hz for ViceCityVR in SteamVR's per-app settings, launch from the Steam library, and check `[vr] refresh:`, `render scale AUTO` and `per-eye render target: 2160x2160` and `[vr-perf]` (fps, missed) in the log. If 120 Hz does not hold, the fallback steps down and logs it; the measurement then decides whether the default should drop to 90 (standard section 9).
- `XR_META_performance_metrics` is absent on SteamVR, so the in-menu GPU line reads from Vulkan.
- `SaveProvider` still serves the app-dir saves. On the Frame, saves are in `~/Documents/ViceCityVR/userfiles`, reachable over SSH.
