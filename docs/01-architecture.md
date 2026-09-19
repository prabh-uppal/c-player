# Architecture — where a frame actually goes

## The whole thing in one picture

```
   movie.mp4
       │
       ├──────────────► ffprobe ──► VideoInfo {w, h, fps, duration, audio}
       │                              src/media/probe.cpp
       │                                      │
       │                                      ▼
       │                              Player::layout()
       │                    "the terminal is 120x40 characters,
       │                     the video is 16:9, a cell is half as
       │                     wide as it is tall → 118 x 33 pixels"
       │                                      │
       │        ┌─────────────────────────────┤
       │        │                             │
       ▼        ▼                             ▼
   ffmpeg (video)                         ffmpeg (audio)
   -ss T -vf fps,scale                    -ss T -af aresample
   -f rawvideo -pix_fmt rgb24                     │
       │                                          ▼
       │ raw RGB24 bytes                       ffplay
       │ over a pipe                        (plays the sound,
       ▼                                     prints its clock
   Player::readFrame()                       to stderr)
   frame[] = one frame                           │
       │                                         │ "  1.234 M-A: ..."
       ▼                                         │
   Player::render()   ◄──── clock correction ────┘
   pixels → characters                    Player::syncToAudio()
       │
       ▼
   one big std::string of ANSI escapes
       │
       ▼
   writeAll() → your terminal
```

## Why two ffmpeg processes and not one

They are seeked independently and restarted independently. Pausing stops the
sound but keeps the decoded video position; seeking restarts both at an exact
timestamp. Keeping them separate means each can be killed and respawned
without disturbing the other.

The cost is that they can drift apart, which is what `syncToAudio()` exists to
fix — see [04-av-sync.md](04-av-sync.md).

## Why we shell out at all

Video decoding is genuinely hard: dozens of codecs, container formats,
hardware paths. ffmpeg already does it, better than any code we would write.
By asking ffmpeg to *also* scale the video to exactly our grid size, we get
high-quality resampling for free and the pixels arrive in the simplest
possible format: three bytes per pixel, rows top to bottom, no padding.

That is the real design decision in this program: **push everything hard onto
a tool that already solved it, and keep only the part that is ours** — turning
pixels into characters.

## Module dependency graph

Nothing here is circular. Arrows point at what a module needs.

```
main.cpp
  ├─► browser.h ──┐
  ├─► player.h ───┤
  │     ├─► probe.h ──► shell.h
  │     ├─► audio.h ──► shell.h
  │     ├─► modes.h
  │     ├─► shape_lut.h ──► glyphs.h
  │     ├─► message.h ─────┐
  │     ├─► zoom.h ──► shell.h
  │     └─► common.h       │
  ├─► terminal.h ◄─────────┴─┬── input.h
  │     └─► zoom.h           └── ansi.h
  └─► shell.h
```

Two dependencies are worth noticing:

- **terminal → zoom.** `restoreTerminal()` must also undo the font zoom,
  otherwise quitting with Ctrl-C leaves your terminal at 5 pt. Putting that
  one call in `restoreTerminal()` means every exit path is covered, because
  `restoreTerminal` is registered with `std::atexit`.
- **Nothing depends on `player.h` except `main.cpp`.** The Player is the top
  of the program, not a library. That is why it is allowed to include almost
  everything.

## State that lives outside the Player

Three globals, and each has a reason:

| Global | Where | Why it cannot be a member |
|--------|-------|---------------------------|
| `g_quit`, `g_resized` | `terminal.cpp` | Written by a signal handler, which has no `this` |
| `g_orig`, `g_rawOn` | `terminal.cpp` | The terminal is a process-wide resource, not the Player's |
| `g_zoomOrig`, `g_cellAspect` | `zoom.cpp` | Same: the font belongs to the window, not to one video |

Everything else is a member of `Player` and dies with it.
