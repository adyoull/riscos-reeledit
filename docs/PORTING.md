# MLT on RISC OS: what was changed, and why

MLT 7.42.0 builds for RISC OS with four patches (`patches/mlt/`), one small
compatibility header (`build/riscos-compat.h`) and these build settings
(`build/build-riscos.sh`). Each patch is written so it could go upstream:
the RISC OS behaviour is an option, off by default.

## The patches

| Patch | What | Why |
|---|---|---|
| 0001 `MLT_STATIC` | The framework and the modules (core, avformat, xml, plus) as static libraries. Each module's `mlt_register` is renamed after it; the generated `mlt-static-modules` library's `mlt_static_modules_register()` adds them with the new `mlt_repository_add_static()`; `mlt_repository_init` registers those instead of loading modules from a directory. No `dlopen` at all. `mlt++` follows. The `.pc` file's Cflags carry `-DMLT_STATIC_DEFINE`. | RISC OS programs are static: there is no `dlopen` |
| 0002 `MLT_NO_THREADS` (framework) | `mlt_threads_disabled()`: the build's default (`-DMLT_NO_THREADS=ON`), overridden by `MLT_NO_THREADS=0` or `1` in the environment. Slices run on the caller; consumers turn `real_time` off. | UnixLib's threads are switched by a ticker on one CPU, and a Wimp program must give up the CPU itself. Work has to happen when the program asks for it |
| 0003 `MLT_NO_THREADS` (avformat) | No packet-reading thread (`packets_read_inline`); one thread for decoders, encoders and filter graphs that would otherwise use FFmpeg's automatic count; the avformat and null consumers run inside `mlt_consumer_start`; melt reports progress from `consumer-frame-show`. | As above. FFmpeg's automatic thread count started 12 threads for two clips |
| 0004 preferred decoders | The `prefer` property, `MLT_AVFORMAT_PREFER`, or the build's default (`h264=h264_vchiq,hevc=hevc_hwdec` here): decoders tried first, replaced by the default one when they refuse a clip at open or fail part way (ENOSYS, AVERROR_EXTERNAL), with a seek back to where it was. After a jump, `drop_before` (an option of the RISC OS hardware decoders) so the pictures before the one asked for aren't copied out. `meta.media.decoder` names the decoder. | The Pi's decoders take only some streams (H.264 Baseline/Main/High 4:2:0 8-bit up to 1088 lines; HEVC on a Pi 4), and refuse the rest, sometimes only at the first picture |

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

`check-unaligned.sh` finds 59 casts in MLT's sources that raise the
alignment a pointer needs. Each was looked at; they are listed in
`build/unaligned-safe.txt`, and a new one fails the build:

- **16-bit image planes** (`uint16_t *` from a plane pointer: `mlt_image.c`,
  most core filters, `transition_luma.c`, `filter_avcolour_space.c`): image
  buffers come from `mlt_pool_alloc` or `av_malloc`, aligned to 16 bytes, and
  the 16-bit formats' strides and plane offsets are even.
- **Audio** (`filter_audiochannels.c`, `int16_t`/`int32_t`/`float`): audio
  buffers are allocated aligned, and the planar offsets are whole samples.
- **FFmpeg side data** (`producer_avformat.c`: the display matrix, spherical
  mapping, hardware frames context): FFmpeg allocates these with `av_malloc`.

Not yet done: the QEMU layer (MLT and the tests built for ARM Linux and run
with the alignment trap on), as riscos-ffmpeg does for its own code.
