#!/bin/bash
# Linux tests of the patched MLT (patches/mlt), built as on RISC OS (static,
# MLT_NO_THREADS, the Pi's hardware decoders preferred) by build/build-host.sh.
# Needs the host's ffmpeg command (with libx264) to make the test clips.
#   melt:         a crossfade project made, saved and rendered by melt with no
#                 threads; progress reported; the output checked
#   pull_test:    the project played as ReelEdit plays it (no consumer thread)
#   convert_test: pictures asked for in their own format and size aren't converted
#   render_test:  a started consumer renders each frame's picture and sound
#   engine_test:  src/engine.c, the player, polled as the Wimp polls it
#   project_test: the editing model (src/project.c), and the engine playing it
#   editor_test:  !ReelEdit (src/riscos) with a scripted fake Wimp
#   prefer_test:  the preferred decoder taken, refused, failing part way, off
# Usage: tests/host/run.sh   (after build/build-host.sh)
#        SCREENS=dir tests/host/run.sh: editor_test's screenshots too (PNGs; screens.py)
# Part of riscos-reeledit. GPL version 2 or later.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
H=${OUT:-$TOP/out}/host
S=$H/stage
O=$H/tests
CC=${CC:-gcc}
mkdir -p "$O"
bad=0
# (never run a test program left from before: a failed build would go unseen)
rm -f "$O"/pull_test "$O"/prefer_test "$O"/engine_test "$O"/convert_test "$O"/render_test "$O"/editor_test "$O"/project_test
fail() { echo "FAIL: $*"; bad=1; }

# ---- the clips
mk() { ffmpeg -v error -y "$@"; }
mk -f lavfi -i color=c=red:s=320x180:r=25:d=3 -c:v libx264 -qp 0 -bf 0 -pix_fmt yuv420p "$O/a.mp4"
mk -f lavfi -i color=c=blue:s=320x180:r=25:d=3 -c:v libx264 -qp 0 -bf 0 -pix_fmt yuv420p "$O/b.mp4"
mk -f lavfi -i color=c=gray:s=320x180:r=25:d=3 -vf "geq=lum='16+2*N':cb=128:cr=128" \
  -c:v libx264 -qp 0 -bf 0 -g 25 -pix_fmt yuv420p "$O/numbered.mp4"
mk -f lavfi -i color=c=gray:s=320x180:r=25:d=3 -f lavfi -i sine=frequency=440:sample_rate=44100:duration=3 \
  -vf "geq=lum='16+2*N':cb=128:cr=128" -c:v libx264 -qp 0 -bf 0 -g 25 -pix_fmt yuv420p -c:a aac -shortest \
  "$O/numbered-snd.mp4"
# (raw MPEG-2 video: no container, so no length; MLT takes it to be endless)
mk -i "$O/numbered.mp4" -c:v mpeg2video -q:v 2 -f mpeg2video "$O/numbered.m2v"
# (other sizes and shapes: 4:3 in BT.709, and bigger than the overlay takes)
mk -f lavfi -i color=c=green:s=480x360:r=25:d=3 -c:v libx264 -bf 0 -pix_fmt yuv420p -colorspace bt709 \
  -color_primaries bt709 -color_trc bt709 "$O/four3-709.mp4"
mk -f lavfi -i color=c=yellow:s=2560x1440:r=25:d=1 -c:v libx264 -bf 0 -pix_fmt yuv420p "$O/big.mp4"
# (pictures: as big as a.mp4, its left half see-through and its right white; a white logo; a JPEG)
mk -f lavfi -i "color=c=white:s=320x180,format=rgba,geq=r='255':g='255':b='255':a='if(lt(X,160),0,255)'" -frames:v 1 "$O/full.png"
mk -f lavfi -i color=c=white:s=32x16 -frames:v 1 "$O/logo.png"
mk -f lavfi -i color=c=green:s=160x90 -frames:v 1 "$O/pic.jpg"
# (for green screen: green, a white box in the middle)
mk -f lavfi -i "color=c=0x00ff00:s=320x180:r=25:d=3,drawbox=x=120:y=60:w=80:h=60:color=white:t=fill" -c:v libx264 -qp 0 \
  -bf 0 -pix_fmt yuv420p "$O/green.mp4"
