#!/bin/bash
# package-android-zh.sh
#
# Build + package the Android (arm64-v8a) APK for C&C Generals Zero Hour.
# The Android analog of ios/package-ios-zh.sh.
#
# GeneralsX @feature android-port 06/07/2026
#
# Pipeline:
#   1. Verify prerequisites (NDK, SDK, Gradle, SDL3 fork submodule).
#   2. Stage fonts (Liberation renamed to Windows names).
#   3. Copy runtime .so libs (DXVK d3d8/d3d9, SDL3, SDL3_image, openal) produced
#      by the CMake build into android/app/src/main/jniLibs/arm64-v8a/.
#   4. Copy dxvk.conf into assets/ (engine CWD resolves it).
#   5. ./gradlew assembleRelease -> APK.
#   6. apksigner sign with the debug (or user) keystore.
#   7. Optional: adb install.
#
# Unlike iOS there is NO MoltenVK fetch/embed step (Android has native Vulkan),
# NO install_name_tool/@rpath rewrite (Android's loader resolves bare sonames),
# and NO inside-out codesign re-sign (apksigner signs the whole APK once).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "${REPO_ROOT}"

ABI="arm64-v8a"
ANDROID_DIR="${REPO_ROOT}/android"
APP_DIR="${ANDROID_DIR}/app"
JNI_LIBS="${APP_DIR}/src/main/jniLibs/${ABI}"
ASSETS="${APP_DIR}/src/main/assets"
# GeneralsX @build android-port 07/07/2026 The CMake android-vulkan preset
# builds into build/android-game/ (the preset name), not build/android-vulkan/.
BUILD_DIR="${REPO_ROOT}/build/android-game"

# ---- 1. Prerequisites ---------------------------------------------------------
: "${ANDROID_NDK_HOME:=${HOME}/Library/Android/sdk/ndk/27.1.12297006}"
export ANDROID_NDK_HOME
export ANDROID_HOME="${ANDROID_HOME:-${HOME}/Library/Android/sdk}"
if [[ ! -d "${ANDROID_NDK_HOME}" ]]; then
    echo "ERROR: ANDROID_NDK_HOME not found: ${ANDROID_NDK_HOME}"
    exit 1
fi
if [[ ! -d "${REPO_ROOT}/references/fbraz3-dxvk/.git" ]]; then
    echo "ERROR: DXVK fork submodule missing. Run: git submodule update --init references/fbraz3-dxvk"
    exit 1
fi
command -v gradle >/dev/null 2>&1 || command -v ./gradlew >/dev/null 2>&1 || {
    echo "ERROR: Gradle not found (and no wrapper). Install Gradle or run from android/ with the wrapper."
    exit 1
}

# ---- 2. Stage fonts -----------------------------------------------------------
echo "==> Staging fonts"
GX_FONTS="${HOME}/GeneralsX/android-staging/fonts" bash scripts/build/android/stage-fonts.sh

# ---- 3. Copy native runtime .so into jniLibs ----------------------------------
echo "==> Staging native libraries into jniLibs/${ABI}"
mkdir -p "${JNI_LIBS}"

# The CMake build produces these in build/android-vulkan/ (or the Gradle build
# dir). Copy whatever exists; the engine's libmain.so is produced by Gradle's
# externalNativeBuild directly into the APK, so we only stage the dlopen'd deps.
copy_if_exists() {
    local src="$1" name="$2"
    if [[ -f "${src}" ]]; then
        cp -f "${src}" "${JNI_LIBS}/${name}"
        echo "    staged ${name}"
    else
        echo "WARNING: ${name} not found at ${src} — the APK may fail to load it at runtime"
    fi
}

# DXVK d3d8 + d3d9 (built by the CMake dxvk_android_build ExternalProject).
copy_if_exists "${BUILD_DIR}/libdxvk_d3d8.so"   "libdxvk_d3d8.so"
copy_if_exists "${BUILD_DIR}/libdxvk_d3d9.so"   "libdxvk_d3d9.so"
# SDL3 + SDL3_image (FetchContent build).
copy_if_exists "${BUILD_DIR}/_deps/sdl3-build/libSDL3.so"        "libSDL3.so"
copy_if_exists "${BUILD_DIR}/_deps/sdl3_image-build/libSDL3_image.so" "libSDL3_image.so"
# OpenAL (FetchContent build).
copy_if_exists "${BUILD_DIR}/openal-soft/libopenal.so" "libopenal.so"
# FreeType (FetchContent build — engine dlopens it for font rendering).
copy_if_exists "${BUILD_DIR}/_deps/freetype-build/libfreetype.so" "libfreetype.so"
# GLM (FetchContent build — engine dlopens it for matrix math).
copy_if_exists "${BUILD_DIR}/_deps/glm-build/glm/libglm.so" "libglm.so"
# GameSpycompat shim (built by the CMake engine build).
copy_if_exists "${BUILD_DIR}/libgamespy.so" "libgamespy.so"
# libc++_shared.so (from the NDK — required by libc++ runtime).
copy_if_exists "${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "libc++_shared.so"
# libmain.so — the engine itself, built by externalNativeBuild into the APK.
# When packaging from pre-staged libs (no CMake), it must be staged manually.
# Strip debug symbols to reduce the 85MB debug .so to ~16MB release size.
ENGINE_MAIN="${BUILD_DIR}/GeneralsMD/Code/Main/libmain.so"
if [[ -f "${ENGINE_MAIN}" ]]; then
    "${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip" --strip-debug \
        -o "${JNI_LIBS}/libmain.so" "${ENGINE_MAIN}"
    echo "    staged libmain.so (stripped)"
