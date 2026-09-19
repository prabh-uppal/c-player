# Rendering — how a pixel becomes a character

This is the part that is actually *ours*. Everything else is plumbing.

## The four modes

Press `m` to cycle. They differ in what one character cell represents.

### `RGB BLOCKS` — no letters at all

Uses `▀` (U+2580, upper half block). The **foreground** colour paints the top
half of the cell, the **background** colour paints the bottom half. So one
character carries two independent pixels, and a cell is effectively square.

This gives the best image quality by a wide margin — it is real pixel art. It
is also the least interesting, because the terminal does all the work.

```
ffmpeg scales to  pw x ph  where ph = 2 * cellH
cell (x, y) draws frame[2y][x] as fg, frame[2y+1][x] as bg
```

### `RGB ASCII` / `MONO ASCII` — one pixel per cell, brightness picks a letter

The classic ASCII-art approach.
ffmpeg scales the video so there is exactly **one pixel per character cell**.
Then:

```c++
lum   = 0.299*R + 0.587*G + 0.114*B     // perceived brightness
index = lum/255 * (len(ramp) - 1)
ch    = ramp[index]                      // " .:-=+*#%@"
```

In `RGB ASCII` the pixel's colour also becomes the foreground. In
`MONO ASCII` it does not, so you get plain white-on-black text.

The `0.299 / 0.587 / 0.114` weights are not arbitrary — human eyes are far
more sensitive to green than to blue, so a "brightness" that just averaged R,
G and B would make blue skies look far too bright.

### `RGB SHAPES` — 2x4 sub-pixels per cell, *shape* picks the letter

The interesting one. See below.

## Styles (`s` key) — only for the ramp modes

A plain ramp uses brightness **twice**: a dark pixel gets both
a sparse character (`.`) *and* a dark colour. Those multiply, so shadows
disappear into nothing.

- **`exact`** — the plain mapping, with no lift. True to the raw brightness,
  but dark scenes are unreadable.
- **`lift`** — the character and the colour each carry `sqrt(brightness)`.
  Their product is still the true brightness, but neither factor is crushed,
  so dark scenes stay visible.
- **`rich`** — `lift`, plus the cell background is filled with the pixel's
  colour at half strength. That fills the gaps between characters and between
  rows, which is what makes it look like a picture rather than like text.

`rich` is the default because it looks best on a real terminal with line
spacing.

## The shape matcher (`RGB SHAPES`)

### The problem

Picking one character from a cell's *average* brightness throws away
everything happening inside that cell. A cell that is bright on top and dark
on the bottom averages to mid-grey and gets `+`, exactly like a uniformly
grey cell. All the edges in the picture are lost, which is why character mode
looks so much coarser than block mode.

### The idea

Sample the cell on a **2 wide x 4 tall** sub-grid — 8 samples, and square-ish
given a cell about twice as tall as it is wide. Then pick the character whose
*real ink distribution* best matches that little patch.

`include/asciiplayer/glyphs.h` holds the measurements: for each of ~95 ASCII
characters, how much ink it puts in each of those 8 sub-cells, measured by
rendering it in Menlo. So `_` is `[0,0, 0,0, 0,0, 1,1]` — all its ink at the
bottom — and `"` is the reverse.

Now a cell that is bright on top and dark on the bottom matches `"`, and a
diagonal edge matches `/` or `\`. The character carries the sub-cell
structure.

### Why one score is not enough

The obvious implementation — score each glyph by the squared difference across
all 8 values — collapses the tonal range. Nearly every glyph leaves the top
and bottom of the cell empty (there is leading above the capitals and
descender space below the baseline), so a *uniformly lit* cell matches no
glyph well and every flat region in the picture lands on the same two or three
characters.

So we score two things separately:

- **level error** — how close the glyph's total ink is to the cell's brightness
- **shape error** — how close the glyph's ink *deviations from its own mean*
  are to the cell's deviations from *its* mean

A flat cell has no deviations, so level decides → smooth gradients.
A structured cell has strong deviations, so shape decides → real edges.

The blend between them is chosen by the cell's contrast, in three steps called
"planes":

| plane | when | level weight | shape weight |
|-------|------|--------------|--------------|
| 0 | contrast ≤ 14 | full | 0 — pure brightness ramp |
| 1 | contrast ≤ 40 | full | 0.6 |
| 2 | contrast > 40 | full | 1.0 — full shape matching |

### Why a lookup table

Scoring 95 glyphs per cell, for ~5000 cells, 20 times a second is 9.5 million
scoring loops a second. Too slow.

But look at the *inputs* to that decision:

- which plane (3 options)
- the cell's mean brightness, quantised to 32 levels
- which of the 8 sub-cells are above the mean — an 8-bit pattern, 256 options

That is only `3 × 32 × 256 = 24576` distinct questions. So `buildShapeLut()`
answers all of them once, at startup, into a 24 KB array. After that every
cell is one array lookup:

```c++
return g_shapeLut[plane][lvl][sig];
```

This is the classic **precompute trade**: spend a few milliseconds and 24 KB
of memory to make the hot path free. Worth recognising — it shows up
everywhere in graphics and audio code.

## The byte budget

A terminal only swallows so many bytes per second (Terminal.app manages about
5 MB/s). A zoomed-in 5 pt font means tens of thousands of cells, and a full
colour escape is 19 bytes. A naive frame can easily be 800 KB, which stalls
the terminal for a quarter of a second and you see it as a freeze.

Three mechanisms keep frames inside a budget, and none of them change what the
picture looks like in the normal case:

1. **Only redraw what changed.** `Player::drawn` remembers what each cell
   currently shows (`colour << 8 | glyph`). A cell that would draw the same
   thing is skipped, and so is the cursor move to reach it.

2. **Colour tolerance.** When a frame is still too big, colours within `tol`
   of the one already on screen count as equal. `tol` starts at 0 (exact) and
   only rises under pressure, then creeps back down.

3. **Retry an oversized frame immediately.** A scene cut changes every cell at
   once and can come out several times over budget. Rather than letting `tol`
   creep up over the next second — during which you *see* the stall — the
   frame is thrown away and redrawn at once with a coarser tolerance, up to 3
   times.

The budget itself adapts: if `writeAll()` blocked for more than half a frame's
time the terminal is falling behind, so the budget drops by 30%; if writes are
quick it grows by 1%. Back off hard, recover slowly — a stall is what the
viewer notices.

## Why the picture is centred, not stretched

`Player::layout()` fits the video into the grid keeping its aspect ratio, then
centres it with `offX` / `offY`. The aspect calculation has to account for the
character cell not being square:

```c++
double ratio = (double)vi.h / vi.w * cellAspect();
int h = lround(w * ratio);
```

Get `cellAspect()` wrong and everybody looks tall and thin. This is why the
player reads the terminal's pixel dimensions when it can.
