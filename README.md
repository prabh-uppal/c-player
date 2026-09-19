# asciiplayer

A video player that draws to your terminal. No window, no GPU — just
characters and 24-bit colour escape codes.

```sh
make
./asciiplayer                 # file browser
./asciiplayer movie.mp4       # play directly
./asciiplayer -g movie.mp4    # relaunch in Ghostty — much smoother (macOS)
```

Needs `ffmpeg` and `ffprobe`; `ffplay` for sound.
`brew install ffmpeg` or `apt install ffmpeg`.

| Key | Action |
|-----|--------|
| `space` | pause / resume |
| `←` `→` | seek 5 s |
| `↑` `↓` | seek 30 s |
| `m` | render mode: RGB ASCII → MONO ASCII → RGB SHAPES → RGB BLOCKS |
| `c` | character set |
| `s` | style: rich → lift → exact |
| `-` `+` | text size (macOS Terminal.app) |
| `a` | mute / unmute |
| `q` | back to the browser |

## Reading the code

Start at **[docs/00-START-HERE.md](docs/00-START-HERE.md)** — it gives the
order to read the modules in and what each one teaches.

```
include/asciiplayer/   the interfaces — what each module does and why
src/
  core/       the steady clock and time formatting
  terminal/   raw mode, escape codes, keyboard
  platform/   running other programs, macOS font zoom
  media/      ffprobe, and the ffmpeg -> ffplay audio pipeline
  render/     modes, the glyph ink table, the shape lookup table
  player/     the main loop
  ui/         file browser, message screen
  main.cpp    start-up
docs/         architecture, deep dives, exercises
legacy/       the original 1435-line single-file version
```

## Build targets

| Command | What it does |
|---------|--------------|
| `make` | build `./asciiplayer` |
| `make run` | build, then open the file browser |
| `make legacy` | build the original single file as `./asciiplayer-legacy` |
| `make clean` | remove `build/` and both binaries |

## How it works, in six lines

1. `ffprobe` reports the video's size, frame rate, duration and audio tracks.
2. `ffmpeg` decodes it, scales it to exactly the terminal's character grid,
   and pipes raw RGB24 frames to us.
3. Each pixel (or 2x4 patch of pixels) becomes one character plus a 24-bit
   colour escape.
4. A byte budget keeps each frame small enough for the terminal to swallow in
   time — only changed cells are redrawn.
5. A second `ffmpeg` decodes the audio from the same exact timestamp and pipes
   it to `ffplay`.
6. `ffplay` reports its own clock on stderr, and the video clock is nudged to
   follow it.
