// ============================================================================
//  player.cpp — see include/asciiplayer/player.h
//
//  Reading order inside this file:
//    layout()        how many characters the picture gets, and its shape
//    openAt()        spawning ffmpeg (and the sound) at an exact timestamp
//    syncToAudio()   nudging the video clock to follow ffplay
//    render()        the hot loop: pixels -> characters -> one big write
//    run()           the main loop that ties it all together
// ============================================================================
#include "asciiplayer/player.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>

#include "asciiplayer/ansi.h"
#include "asciiplayer/audio.h"
#include "asciiplayer/glyphs.h"
#include "asciiplayer/message.h"
#include "asciiplayer/shape_lut.h"
#include "asciiplayer/shell.h"
#include "asciiplayer/terminal.h"
#include "asciiplayer/zoom.h"

namespace fs = std::filesystem;


Player::Player() {
    std::error_code ec;
    fs::path t = fs::temp_directory_path(ec);
    if (ec) t = "/tmp";
    errLog = (t / ("asciiplayer-" + std::to_string((long)getpid()) + ".log")).string();
}


Player::~Player() {
    closeDecoder();
    stopSound();
    std::error_code ec;
    fs::remove(errLog, ec);
}


void Player::closeDecoder() {
    if (dec) { pclose(dec); dec = nullptr; }
}


// Fit the video into the terminal, keeping its aspect ratio.
void Player::layout() {
    termSize(cols, rows);
    int availW = cols;
    int availH = std::max(1, rows - 1);          // last row = status bar
    double aspect = (double)vi.w / vi.h;

    if (mode == Mode::Blocks) {
        // '▀' = 2 square-ish pixels per cell (fg = top, bg = bottom)
        int w = availW;
        int h = (int)(w / aspect + 0.5);
        if (h > availH * 2) { h = availH * 2; w = (int)(h * aspect + 0.5); }
        h = std::max(2, h & ~1);
        w = std::clamp(w, 1, availW);
        pw = w; ph = h;
        cellW = w; cellH = h / 2;
    } else if (ramped()) {
        // A one-cell margin all round, rows = cols * (h/w) * cell aspect
        // (0.5 unless the terminal lets us measure it). No column cap: more
        // columns is exactly what makes the picture finer.
        double ratio = (double)vi.h / vi.w * cellAspect();
        int w = std::max(1, cols - 2);
        int limit = std::max(1, rows - 2);
        int h = std::max(1, (int)std::lround(w * ratio));
        if (h > limit) { h = limit; w = std::max(1, (int)std::lround(h / ratio)); }
        cellW = w; cellH = h;
        pw = w; ph = h;                  // one pixel per character
    } else {
        // a terminal cell is roughly twice as tall as it is wide
        int w = availW;
        int h = (int)(w / aspect / 2.0 + 0.5);
        if (h > availH) { h = availH; w = (int)(h * 2.0 * aspect + 0.5); }
        h = std::max(1, h);
        w = std::clamp(w, 1, availW);
        cellW = w; cellH = h;
        pw = w * kSubX; ph = h * kSubY;    // 8 sub-samples per character
    }
    offX = (cols - cellW) / 2;
    offY = (availH - cellH) / 2;
    frame.assign((size_t)pw * ph * 3, 0);
}


// (Re)start decoding at time t. Used for start, seek, resume, resize, mode change.
void Player::openAt(double t) {
    closeDecoder();
    stopSound();
    if (vi.duration > 0) t = std::min(t, std::max(0.0, vi.duration - 1.0));
    t = std::max(0.0, t);

    layout();
    drawn.assign((size_t)cellW * cellH, -1);
    if (cols != lastCols || rows != lastRows || mode != lastMode) {
        writeAll("\x1b[0m\x1b[2J");
        lastCols = cols; lastRows = rows; lastMode = mode;
    }

    fps = ramped() ? kRampFps : vi.fps;   // callers took position() at the old rate already
    char ts[32], fpsArg[32];
    snprintf(ts, sizeof ts, "%.3f", t);
    snprintf(fpsArg, sizeof fpsArg, "%.5f", fps);
    std::string cmd =
        "ffmpeg -nostdin -loglevel error -ss " + std::string(ts) +
        " -i " + shq(path) +
        // start_time=0 pads with the first picture when the video track starts
        // after the seek point (this mkv's video begins 0.105 s after its audio).
        " -an -sn -vf fps=" + fpsArg + ":start_time=0" +
        ",scale=" + std::to_string(pw) + ":" + std::to_string(ph) +
        (ramped() ? "" : ":flags=area") +     // ramp modes: ffmpeg's default (bicubic) scaler
        " -f rawvideo -pix_fmt rgb24 - </dev/null 2>" + shq(errLog);
    dec = popen(cmd.c_str(), "r");
    if (dec) fcntl(fileno(dec), F_SETFD, FD_CLOEXEC);   // never leak into ffplay

    startPos = t;
    framesRead = 0;
    t0 = Clock::now();
    if (!paused && !muted && audioOk) startSound(t);
}


