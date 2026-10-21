# Reel Edit: design

Reel Edit is a video editor for RISC OS: a Wimp program in C, with the MLT
framework as its engine (patched for RISC OS, see `patches/mlt`) and the
Raspberry Pi's video hardware through riscos-reelhwaccel. This page says
what it borrows from the editors people already know, what it changes to
fit RISC OS, and the rules that keep the rest of the desktop running while
it works.

## What it borrows, and from where

| Idea | From | In Reel Edit |
|---|---|---|
| The project is MLT XML | Shotcut, Kdenlive | Projects open in Shotcut and Kdenlive on other machines, and theirs open here when they use only what Reel Edit supports (core, avformat, xml services). Unknown services are kept and shown as "not supported here", never dropped. |
| A main storyline that closes up after a delete | Final Cut Pro (the magnetic timeline), Shotcut (ripple) | Track V1 ripples by default: delete or trim a clip and everything after it moves. A Ripple toggle on the toolbar turns it off. Tracks above V1 never ripple. |
| Overlap two clips to cross-fade them | Shotcut | Drag a clip's start over the end of the one before it on the same track: the overlap becomes a dissolve, which is exactly MLT's playlist mix. |
| Mark in and out, then insert or overwrite | Avid, DaVinci Resolve (three-point editing) | I and O mark the source in the Viewer; Insert and Overwrite put that range at the playhead. |
| J, K, L to shuttle | every professional editor | J back, K stop, L forward; pressed again they go faster (x2, x4). |
| Split at the playhead | Shotcut (S), Resolve (blade) | S splits the selected clips at the playhead, or all of them when none is selected. |
| Trim by dragging an edge, seeing that frame | Resolve, Shotcut | While an edge is dragged, the Viewer shows the frame at the edge. |
| Proxies for heavy media | Kdenlive, Resolve | A clip the Pi can't decode in real time (not H.264 or HEVC, or over 1080p H.264) gets a 720p H.264 proxy, made in the background. Export always uses the original. |
| Export presets, running beside the editing | Shotcut, !FFmpeg's Convert window | Export is melt in a TaskWindow (pre-emptively multitasked), with a progress bar; editing carries on meanwhile. |

What it leaves out at first: colour grading, keyframed effects other than
volume, multicam, motion tracking, GPU effects. MLT has most of these; they
come later if the Pi can run them.

## How it looks on RISC OS

RISC OS's Style Guide takes precedence over habits from other systems where
they disagree.

- **Icon bar.** Select opens a new project. Dropping video files on the icon
  opens a new project with them on V1, in the order dropped. Menu: Info,
  Choices..., Quit.
- **Windows, one set per project** (each can be closed, and reopened from
  the project's menu):
  - **Timeline**: tracks (V2, V1, A1, A2 to start with), a ruler, the
    playhead; a toolbar pane above it (tools, Ripple, snapping, zoom). The
    project's title bar shows its file name and a `*` when it's modified, as
    any RISC OS document does.
  - **Viewer**: the picture, through a hardware overlay; transport controls in
    a pane below it; source/project switch. It can be any size: the overlay
    scales on the GPU.
  - **Clips**: the media in the project, with thumbnails and durations. Files
    dragged from the Filer land here, or straight on a track at the point
    they're dropped (`Message_DataLoad` carries the position).
  - **Inspector**: the selected clip's settings (in/out, speed, volume,
    fades), as a ordinary dialogue box with writable icons.
- **Mouse**: Select selects and drags, Adjust adds to or removes from the
  selection, Menu opens the menu for what's under the pointer. Dragging a
  clip with Adjust held moves it without snapping.
- **Saving** is the RISC OS save box: F3, or the menu's Save, offers a
  draggable file icon. A project has its own filetype (an MLT XML file
  inside; to be allocated).
- **Keys** (when the Timeline or Viewer has the caret):

  | Key | Does | Note |
  |---|---|---|
  | Space | Play / stop | |
  | J, K, L | Shuttle back, stop, forward | |
  | Left, Right | One frame back, forward | ⇑ for one second |
  | Home, End | Start, end of the project | |
  | I, O | Mark in, out | |
  | S | Split at the playhead | |
  | Delete | Delete the selection (ripples on V1) | ⇑Delete: leave a gap |
  | ^C, ^X, ^V | Copy, cut, paste clips | Style Guide shortcuts |
  | ^K | Delete without the clipboard | Style Guide shortcut |
  | ^A | Select all | |
  | ^Z | Clear the selection | **Not undo**: the Style Guide gives ^Z to Clear |
  | F8, F9 | Undo, redo | Style Guide function keys |
  | F3 | Save | |
  | F1 | Help | |
  | Escape | Cancel a drag or an operation | |

  No Alt shortcuts (the Style Guide reserves Alt).

## Keeping the desktop running

RISC OS multitasks co-operatively: while Reel Edit holds the CPU, nothing
else on the desktop moves. These rules keep that time short.

1. **No threads.** MLT is built with `MLT_NO_THREADS`: it renders a frame
   when Reel Edit asks for one, on Reel Edit's own time. (Tested on Linux: no
   `pthread_create` at all while playing a project.)
2. **At most one picture per poll.** Each null event renders at most one
   frame. While playing, Reel Edit asks for its next null event at the next
   frame's time (`Wimp_PollIdle`), not sooner.
3. **Nothing when nothing happens.** Stopped, with every thumbnail made, Reel
   Edit masks out null events: no CPU at all.
