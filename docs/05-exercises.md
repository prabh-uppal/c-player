# Exercises — learn by breaking it

Work down the list. Each one is a real change to real code, and each one
forces you to have understood something specific. Rebuild with `make` after
every change.

If you get lost, `legacy/asciiplayer.cpp` is the original single-file version
and `make legacy` builds it as `./asciiplayer-legacy`, so you always have a
known-good binary to compare against.

---

## Warm-up — find your way around

**1. Change the frame rate.**
`kRampFps` in `include/asciiplayer/modes.h` is 20. Try 5, then 60. Play
something and watch what happens.
*What you learn:* where the frame rate enters the pipeline (it is passed to
ffmpeg's `fps=` filter in `openAt()`, not just used for timing), and why 60
does not actually give you 60.

**2. Add a fourth charset.**
Add an entry to `kRamps` in `src/render/modes.cpp` — try `" ░▒▓█"` for a
block-shading look. Press `c` to reach it.
*What you learn:* that `kRampCount` is computed with `sizeof`, so nothing else
needs changing. Also that a multi-byte UTF-8 character breaks the indexing —
work out why, and what would have to change to support it.

**3. Change the status bar.**
Make it show the frame number as well as the time. `Player::drawStatus()`,
`src/player/player.cpp`.
*What you learn:* the width-clamping logic — try making your addition long
enough that it stops fitting, and watch the hint text step down through the
`hints[]` variants.

---

## Getting into it

**4. Add a keybinding.**
Make `f` toggle a "fit to width / fit to height" preference that `layout()`
respects.
*What you learn:* the full path of a user action — `handleKey()` → change
state → `openAt(position())` to restart the decoder at the new size → redraw
if paused. Notice you cannot just change the layout; the decoder has to be
restarted because ffmpeg is scaling for us.

**5. Add a new brightness weighting.**
`render()` uses `0.299*R + 0.587*G + 0.114*B`. Try a plain average
`(R+G+B)/3`, and try the Rec.709 weights `0.2126 / 0.7152 / 0.0722`. Compare
on a colourful scene.
*What you learn:* why perceptual weighting matters — watch what happens to
blue skies and to red.

**6. Make the progress bar seekable-looking.**
Show a marker at the position the sound thinks we are at, alongside the
position the video thinks we are at. (`audioBase + last clock reading` vs
`position()`.)
*What you learn:* how far apart the two clocks actually run in practice. You
will probably be surprised how much correction `syncToAudio()` is doing.

---

## The real thing

**7. Make the shape matcher use a 2x3 grid instead of 2x4.**
Change `kSubY` to 3 in `shape_lut.h`. It will compile and produce nonsense,
because `kGlyphs` still holds 8 measurements per glyph.
*What you learn:* where the coupling between the glyph table and the sub-grid
actually lives. Then decide: do you regenerate the table, or average pairs of
rows at load time? Implement one.

**8. Add a "dither" mode.**
In `RGB BLOCKS`, quantise each pixel to a 6x6x6 colour cube and diffuse the
error to its neighbours (Floyd–Steinberg). Compare against the current
true-colour output on a gradient.
*What you learn:* why 24-bit colour makes dithering unnecessary here — and how
much smaller the frames get, which you can see directly in the byte budget.

**9. Remove the byte budget entirely and measure.**
Delete the tolerance and retry logic from `render()`. Play a film with a lot
of scene cuts at a small font size.
*What you learn:* exactly what those 30 lines are buying. Then put it back and
instrument it: print the frame size and `tol` to a log file and watch them
move.

**10. Replace ffplay with direct audio output.**
Feed ffmpeg's raw s16le output to CoreAudio (macOS) or ALSA (Linux) yourself,
and use the number of samples you have submitted as the clock.
*What you learn:* this is the big one. You get an exact sample-accurate clock
instead of parsing stderr, and you will understand why the current design
chose the hack — it is 20 lines instead of 300.

---

## Questions to answer by reading

No code changes required. If you can answer these, you have understood the
program.

1. Why does `position()` use the frame **count** rather than a clock?
   What would break if it used `secondsSince(t0)`?
2. Why is `restoreTerminal()` registered with `std::atexit()` *and* called
   explicitly at the end of `main()`?
3. `readKey()`'s timeout is the frame timer. What would have to change to add
   a second thing that needs waking on a timer?
4. Why does `openAt()` compare `cols`/`rows`/`mode` against `lastCols`/
   `lastRows`/`lastMode` before clearing the screen?
5. `lastMode` is initialised to `Mode::Blocks` while `mode` starts as
   `Mode::Color`. That looks like a bug. Why is it deliberate?
6. Why does the child process in `startAudio()` close every file descriptor
   from 3 to 1024, and what exactly hangs if it does not?
7. In `syncToAudio()`, why `rfind('\r')` twice rather than `find` once?
8. What happens if you run the player with its output piped to a file instead
   of a terminal? Find the code that prevents it.
