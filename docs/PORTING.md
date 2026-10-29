# MLT on RISC OS: what was changed, and why

MLT 7.42.0 builds for RISC OS with nineteen patches (`patches/mlt/`), one small
compatibility header (`build/riscos-compat.h`) and these build settings
(`build/build-riscos.sh`). Each patch is written so it could go upstream:
the RISC OS behaviour is an option, off by default.

## The patches

| Patch | What | Why |
|---|---|---|
| 0001 `MLT_STATIC` | The framework and the modules (core, avformat, xml; normalize from 0007) as static libraries. Each module's `mlt_register` is renamed after it; the generated `mlt-static-modules` library's `mlt_static_modules_register()` adds them with the new `mlt_repository_add_static()`; `mlt_repository_init` registers those instead of loading modules from a directory. No `dlopen` at all. `mlt++` follows. The `.pc` file's Cflags carry `-DMLT_STATIC_DEFINE`. | RISC OS programs are static: there is no `dlopen` |
| 0002 `MLT_NO_THREADS` (framework) | `mlt_threads_disabled()`: the build's default (`-DMLT_NO_THREADS=ON`), overridden by `MLT_NO_THREADS=0` or `1` in the environment. Slices run on the caller; consumers turn `real_time` off, and `mlt_consumer_rt_frame` renders each frame's sound and picture itself, as the read-ahead thread would have (without it the null consumer rendered nothing: the first Pi test's speeds were meaningless). | UnixLib's threads are switched by a ticker on one CPU, and a Wimp program must give up the CPU itself. Work has to happen when the program asks for it |
| 0003 `MLT_NO_THREADS` (avformat) | No packet-reading thread (`packets_read_inline`); one thread for decoders, encoders and filter graphs that would otherwise use FFmpeg's automatic count; the avformat and null consumers run inside `mlt_consumer_start`; melt reports progress from `consumer-frame-show`. | As above. FFmpeg's automatic thread count started 12 threads for two clips |
| 0004 preferred decoders | The `prefer` property, `MLT_AVFORMAT_PREFER`, or the build's default (`h264=h264_vchiq,hevc=hevc_hwdec` here): decoders tried first, replaced by the default one when they refuse a clip at open or fail part way (ENOSYS, AVERROR_EXTERNAL), with a seek back to where it was. After a jump, `drop_before` (an option of the RISC OS hardware decoders) so the pictures before the one asked for aren't copied out. `meta.media.decoder` names the decoder. A preferred decoder is opened only when the clip's first picture is decoded. `MLT_AVFORMAT_PREFER_MAX` (`h264_vchiq=3,hevc_hwdec=1` here, since riscos23 brought devkit 0.2.18: a number for every decoder, or decoder=number pairs) is how many of each may be open; a clip that needs one while it's taken uses the default decoder, and takes it over once the holder has gone `MLT_AVFORMAT_PREFER_IDLE` (5) pictures unused, the holder's closed first. Any still open at exit are closed. | The Pi's decoders take only some streams (H.264 Baseline/Main/High 4:2:0 8-bit up to 1088 lines; HEVC on a Pi 4), and refuse the rest, sometimes only at the first picture. Opened with every clip, they held VideoCore memory before any was used, and the encoder couldn't start (first Pi test). Only one `h264_vchiq` works at a time with gpu_mem=128; four can wedge the VideoCore, and nothing frees a program's decoders when it stops (riscos-reelhwaccel, 2026-10-20). With devkit 0.2.18's direct output three 1080p `h264_vchiq` work at once (riscos-reelhwaccel, 2026-10-22): `h264_vchiq=3` once riscos-ffmpeg's devkit has it; the HEVC block takes one. A clip whose first picture is part way in keeps its "initial" state across the decoder change, so the seek finds the stream's first pts (it was a GOP late) |
| 0005 no needless conversions | `avdeinterlace` passes the format asked for through (it asked every frame for yuv422); the producer and the avformat consumer copy planes when nothing is to change; the luma transition dissolves yuv420p; the avformat consumer asks for yuv420p when the encoder takes it; the colour producer works in the profile's colour space. Two build options cut whole-picture copies: `MLT_AVFORMAT_CACHE_DEFAULT=0` (the image cache copied each picture in, and again out) and `MLT_AVFORMAT_CONCEAL_BY_REFERENCE` (the last good picture, kept for error concealment, by reference instead of a deep copy of each). | Profiled on Linux: with the xine module not built, every 1080p frame went yuv420p → yuv422 → yuv420p, which took longer than decoding. A clip: 19.6 → 6.6 ms a picture; a dissolve: 25 → 9 ms. On the Pi (engine test 3) a flat-colour 1080p picture, which decodes in almost no time, still took 20 ms: two of its three 3 MB copies went |
| 0006 `consumer.source_colours` | A frame asked for with `consumer.source_colours` set comes in the clip's own colour space and range (its `colorspace` and `full_range` properties say which) instead of being converted to the profile's. | ReelEdit's display (the overlay, or its own conversion to a sprite) takes the picture in its own colours, so MLT's conversion to the profile's was a needless pass over every picture |
| 0007 the normalize module, static | The normalize module (the `volume` and `audiolevel` filters) built into a static MLT as core, avformat and xml are. | Fades: the sound's gain animated by the `volume` filter, as Shotcut fades sound |
| 0008 yuv420p and yuyv422 without swscale | 8-bit yuv420p to yuyv422 and back at the same size, its colours kept (matrix and range), by a loop in the avformat module's common.c (chroma interpolated between rows; averaged going back): in the avformat producer (progressive video asked for as yuv422) and in avcolour_space's converter. | Overlays: the composite transition works in yuv422, so each frame under an overlay went to yuv422 and back. swscale with MLT's flags (accurate rounding, full chroma) took no unscaled shortcut, cost twice the H.264 decoding and made a context for every slice of every frame |
| 0009 composite in yuv420p | Asked for yuv420p, a plain composite (no luma wipe, operator, titles, crop, alpha overrides, inverting or fields) blends the b picture (with its alpha) over the a picture plane by plane in yuv420p, at the geometry's place made even: luma by each pixel's alpha, chroma by the 2x2 average; see-through pixels skipped. Anything else goes the yuv422 way. | Overlays: no conversion of the frame to yuv422 and back at all. On RISC OS for Linux (tools/bench, 640x360): a full-frame PNG over the video 18 ms a picture without 0008, 9.7 with it, 8.7 with 0009 (6.7 with nothing over it) |
| 0010 swscale contexts kept; quick scaling | Without threads, filter_scale keeps the last four swscale contexts with what each was made for (sizes, format, flags, colours), instead of making one per picture. A frame with consumer.fast_scale set leaves out SWS_ACCURATE_RND and the full-chroma flags; composite passes it on to the b frame (0011). ReelEdit's player sets it; exports don't. | A picture in the picture: making the context was most of the scaling's time on the host (callgrind). On RISC OS for Linux the gain was within the noise |
| 0011 yuv420p brightness; NEON loops | brightness works on yuv420p as it is (luma towards black, chroma towards grey) instead of going to yuv422 and back. New core_neon.c (built with -mfpu=neon on 32-bit ARM only): NEON versions of the dissolve's mix, brightness's scale and composite's yuv420p blends, run only when the application sets MLT_NEON=1 in MLT's environment (ReelEdit does when FFmpeg's av_get_cpu_flags says NEON); 8.8 fixed point, within 2 of the C (tools/neon_check). | Fades: each faded frame went to yuv422 and back. On RISC OS for Linux (tools/bench): a fade 11 ms a picture before, 8.3 with the yuv420p brightness, 7.0 with NEON as well. The overlay and dissolve loops with NEON measured within the run-to-run noise there (about 1.5 ms) |
| 0013 luma: yuv420p wipes | transition_luma.c: a wipe (a luma map) of two opaque progressive yuv420p pictures done plane by plane: each pixel's mix from a table made for the frame (the map in 1024 steps, the yuv422 path's integer smoothstep), a row of weights mixing the rows (core_neon.c's new `mlt_core_neon_mix_w`, the same as the C to the bit) | Wipes converted both pictures to yuv422 and back and worked out a float smoothstep for every pixel: on the testbed 17.2 ms a picture, now 14.1 (a plain dissolve 12.7) |
| 0012 avfilter with FFmpeg before 7.1 | filter_avfilter.c sets the buffer source's colorspace and range options, which FFmpeg 5.1's buffersrc doesn't have; a missing option is now skipped instead of failing the graph. | Every avfilter.* video filter (ReelEdit's colour uses lutyuv) failed to start ("Cannot set src colorspace 5") and passed its pictures through unchanged |
| 0014 avformat: a quick yuv420p scaler | New fast_scale.c (NEON for its rows, built with -mfpu=neon alone, used only when `MLT_NEON` is 1; the same as its C to the bit): with `consumer.fast_scale` set (a preview) and nothing but the size to change (yuv420p, even sizes, the same colorspace and range in and out), filter_swscale scales the three planes itself, halved by 2x2 means while still twice the size wanted or more, then bilinear in 8-bit fixed point. Anything else, and every export, is swscale's as before | A picture in a picture's scaling was a quarter of its time on the desktop, in swscale's general C (`hScale8To15_c`) |
| 0015 resize: yuv420p padded as yuv420p | A picture that fits the frame with room to spare is padded in yuv420p (in the middle, at even offsets, its alpha too) when the sizes are even, and a size one pixel short of the one asked for is made even (a pixel's stretch, not a line of padding); odd sizes as before | A picture in a picture was usually a pixel short (209 for 210, by rounding), so every frame of it was converted to yuv422, padded, and scaled in yuv422 for the composite to have it back in yuv420p. With 0014: on the Linux build, the picture in a picture's instructions a frame down by a quarter; on the testbed, a keyframed one from 13.1-13.8 ms a picture to 11.8-12.3 (a still one's within the testbed's noise, about 10 ms either way) |
| 0016 `MLT_ANIMATION_TIME_FORMAT` read once | mlt_property.c and mlt_animation.c read it the first time it's needed, not every time | Every property got as a string asked the environment, about 430 times a frame in a picture in a picture; UnixLib's getenv looks for a name that isn't there among the OS's variables too |
| 0017 luma: wipes mix only where the pictures meet | 0013's rows: runs of weight 0 left as they are, runs of 256 copied, only the rest mixed | Most of a wipe's picture is one clip or the other (on the testbed, within its noise: 14.2-15.4 ms a picture against 14.9-15.8) |
| 0018 core_neon: blends skip see-through runs | `mlt_core_neon_blend` and `_blend_chroma`: sixteen pixels of alpha 0 left as they are, sixteen of alpha 255 at full strength copied, without the NEON mix (the alphas looked at with byte loads first: a NEON-to-ARM move stalls a Cortex-A8). The same to the bit | A logo's box and a keyed picture are mostly see-through. On the testbed, green screen from 12.3-14.2 ms a picture to 10.6-11.5 |
| 0019 yuv420p at an odd size: one layout | MLT lays yuv420p's chroma out w/2 x h/2 (`mlt_image_format_planes`), FFmpeg (w+1)/2 x (h+1)/2. New common.c `mlt_avformat_yuv420p_av_to_mlt` / `_mlt_to_av` copy one into the other; filter_avcolour_space (in and out), filter_swscale (in) and producer_avformat (a decoded odd-sized picture) use them, so FFmpeg always sees its own layout and MLT its own. The preview scaler (0014) takes odd sizes, plane by plane in MLT's layout. The yuv420p composite (0009) gives an odd-sized picture's last row and column the chroma before them and draws no chroma for an odd-height frame's last row | At an odd height V starts a chroma row later in FFmpeg's layout: an overlay or picture of an odd height (common with keyframed sizes) had its V plane a row out (a line of the wrong colour), and FFmpeg's extra chroma row was written past the end of MLT's buffer |

## UnixLib

UnixLib 5.0.3.3 has no `pthread_attr_setscope`, `pthread_attr_setinheritsched`
or `pthread_attr_setschedpolicy`, nor their constants. `build/riscos-compat.h`
gives MLT versions that accept and do nothing (UnixLib's threads have one
scope and no policies). With `MLT_NO_THREADS` they're never reached.

## Build settings

- `-mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection` are repeated in
  `CMAKE_C_FLAGS`: setting it replaces the toolchain file's defaults, and
  without them crossdev's stack-probe check found 5 large frames unprobed.
- `-D_FILE_OFFSET_BITS=64`: video files over 2 GB.
- The FFmpeg libraries are added at the end of every link
  (`CMAKE_C_STANDARD_LIBRARIES`), in dependency order, so the static
  libraries resolve.
- libxml2 (for the xml module) is built without threads, network, iconv or
  zlib, and installed into the merged devkit, where CMake's searches look.
- `USE_AVDEVICE=OFF`: riscos-ffmpeg's libavdevice needs EGL.

## Checks

crossdev's `check-all.sh` passes on melt: static, every frame of 4 KB or
more probed, no VFPv4, UnixLib consistent.

`check-unaligned.sh` finds 72 casts in MLT's sources (59 before the plus
module was built in) that raise the alignment a pointer needs. Each was looked at; they are listed in
`build/unaligned-safe.txt`, and a new one fails the build:

- **16-bit image planes** (`uint16_t *` from a plane pointer: `mlt_image.c`,
  most core filters, `transition_luma.c`, `filter_avcolour_space.c`): image
  buffers come from `mlt_pool_alloc` or `av_malloc`, aligned to 16 bytes, and
  the 16-bit formats' strides and plane offsets are even. The plus
  module's (`filter_hslprimaries`, `filter_hslrange`, `filter_lift_gamma_gain`,
  `filter_shape`, `filter_spot_remover`, `filter_strobe`) are the same:
  RGBA64 pictures and 16-bit alpha masks from the frame.
- **Audio** (`filter_audiochannels.c`, `int16_t`/`int32_t`/`float`): audio
  buffers are allocated aligned, and the planar offsets are whole samples;
  plus's `producer_blipflash.c` and `producer_count.c` (`float`) fill the
  frame's own audio buffer.
- **FFmpeg side data** (`producer_avformat.c`: the display matrix, spherical
  mapping, hardware frames context): FFmpeg allocates these with `av_malloc`.

Not yet done: the QEMU layer (MLT and the tests built for ARM Linux and run
with the alignment trap on), as riscos-ffmpeg does for its own code.