4. **Behind? Drop pictures, not sound.** When rendering falls behind the clock,
   the pictures in between are skipped (their sound is still taken, so it
   doesn't stutter); a hardware decoder is told with `drop_before` so it
   doesn't copy them out.
5. **Scrubbing renders only the newest position.** Positions asked for while a
   frame is being made replace each other; only the latest is drawn.
   **Shuttling faster shows fewer frames, not more:** at x2 and x4 every
   second or fourth frame, so no more pictures a second are made than at x1
   (Viewer test 2: at x4 every frame was made and each waited for the
   overlay's vsync, the desktop asleep 1% of the time). **Backwards**, a
   frame on its own is decoded from its keyframe (a long-GOP AVI on the Pi:
   one picture every 1-1.5 s, the desktop held each time), so runs of up to
   32 frames (48 MB) before the one due are decoded forwards, one a poll,
   kept and shown going back.
6. **Preview at the project's size; the overlay scales it.** Rendering at a
   smaller size costs a software scale per clip, which on Linux made a half-size
   preview slower than full size (21 against 13 ms a 1080p picture). The
   hardware overlay scales for nothing. Half size stays an option, to be
   measured on the Pi, for projects whose effects cost more than the scaling.
7. **Long jobs in a TaskWindow.** Export, proxies and audio waveforms for long
   clips run as melt in a TaskWindow, which RISC OS switches pre-emptively.
   Reel Edit reads their progress from `TaskWindow_Output`, as !FFmpeg's
   Convert window does.
8. **Background work in slices.** Thumbnails and short waveforms are made one
   item per null event, only while stopped.
9. **Sound is the clock** while playing, through SharedSoundBuffer, as in Reel.
   (Not yet in 0.1: the engine follows the system clock and gives each
   frame's sound as the frame is shown, so the sound is about two
   StreamManager blocks, 85 ms, behind the picture, and a stall longer than
   that leaves a gap. Next: the engine takes the sound half a second ahead
   and the pictures follow StreamManager's count.)
10. **Nothing copied or converted that needn't be.** MLT copies each picture
    once, into its own buffer; its image cache (two more copies) is off, so
    Reel Edit's player keeps the few pictures it has just shown, for stepping
    back a frame without decoding again. That one copy matters: reading a 1080p
    picture out of the VideoCore's (cacheable) buffers takes 1.4 ms alone but
    about 9-10 ms while the VideoCore is decoding the next one
    (riscos-reelhwaccel's VCDecTest 0.3; engine test 4 found 9.7 ms, against
    2.3 ms from the ARM decoder's pictures). Reel copies once, from the
    VideoCore's buffer straight into the overlay; through MLT it's into MLT's
    buffer, then the overlay (about 2 ms more, from ordinary memory). Worth
    doing in the Viewer: when a frame is a single clip with no effects, copy
    from the decoder's frame (kept by reference, patch 0005) straight into
    the overlay. Otherwise MLT costs about 2-3 ms a picture more than
    FFmpeg alone.
    **Nothing converted either.** Decoded video is yuv420p, and so is
    what the overlay and the VideoCore's encoder take: MLT is patched (0005) so
    pictures stay yuv420p from the decoder to the screen or the encoder,
    including through a dissolve. Each conversion of a 1080p picture costs more
    on the ARM than the VideoCore takes to decode it.
11. **One VideoCore decoder, handed from clip to clip.** On a Pi 4 with
    `gpu_mem=128` a second `h264_vchiq` decoder opens but never gives a picture,
    and four or more can stop the VideoCore answering (riscos-reelhwaccel's
    measurements, 2026-10-20). MLT (patch 0004) opens a clip's decoder only
    when its first picture is needed, keeps one open, and hands it to another
    clip once the holder has gone 5 pictures unused, closing the old one
    first. In a crossfade the incoming clip is decoded on the ARM until the
    outgoing one finishes. Decoders still open at exit are closed: nothing
    else would free them before a restart. Reel Edit can do better than this
    rule, since it knows the timeline: give the decoder to the clip that will
    play longest, and hand it over at a cut rather than after one.

Targets, measured on a Pi 4 (gpu_mem=128) by the engine test, `!MLT.Test`
(test 5, 2026-10-21): a busy 1080p H.264 clip (random noise, 8 Mbit/s)
usually takes 10.1 ms a picture through MLT on the VideoCore (FFmpeg
alone: 12.4 ms average) and 43 ms on the ARM (FFmpeg alone 50.7 ms); the
crossfade project 11.3 ms (VideoCore) and 10.5 ms (ARM, flat colour); an
export of 450 frames 20 s, two of them the VideoCore decoder's 2 s start
(riscos-reelhwaccel is looking at that). The polling and idle targets
need the Wimp front end.

| Case | Target |
|---|---|
| 1080p25 H.264, one track, preview | 25 fps, VideoCore decoding |
| 1080p25 dissolve between two H.264 clips | 25 fps, full size |
| Longest time between two polls while playing | under one frame time (40 ms at 25 fps) |
| CPU used while stopped | none |

## The parts

| Part | Where | What |
|---|---|---|
| MLT, patched | `patches/mlt`, built by `build/build-riscos.sh` | Engine: timeline model, decoding, mixing, effects, MLT XML, export |
| Engine wrapper | `src/engine.c` | Plays a project from Wimp null events: pacing, skipping, seeking, sound. No RISC OS calls: the display, sound and clock are callbacks, so it's tested on Linux |
| Display | `src/riscos/display.c` | Pictures to VideoOverlay as YV12 while playing with nothing over them; otherwise a 32bpp sprite in the window, converted by swscale (Reel's code) |
| Sound | `src/riscos/sound.c` | SharedSoundBuffer and StreamManager (reelcore's code) |
| Wimp front end | `src/riscos/viewer.c` | The icon bar icon and the Viewer (0.1). The Timeline, Clips and Inspector windows, menus, drags and saving are to come |
| Export | `src/export.c` (to come) | melt in a TaskWindow, presets, progress |
