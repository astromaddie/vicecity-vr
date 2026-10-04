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
| OpenXR API | `XR_CURRENT_API_VERSION` | 1.0 |
| Controller profiles | Touch | Frame, Index, Touch |
| Recentre | both stick clicks | View hold 1 s (and the system recentre event) |
| Shared library | none | `third_party/sfvr` 0.1.0 (View timing, haptic table, `[vr-perf]`) |

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
| Sticks, triggers, grips, stick clicks | As on the Quest | |
| Bumpers | Third person on foot: previous / next weapon (L2 / R2). First person: unbound | Frame profile only |
| Both grips + Menu | VR menu (the Quest port's chord) | |

- The Frame profile is suggested only when `XR_VALVE_frame_controller_interaction` is advertised.
- Index fallback: left B is Menu and left A is View. X and Y are reachable only through remapping.
- Each profile's suggest result and the active profile are logged with `[vr]`.
- The bindings are checked on the host by `tools/steam_frame/tests/check_bindings.py`.

## 6. Status

| Item | State |
| --- | --- |
| Quest debug APK builds on the Mac from the kit (Gradle 8.13, NDK 27.2) | host build |
| Frame APK builds; Frame strings only in the Frame library | host build |
| Bindings: component paths, hands, unique names | host test |
| Lepton facts in section 1 | device, read-only |
| Install from the Steam library title, game boots and plays, data in `~/Documents/ViceCityVR` | **worn**: the user reported "it works good" (2026-10-03, first session). The game wrote `vr_settings.ini` and its logs to the Documents folder |
| CINEMA 3D cutscenes | host build, plus a numeric check of the perceived depths. **Not worn** |
| Third-person view (`ViewMode`: 3D SCREEN / OVER SHOULDER, View short-press toggle) | host build. **Not worn**. Risks: chase-camera collision jumps, crosshair against the head-locked HUD plane, screen-to-stereo switches on mode change |
| Diagnostics (`vr-diag.ini`: force_render, synthetic head, eye dumps, `[vr-perf]`) | host build |
| Fresh `build.sh` from the kit reproduces the dev tree byte-for-byte and builds both APKs | host build |
| Stereo eye dumps at 0, 90 and 180 degrees of yaw | **not run** |
| Individual controls (View recentre, D-pad, Index fallback), colour correctness, performance numbers | not reported on yet |

## 7. Open items

- Run phase 0/1 on the device. This needs the Steam download to finish and an idle headset.
- Rendering and the game step only after `APP_CMD_RESUME` (`AppState.resumed`). If Lepton never sends it, nothing draws. Check this on the first run.
- Comfort set, the standard's turn settings, the VR settings page and "Quit to Steam" all follow the Quest port's own menu for now. Map them in a later pass.
- The refresh rate stays at the Quest default of 72 Hz. 90 Hz needs a wear test.
- `XR_META_performance_metrics` is absent on SteamVR, so the in-menu GPU line reads from Vulkan.
- `SaveProvider` still serves the app-dir saves. On the Frame, saves are in `~/Documents/ViceCityVR/userfiles`, reachable over SSH.
