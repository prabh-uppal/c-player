# Terminal basics — raw mode and escape codes

Two ideas carry the whole UI. Neither is specific to this program; they are
how `vim`, `htop` and `top` work too.

## 1. Raw mode

By default a terminal is in **canonical mode**: it collects what you type into
a line, lets you edit it with backspace, echoes it as you go, and only hands
your program the finished line when you press Enter.

That is exactly wrong for a player. We want:

- every keypress the instant it happens (not after Enter) — no `ICANON`
- nothing echoed to the screen — no `ECHO`
- `read()` to return immediately if nothing was typed — `VMIN = 0, VTIME = 0`

That is `enableRaw()` in `src/terminal/terminal.cpp`:

```c++
raw.c_lflag &= ~(ECHO | ICANON);
raw.c_cc[VMIN]  = 0;
raw.c_cc[VTIME] = 0;
tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
```

**You must put it back.** A program that exits in raw mode leaves the user's
shell with no echo — they type and nothing appears. So we save the original
`termios` in `g_orig`, restore it in `restoreTerminal()`, and register that
with `std::atexit()` so it runs on a normal exit, and from a signal handler
path on Ctrl-C.

## 2. Escape sequences

Everything visual is text with escape sequences mixed in. `ESC` is byte 0x1b,
written `\x1b` in C++. The ones this program uses:

| Sequence | Effect | Used for |
|----------|--------|----------|
| `ESC[?1049h` / `l` | enter / leave the **alternate screen** | so quitting restores the user's scrollback, like vim |
| `ESC[?25l` / `h` | hide / show the cursor | a blinking cursor in the middle of a film is ugly |
| `ESC[?7l` / `h` | disable / enable line wrap | a character in the last column must not scroll the screen |
| `ESC[2J` | clear the screen | on mode change and resize |
| `ESC[row;colH` | move the cursor (1-based!) | positioning every row of the picture |
| `ESC[38;2;R;G;Bm` | 24-bit foreground colour | the pixel's colour |
| `ESC[48;2;R;G;Bm` | 24-bit background colour | the half-strength background fill in `rich` style |
| `ESC[0m` | reset all attributes | end of every row |
| `ESC[?2026h` / `l` | begin / end a **synchronized update** | the terminal shows the whole frame at once instead of mid-repaint |

`ESC[?2026` is the one worth knowing about. Without it, a terminal may repaint
while we are still halfway through writing a frame, and you see tearing. With
it, the terminal buffers everything between `h` and `l` and swaps it in one go.
Terminals that do not understand it simply ignore it.

## 3. Reading an arrow key

Arrow keys are not one byte. Pressing Up sends three: `ESC` `[` `A`. So
`readKey()` reads a byte, and if it is `ESC` it reads up to two more with a
short (25 ms) timeout.

The timeout is the subtle part. If the user presses the actual Escape key,
only one byte arrives and no more are coming. Without a timeout we would block
forever waiting for the rest of a sequence that does not exist. 25 ms is long
enough for a real escape sequence (they arrive in one packet) and short enough
that Escape feels instant.

## 4. Knowing the terminal's size

`ioctl(STDOUT_FILENO, TIOCGWINSZ, &w)` fills a `winsize`:

```c
struct winsize {
    unsigned short ws_row, ws_col;        // size in CHARACTERS
    unsigned short ws_xpixel, ws_ypixel;  // size in PIXELS (often 0)
};
```

`ws_col` and `ws_row` are the grid. `ws_xpixel`/`ws_ypixel` are optional —
Terminal.app reports 0, but Ghostty, kitty, iTerm2 and WezTerm fill them in.
When they are there we can compute the **exact** shape of a character cell:

```
cell aspect = (xpixel / cols) / (ypixel / rows)
```

which is what stops the picture looking stretched. See `cellAspect()` in
`src/platform/zoom.cpp`. Without it we guess 0.5 (a cell twice as tall as it
is wide), which is close but not right for every font.

## 5. Resizing

The kernel sends `SIGWINCH` when the window changes size. A signal handler can
do almost nothing safely — no `printf`, no allocation, no locks — so
`onSignal()` sets `g_resized = 1` and returns. The main loop notices the flag
on its next pass, re-runs `layout()` and restarts the decoder at the current
position with the new dimensions.

This is the standard pattern for handling signals: **the handler sets a flag,
the main loop does the work.**
