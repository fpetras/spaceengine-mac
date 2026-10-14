#!/bin/sh
# Builds the FFmpeg the game's video recorder needs, into recomp/third_party/ffmpeg.
#
# The game was built against FFmpeg 8 (avcodec-62) and accepts only an LGPL
# build, which it checks through avcodec_license(). Homebrew's FFmpeg is a GPL
# build of a newer major version, so this builds 8.1.3 itself: LGPL, shared,
# with the SVT-AV1 encoder (Homebrew's svt-av1, BSD licensed; the game lists
# libsvtav1 among its encoders) and Apple's VideoToolbox. Nothing is
# installed outside the project. The source is checked against the checksum
# Homebrew records for this release.
set -e
VERSION=8.1.3
SHA256=7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/recomp/third_party/src"
OUT="$ROOT/recomp/third_party/ffmpeg"
mkdir -p "$SRC"
TARBALL="$SRC/ffmpeg-$VERSION.tar.xz"
[ -f "$TARBALL" ] || curl -fL -o "$TARBALL" "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"
echo "$SHA256  $TARBALL" | shasum -a 256 -c -
rm -rf "$SRC/ffmpeg-$VERSION"
tar -xf "$TARBALL" -C "$SRC"
cd "$SRC/ffmpeg-$VERSION"
PKG_CONFIG_PATH=/opt/homebrew/opt/svt-av1/lib/pkgconfig ./configure \
    --prefix="$OUT" --enable-shared --disable-static --disable-programs --disable-doc \
    --disable-gpl --disable-nonfree --enable-libsvtav1 --enable-videotoolbox \
    --disable-sdl2 --disable-xlib --disable-lzma \
    --pkg-config="$ROOT/tools/pkg-config-lite.py" --install-name-dir="$OUT/lib"
make -j"$(sysctl -n hw.ncpu)"
make install
echo "license: $(grep -h 'define FFMPEG_LICENSE' libavutil/ffversion.h config.h 2>/dev/null | head -1)"