void Player::startSound(double t) {
    stopSound();
    apid = startAudio(path, vi.audioIndex, t, afd);
    audioBase = t;
    audioClock = false;
    abuf.clear();
    audioSpawned = Clock::now();
}


void Player::stopSound() {
    stopAudio(apid, afd);
    audioClock = false;
}


// True while the picture should wait for the sound to start: right after
// (re)opening, ffplay needs ~0.1-0.4 s before its first sample is heard.
bool Player::waitingForSound() const {
    return apid > 0 && !audioClock && audioBase == startPos && secondsSince(audioSpawned) < 2.0;
}


// Keep the picture on the sound's clock. ffplay reports how far into the
// audio it is; the frame due now is the one at that same media time.
void Player::syncToAudio() {
    if (afd < 0) return;
    char b[4096];
    ssize_t n;
    while ((n = ::read(afd, b, sizeof b)) > 0) abuf.append(b, (size_t)n);
    size_t end = abuf.rfind('\r');
    if (end == std::string::npos) return;
    size_t beg = abuf.rfind('\r', end == 0 ? 0 : end - 1);
    beg = (beg == std::string::npos || beg >= end) ? 0 : beg + 1;
    std::string line = abuf.substr(beg, end - beg);
    abuf.erase(0, end + 1);
    double v = 0.0;
    if (std::sscanf(line.c_str(), "%lf", &v) != 1 || !(v == v)) return;   // "nan" before it starts

    double want = audioBase + v - startPos;         // video clock that matches the sound
    double drift = secondsSince(t0) - want;         // > 0: picture is ahead
    if (audioClock && std::fabs(drift) < 0.25) drift *= 0.25;   // ease small jitter
    t0 += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(drift));
    audioClock = true;
}


bool Player::readFrame() {
    if (!dec) return false;
    if (fread(frame.data(), 1, frame.size(), dec) != frame.size()) return false;
    ++framesRead;
    ++totalFrames;
    return true;
}


// Why did we decode nothing? ffprobe can happily report a width and height
// for a file ffmpeg cannot actually decode (truncated download, unsupported
// codec), so without this the player just bounced back to the browser.
// True when the decoder closed the pipe well before the end of the file.
bool Player::stoppedEarly() const {
    if (vi.duration <= 0) return totalFrames == 0;   // live stream: no end to fall short of
    return position() < vi.duration - 0.5;
}


// Clip long paths from the left - the filename is the part worth reading.
std::string Player::shortPath() const {
    int room = std::max(24, cols - 6);
    if ((int)path.size() <= room) return path;
    return "..." + path.substr(path.size() - (size_t)room + 3);
}


std::string Player::decodeError() const {
    std::string msg = totalFrames == 0
        ? "Could not decode this video:\n  " + shortPath() + "\n"
        : "Playback stopped early at " + fmtTime(position()) +
          " of " + fmtTime(vi.duration) + ":\n  " + shortPath() + "\n";

    std::string log;
    if (FILE* f = std::fopen(errLog.c_str(), "r")) {
        char b[8192];
        size_t n = std::fread(b, 1, sizeof b, f);
        std::fclose(f);
        log.assign(b, n);
    }

    // ffmpeg repeats the same complaint per frame - collapse the repeats.
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < log.size()) {
        size_t e = log.find('\n', pos);
        if (e == std::string::npos) e = log.size();
        std::string l = log.substr(pos, e - pos);
        if (!l.empty() && (lines.empty() || lines.back() != l)) lines.push_back(l);
        pos = e + 1;
    }

    if (lines.empty()) {
        msg += "\nffmpeg decoded no frames and reported no error.\n";
        msg += "The file may be empty, truncated, or use an unsupported codec.";
    } else {
        msg += "\nffmpeg said:\n";
        for (size_t i = (lines.size() > 6 ? lines.size() - 6 : 0); i < lines.size(); ++i)
            msg += "  " + lines[i] + "\n";
    }
    return msg;
}


