# Start here

This is a video player that draws to a terminal. No window, no GPU, no
graphics library — just characters and colour codes written to stdout.

It is about 1400 lines of C++, split into small modules. This page is the
order to read them in. Each module's `.h` file explains what it does and why;
the `.cpp` file is the how.

## Build and run it first

```sh
make          # builds ./asciiplayer
./asciiplayer                 # opens the file browser
./asciiplayer movie.mp4       # plays straight away
./asciiplayer -g movie.mp4    # relaunches in Ghostty (much smoother, macOS)
```

You need `ffmpeg` and `ffprobe` on your PATH, and `ffplay` for sound
(`brew install ffmpeg` / `apt install ffmpeg`).

Keys while playing: `space` pause · `←/→` 5 s · `↑/↓` 30 s · `m` mode ·
`c` charset · `s` style · `-`/`+` text size · `a` mute · `q` back.

## The one-sentence summary

`ffmpeg` decodes the video and shrinks it to exactly the size of your
terminal's character grid; we turn each pixel into a character plus a colour
code; a clock keeps it in time with the sound.

## Reading order

Read these in order. Each one only depends on the ones before it, so you are
never reading about something that has not been explained yet.

| # | Module | What you learn |
|---|--------|----------------|
| 1 | `include/asciiplayer/common.h` | Why playback is timed with a *steady* clock |
| 2 | `terminal.h` / `src/terminal/terminal.cpp` | Raw mode: taking over the terminal |
| 3 | `input.h` / `src/terminal/input.cpp` | Why an arrow key is three bytes |
| 4 | `ansi.h` | The escape codes that move the cursor and set colour |
| 5 | `shell.h` / `src/platform/shell.cpp` | Running ffmpeg, and quoting filenames safely |
| 6 | `probe.h` / `src/media/probe.cpp` | Asking ffprobe what is in a file |
| 7 | `modes.h` / `src/render/modes.cpp` | The four drawing modes and the byte budget |
| 8 | `glyphs.h` + `shape_lut.h` | The interesting bit: picking the right character |
| 9 | `audio.h` / `src/media/audio.cpp` | Two processes, one clock |
| 10 | `player.h` / `src/player/player.cpp` | The main loop that ties it together |
| 11 | `browser.h`, `zoom.h`, `main.cpp` | The rest |

Then read the deep dives:

- [01-architecture.md](01-architecture.md) — the data flow, end to end
- [02-terminal-basics.md](02-terminal-basics.md) — raw mode and ANSI escapes
- [03-ascii-rendering.md](03-ascii-rendering.md) — how a pixel becomes a character
- [04-av-sync.md](04-av-sync.md) — keeping picture and sound together
- [05-exercises.md](05-exercises.md) — things to change, in order of difficulty

## How to actually study this

Reading code top to bottom teaches you less than you think. Three things that
work better:

1. **Break it and see what happens.** Change `kRampFps` from 20 to 5 and play
   something. Now you know what that constant *does*, not what it says.
2. **Follow one frame all the way through.** Pick `Player::run()`, find where
   a frame is read, and trace every function it touches until bytes reach the
   terminal. That single path is 80% of the program.
3. **Compare against the original.** `legacy/asciiplayer.cpp` is this exact
   program as one 1435-line file. Reading the same code in both shapes shows
   you what the split actually bought.

## Layout

```
include/asciiplayer/   what each module offers  (read these first)
src/
  core/                the clock and time formatting
  terminal/            raw mode, keyboard
  platform/            shelling out, macOS font zoom
  media/               ffprobe, ffplay
  render/              modes, glyph tables, the lookup table
  player/              the main loop
  ui/                  file browser, message screen
  main.cpp             start-up
legacy/                the original single-file version, for comparison
docs/                  you are here
```
