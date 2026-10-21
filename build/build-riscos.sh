#!/bin/bash
# Cross-build MLT for RISC OS: libxml2, then MLT (patched, static, no
# threads, the Pi's hardware decoders preferred) and melt, against the
# riscos-ffmpeg and riscos-mesa devkits. Everything goes in out/riscos/:
#   devkit/   the two devkits merged (include/, lib/, lib/pkgconfig/)
#   stage/    libxml2 and MLT installed (include/, lib/, share/mlt-7/)
#   melt,ff8  melt, stripped and converted to an Absolute file
# Usage: build/build-riscos.sh   (after build/fetch-sources.sh)
set -euo pipefail
. "$(dirname "$0")/env.sh"
O=$OUT/riscos
DK=$O/devkit
STAGE=$O/stage
mkdir -p "$O/src" "$DK" "$STAGE"

# ---- the devkits, merged: one prefix for CMake and pkg-config
unpack_devkit() {   # file, directory
  if [ ! -d "$O/src/$2" ]; then
    mkdir -p "$O/src/$2.tmp"
    tar xzf "$DL/$1" -C "$O/src/$2.tmp" --strip-components=1
    mv "$O/src/$2.tmp" "$O/src/$2"
  fi
}
unpack_devkit "riscos-ffmpeg-devkit-$FFMPEG_DEVKIT_VERSION.tgz" ffmpeg-devkit
unpack_devkit "riscos-mesa-devkit-$MESA_DEVKIT_VERSION.tgz" mesa-devkit
rm -rf "$DK" && mkdir -p "$DK/lib/pkgconfig" "$DK/include"
for d in mesa-devkit ffmpeg-devkit; do
  for f in "$O/src/$d"/include/*; do ln -sfn "$f" "$DK/include/"; done
  for f in "$O/src/$d"/lib/*.a; do ln -sfn "$f" "$DK/lib/"; done
  for f in "$O/src/$d"/lib/pkgconfig/*.pc; do ln -sfn "$f" "$DK/lib/pkgconfig/"; done
done
# (the .pc files' prefix is ${pcfiledir}/../..: pkg-config takes it from
# where it found the file, the merged devkit, whose include/ and lib/ have
# both devkits' files)

CMAKE_RISCOS=(-DCMAKE_TOOLCHAIN_FILE="$RCD_DIR/cmake/riscos.cmake" -DRISCOS_DEVKIT="$DK"
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$STAGE" -DCMAKE_PREFIX_PATH="$STAGE")
# (CMAKE_C_FLAGS given here replaces the toolchain file's defaults, so they are
# repeated: -fstack-clash-protection and the FPU are required on RISC OS)
CFLAGS_EXTRA="-mfpu=$FPU -mfloat-abi=hard -fstack-clash-protection -D_FILE_OFFSET_BITS=64"

# ---- libxml2 (for MLT's xml module): no threads, no network, no iconv.
# Installed into the merged devkit, where CMake's find_library looks.
XML=$O/src/libxml2
if [ ! -f "$XML/build/libxml2.a" ]; then
  rm -rf "$XML" && mkdir -p "$XML"
  tar xJf "$DL"/libxml2_*.orig.tar.xz -C "$XML" --strip-components=1
  cmake -S "$XML" -B "$XML/build" "${CMAKE_RISCOS[@]}" -DCMAKE_INSTALL_PREFIX="$DK" -DCMAKE_C_FLAGS="$CFLAGS_EXTRA" \
    -DBUILD_SHARED_LIBS=OFF -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_ICONV=OFF \
    -DLIBXML2_WITH_ICU=OFF -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ZLIB=OFF \
    -DLIBXML2_WITH_HTTP=OFF -DLIBXML2_WITH_THREADS=OFF -DLIBXML2_WITH_PROGRAMS=OFF \
    -DLIBXML2_WITH_TESTS=OFF -DLIBXML2_WITH_MODULES=OFF -DLIBXML2_WITH_READLINE=OFF \
    -DLIBXML2_WITH_HISTORY=OFF -DLIBXML2_WITH_DOCS=OFF > "$O/libxml2-configure.log"
  cmake --build "$XML/build" -j"$JOBS" > "$O/libxml2-build.log" 2>&1 || { tail -30 "$O/libxml2-build.log"; exit 1; }
fi
cmake --install "$XML/build" > /dev/null

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
# Static libraries are linked in order: the whole FFmpeg set (avformat
# before avcodec, zlib last) again at the end of every link
FF_LIBS="$(PKG_CONFIG_LIBDIR=$DK/lib/pkgconfig PKG_CONFIG_PATH= pkg-config --static --libs \
  libavfilter libavformat libavcodec libswresample libswscale libavutil) -lz"
cmake -S "$M" -B "$M/build" "${CMAKE_RISCOS[@]}" -DCMAKE_C_FLAGS="$CFLAGS_EXTRA -include $TOP/build/riscos-compat.h" \
  -DCMAKE_C_STANDARD_LIBRARIES="$FF_LIBS" -DCMAKE_CXX_STANDARD_LIBRARIES="$FF_LIBS" \
  -DMLT_STATIC=ON -DMLT_NO_THREADS=ON -DMLT_AVFORMAT_PREFER_DEFAULT="h264=h264_vchiq,hevc=hevc_hwdec" -DMLT_AVFORMAT_PREFER_MAX_DEFAULT=1 -DMLT_AVFORMAT_CACHE_DEFAULT=0 -DMLT_AVFORMAT_CONCEAL_BY_REFERENCE=ON \
  -DMOD_AVFORMAT=ON -DUSE_AVDEVICE=OFF -DMOD_XML=ON "${MODS_OFF[@]}" -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF \
  -DSWIG_PYTHON=OFF -DSWIG_RUBY=OFF -DSWIG_LUA=OFF -DSWIG_PERL=OFF -DSWIG_PHP=OFF -DSWIG_TCL=OFF \
  -DSWIG_CSHARP=OFF -DSWIG_JAVA=OFF -DSWIG_NODEJS=OFF > "$O/mlt-configure.log" 2>&1 || { tail -30 "$O/mlt-configure.log"; exit 1; }
# (never test or package a build left from before)
rm -rf "$STAGE"
cmake --build "$M/build" -j"$JOBS" > "$O/mlt-build.log" 2>&1 || { tail -30 "$O/mlt-build.log"; exit 1; }
cmake --install "$M/build" > /dev/null

# ---- MLT's sources: casts that could load or store unaligned. Each one
# left was checked by hand (docs/PORTING.md); a new one fails the build.
(cd "$M" && "$RCD_DIR/checks/check-unaligned.sh" -s "$TOP/build/unaligned-safe.txt" \
  -Isrc -Isrc/framework -Ibuild/src/framework -Ibuild/src/modules/core -Ibuild/src/modules/avformat \
  -Ibuild/src/modules/xml -I"$DK/include" -include "$TOP/build/riscos-compat.h" \
  src/framework/*.c src/modules/core/*.c src/modules/avformat/*.c src/modules/xml/*.c src/melt/*.c)

# ---- melt: checked, then an Absolute file
"$RCD_DIR/checks/check-all.sh" "$STAGE/bin/melt"
"$RISCOS_TOOLCHAIN/bin/arm-riscos-gnueabihf-strip" -o "$O/melt.elf" "$STAGE/bin/melt"
"$RISCOS_TOOLCHAIN/bin/elf2aif" -e "$O/melt.elf" "$O/melt,ff8" > /dev/null
echo "built $O/melt,ff8"

# ---- bench (tools/bench.c): pictures made as Reel Edit's player asks for them
CC="$RISCOS_TOOLCHAIN/bin/arm-riscos-gnueabihf-gcc"
"$CC" -O2 $CFLAGS_EXTRA -DMLT_STATIC_DEFINE -I"$STAGE/include/mlt-7" -I"$DK/include" "$TOP/tools/bench.c" -o "$O/bench" \
  -static -L"$STAGE/lib" -L"$DK/lib" -lmlt-static-modules -lmltcore -lmltavformat -lmltxml -lmlt-7 -lxml2 \
  $FF_LIBS -lm
"$RCD_DIR/checks/check-all.sh" "$O/bench" > /dev/null || { "$RCD_DIR/checks/check-all.sh" "$O/bench"; exit 1; }
"$RISCOS_TOOLCHAIN/bin/arm-riscos-gnueabihf-strip" -o "$O/bench.elf" "$O/bench"
"$RISCOS_TOOLCHAIN/bin/elf2aif" -e "$O/bench.elf" "$O/bench,ff8" > /dev/null
echo "built $O/bench,ff8"

# ---- !ReelEdit (src/): the engine and the desktop front end
RE_VERSION=${RE_VERSION:-0.1}
"$RCD_DIR/checks/check-unaligned.sh" -I"$STAGE/include/mlt-7" -I"$DK/include" -I"$TOP/src" "$TOP"/src/*.c "$TOP"/src/riscos/*.c
"$CC" -O2 -Wall $CFLAGS_EXTRA -DMLT_STATIC_DEFINE -DRE_VERSION="\"$RE_VERSION\"" -I"$STAGE/include/mlt-7" -I"$DK/include" \
  "$TOP/src/engine.c" "$TOP/src/project.c" "$TOP/src/riscos/viewer.c" "$TOP/src/riscos/timeline.c" \
  "$TOP/src/riscos/savebox.c" "$TOP/src/riscos/display.c" "$TOP/src/riscos/sound.c" \
  "$TOP/src/riscos/main.c" -o "$O/reeledit" \
  -static -L"$STAGE/lib" -L"$DK/lib" -lmlt-static-modules -lmltcore -lmltavformat -lmltxml -lmlt-7 -lxml2 \
  $FF_LIBS -lm
"$RCD_DIR/checks/check-all.sh" "$O/reeledit" > /dev/null || { "$RCD_DIR/checks/check-all.sh" "$O/reeledit"; exit 1; }
"$RISCOS_TOOLCHAIN/bin/arm-riscos-gnueabihf-strip" -o "$O/reeledit.elf" "$O/reeledit"
"$RISCOS_TOOLCHAIN/bin/elf2aif" -e "$O/reeledit.elf" "$O/reeledit,ff8" > /dev/null
echo "built $O/reeledit,ff8 ($RE_VERSION)"
