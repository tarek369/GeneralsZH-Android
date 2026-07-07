# Overlay triplet: build the shared C++ deps (freetype, curl, libpng, zlib,
# openal, ...) for Android arm64-v8a against the NDK so they match the engine's
# ANDROID_PLATFORM (API 24 — Vulkan + the minimum for a stable libvulkan.so).
#
# GeneralsX @feature android-port 06/07/2026
#
# NOTE: ffmpeg is deliberately NOT built via vcpkg on Android — its port is
# broken for arm64-android (microsoft/vcpkg#33963). The build scripts hand-build
# ffmpeg with the NDK standalone toolchain instead. See scripts/build/android/.
#
# Requires: ANDROID_NDK_HOME set in the environment, pointing at the NDK
# installation CMake/vcpkg will drive.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME Android)
set(VCPKG_CMAKE_SYSTEM_VERSION 24)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-linux-android")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS "-DANDROID_ABI=arm64-v8a;-DANDROID_PLATFORM=android-24")
