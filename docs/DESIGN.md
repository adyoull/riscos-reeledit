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
| The project is MLT XML | Shotcut, Kdenlive | Projects open in Shotcut and Kdenlive on other machines, and theirs open here when they use only what Reel Edit supports (core, avformat, xml and normalize services). Unknown services are kept and shown as "not supported here", never dropped. |
| A main storyline that closes up after a delete | Final Cut Pro (the magnetic timeline), Shotcut (ripple) | Track V1 ripples by default: delete or trim a clip and everything after it moves. A Ripple toggle on the toolbar turns it off. Tracks above V1 never ripple. |
| Overlap two clips to cross-fade them | Shotcut | On the same track: drag from the white mark at the bottom of a cut, and the clips overlap by as far as you drag; the overlap becomes a dissolve, which is exactly MLT's playlist mix. (Done from the cut's corner rather than by dragging a clip's start, which trims.) D makes a one-second dissolve at the cut nearest the playhead from the clips' spare frames, as Premiere's and Resolve's at-the-cut dissolves do, so nothing moves. |
| Effects from the clip's context menu, each a box over what it covers | Premiere, Resolve (a transition is a box over the cut), Shotcut (fades) | Menu over a clip on the Timeline: Fade in, Fade out, Dissolve from the clip before, into the clip after. A fade is a box along the top of the clip over the frames it covers, a dissolve a band between the clips; an effect's inner edge or ends drag to change its length. While dragging, the Viewer's picture is made only when the pointer rests, so the Timeline keeps up on the Pi. |
| Mark in and out | Avid, Premiere, DaVinci Resolve | I and O mark a stretch of the Timeline (shown on the ruler); Shift-I/O go to the marks, Return plays in to out; ' Extract and ; Lift (Premiere's keys) take the stretch out, closing up or leaving a gap (0.3). Then insert and overwrite: a source range (marked in a Clips window, 0.4) put at the playhead (three-point editing). |
| Pictures and videos over the video on a track above it | Shotcut, Premiere (V2 and up), Final Cut (connected clips) | V2, above V1: a PNG (with alpha), JPEG or sprite dropped there is an overlay, shown for 5 seconds; a video dropped there is a picture in the picture (bottom right, a third of the size), its sound mixed with V1's by a mix transition, trimmed as a clip is; dragged to move or trim; its menu places it (full frame, centre, corners, lower third), sizes it, sets its opacity, fades it. Saved as MLT composite transitions, the pictures as qimage producers as Shotcut saves them (not tried in Shotcut yet; plain melt reads the pictures through avformat, which here loses their alpha past the first frame). Sprites are saved as PNGs beside their files first. Titles drawn with the Font Manager (to come) will be overlays too. |
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
- **Windows, one set per project** (closing the Viewer or the Timeline
  closes the project, asking first if it has changes; 0.2 has the Viewer,
  with a toolbar, and the Timeline):
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

## Working fast: the editing UX

Reel Edit should feel like the editors people already know (Final Cut Pro,
Premiere Pro, DaVinci Resolve, Shotcut), be quick to drive from the
keyboard, and still be a RISC OS program. Where those editors agree, Reel
Edit does the same; where RISC OS's Style Guide says otherwise (^Z, F8/F9,
the save box, Menu for menus), the Style Guide wins.

**Principles**

1. **The playhead is always where the action is.** Split, ripple trims,
   inserts and the Viewer all work at the playhead. Moving it must be easy
   everywhere: click or drag the ruler, grab the playhead line itself, click
   empty track space, Up/Down to the previous/next cut, Left/Right a frame,
   J/K/L.
2. **Every action three ways**: a key (shown in the menu beside it), the
   menu, and for the common ones a toolbar button. Interactive help
   (`Message_HelpRequest`, shown by !Help) on every button and window part,
   which is RISC OS's tooltip.
