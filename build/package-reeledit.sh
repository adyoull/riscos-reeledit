#!/bin/bash
# Make the zip of !ReelEdit: the program, MLT's data files, the Obey files
# and icons from app/!ReelEdit. Out: out/ReelEdit-$RE_VERSION.zip
# Usage: RE_VERSION=0.1-test1 build/package-reeledit.sh   (after build/build-riscos.sh
#        with the same RE_VERSION)
# Part of riscos-reeledit. GPL version 2 or later.
set -euo pipefail
. "$(dirname "$0")/env.sh"
RE_VERSION=${RE_VERSION:-0.1}
P=$OUT/package-reeledit
A="$P/!ReelEdit"
rm -rf "$P" && mkdir -p "$A"
cp "$TOP"/app/'!ReelEdit'/* "$A/"
cp "$OUT/riscos/reeledit,ff8" "$A/!RunImage,ff8"
python3 "$RCD_DIR/package/png2sprite.py" "$A/!Sprites,ff9" '!reeledit='"$TOP/app/icons/reeledit.png" \
  'sm!reeledit='"$TOP/app/icons/sm-reeledit.png" > /dev/null
mkdir -p "$A/share"
cp -r "$OUT/riscos/stage/share/mlt-7" "$A/share/"
mkdir -p "$A/Guide" && cp -r "$TOP/docs/guide/index.html" "$TOP/docs/guide/images" "$A/Guide/"
# the licences of everything in !RunImage (app/Licences/ReadMe lists them)
FL="$OUT/riscos/src/ffmpeg-devkit-$FFMPEG_DEVKIT_VERSION/Licences"
LC="$A/Licences"
mkdir -p "$LC"
cp "$TOP/app/Licences/ReadMe" "$LC/ReadMe,fff"
for f in FFmpeg GPLv2 LGPLv21 x264 ReelHWAccel LAME Vorbis Ogg Opus dav1d PThreadTicker; do
  cp "$FL/$f,fff" "$LC/$f,fff"
done
cp "$OUT/riscos/src/mlt-7.42.0/COPYING" "$LC/MLT,fff"
cp "$OUT/riscos/src/libxml2/Copyright" "$LC/libxml2,fff"
cp "$OUT/riscos/src/mlt-7.42.0/src/modules/plus/ebur128/COPYING" "$LC/libebur128,fff"
cp "$OUT/riscos/src/mesa-devkit-$MESA_DEVKIT_VERSION/LICENCES.txt" "$LC/Toolchain,fff"
python3 "$RCD_DIR/package/rozip.py" "$OUT/ReelEdit-$RE_VERSION.zip" -C "$P" '!ReelEdit'
STRS=$(strings -a "$A/!RunImage,ff8")
if grep -iqE 'cl[a]ude|anthr[o]pic' <<< "$STRS"; then
  echo "package: a name that mustn't be there is in !RunImage" >&2; exit 1
fi
grep -qx "$RE_VERSION" <<< "$STRS" || { echo "package: !RunImage isn't version $RE_VERSION" >&2; exit 1; }
(cd "$OUT" && sha256sum "ReelEdit-$RE_VERSION.zip")
