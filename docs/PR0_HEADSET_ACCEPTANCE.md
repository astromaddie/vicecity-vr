# PR0 focused headset acceptance — pending

**EXPERIMENTAL. No current candidate has been installed or accepted while worn.**

The tested kit source is `6a4a784e94c7961b5e25f2df1263ba3fbb943039`, public reVC is
`026cd10f3fdbd92c089830e5067c4457c53c1b51`, and the assembled Git tree is
`a6199bd3a46f08f9108e286692a3a04fc1162d61`. The prepared acceptance APK SHA-256 is
`b99445d8a87d423ae1b500bcca87ed4f91f38080a2b7dcd52aa381aef68c44a7`.
Its existing debug certificate matches the installed package. A separate build
under isolated Java/Gradle homes passed, with APK SHA-256
`e24615d6ca4dce9ed2a313e4ffa97ddede12f45ff35179cf30a9734a1c1e7fd9`.
The two native game libraries differ; artifact identity and source identity are
recorded separately in [PR0 metadata](PR0_METADATA.json).

Before installation, obtain a fresh Frame booking and the user's GO while they
are out of the headset. Back up current package identity, mutable settings,
saves, permissions and sidecars. Update the existing installation without
clearing data, then launch through Steam for real controller input. During the
wear session, do not run synthetic head or hand diagnostics.

| Check | Short wearer task | Result |
| --- | --- | --- |
| Install/start | Launch the updated title through Steam | Pending |
| Real controllers | Use both hands, primary actions, locomotion and menus; exercise the camera modifier/remaps | Pending |
| Tracking/stereo | Look and move naturally, recenter, inspect both eyes for depth and artifacts | Pending |
| Menus | Open settings and camera preview; confirm readable HUD; return to play | Pending |
| Gameplay | Complete a meaningful mission or driving/combat segment; record scene and duration | Pending |
| Pause/resume | Pause, remove and replace the headset, resume and verify control | Pending |
| Save retention | Save progress, quit, relaunch through Steam and load that progress | Pending |
| Exit | Quit to Steam; check for a crash or stuck scene process | Pending |
| Performance | Record worn duration, refresh, eye resolution, measured timing/misses and subjective comfort separately | Unmeasured |

Record hardware/runtime versions, settings, actual start/end times and each
failure with its scene. Requested refresh or off-head cadence is insufficient
performance evidence. If compositor/GPU timing cannot be collected, publish
that measurement gap explicitly. Earlier worn builds are historical evidence
and do not complete this checklist for the prepared APK.
