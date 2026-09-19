# Audio/video sync — two processes, one clock

## The problem

Video and audio are decoded by two separate processes that we start at roughly
the same moment. They will not stay together:

- they take different amounts of time to start (ffplay needs 0.1–0.4 s before
  the first sample is audible)
- the audio device runs on its own crystal and plays samples at its own rate
- our drawing can fall behind when the terminal is busy

Drift of 50 ms is invisible. 150 ms and lips are visibly out of step.

## The decision: sound is the master clock

A dropped or repeated *frame* is nearly invisible. A gap or a click in the
*sound* is immediately obvious. So the audio is never adjusted — the picture
is bent to match it.

## Getting picture and sound to share a zero

Why not just hand the file to ffplay and let it seek?

> ffplay's own `-ss` seeks to the nearest keyframe. On some files that is a
> **minute** early — asked for 600 s of a screen recording it played from
> 537 s.

So the audio goes through two processes:

```sh
ffmpeg -nostdin -ss <T> -i file -map 0:<stream> -vn -sn \
       -af aresample=async=1:first_pts=0 -f s16le -ac 2 -ar 48000 - \
  | ffplay -nodisp -autoexit -stats -f s16le -sample_rate 48000 -ch_layout stereo -i -
```

- `ffmpeg -ss` before `-i` is an **input seek**: it decodes up to the exact
  time, the same way the video decoder does. Both streams now start at the
  same media timestamp.
- `aresample=async=1:first_pts=0` pads silence when the audio track begins
  *after* the seek point, so the sound does not start early.
- The video side has the matching trick: `fps=...:start_time=0` pads with the
  first picture when the *video* track starts late. (One test file's video
  begins 0.105 s after its audio.)
- ffplay only *plays*. It does no seeking and no decoding of its own.

## Reading ffplay's clock

`-stats` makes ffplay print its playback position to **stderr**, roughly every
35 ms, as lines like:

```
   1.23 M-A:  0.000 fd=   0 aq=   16KB vq=    0KB sq=    0B
```

We keep that pipe (`Player::afd`) and parse the leading number. The lines are
separated by `\r`, not `\n`, because it is meant to overwrite itself on a
terminal — which is why `syncToAudio()` searches with `rfind('\r')` and always
takes the *last complete* line rather than the first.

## Correcting the drift

Everything hangs off `t0`, the moment the current decoder started. Frame *n*
is due at `t0 + n/fps`. So to shift the video clock, we do not touch the frame
counter — we move `t0`:

```c++
double want  = audioBase + v - startPos;     // where the sound says we are
double drift = secondsSince(t0) - want;      // > 0 means the picture is ahead
if (audioClock && fabs(drift) < 0.25) drift *= 0.25;   // ease small jitter
t0 += drift;
```

Two details worth copying into your own code:

- **Damping.** Applying the full correction every time would make the video
  jitter along with ffplay's own reporting noise. Small errors are corrected a
  quarter at a time; large errors (a real desync, after a seek) are corrected
  at once.
- **`nan` guard.** ffplay prints `nan` before playback actually starts. The
  check is `v != v`, which is true only for NaN — the standard trick, since
  NaN is the one value not equal to itself.

## Waiting for the sound to start

Right after opening, ffplay needs a moment. If the video ran freely during
that time it would be ahead by the time the first sample plays, and then get
yanked backwards.

So `waitingForSound()` holds the picture: it shows the first frame (so the
screen is not blank) and then waits, up to 2 seconds, for the first real clock
reading. `run()` polls more often while holding (10 ms instead of 40 ms) so it
starts the instant the sound does.

## Skipping frames when late

```c++
int maxSkip = ramped() ? kRampMaxSkip : (int)(fps / 2);
double late = secondsSince(t0) - framesRead / fps;
if (late > 0 && (dropped < maxSkip || late > 0.2) && secondsSince(lastDraw) < 0.5) {
    ++dropped;
    continue;                 // decoded, but not drawn
}
```

Read the conditions as three separate policies:

- `dropped < maxSkip` — a few skips in a row is normal, do not panic.
- `|| late > 0.2` — but if we are *really* behind the sound, skip as many as
  it takes to catch up, however many that is.
- `&& secondsSince(lastDraw) < 0.5` — never go more than half a second without
  putting *something* on screen. A frozen picture looks broken even when the
  sound is fine.

Note the frame is still **read** from the pipe, just not drawn. Reading is
cheap; drawing is what costs.

## Cleaning up three processes

The audio side is `sh`, `ffmpeg` and `ffplay` — three processes. Killing the
shell alone would leave the sound playing. So the child calls `setpgid(0, 0)`
to put all three in one process group, and `stopAudio()` kills the **group**:

```c++
kill(-pid, SIGTERM);     // negative pid = the whole group
```

There are two more pieces of defensive cleanup worth understanding:

- **The watchdog.** The first thing in the shell command is
  `(while kill -0 $PPID 2>/dev/null; do sleep 0.5; done; kill -TERM 0) &`.
  If the player itself is killed with `-9` — no chance to clean up — this loop
  notices its parent is gone and takes the audio group down with it. Without
  it, a crash leaves sound playing with no window to stop it.

- **Closing inherited descriptors.** After `fork()` the child closes every
  descriptor from 3 up. The important one is the read end of the *video* pipe.
  If ffplay kept it open, ffmpeg would never see a broken pipe when we close
  our end, would block forever writing to a full pipe, and `pclose()` would
  hang. This is a classic fork/exec bug and it is worth remembering the shape
  of it.