# (for pan and zoom: dark at the left to light at the right)
mk -f lavfi -i "color=c=black:s=320x180,format=rgb24,geq=r='X*255/319':g='X*255/319':b='X*255/319'" -frames:v 1 "$O/ramp.png"
# (sounds for the audio tracks: two tones, 2 seconds each)
mk -f lavfi -i sine=frequency=880:sample_rate=48000:duration=2 -ac 2 "$O/tone880.wav"
mk -f lavfi -i sine=frequency=1320:sample_rate=48000:duration=2 -ac 2 "$O/tone1320.wav"
# (a waveform's: a second of tone, then a second of silence; 16-bit mono)
mk -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=1,apad=pad_dur=1" "$O/half.wav"
# (for sound clean-up: a low hum, a tone where voices are, a quiet tone)
mk -f lavfi -i sine=frequency=50:sample_rate=48000:duration=2 -ac 2 "$O/low.wav"
mk -f lavfi -i sine=frequency=3000:sample_rate=48000:duration=2 -ac 2 "$O/tone3k.wav"
mk -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2,volume=0.03" -ac 2 "$O/quiet.wav"
# (anamorphic: 720x576 shown 16:9, as DVDs are)
# (an odd height: FFV1 keeps it; yuv420p's chroma laid out as MLT does, patches/mlt 0019)
mk -f lavfi -i "color=c=red:s=64x38:r=25:d=1,format=yuv444p,crop=64:37:0:0,format=yuv420p" -c:v ffv1 "$O/odd.mkv"
mk -f lavfi -i color=c=blue:s=720x576:r=25:d=1 -vf setsar=64/45 -c:v libx264 -bf 0 -pix_fmt yuv420p "$O/wide-dvd.mp4"

# ---- the test programs, linked as ReelEdit will be: every module built in
export PKG_CONFIG_PATH="$H/ffmpeg/lib/pkgconfig"
FFLIBS=$(pkg-config --static --libs libavfilter libavformat libavcodec libswresample libswscale libavutil)
MLTLIBS="-L$S/lib -lmlt-static-modules -lmltcore -lmltavformat -lmltxml -lmltnormalize -lmltplus -lmlt-7 $(pkg-config --libs libxml-2.0) -lstdc++"
CFLAGS="-O1 -g -Wall $(PKG_CONFIG_PATH=$S/lib/pkgconfig pkg-config --cflags mlt-framework-7) -I$H/ffmpeg/include"
$CC $CFLAGS "$HERE/pull_test.c" -o "$O/pull_test" -Wl,--wrap=pthread_create $MLTLIBS $FFLIBS -lpthread -lm || bad=1
$CC $CFLAGS "$HERE/prefer_test.c" -o "$O/prefer_test" \
  -Wl,--wrap=avcodec_find_decoder_by_name,--wrap=avcodec_open2,--wrap=avcodec_send_packet \
  -Wl,--wrap=avcodec_free_context,--wrap=av_opt_set_int $MLTLIBS $FFLIBS -lpthread -lm || bad=1
$CC $CFLAGS -I"$TOP/src" "$TOP/src/engine.c" "$TOP/src/still.c" "$TOP/src/affine.c" "$TOP/src/scale.c" "$TOP/src/chroma.c" "$TOP/src/looks.c" "$TOP/src/lut.c" "$TOP/src/neon.c" "$TOP/src/hold.c" "$TOP/src/title.c" "$HERE/engine_test.c" -o "$O/engine_test" \
  -Wl,--wrap=pthread_create $MLTLIBS $FFLIBS -lpthread -lm || bad=1
$CC $CFLAGS "$HERE/convert_test.c" -o "$O/convert_test" -Wl,--wrap=sws_scale,--wrap=mlt_frame_clone,--wrap=mlt_frame_clone_image,--wrap=avcodec_receive_frame $MLTLIBS $FFLIBS -lpthread -lm || bad=1
$CC $CFLAGS "$HERE/render_test.c" -o "$O/render_test" $MLTLIBS $FFLIBS -lpthread -lm || bad=1
$CC $CFLAGS -DRE_TEST -I"$HERE/fake" -I"$TOP/src" "$TOP/src/engine.c" "$TOP/src/project.c" "$TOP/src/dissolve.c" "$TOP/src/fade.c" "$TOP/src/clipsound.c" "$TOP/src/overlay.c" "$TOP/src/track.c" "$TOP/src/audio.c" "$TOP/src/source.c" "$TOP/src/clipboard.c" "$TOP/src/speed.c" "$TOP/src/colour.c" "$TOP/src/soundfx.c" "$TOP/src/motion.c" "$TOP/src/freeze.c" "$TOP/src/wipe.c" "$TOP/src/effect.c" "$TOP/src/key.c" "$TOP/src/loudness.c" "$TOP/src/render.c" "$TOP/src/proxy.c" "$TOP/src/still.c" "$TOP/src/affine.c" "$TOP/src/scale.c" "$TOP/src/chroma.c" "$TOP/src/looks.c" "$TOP/src/lut.c" "$TOP/src/neon.c" "$TOP/src/hold.c" "$TOP/src/title.c" "$TOP/src/export.c" "$TOP/src/sprites.c" "$TOP/src/measure.c" "$TOP/src/media.c" "$TOP/src/riscos/viewer.c" \
  "$TOP/src/riscos/timeline.c" "$TOP/src/riscos/savebox.c" "$TOP/src/riscos/info.c" "$TOP/src/riscos/effects.c" "$TOP/src/riscos/marks.c" "$TOP/src/riscos/overlays.c" "$TOP/src/riscos/titles.c" "$TOP/src/riscos/exportbox.c" "$TOP/src/riscos/audiotracks.c" "$TOP/src/riscos/autosave.c" "$TOP/src/riscos/clips.c" "$TOP/src/riscos/previews.c" "$TOP/src/riscos/copypaste.c" "$TOP/src/riscos/renders.c" "$TOP/src/riscos/proxies.c" "$TOP/src/riscos/children.c" "$TOP/src/riscos/logging.c" "$TOP/src/riscos/guide.c" "$TOP/src/riscos/inspector.c" "$TOP/src/riscos/display.c" \
  "$TOP/src/riscos/sound.c" "$HERE/editor_test.c" -o "$O/editor_test" $MLTLIBS $FFLIBS -lpthread -lm || bad=1