3. **Direct manipulation on the Timeline**: drag clips to move them (an
   insertion line shows where they'll land), drag a clip's edge to trim it
   (the Viewer shows the frame at the edge), drop files where they should go.
   The pointer shape says what a drag will do (move, trim, scrub).
4. **Snapping** (N, toolbar toggle, Adjust-drag to override): the playhead,
   clip edges and drops snap to cuts and to the playhead.
5. **Magnetic V1** (Final Cut): deleting or trimming on V1 closes the gap;
   Shift-Delete lifts and leaves one.
6. **Undo everything** (F8, redo F9; many levels), so trying things is safe.
7. **Clear selection display**: a selected clip keeps its colour with a bright
   outline (filling it orange hid its name and its neighbours, Pi test).
8. **Readable time**: the ruler's marks never overlap; the playhead's
   timecode shown on the ruler beside it; clips show name and thumbnails (at
   their start, then along them when there's room), audio its waveform.
9. **Zoom that keeps your place**: + and - zoom about the playhead,
   Shift-Z (and `\`) fits the project, Ctrl with the scroll wheel zooms about the
   pointer, the wheel alone scrolls.
10. **Never lose work**: autosave to Choices every few minutes while changed;
   after a crash, offered back at the next start.

**Keys** (beyond the table above; the same in the Viewer and the Timeline)

| Key | Does | Like |
|---|---|---|
| Up, Down | Previous / next cut (edit point) | Premiere, Final Cut, Resolve, Shotcut |
| Shift-Left, Shift-Right | One second back / forward | |
| Q, W | Ripple trim the clip's start / end to the playhead | Premiere |
| N | Snapping on / off | Final Cut, Resolve |
| Shift-Z, `\` | Zoom the Timeline to the whole project | Final Cut, Premiere |
| +, - | Zoom the Timeline in / out about the playhead | |
| I, O; Shift-I, Shift-O | Mark in / out; go to them | everywhere |
| Return | Play from the in mark to the out mark | |

**Order of work** (each a test build): undo/redo, Up/Down, Q/W, playhead
grab, selection outline, snapping, interactive help, wheel zoom (0.2, done
in 0.2 test 2); trimming by dragging edges with the Viewer showing the edge
frame, moving clips by dragging (0.3 test 1), dissolves by overlap, fades, I/O marks (0.3); thumbnails and waveforms, the
Clips window, autosave (0.4); then audio tracks, V2 and overlays, titles,
export.

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
   The engine takes the sound 0.4 s ahead of the picture (a few frames'
   a poll, without decoding their pictures), and steers the picture clock
   to what StreamManager says has been heard: a quarter of the way each
   time its reading moves (a block, 46 ms), the whole way when more than
   0.1 s out (as reelcore). Pictures left out cost nothing for the sound.
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
| Project | `src/project.c`, `src/project.h` | The editing model: MLT's tractor with V1 a playlist (Shotcut's layout); add, split, trim, move, remove (ripple or gap), undo (XML snapshots), save and open MLT XML. No RISC OS calls |
| Dissolves | `src/dissolve.c` (shares `src/project_internal.h` with project.c) | Dissolves between clips on V1: MLT playlist mixes made (from spare frames or by overlapping), resized, removed back to the cut they came from; MLT's in-memory links between a mix and its clips made again after every edit and every load (undo restores from XML, which doesn't keep them) |
| Fades | `src/fade.c` (shares `src/project_internal.h`) | Fades from and to black at a clip's ends (picture and sound together, or the sound alone: clipsound.c): MLT brightness and volume filters on the clip's cut (Shotcut's fadeIn/OutBrightness and Volume), kept to the clip's ends after every edit; a split gives the fade out to the second part; none at an end in a dissolve |
| Clip sound | `src/clipsound.c` (shares `src/project_internal.h`) | A clip's (or a V2 video's) sound on its own: its level (Shotcut's audioGain volume filter, level in dB, +12 to -36 from the menu), muted (a volume filter, gain 0, reeleditMute), or faded without the picture (the fade's volume filter alone); a dissolve's clip ends muted as their clips are, after every edit and load; a split keeps the mute on both parts. Menu: Sound (Mute, Fade in, Fade out) on the clip's and the overlay's context menus; drawn as a red line (muted) or green wedges along the clip's bottom |
| Audio tracks | `src/audio.c`, `src/track.c`, `src/riscos/audiotracks.c` | A1, A2... below V1: playlists marked "shotcut:audio" (Shotcut's audio tracks), their pictures hidden ("hide" 1), each mixed with V1 by a core mix transition (made again after every edit; not if the project mixes it already). A sound dropped where one plays goes on the next track with room for all of it (a new one if none), so effects stack. Sounds move, trim, mute, fade and take a level as overlays and clips' sound do; an empty last track goes. track.c is the shared code of V2's and the audio tracks' separate items (slots, room, move/trim). The Timeline grows a row per track, and one more to drop on; Menu over a sound: Mute, Fade in, Fade out, Volume, Delete |
| Layout | `src/riscos/viewer.c` | The editor is one workspace: the Timeline most of the screen's width (90%), the Viewer centred above it at a share of the screen's width (View > Viewer size: Small 35%, Medium 50%, Large 65%), so the picture costs what it must while the tracks have room |
| Autosave | `src/riscos/autosave.c` | After an edit, a project with changes not saved is copied (MLT XML) to <Choices$Write>.ReelEdit.Autosave, at most every half minute, with its file's name in AutosaveFor. Saving, closing (kept or discarded) and quitting take the copy away; one left means Reel Edit stopped. Opening the editor then asks: Recover (the project as it was, still to be saved, its name kept) or Discard |
| Clips and three-point editing | `src/source.c`, `src/riscos/clips.c` | The Clips window (View > Clips..., Ctrl-B): the project's clips list (every clip put on the Timeline, and files dropped on it), saved as the tractor's `shotcut:reeledit_bin`. Double-click (or Return) shows a clip in the Viewer as a source: I and O mark it, `,` inserts and `.` overwrites its marked frames on V1 at the project's playhead, which then moves after them; Escape goes back to the project. A row dragged to the Timeline is put there as a dropped file (on an audio row, its sound). Delete takes a clip off the list. Neither edit goes into a dissolve |
| Thumbnails and waveforms | `src/media.c`, `src/riscos/previews.c` | V1 clips show their first frame, and their last when there's room (Premiere's head and tail); their sound's waveform runs along their bottom, a sound's along it on an audio track, drawn in dB (48 dB to full scale) at the clip's level and through its fades (none when muted). Made with FFmpeg's software decoders (not the VideoCore's, kept for playing), on null events while nothing plays or is dragged, at most 4 cs at a time, what was last asked for (in view) first; thumbnails before peaks. Until a thumbnail is made, the nearest made of its file stands in. Thumbnails are kept (96, least recently asked for go), peaks (a byte a frame) for every file; a file that can't be read isn't tried again; the decoders are closed when there's nothing to make. Plotted from one grid sprite of a megabyte or more (the Pi's small-sprite replot problem). V1 is 160 OS units tall for them |
| Copy and paste; rolls | `src/clipboard.c`, `src/riscos/copypaste.c`, `re_project_roll` (project.c) | Ctrl-C, Ctrl-X, Ctrl-V (Edit menu): the selection (a V1 clip, an overlay, a sound) copied with its fades, mute, level and place; a copy holds its producer (a reference of its own: the original can go) and is pasted at the playhead as one edit: a clip inserted, an overlay where there's room, a sound stacked; a title is made afresh (two titles mustn't share a producer). Pasted, it's selected and the playhead goes to its end. A roll moves a cut, the clips (or a gap) either side trimmed together, the length kept: E rolls the cut nearest the playhead to it (Premiere's extend edit), Ctrl-drag on a clip's edge rolls the cut there. Within re_project_begin/end, rp_checkpoint and rp_no_edit do nothing (the edit's undo point is begin's) |
| Speed | `src/speed.c`, effects.c's Speed submenu | A clip at another speed is a cut of MLT's timewarp producer ("2:file", "-1:file", as Shotcut makes), made through the loader ("timewarp:2:file") so it has the normalisers (its sound resampled to 48 kHz; made directly, it played silent). Its sound changes pitch with it (pitch keeping needs Rubber Band, not built). Changing a speed keeps the part of the file shown and the clip's start; what follows ripples; fades, mute and level are kept (begin/end: one undo). Not pictures, nor clips in a dissolve. `re_item.speed` (%), `re_item_file_frame` maps its frames to the file's (thumbnails, waveforms) |
| Render previews | `src/render.c`, `src/riscos/renders.c`, `re_engine_set_producer` | Costly stretches (V2 overlays, dissolves, picture fades, clips at another speed; joined within 2 s) are made into MP4 files by the export program (`-export proj file height 2 hw in out`: a cut of the project) one at a time in TaskWindows (txt &52455056), at the engine's preview height. Each stretch has a signature (FNV-1a of everything in it, placed from its start, its sounds too): a preview is found by signature and length, so a V1 stretch moved along by a ripple keeps it, a changed one loses it. While playing, the engine plays a playlist of the project with the rendered stretches from their files (switched back when stopped or edited). The bar under the ruler: red to render, green rendered, amber rendering. Files in <Wimp$ScrapDir>.ReelEdPrv, deleted when the editor closes (at most 64, least recently used go) |
| Export | `src/export.c`, `src/riscos/exportbox.c` | The project made into an MP4 (H.264 and AAC): File > Export... (Ctrl-E). The project is saved to a scrap copy and !RunImage run again with -export in a TaskWindow (pre-emptive, so editing goes on); its "progress"/"done"/"failed" lines come back as TaskWindow_Output, its end as Morio; Stop sends Morite and deletes the part made. MLT's avformat consumer (no threads: on the calling thread) at the project's size or fewer lines; the VideoCore's h264_encvchiq when VCHIQ is there (bit rate from size and quality), else libx264 (veryfast, CRF), else mpeg4; a file without video tries the next codec. The file icon drags as the Save as box's (savebox.c's sb_drag_file) |
| Titles | `src/title.c`, `src/riscos/titles.c`, `src/riscos/fonts.c` | Text over the video: a picture made from the text (each line by the Font Manager into a 32bpp sprite, its brightness the coverage; blocks on Linux), coloured over a shadow, an outline or a box; an MLT producer registered as "qtext" (Shotcut's text producer's name and properties), a still made again when its properties change (still.c's re_still_made). Put on V2 as an overlay at its own size (overlay.c's title_box), lower third by default. The Title window: two lines, Font, Size, Colour, Style, Lines, Place; Edit > New title... (T), the overlay menu's Edit title... |
| Overlays | `src/overlay.c` (shares `src/project_internal.h`) | V2, the playlist above V1: pictures and videos over the video (a video's sound mixed in by one core mix transition, unless the project has its own). Each picture's place, size, opacity and fades are kept on its playlist entry (shotcut:reeledit_*); a core composite transition per picture is made again from them after every edit and every load. V2's edits rebuild its playlist from its pictures' cuts, keeping them apart and within the project |
| Still pictures | `src/still.c`, `src/still.h` | MLT's "qimage" producer in this build (the name Shotcut saves pictures under, and MLT's loader tries first): a PNG or JPEG known by its first bytes, decoded once by FFmpeg, the last size and form asked for kept (avformat decoded and converted every frame: 26 ms a frame on a PC at 1080p, about 5 here). Writes PNGs |
| Sprites | `src/sprites.c`, `src/sprites.h` | RISC OS sprite files read as bytes (no RISC OS calls): old and new formats, RISC OS 5's mode words, 1 to 32 bits, palettes (or the Wimp's), masks and alpha, pixels made square |
| Wimp front end | `src/riscos/viewer.c`, `timeline.c`, `effects.c` (fades and dissolves from the menus; the Timeline's context menu), `marks.c` (I/O marks: play them, Extract, Lift), `overlays.c` (pictures dropped on V2, the sprite menu, the overlay's menu), `menu.h`, `savebox.c`, `info.c` | The icon bar icon; the editor: the Viewer (picture, transport, toolbar) with the Timeline under it (ruler, playhead, V2, V1), the menus and keys, the Save as box (0.2); trimming, moving and dissolves by dragging (0.3). Fades, overlays (0.3). Clips and Inspector windows, more tracks and export are to come |
| Export | `src/export.c` (to come) | melt in a TaskWindow, presets, progress |
