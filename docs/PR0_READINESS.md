# PR0 source readiness

**EXPERIMENTAL — public source.** No binary release is included.

- **Checked source commit:** `6c144d744dabf52547603823ca5efdafa5793013`.
- **Targets:** Steam Frame Lepton/Android ARM64 and the inherited Quest target. This PR0 record covers the Frame build route.
- **Game data:** supplied separately by the player. The repository contains port patches, not a complete reVC source tree or original game assets; see [NOTICE.md](../NOTICE.md).
- **Build route:** `tools/steam_frame/build.sh --work /path/to/disposable-build`. It fetches reVC at `026cd10f3fdbd92c089830e5067c4457c53c1b51`. The verified host used JDK 17.0.20.1, Android platform 35, build-tools 35.0.0, NDK 27.2.12479018, CMake 3.22.1, Ninja 1.10.2 and Gradle 8.13.

## Fresh checks

These production host checks passed from a fresh checkout of the checked commit:

~~~sh
python3 tools/steam_frame/tests/check_bindings.py
python3 tools/steam_frame/tests/test_camera_modifier.py
python3 tools/steam_frame/tests/test_camera_scale.py
bash -n tools/steam_frame/deploy.sh
bash overlay/tools/steam_frame/tests/run_gamedata_test.sh
~~~

The deploy guard was also exercised with `FRAME_HOST` unset. It stopped before any SSH or ADB operation.

## Build and artifact

The complete Frame APK build passed once from the clean checkout. The assembled tree has commit `6846a7abc4494dcb4ebbfdab27a8e91b86cf9be3`, Git tree `a6199bd3a46f08f9108e286692a3a04fc1162d61`, and no tracked changes. The final local APK is SHA-256 `32a3dec10f6c8777ae908581e8a95e26980805c085257c97dffcb5c63611bd7e` (3,176,920 bytes), package `com.miamivr.quest.frame`, version `0.5.6-frame` (522).

`apksigner` verified APK Signature Scheme v2 with a temporary Android Debug key whose certificate SHA-256 is `1ae9f15b801a1b66822f6d8a00da22863f66fab05f2beddb874efed12f4f6d71`. The JVM user home, shell HOME and Gradle home were separate disposable directories; the Gradle home started empty, and the APK signer matches the key generated in the temporary JVM home. `zipalign -c -P 16 4` and the ZIP integrity check passed. The APK contains only ARM64 native libraries and has no ROM or game-data-like entries.

The APK was kept as a local build artifact and was not uploaded, installed or launched in this check. Source and artifact hashes, commands and logs are recorded in [PR0_METADATA.json](PR0_METADATA.json); the detailed port history remains in [VR_ARCHITECTURE.md](VR_ARCHITECTURE.md) and [STEAM_FRAME.md](STEAM_FRAME.md).
