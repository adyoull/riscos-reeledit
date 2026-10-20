# riscos-reeledit

Reel Edit: a video editor for RISC OS, being written. Its engine is the
[MLT multimedia framework](https://www.mltframework.org) (the engine of
Shotcut and Kdenlive), patched to run on RISC OS without threads, with the
Raspberry Pi's video hardware through
[riscos-reelhwaccel](https://github.com/adyoull/riscos-reelhwaccel) and
FFmpeg from [riscos-ffmpeg](https://github.com/adyoull/riscos-ffmpeg).

**State:** the engine. MLT 7.42.0 and melt build for RISC OS and pass every
check and Linux test below. Nothing here has run on RISC OS yet; `!MLT`
(the engine test) is for that. The editor's windows come next: see
[docs/DESIGN.md](docs/DESIGN.md).

## What's here

| Path | What |
|---|---|
| `patches/mlt/` | Five patches to MLT 7.42.0: static modules (no dlopen), no threads, the avformat module without threads, preferred (hardware) decoders with a fallback, opened only when used, and no needless picture conversions |
| `build/` | Fetching the sources, the RISC OS build, the Linux build for the tests, the `!MLT` zip |
| `src/engine.c` | Reel Edit's player: a project played from Wimp null events, a picture at a time |
| `app/!MLT/` | The engine test application: `*melt`, `*mltbench` (`tools/bench.c`), and `Test`, which tries MLT on the machine |
| `tests/host/` | Linux tests |
| `docs/` | [DESIGN.md](docs/DESIGN.md): the editor; [PORTING.md](docs/PORTING.md): what was changed for RISC OS and why |

## Building

Needs x86-64 Linux with [riscos-crossdev](https://github.com/adyoull/riscos-crossdev)
(v1.8.2, `make prebuilt`), cmake, pkg-config and python3; for the tests,
gcc, libxml2's headers and the `ffmpeg` command (with libx264).

```sh
export RCD_DIR=/path/to/riscos-crossdev
build/fetch-sources.sh        # MLT, libxml2, the riscos-ffmpeg and riscos-mesa devkits, FFmpeg 5.1.10
build/build-riscos.sh         # libxml2 and MLT for RISC OS; melt,ff8, checked
build/build-host.sh           # the same MLT for Linux, for the tests
tests/host/run.sh             # must end "== all passed"
VERSION=7.42.0-riscos1-test1 build/package.sh   # out/MLT-<VERSION>.zip
```

In riscos-crossdev's container image, `RCD_DIR` is `/opt/riscos-crossdev`.

## The tests

`tests/host/run.sh` builds MLT as it is built for RISC OS (static, no
threads, the same preferred decoders) and checks:

- **melt** saves a two-clip crossfade as MLT XML and renders it, with no
  thread started (counted with an `LD_PRELOAD` wrapper), reporting progress;
  with `MLT_NO_THREADS=0` it uses threads again.
- **pull_test** plays the project as Reel Edit does, a frame at a time from
  the program's own loop: the right pictures in any order, the dissolve the
  same in yuv420p as in yuv422, and no threads.
- **render_test**: a started consumer renders every frame's picture and
  sound with no threads (the null consumer rendered nothing before).
- **convert_test** counts swscale calls: none when pictures are asked for in
  their own format and size, through a dissolve, in an export, and for a
  colour in an HD profile (which must be BT.709).
- **engine_test** runs `src/engine.c` against a clock the test controls:
  every frame on a fast machine, keeping to the clock on a slow one with the
  sound unbroken, catching up after a stall, seeks that replace each other,
  stopping at the end, backwards at double speed, half-size preview.
- **prefer_test** stands in for the Pi's H.264 decoder: used when it works,
  replaced by the ARM decoder when it refuses a clip at the start or part
  way, given `drop_before` after each jump, never asked for when
  `MLT_AVFORMAT_PREFER` is empty, opened only when the first picture is
  decoded, and limited by `MLT_AVFORMAT_PREFER_MAX`.

Each test was mutation-checked: breaking the code it covers makes it fail.

## Licence

The patches to MLT are under MLT's licences (LGPL 2.1 or later for the
framework and the modules used). Reel Edit's own code is GPL version 2 or
later. Built with riscos-ffmpeg's FFmpeg (GPL with the hardware decoders),
the programs as a whole are GPL version 2.