elif [[ ! -f "${JNI_LIBS}/libmain.so" ]]; then
    echo "WARNING: libmain.so not found at ${ENGINE_MAIN} — the APK will have no engine"
fi

# ---- 3b. Verify the full runtime library set is present -----------------------
# The engine dlopens all of these at startup; a missing one crashes on launch
# with "dlopen failed: library X not found". Assert before building the APK.
REQUIRED_LIBS=(
    libdxvk_d3d8.so libdxvk_d3d9.so libSDL3.so libSDL3_image.so
    libopenal.so libfreetype.so libglm.so libgamespy.so
    libc++_shared.so libmain.so
)
MISSING=()
for lib in "${REQUIRED_LIBS[@]}"; do
    [[ -f "${JNI_LIBS}/${lib}" ]] || MISSING+=("${lib}")
done
if [[ ${#MISSING[@]} -gt 0 ]]; then
    echo "ERROR: Missing native libraries: ${MISSING[*]}"
    echo "       These are dlopen'd at runtime and must be in jniLibs/arm64-v8a/."
    exit 1
fi
echo "    all ${#REQUIRED_LIBS[@]} runtime libraries present"

# ---- 4. Stage dxvk.conf + fonts into assets -----------------------------------
echo "==> Staging runtime config into assets/"
mkdir -p "${ASSETS}"
cp -f "${ANDROID_DIR}/config/dxvk.conf" "${ASSETS}/dxvk.conf"
mkdir -p "${ASSETS}/fonts"
cp -f "${HOME}/GeneralsX/android-staging/fonts/"*.ttf "${ASSETS}/fonts/"

# ---- 5. Gradle assembleRelease ------------------------------------------------
echo "==> Building APK (./gradlew assembleRelease)"
cd "${ANDROID_DIR}"
if [[ -x "./gradlew" ]]; then
    ./gradlew assembleRelease
else
    gradle assembleRelease
fi

APK="${APP_DIR}/build/outputs/apk/release/app-release-unsigned.apk"
if [[ ! -f "${APK}" ]]; then
    # AGP may name it differently depending on version.
    APK="$(find "${APP_DIR}/build/outputs/apk/release" -name '*.apk' | head -1)"
fi
[[ -f "${APK}" ]] || { echo "ERROR: no release APK produced"; exit 1; }
echo "==> APK built: ${APK}"

# ---- 6. Sign ------------------------------------------------------------------
echo "==> Signing APK"
KEYSTORE="${ANDROID_KEYSTORE:-${HOME}/.android/debug.keystore}"
if [[ ! -f "${KEYSTORE}" ]]; then
    echo "    no keystore at ${KEYSTORE}; creating a debug keystore"
    mkdir -p "$(dirname "${KEYSTORE}")"
    keytool -genkeypair -keystore "${KEYSTORE}" -storepass android -alias androiddebugkey \
        -keypass android -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US"
fi
SIGNED_APK="${APP_DIR}/build/outputs/apk/release/app-release.apk"
APKSIGNER="${ANDROID_HOME}/build-tools/35.0.0/apksigner"
"${APKSIGNER}" sign --ks "${KEYSTORE}" --ks-pass pass:android --key-pass pass:android \
    --out "${SIGNED_APK}" "${APK}"
echo "==> Signed APK: ${SIGNED_APK}"

# ---- 7. Optional install ------------------------------------------------------
if [[ "${1:-}" == "--install" ]]; then
    echo "==> adb install"
    adb install -r "${SIGNED_APK}"
fi

echo ""
echo "Done. Install with: adb install -r ${SIGNED_APK}"
echo "GameData note: this script does NOT bundle the ~1.5GB of .big archives."
echo "  Push them to the device's external storage (matches the README):"
echo "  adb shell mkdir -p /sdcard/Android/data/me.generalsx.zh/files/GameData/Data"
echo "  adb push <local Data/*.big> /sdcard/Android/data/me.generalsx.zh/files/GameData/Data/"