void Player::drawStatus() {
    moveTo(out, rows, 1);
    out += "\x1b[0m\x1b[48;2;25;25;30m\x1b[38;2;220;220;220m\x1b[2K";

    const char* state = !audioOk ? "no-audio" : (muted ? "muted" : "sound");
    const std::string tag = std::string("  [") + modeName(mode) +
        (ramped() ? std::string(" ") + kRamps[ramp].name + " " + styleName(style) : std::string()) +
        "] " + state;

    std::string left = paused ? " || " : " >  ";
    left += fmtTime(position());
    if (vi.duration > 0) left += " / " + fmtTime(vi.duration);
    left += "  ";

    // The key clamp: too narrow for the clock at all -> show what we can.
    if ((int)left.size() >= cols) {
        out += left.substr(0, (size_t)std::max(0, cols));
        out += "\x1b[0m";
        return;
    }

    // Help text variants, widest first. Use the longest one that fits
    // whole — a hint chopped mid-word is worse than a shorter hint.
    const std::string hints[] = {
        tag + " | spc pause  <-/-> 5s  up/dn 30s  m mode  c chars  s style" +
            (zoomActive() ? "  -/+ text size" : "") + "  a audio  q back ",
        tag + " | spc pause  <-/-> 5s  up/dn 30s  m mode  c chars  -/+ text size  a audio  q back ",
        tag + " | spc pause  <-/-> 5s  up/dn 30s  m mode  c chars  a audio  q back ",
        tag + " | spc pause  <-/-> 5s  up/dn 30s  m mode  a audio  q back ",
        tag + " | spc  <>5s  ^v30s  m mode  q back ",
        tag + " | spc  m  q back ",
        tag + " ",
        std::string("  ") + state + " ",
        std::string(),
    };

    const int kMinBar = 8;
    std::string right;
    for (const std::string& h : hints)
        if ((int)left.size() + kMinBar + (int)h.size() <= cols) { right = h; break; }

    out += left;

    int barW = cols - (int)left.size() - (int)right.size();
    if (barW > 0) {
        if (vi.duration > 0) {
            int filled = (int)(barW * std::clamp(position() / vi.duration, 0.0, 1.0));
            out += "\x1b[38;2;80;180;255m";
            for (int i = 0; i < filled; ++i) out += "\xE2\x94\x81";        // ━
            out += "\x1b[38;2;70;70;80m";
            for (int i = filled; i < barW; ++i) out += "\xE2\x94\x80";     // ─
            out += "\x1b[38;2;220;220;220m";
        } else {
            out.append((size_t)barW, ' ');                      // live stream: no bar
        }
    }
    out += right;
    out += "\x1b[0m";
}


