#!/bin/bash
# Linux tests of the patched MLT (patches/mlt), built as on RISC OS (static,
# MLT_NO_THREADS, the Pi's hardware decoders preferred) by build/build-host.sh.
# Needs the host's ffmpeg command (with libx264) to make the test clips.
#   melt:         a crossfade project made, saved and rendered by melt with no
#                 threads; progress reported; the output checked
#   pull_test:    the project played as Reel Edit plays it (no consumer thread)
#   engine_test:  src/engine.c, the player, polled as the Wimp polls it
#   prefer_test:  the preferred decoder taken, refused, failing part way, off
# Usage: tests/host/run.sh   (after build/build-host.sh)
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
H=${OUT:-$TOP/out}/host
S=$H/stage
O=$H/tests
CC=${CC:-gcc}
mkdir -p "$O"
bad=0
fail() { echo "FAIL: $*"; bad=1; }

# ---- the clips
mk() { ffmpeg -v error -y "$@"; }
mk -f lavfi -i color=c=red:s=320x180:r=25:d=3 -c:v libx264 -qp 0 -bf 0 -pix_fmt yuv420p "$O/a.mp4"
mk -f lavfi -i color=c=blue:s=320x180:r=25:d=3 -c:v libx264 -qp 0 -bf 0 -pix_fmt yuv420p "$O/b.mp4"
mk -f lavfi -i color=c=gray:s=320x180:r=25:d=3 -vf "geq=lum='16+2*N':cb=128:cr=128" \
  -c:v libx264 -qp 0 -bf 0 -g 25 -pix_fmt yuv420p "$O/numbered.mp4"

# ---- the test programs, linked as Reel Edit will be: every module built in
export PKG_CONFIG_PATH="$H/ffmpeg/lib/pkgconfig"
FFLIBS=$(pkg-config --static --libs libavfilter libavformat libavcodec libswresample libswscale libavutil)
MLTLIBS="-L$S/lib -lmlt-static-modules -lmltcore -lmltavformat -lmltxml -lmlt-7 $(pkg-config --libs libxml-2.0)"
CFLAGS="-O1 -g -Wall $(PKG_CONFIG_PATH=$S/lib/pkgconfig pkg-config --cflags mlt-framework-7) -I$H/ffmpeg/include"
$CC $CFLAGS "$HERE/pull_test.c" -o "$O/pull_test" -Wl,--wrap=pthread_create $MLTLIBS $FFLIBS -lpthread -lm || bad=1
$CC $CFLAGS "$HERE/prefer_test.c" -o "$O/prefer_test" \
  -Wl,--wrap=avcodec_find_decoder_by_name,--wrap=avcodec_open2,--wrap=avcodec_send_packet \
  -Wl,--wrap=avcodec_free_context,--wrap=av_opt_set_int $MLTLIBS $FFLIBS -lpthread -lm || bad=1
$CC $CFLAGS -I"$TOP/src" "$TOP/src/engine.c" "$HERE/engine_test.c" -o "$O/engine_test" \
  -Wl,--wrap=pthread_create $MLTLIBS $FFLIBS -lpthread -lm || bad=1
$CC -shared -fPIC "$HERE/count_threads.c" -o "$O/count_threads.so" -ldl || bad=1

# ---- melt: make and save the project, then render it with no threads
cd "$O"
echo "== melt: a crossfade project, saved as MLT XML"
"$S/bin/melt" -profile hdv_720_25p a.mp4 b.mp4 -mix 25 -mixer luma -consumer xml:project.mlt > melt-xml.log 2>&1 \
  || fail "melt couldn't save the project (melt-xml.log)"
grep -q 'mlt_service">luma' project.mlt || fail "project.mlt has no luma transition"

echo "== melt: rendered, with no threads"
LD_PRELOAD=$O/count_threads.so "$S/bin/melt" project.mlt -consumer avformat:render.mp4 vcodec=mpeg4 an=1 \
  > melt-render.log 2>&1 || fail "melt couldn't render (melt-render.log)"
grep -q "count_threads: 0" melt-render.log || fail "melt started threads: $(grep count_threads melt-render.log)"
grep -q "Current Frame:.*percentage:" melt-render.log || fail "melt reported no progress"
frames=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 render.mp4)
[ "$frames" = 125 ] || fail "render.mp4 has $frames frames, not 125"

echo "== melt: with MLT_NO_THREADS=0, threads again (the setting works both ways)"
MLT_NO_THREADS=0 LD_PRELOAD=$O/count_threads.so "$S/bin/melt" project.mlt -consumer avformat:render2.mp4 \
  vcodec=mpeg4 an=1 > melt-render2.log 2>&1 || fail "melt couldn't render with threads"
grep -q "count_threads: 0" melt-render2.log && fail "MLT_NO_THREADS=0 started no threads"

echo "== pull_test"
"$O/pull_test" project.mlt a.mp4 b.mp4 || bad=1
echo "== engine_test"
"$O/engine_test" project.mlt || bad=1
for m in normal refuse late off; do
  echo "== prefer_test $m"
  "$O/prefer_test" $m numbered.mp4 2>&1 | grep -v "^\[" || bad=1
done

if [ $bad = 0 ]; then echo "== all passed"; else echo "== FAILED"; fi
exit $bad
