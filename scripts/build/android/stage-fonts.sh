#!/bin/bash
# Stage the fonts the Android app bundles. Mirrors ios/stage-fonts.sh: the game
# asks for Windows font names (arial.ttf etc.); we use the metric-compatible
# Liberation fonts (SIL OFL) renamed accordingly. Pinned release for reproducibility.
#
# GeneralsX @feature android-port 06/07/2026
set -euo pipefail

LIB_VERSION="2.1.5"
LIB_SHA256="7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0"
DEST="${GX_FONTS:-${HOME}/GeneralsX/android-staging/fonts}"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

mkdir -p "${DEST}"
if [[ -f "${DEST}/arial.ttf" && -f "${DEST}/arialbold.ttf" && -f "${DEST}/couriernew.ttf" && -f "${DEST}/timesnewroman.ttf" ]]; then
    echo "Fonts already staged at ${DEST}"
    exit 0
fi
echo "==> Downloading Liberation fonts ${LIB_VERSION}"
if ! curl -fL -o "${TMP}/liberation.tar.gz" \
    "https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-${LIB_VERSION}.tar.gz" &&
   ! curl -fL -o "${TMP}/liberation.tar.gz" \
    "https://github.com/liberationfonts/liberation-fonts/releases/download/${LIB_VERSION}/liberation-fonts-ttf-${LIB_VERSION}.tar.gz"; then
    echo "ERROR: could not download Liberation fonts ${LIB_VERSION} (check network / URLs)."
    exit 1
fi
echo "${LIB_SHA256}  ${TMP}/liberation.tar.gz" | shasum -a 256 -c -
tar -xzf "${TMP}/liberation.tar.gz" -C "${TMP}"
SRC="$(find "${TMP}" -name "LiberationSans-Regular.ttf" -exec dirname {} \; | head -1)"
[[ -n "${SRC}" ]] || { echo "ERROR: Liberation fonts not found in archive"; exit 1; }
cp "${SRC}/LiberationSans-Regular.ttf"   "${DEST}/arial.ttf"
cp "${SRC}/LiberationSans-Bold.ttf"      "${DEST}/arialbold.ttf"
cp "${SRC}/LiberationMono-Regular.ttf"   "${DEST}/couriernew.ttf"
cp "${SRC}/LiberationSerif-Regular.ttf"  "${DEST}/timesnewroman.ttf"
echo "Fonts staged at ${DEST}"