void Player::render() {
    if (mode == Mode::Shapes) buildShapeLut();
    out.clear();
    out += "\x1b[?2026h";                     // begin synchronized update (less flicker)

    if (mode == Mode::Blocks) {
        for (int y = 0; y < cellH; ++y) {
            moveTo(out, offY + y + 1, offX + 1);
            int lastTop = -1, lastBot = -1;
            const unsigned char* top = &frame[(size_t)(2 * y) * pw * 3];
            const unsigned char* bot = &frame[(size_t)(2 * y + 1) * pw * 3];
            for (int x = 0; x < pw; ++x, top += 3, bot += 3) {
                int tc = (top[0] << 16) | (top[1] << 8) | top[2];
                int bc = (bot[0] << 16) | (bot[1] << 8) | bot[2];
                if (tc != lastTop) { setColor(out, false, top[0], top[1], top[2]); lastTop = tc; }
                if (bc != lastBot) { setColor(out, true,  bot[0], bot[1], bot[2]); lastBot = bc; }
                out += "\xE2\x96\x80";              // ▀
            }
            out += "\x1b[0m";
        }
    } else if (ramped()) {
        bool color = (mode == Mode::Color);
        const char* chars = kRamps[ramp].chars;
        const int top = (int)std::strlen(chars) - 1;
        auto near = [this](int a, int b) {
            return std::abs((a >> 16) - (b >> 16)) <= tol &&
                   std::abs(((a >> 8) & 255) - ((b >> 8) & 255)) <= tol &&
                   std::abs((a & 255) - (b & 255)) <= tol;
        };
        if (drawn.size() != (size_t)cellW * cellH) drawn.assign((size_t)cellW * cellH, -1);
        const bool lift = style != Style::Exact;
        const bool fill = color && style == Style::Rich;
        // A scene cut changes every cell at once and can come out at several
        // times the budget; one such frame stalls Terminal.app for a quarter
        // of a second. So an oversized frame is redone straight away with a
        // coarser tolerance, rather than letting tol creep up over many frames.
        const size_t mark = out.size();
        drawnSaved = drawn;
        for (int attempt = 0;; ++attempt) {
            int last = -1;                       // pixel colour the active SGR state was made from
            for (int y = 0; y < cellH; ++y) {
                const unsigned char* px = &frame[(size_t)y * pw * 3];
                int* was = &drawn[(size_t)y * cellW];
                int cursor = -1;                 // column the terminal cursor is at, -1 = elsewhere
                for (int x = 0; x < cellW; ++x, px += 3) {
                    int r = px[0], g = px[1], b = px[2];
                    float lum = 0.299f * r + 0.587f * g + 0.114f * b;
                    float root = std::sqrt(lum / 255.0f);
                    float level = lift ? root : lum / 255.0f;
                    char ch = chars[std::clamp((int)(level * top), 0, top)];
                    bool inked = color && (fill || ch != ' ');
                    int c = 0;
                    if (inked) {
                        r = r / kQuant * kQuant; g = g / kQuant * kQuant; b = b / kQuant * kQuant;
                        c = (r << 16) | (g << 8) | b;
                    }
                    int old = was[x];
                    if (old >= 0 && (old & 255) == (unsigned char)ch && (c == (old >> 8) || (inked && near(c, old >> 8))))
                        continue;                // already on screen
                    if (cursor != x) moveTo(out, offY + y + 1, offX + x + 1);
                    if (inked) {
                        if (last >= 0 && near(c, last)) c = last;
                        else {
                            int cr = c >> 16, cg = (c >> 8) & 255, cb = c & 255;
                            float k = 1.0f;          // colour carries the other sqrt(brightness)
                            if (lift && root > 0.0f)
                                k = std::min(1.0f / root, 255.0f / std::max({cr, cg, cb, 1}));
                            setColor(out, false, (int)(cr * k), (int)(cg * k), (int)(cb * k));
                            if (fill) {                  // fold the background into the same escape
                                out.back() = ';';
                                out += "48;2;";
                                appendInt(out, cr / 2); out += ';';
                                appendInt(out, cg / 2); out += ';';
                                appendInt(out, cb / 2); out += 'm';
                            }
                            last = c;
                        }
                    }
                    out += ch;
                    was[x] = (c << 8) | (unsigned char)ch;
                    cursor = x + 1;
                }
            }
            out += "\x1b[0m";
            if (out.size() - mark <= budget + budget / 4 || tol >= kMaxTol || attempt == 3) break;
            out.resize(mark);
            drawn = drawnSaved;
            tol = std::min(kMaxTol, std::max(tol * 2, tol + 8));
        }
        size_t used = out.size() - mark;
        if (used > budget)          tol = std::min(kMaxTol, tol + 2);
        else if (used < budget / 2) tol = std::max(0, tol - 1);
    } else {
        for (int y = 0; y < cellH; ++y) {
            moveTo(out, offY + y + 1, offX + 1);
            int last = -1;
            for (int x = 0; x < cellW; ++x) {
                // Gather the cell's 2x4 sub-pixels: one pattern index for
                // the glyph, one colour average for the foreground.
                int lum[kSubN];
                int rs = 0, gs = 0, bs = 0;
                for (int sy = 0; sy < kSubY; ++sy) {
                    const unsigned char* sp =
                        &frame[((size_t)(y * kSubY + sy) * pw + (size_t)x * kSubX) * 3];
                    for (int sx = 0; sx < kSubX; ++sx, sp += 3) {
                        int r = sp[0], g = sp[1], b = sp[2];
                        rs += r; gs += g; bs += b;
                        lum[sy * kSubX + sx] = (r * 299 + g * 587 + b * 114) / 1000;
                    }
                }
                char ch = glyphFor(lum);
                if (ch != ' ') {
                    const int n = kSubN;
                    int r = rs / n, g = gs / n, b = bs / n;
                    int c = (r << 16) | (g << 8) | b;
                    if (c != last) { setColor(out, false, r, g, b); last = c; }
                }
                out += ch;
            }
            out += "\x1b[0m";
        }
    }

    drawStatus();
    out += "\x1b[?2026l";                     // end synchronized update
    auto w0 = Clock::now();
    writeAll(out);                            // blocks while the terminal catches up
    double took = secondsSince(w0), frameT = 1.0 / fps;
    if (took > 0.5 * frameT)                  // back off hard, recover slowly:
        budget = std::max(kFrameBudgetMin, (size_t)(budget * 0.7));   // a stall is what shows
    else if (took < 0.2 * frameT && out.size() > budget / 2)
        budget = std::min(kFrameBudgetMax, (size_t)(budget * 1.01));
    lastDraw = Clock::now();
}


