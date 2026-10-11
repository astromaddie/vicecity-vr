# Vice City VR on Steam Frame — experimental source

**Upstream Quest port:** [dubrovskiy-yevhen-stakelogic/vice-city-vr-quest](https://github.com/dubrovskiy-yevhen-stakelogic/vice-city-vr-quest). This repository is the [astromaddie Steam Frame fork](https://github.com/astromaddie/vicecity-vr/tree/steam-frame), based on [reVC](https://github.com/mrxenginner/reVC). It adds a Lepton build, Frame controller bindings, persistent Documents storage, and an experimental live camera settings preview and physical adjustment control. The fork also retains its Meta Quest build.

**Status: EXPERIMENTAL.** This is source-preparation work for personal use; it does not change repository visibility or publish a binary. The prior camera migration passed host camera checks. PR0 rebuilt the APK from a clean assembled source tree using public reVC; an independent build passed with isolated Java/Gradle homes. Both artifacts remain unworn. A predecessor APK's Steam-title boot and profile suggestions were verified unattended. Earlier port behavior was worn, but the current live camera preview, physical adjustment, remaps and tracked recentering have not been worn. See [the architecture record](docs/VR_ARCHITECTURE.md) and [the Steam Frame validation notes](docs/STEAM_FRAME.md).

| Area | Evidence and limit |
|---|---|
| Fresh build and package | Kit `6a4a784e94c7961b5e25f2df1263ba3fbb943039` plus public reVC `026cd10f3fdbd92c089830e5067c4457c53c1b51` produced clean assembled Git tree `a6199bd3a46f08f9108e286692a3a04fc1162d61`. Host tests, two full APK builds, signatures and 16 KiB alignment passed. Exact artifact identities and build isolation are in PR0 metadata. |
| Frame launch | Previously observed installed candidate APK SHA-256 `ebee41a762deae304d8f41d0d8b4121ac03c1270c890d9f9e05059d8f1e7e9e3`; Steam title launch and profile suggestions were observed unattended. |
| Worn behavior | Earlier builds were worn; current camera preview and physical modifier/remaps remain pending. |
| Known issue | A previous intermittent first-scene `CdStreamSync` wait was fixed and passed one unattended new-game check; continue watching for recurrence. |
| Performance | No current-source sustained-performance result is claimed. Historical runtime cadence is not a performance acceptance result. |

## Build from a fresh checkout

Build on macOS (Apple Silicon with Rosetta for the pinned NDK tools) or Linux. Install Git, JDK 17 or 21 and Android SDK platform 35, NDK 27.2.12479018 and CMake 3.22.1. The build fetches the public reVC source at its pinned commit.

```sh
git clone --branch steam-frame https://github.com/astromaddie/vicecity-vr.git
cd vicecity-vr
sdkmanager "platforms;android-35" "ndk;27.2.12479018" "cmake;3.22.1"
tools/steam_frame/build.sh --work "$PWD/.frame-build"
```

The build does not include the original game. For gameplay, supply data from a legally owned Vice City copy, or install Vice City from Steam on the Frame. The Steam-in-place lookup path has host coverage but has not been confirmed on a Frame with the game installed from Steam. See [BUILDING.md](BUILDING.md) for the source-kit routes and [NOTICE.md](NOTICE.md) for license scope.

A focused headset acceptance run is still needed for installation, real controller input, tracking/recentering, stereo, menus, gameplay, pause/resume, saves, clean exit and actual performance. No Frame launch or mutation is part of this source-preparation task.

The previous root README, including the Quest source-kit instructions, is preserved in [docs/README_PRE_PR0.md](docs/README_PRE_PR0.md). The concise current-branch source identity and remaining limits are in [docs/PR0_READINESS.md](docs/PR0_READINESS.md).

Immutable tested source, final checks and open gates: [PR0 metadata](docs/PR0_METADATA.json).

The prepared candidate has not been installed. Record the focused session in [PR0 headset acceptance](docs/PR0_HEADSET_ACCEPTANCE.md).
