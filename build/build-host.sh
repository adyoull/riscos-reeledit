#!/bin/bash
# Build MLT for the Linux tests: the same patches and settings as the
# RISC OS build (static, MLT_NO_THREADS, the same preferred decoders),
# against FFmpeg 5.1.10 built here, with the host's gcc and libxml2.
# Everything goes in out/host/: ffmpeg/ (FFmpeg installed), stage/ (MLT).
# Usage: build/build-host.sh   (after build/fetch-sources.sh)
set -euo pipefail
TOP=$(cd "$(dirname "$0")/.." && pwd)
DL=${DL:-$TOP/dl}
O=${OUT:-$TOP/out}/host
JOBS=${JOBS:-$(nproc)}
CC=${CC:-gcc}
MLT_VERSION=$(sed -n 's/^mlt-\([0-9.]*\)\.tar\.gz.*/\1/p' "$TOP/build/SOURCES")
mkdir -p "$O/src"

# ---- FFmpeg 5.1.10, static, no external libraries
FF=$O/src/ffmpeg-5.1.10
if [ ! -f "$O/ffmpeg/lib/libavcodec.a" ]; then
  rm -rf "$FF" && mkdir -p "$FF"
  tar xJf "$DL/ffmpeg-5.1.10.tar.xz" -C "$FF" --strip-components=1
  (cd "$FF" && ./configure --prefix="$O/ffmpeg" --cc="$CC" --disable-programs --disable-doc \
      --disable-network --disable-autodetect --disable-x86asm --enable-gpl \
      --disable-shared --enable-static > "$O/ffmpeg-configure.log" &&
    make -j"$JOBS" > "$O/ffmpeg-build.log" 2>&1 && make install > /dev/null)
fi

# ---- MLT, with patches/mlt
M=$O/src/mlt-$MLT_VERSION
if [ ! -f "$M/.patched" ]; then
  rm -rf "$M" && mkdir -p "$M"
  tar xzf "$DL/mlt-$MLT_VERSION.tar.gz" -C "$M" --strip-components=1
  for p in "$TOP"/patches/mlt/*.patch; do patch -s -p1 -d "$M" < "$p"; done
  touch "$M/.patched"
fi
MODS_OFF=()
for m in DECKLINK FREI0R GDK GLAXNIMATE_QT6 JACKRACK KDENLIVE MOVIT NDI NORMALIZE OLDFILM OPENCV OPENFX \
         PLACEBO PLUS PLUSGPL QT6 RESAMPLE RTAUDIO RUBBERBAND RNNOISE SDL1 SDL2 SOX SPATIALAUDIO VIDSTAB \
         VORBIS XINE; do
  MODS_OFF+=(-DMOD_$m=OFF)
done
export PKG_CONFIG_PATH="$O/ffmpeg/lib/pkgconfig"
FF_LIBS="$(pkg-config --static --libs libavfilter libavformat libavcodec libswresample libswscale libavutil)"
cmake -S "$M" -B "$M/build" -DCMAKE_C_COMPILER="$CC" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$O/stage" -DCMAKE_C_STANDARD_LIBRARIES="$FF_LIBS" -DCMAKE_CXX_STANDARD_LIBRARIES="$FF_LIBS" \
  -DMLT_STATIC=ON -DMLT_NO_THREADS=ON -DMLT_AVFORMAT_PREFER_DEFAULT="h264=h264_vchiq,hevc=hevc_hwdec" \
  -DMOD_AVFORMAT=ON -DUSE_AVDEVICE=OFF -DMOD_XML=ON "${MODS_OFF[@]}" -DBUILD_TESTING=OFF \
  -DBUILD_DOCS=OFF -DCLANG_FORMAT=OFF > "$O/mlt-configure.log"
cmake --build "$M/build" -j"$JOBS" > "$O/mlt-build.log" 2>&1 || { tail -30 "$O/mlt-build.log"; exit 1; }
cmake --install "$M/build" > /dev/null
echo "built $O/stage"
