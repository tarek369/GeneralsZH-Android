# Android build scripts

`package-android-zh.sh` — build + package the Zero Hour Android APK (arm64-v8a).
`stage-fonts.sh` — stage the Liberation fonts (renamed to Windows names).

## What this does NOT do (yet)

These items are intentionally out of scope for the initial scaffolding and are
tracked as follow-up work:

- **ffmpeg**: disabled by default (`-DRTS_BUILD_OPTION_FFMPEG=OFF`).
  vcpkg's `ffmpeg:arm64-android` port is broken (microsoft/vcpkg#33963). The
  video path (intro movies, mission briefings) needs ffmpeg hand-built with the
  NDK standalone toolchain and dropped into `jniLibs/` + linked via CMake.
  Until then, in-game video is stubbed (the engine already has a Bink stub).
- **GameData asset bundling**: the ~1.5 GB of `.big` archives cannot live in the
  APK compressed (raw `fopen()` cannot read APK assets). For sideloading, push
  them to the device's app-private storage:
  ```
  adb shell mkdir -p /data/data/me.generalsx.zh/files/GameData
  adb push <local GameData> /data/data/me.generalsx.zh/files/GameData/
  ```
  The engine chdir()s into `<files>/GameData` on launch (see SDL3Main.cpp). A
  future first-run extraction step (or an OBB / Play Asset Delivery pipeline)
  would automate this.
- **The DXVK-on-Android GO/NO-GO spike**: DXVK has never been built for Android
  before. `Patches/dxvk-android.patch` (gate `-msse` behind x86 + the high-DPI
  WSI fix) + `cmake/meson-android-aarch64-cross.ini.in` are the build-side
  groundwork, but the runtime gate — "does a d3d8 clear render via DXVK on an
  Android Vulkan driver?" — must be spiked on real hardware before this is
  considered a working port. See docs/port/ANDROID_FEASIBILITY.md §DXVK.

## Current status

**Scaffolding complete; runtime unverified.** The engine source, build system,
DXVK patch, Gradle project, and packaging pipeline are in place, but no APK has
been built or run on a device yet. The single highest-risk unknown remains
DXVK-on-Android driver compatibility.