$CC $CFLAGS -I"$TOP/src" "$TOP/src/engine.c" "$TOP/src/project.c" "$TOP/src/dissolve.c" "$TOP/src/fade.c" "$TOP/src/clipsound.c" "$TOP/src/overlay.c" "$TOP/src/track.c" "$TOP/src/audio.c" "$TOP/src/source.c" "$TOP/src/clipboard.c" "$TOP/src/speed.c" "$TOP/src/colour.c" "$TOP/src/soundfx.c" "$TOP/src/motion.c" "$TOP/src/freeze.c" "$TOP/src/wipe.c" "$TOP/src/effect.c" "$TOP/src/key.c" "$TOP/src/loudness.c" "$TOP/src/render.c" "$TOP/src/proxy.c" "$TOP/src/still.c" "$TOP/src/affine.c" "$TOP/src/scale.c" "$TOP/src/chroma.c" "$TOP/src/looks.c" "$TOP/src/lut.c" "$TOP/src/neon.c" "$TOP/src/hold.c" "$TOP/src/title.c" "$TOP/src/export.c" "$TOP/src/sprites.c" "$TOP/src/measure.c" "$TOP/src/media.c" "$HERE/project_test.c" -o "$O/project_test" \
  -Wl,--wrap=mlt_fast_scale_plane $MLTLIBS $FFLIBS -lpthread -lm || bad=1
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
echo "== convert_test"
# the project again at the clips' own size and colours, 320x180 BT.601
sed -e 's/width="1280"/width="320"/' -e 's/height="720"/height="180"/' -e 's/colorspace="709"/colorspace="601"/' project.mlt > project-small.mlt
"$O/convert_test" a.mp4 project-small.mlt numbered.mp4 || bad=1
echo "== render_test"
"$O/render_test" project.mlt || bad=1
echo "== engine_test"
"$O/engine_test" project.mlt || bad=1
echo "== project_test"
"$O/project_test" a.mp4 b.mp4 numbered.mp4 project_test.mlt numbered.m2v four3-709.mp4 big.mp4 wide-dvd.mp4 numbered-snd.mp4 \
  full.png logo.png pic.jpg ramp.png green.mp4 quiet.wav 2>&1 | grep -v "^\[" || bad=1
echo "== editor_test"
env -u SCREENS "$O/editor_test" numbered-snd.mp4 numbered.mp4 numbered.m2v big.mp4 2>&1 | grep -v "^\[" || bad=1
# (screenshots, when asked for: SCREENS=dir tests/host/run.sh; PNGs of the editor at points in the script)
if [ -n "${SCREENS:-}" ]; then
  mkdir -p "$SCREENS" && rm -f "$SCREENS"/*.txt "$SCREENS"/*.raw "$SCREENS"/*.yuv "$SCREENS"/*.png
  SCREENS=$(cd "$SCREENS" && pwd) "$O/editor_test" numbered-snd.mp4 numbered.mp4 numbered.m2v big.mp4 2>&1 | grep "screenshots\|FAIL"
  python3 "$HERE/screens.py" "$SCREENS" > /dev/null || fail "screens.py couldn't draw the screenshots"
  rm -f "$SCREENS"/*.txt "$SCREENS"/*.raw "$SCREENS"/*.yuv
fi
for m in normal refuse late off lazy max max3 share exit; do
  echo "== prefer_test $m"
  "$O/prefer_test" $m numbered.mp4 2>&1 | grep -v "^\[" || bad=1
done

if [ $bad = 0 ]; then echo "== all passed"; else echo "== FAILED"; fi
exit $bad
