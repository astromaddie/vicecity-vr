# PR0 source readiness

**State: EXPERIMENTAL source preparation only.** This task does not change repository visibility or publish a release.

- **Baseline source commit before this preparation:** 75feecafeac6ea9464e757d49daeee7043c0b850.
- **Prior runtime source with the camera migration:** 4fd059d78f9b989ba363498cd3ee00cdefa0ad74.
- **Previously observed installed candidate APK SHA-256:** ebee41a762deae304d8f41d0d8b4121ac03c1270c890d9f9e05059d8f1e7e9e3.
- **Targets:** existing Meta Quest Android port and experimental Steam Frame Lepton/ARM64 build. Build host: macOS (Apple Silicon with Rosetta for pinned NDK tools) or Linux.
- **External data:** user supplies data from a legally owned Vice City installation, or installs the game from Steam on the Frame. This repository contains no complete reVC tree or original game data. See NOTICE.md.
- **Fresh build route:** tools/steam_frame/build.sh --work "$PWD/.frame-build"; requires Git, JDK 17/21, Android platform 35, NDK 27.2.12479018 and CMake 3.22.1. It fetches reVC at 026cd10f3fdbd92c089830e5067c4457c53c1b51.

## Evidence

| Surface | Evidence |
|---|---|
| Host source/build | Prior runtime source 4fd059d passed production camera checks. Historical ARM64 package candidate used kit `4fd059d7` and renderer tree `09e446dd` with dirty overlays; package/dependency checks and Android v2 signature verification passed (recorded in the architecture guide). No clean assembled-tree or fresh full APK build is claimed for this preparation. |
| Unattended Frame | The previously observed installed candidate APK above launched via its Steam title and reported profile suggestions. This does not test physical controller input or visuals while worn. |
| Worn | Earlier port builds were worn, including cinema, menu navigation and first/third-person switching. The current camera preview, physical adjustment/remaps and tracked recentering remain unworn. |
| Fresh isolated source checks | Passed in a fresh local clone with temporary HOME and Git config disabled. Exact commands are below. |

## Known gaps and pending focused acceptance

- The first-scene CdStreamSync hang was intermittent, fixed in source and passed one unattended new-game check; recurrence remains possible.
- Steam-installed game-data lookup has host coverage only; the Frame has not been validated with Vice City installed from Steam.
- No current-source sustained-performance result is claimed. Historical device cadence does not establish performance acceptance.
- Current source still needs focused headset checks for install, real input, tracking/recentering, stereo, menus, gameplay, pause/resume, saves, clean exit and actual performance. In particular, exercise the new camera preview and physical modifier/remaps while worn.
- No device install, launch, or mutation is part of this PR0 source preparation.

The old root README is preserved in [README_PRE_PR0.md](README_PRE_PR0.md); the detailed implementation and validation history remains in [VR_ARCHITECTURE.md](VR_ARCHITECTURE.md) and [STEAM_FRAME.md](STEAM_FRAME.md).

ROM-free source checks run from the isolated clone:

~~~sh
python3 tools/steam_frame/tests/check_bindings.py
python3 tools/steam_frame/tests/test_camera_modifier.py
python3 tools/steam_frame/tests/test_camera_scale.py
bash -n tools/steam_frame/deploy.sh
env -u FRAME_HOST tools/steam_frame/deploy.sh --no-install --no-launch
~~~

The last command is expected to stop at the required-host guard before any SSH, ADB, install or launch step. No full APK rebuild was run in this preparation.
