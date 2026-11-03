# asciiplayer

A video player that draws to your terminal. No window, no GPU — just
characters and 24-bit colour escape codes.

```sh
make
./asciiplayer                 # file browser
./asciiplayer movie.mp4       # play directly
./asciiplayer -g movie.mp4    # relaunch in Ghostty (macOS/Linux)
```

Needs `ffmpeg` and `ffprobe`; `ffplay` for sound.
Install the build tools and media dependencies on Ubuntu/Debian:

```sh
sudo apt update
sudo apt install build-essential ffmpeg
```

On macOS, install the Xcode command-line tools and run `brew install ffmpeg`.

## Linux and Windows/WSL

Build and run from the project directory in a Linux terminal:

```sh
cd /path/to/c-player
make
./asciiplayer                 # open the file browser
./asciiplayer movie.mp4       # play a video
```

On Windows, this project runs in WSL rather than as a native Windows build.
Keep the project in a normal user-accessible folder (for example `E:\c-player`,
not under `C:\Windows\System32`). From Ubuntu/WSL, an `E:\c-player` folder is
available at `/mnt/e/c-player`:

```sh
cd /mnt/e/c-player
make
./asciiplayer
```

If you are at a PowerShell prompt, start the Ubuntu profile in Windows Terminal
with:

```powershell
wt.exe -p Ubuntu
```

Then run the Linux commands above in the Ubuntu tab. Font size is controlled by
the terminal application, not by Ubuntu or this player. In Windows Terminal,
change it in **Settings → Profiles → Ubuntu → Appearance → Font size**. In the
classic PowerShell/Console window, use the title-bar menu's **Properties →
Font**. In a desktop Ubuntu terminal, use its **View → Zoom In/Zoom Out** menu
or the shortcuts listed in that terminal's menus; shortcuts vary by terminal.

Ghostty is optional. On Linux, `./asciiplayer -g movie.mp4` opens a new Ghostty
window only when the Linux `ghostty` command is installed and available on
`PATH`; WSL also needs Linux GUI app support (WSLg). The `-` and `+` keys resize
the font directly only in macOS Terminal.app. Linux font resizing is handled
by the terminal emulator; for Ghostty, configure keybindings in its config
file as described in the [Ghostty keybinding documentation](https://ghostty.org/docs/config/keybind).

References: [Ghostty installation](https://ghostty.org/docs/install/binary),
[Windows Terminal command-line arguments](https://learn.microsoft.com/windows/terminal/command-line-arguments),
and [WSL GUI apps](https://learn.microsoft.com/windows/wsl/tutorials/gui-apps).

| Key | Action |
|-----|--------|
| `space` | pause / resume |
| `←` `→` | seek 5 s |
| `↑` `↓` | seek 30 s |
| `m` | render mode: RGB ASCII → MONO ASCII → RGB SHAPES → RGB BLOCKS |
| `c` | character set |
| `s` | style: rich → lift → exact |
| `-` `+` | text size (macOS Terminal.app only) |
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