void Player::refreshStatus() {
    out.clear();
    drawStatus();
    writeAll(out);
}

// Used while paused: pull one more frame so the picture is not left blank
// after a seek. If the decoder has nothing, at least refresh the clock.
void Player::showCurrentFrame() {
    if (readFrame()) render();
    else refreshStatus();
}


void Player::seek(double delta) {
    openAt(position() + delta);
    if (paused) showCurrentFrame();
}


void Player::togglePause() {
    if (!paused) {
        paused = true;
        stopSound();
        refreshStatus();
    } else {
        paused = false;
        openAt(position());                   // restart video+audio in sync
    }
}


// returns false = leave the player
bool Player::handleKey(const Key& k) {
    switch (k.k) {
        case K::Right: seek(+5);  break;
        case K::Left:  seek(-5);  break;
        case K::Up:    seek(+30); break;
        case K::Down:  seek(-30); break;
        case K::Esc:
        case K::Back:  return false;
        case K::Char:
            switch (std::tolower((unsigned char)k.c)) {
                case 'q': return false;
                case ' ': togglePause(); break;
                case 'm':
                    mode = (mode == Mode::Color)  ? Mode::Mono
                         : (mode == Mode::Mono)   ? Mode::Shapes
                         : (mode == Mode::Shapes) ? Mode::Blocks : Mode::Color;
                    openAt(position());
                    if (paused) showCurrentFrame();
                    break;
                case '-': case '_': zoomStep(-1); break;   // smaller characters
                case '+': case '=': zoomStep(+1); break;   // resize signal re-lays out
                case 's':
                    style = style == Style::Rich ? Style::Lift
                          : style == Style::Lift ? Style::Exact : Style::Rich;
                    drawn.assign(drawn.size(), -1);
                    tol = 0;                      // each style has its own byte cost
                    if (ramped()) writeAll("\x1b[0m\x1b[2J");   // drop old background fill
                    if (paused) render();
                    break;
                case 'c':
                    ramp = (ramp + 1) % kRampCount;
                    if (paused) render();         // redraw the held frame with the new ramp
                    break;
                case 'a':
                    if (audioOk) {
                        muted = !muted;
                        if (muted) stopSound();
                        else if (!paused) startSound(position());
                        if (paused) refreshStatus();
                    }
                    break;
            }
            break;
        default: break;
    }
    return true;
}


void Player::run() {
    vi = probe(path);
    if (vi.w <= 0 || vi.h <= 0) {
        showMessage("Could not read video: " + path);
        return;
    }
    audioOk = vi.audio && haveTool("ffplay");
    zoomEnter();                              // small font = fine picture
    openAt(0);

    int dropped = 0;
    while (!g_quit) {
        if (g_resized) {
            g_resized = 0;
            int c, r;
            termSize(c, r);
            if (c != cols || r != rows) {         // the font zoom can signal without a change
                openAt(position());
                if (paused) showCurrentFrame();
            }
        }
        if (!paused) syncToAudio();
        bool holding = waitingForSound() && framesRead >= 1;   // first frame shown, sound not yet

        // Wait for keys until the next frame is due (this is our frame timer).
        int timeout = 50;
        if (holding) timeout = 10;
        else if (!paused) {
            double wait = framesRead / fps - secondsSince(t0);
            timeout = std::clamp((int)(wait * 1000.0), 0, 40);   // wake to read the audio clock too
        }
        Key k = readKey(timeout);
        if (k.k != K::None) {
            if (!handleKey(k)) break;
            continue;
        }
        if (paused || holding) continue;
        if (secondsSince(t0) < framesRead / fps) continue;

        if (!readFrame()) { decoderEof = true; break; }   // decoder ran out

        // Too late for this frame? Skip drawing it. A few in a row is normal;
        // far behind the sound, skip as many as it takes - but still show
        // something at least twice a second.
        int maxSkip = ramped() ? kRampMaxSkip : (int)(fps / 2);
        double late = secondsSince(t0) - framesRead / fps;
        if (late > 0 && (dropped < maxSkip || late > 0.2) && secondsSince(lastDraw) < 0.5) {
            ++dropped;
            continue;
        }
        dropped = 0;
        render();
    }
    closeDecoder();
    stopSound();
    zoomLeave();                              // readable text again for messages/browser
    if (decoderEof && !g_quit && stoppedEarly()) showMessage(decodeError());
    writeAll("\x1b[0m\x1b[2J");
}
