// ============================================================================
//  asciiplayer.cpp  —  Terminal RGB ASCII video player (VLC-style, in your shell)
//
//  Build : g++ -std=c++17 -O2 -o asciiplayer asciiplayer.cpp
//  Run   : ./asciiplayer                 (opens the file browser)
//          ./asciiplayer movie.mp4       (plays directly)
//          ./asciiplayer -g [movie.mp4]  (smoothest: plays in a 5 pt Ghostty window)
//
//  Needs : ffmpeg + ffprobe (video decoding), ffplay (optional, for audio)
//  OS    : Linux / macOS  (on Windows, use WSL)
//
//  How it works
//    1. ffprobe reads the video's width, height, fps and duration.
//    2. ffmpeg decodes + resizes the video to fit the terminal and pipes raw
//       RGB24 frames to us (3 bytes per pixel).
//    3. Each pixel becomes a character: brightness picks the ASCII glyph,
//       the pixel colour becomes a 24-bit ANSI colour  (ESC[38;2;R;G;Bm).
//    4. A clock keeps playback in real time (frames are dropped if late).
//    5. ffmpeg decodes the audio from the same exact seek point and pipes it
//       to ffplay, restarted on seek/pause. ffplay's clock steers the video.
// ============================================================================

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ─────────────────────────────── terminal ───────────────────────────────────
static termios g_orig{};
static bool g_rawOn = false;
static volatile sig_atomic_t g_quit = 0;
static volatile sig_atomic_t g_resized = 0;

static void writeAll(const std::string& s) {
    const char* p = s.data();
    size_t left = s.size();
    while (left > 0) {
        ssize_t n = ::write(STDOUT_FILENO, p, left);
        if (n < 0) { if (errno == EINTR) continue; return; }
        p += n;
        left -= (size_t)n;
    }
}

static void zoomLeave();

static void restoreTerminal() {
    zoomLeave();
    if (!g_rawOn) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
    g_rawOn = false;
    // reset colours, show cursor, re-enable wrap, leave alternate screen
    writeAll("\x1b[0m\x1b[?25h\x1b[?7h\x1b[?1049l");
}

static void enableRaw() {
    tcgetattr(STDIN_FILENO, &g_orig);
    termios raw = g_orig;
    raw.c_lflag &= ~(ECHO | ICANON);   // no echo, read key by key
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    g_rawOn = true;
    // alternate screen, hide cursor, disable line wrap, clear
    writeAll("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[2J");
}

static void termSize(int& cols, int& rows) {
    winsize w{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
        cols = w.ws_col;
        rows = w.ws_row;
    } else {
        cols = 80;
        rows = 24;
    }
}

static void onSignal(int sig) {
    if (sig == SIGWINCH) g_resized = 1;
    else g_quit = 1;
}

// ─────────────────────────────── keyboard ───────────────────────────────────
enum class K { None, Up, Down, Left, Right, Enter, Back, Esc, PgUp, PgDn, Char };
struct Key { K k = K::None; char c = 0; };

static bool readByte(char& c, int timeoutMs) {
    pollfd p{STDIN_FILENO, POLLIN, 0};
    if (poll(&p, 1, timeoutMs) <= 0) return false;
    return ::read(STDIN_FILENO, &c, 1) == 1;
}

static Key readKey(int timeoutMs) {
    char c;
    if (!readByte(c, timeoutMs)) return {};
    if (c == '\x1b') {                       // escape sequence (arrow keys etc.)
        char a, b;
        if (!readByte(a, 25) || (a != '[' && a != 'O')) return {K::Esc};
        if (!readByte(b, 25)) return {K::Esc};
        switch (b) {
            case 'A': return {K::Up};
            case 'B': return {K::Down};
            case 'C': return {K::Right};
            case 'D': return {K::Left};
            case '5': case '6': {            // PgUp = ESC[5~  PgDn = ESC[6~
                char t;
                readByte(t, 25);
                return {b == '5' ? K::PgUp : K::PgDn};
            }
        }
        return {};
    }
    if (c == '\n' || c == '\r') return {K::Enter};
    if (c == 127 || c == 8) return {K::Back};
    return {K::Char, c};
}

// ─────────────────────────────── helpers ────────────────────────────────────
static std::string shq(const std::string& s) {           // safe shell quoting
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else r += c;
    }
    return r + "'";
}

static std::string runCmd(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[512];
    for (;;) {
        size_t n = fread(buf, 1, sizeof buf, p);
        out.append(buf, n);
        if (n > 0) continue;
        // SIGWINCH (e.g. the font zoom resizing us) interrupts the read; retry.
        if (ferror(p) && errno == EINTR) { clearerr(p); continue; }
        break;
    }
    pclose(p);
    return out;
}

static bool haveTool(const char* name) {
    return std::system((std::string("command -v ") + name + " >/dev/null 2>&1").c_str()) == 0;
}

// ── font zoom (macOS Terminal.app) ──────────────────────────────────────────
// How big a character looks is the terminal's font size, not something we can
// draw differently. Terminal.app lets a script change a tab's font size and
// keeps the window the same size on screen, so shrinking the font simply gives
// us more, smaller cells: a finer picture. We do that while a video plays and
// put the font back afterwards. ASCIIPLAYER_ZOOM=<pt> picks the size, 0 = off.
static int g_zoomOrig = 0;           // font size to restore; 0 = not zoomed
static int g_zoomNow = 0;
static std::string g_zoomOrigFont;   // font name to restore
// Width / height of one character cell. 0.5 is the usual guess, but a profile
// with a tall font or extra line spacing can be nearer 0.35 - which both
// stretches the picture and leaves dark gaps between rows. Measured from the
// window when we can.
static double g_cellAspect = 0.5;
// Heavier strokes and a tighter line height than most profile fonts: more ink
// per cell, less empty space between rows. Ships with macOS.
static const char* kZoomFont = "Menlo-Bold";

static std::string zoomScript(const std::string& action) {
    const char* tty = ttyname(STDIN_FILENO);
    if (!tty) return "";
    return std::string("osascript 2>/dev/null") +
        " -e 'tell application \"Terminal\"'"
        " -e 'repeat with w in windows'"
        " -e 'repeat with t in tabs of w'"
        " -e 'if tty of t is \"" + tty + "\" then'" + action +
        " -e 'end if' -e 'end repeat' -e 'end repeat' -e 'end tell'";
}

static bool zoomAvailable() {
    const char* tp = std::getenv("TERM_PROGRAM");
    return tp && std::strcmp(tp, "Apple_Terminal") == 0;
}

// Width / height of a character cell. Terminals that report their pixel size
// (Ghostty, kitty, iTerm2, WezTerm...) give it exactly; Terminal.app reports 0,
// so there it comes from the zoom's window measurement, or the usual 0.5.
static double cellAspect() {
    winsize w{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0 && w.ws_row > 0 &&
        w.ws_xpixel > 0 && w.ws_ypixel > 0)
        return std::clamp(((double)w.ws_xpixel / w.ws_col) / ((double)w.ws_ypixel / w.ws_row), 0.3, 0.7);
    return g_cellAspect;
}

static void zoomSet(int pt, const std::string& font) {
    // Terminal sometimes keeps the grid and shrinks the window instead; putting
    // the window bounds back forces it to re-grid at the new cell size.
    std::string cmd = zoomScript(" -e 'set b to bounds of w'"
                                 " -e 'set font name of t to \"" + font + "\"'"
                                 " -e 'set font size of t to " + std::to_string(pt) + "'"
                                 " -e 'set bounds of w to b'"
                                 " -e 'delay 0.2'"
                                 " -e 'return \"\" & ((item 3 of b) - (item 1 of b)) & \" \" & ((item 4 of b) - (item 2 of b))"
                                 " & \" \" & (number of columns of t) & \" \" & (number of rows of t)'");
    if (cmd.empty()) return;
    std::string r = runCmd(cmd);
    g_zoomNow = pt;
    int W = 0, H = 0, c = 0, n = 0;
    if (std::sscanf(r.c_str(), "%d %d %d %d", &W, &H, &c, &n) == 4 && c > 0 && n > 0) {
        const int kTitleBar = 28;                     // window chrome above the text
        double a = ((double)W / c) / ((double)std::max(1, H - kTitleBar) / n);
        g_cellAspect = std::clamp(a, 0.3, 0.7);
    }
}

static void zoomEnter() {
    if (g_zoomOrig || !zoomAvailable()) return;
    int want = 5;
    if (const char* e = std::getenv("ASCIIPLAYER_ZOOM")) want = std::atoi(e);
    if (want <= 0) return;
    std::string r = runCmd(zoomScript(" -e 'return \"\" & (font size of t) & \"|\" & (font name of t)'"));
    int cur = std::atoi(r.c_str());
    size_t bar = r.find('|');
    if (cur <= 0 || bar == std::string::npos) return;
    g_zoomOrigFont = r.substr(bar + 1);
    while (!g_zoomOrigFont.empty() && std::isspace((unsigned char)g_zoomOrigFont.back())) g_zoomOrigFont.pop_back();
    g_zoomOrig = g_zoomNow = cur;
    zoomSet(std::min(want, cur), kZoomFont);
}

static void zoomStep(int delta) {                 // '-' / '+' while playing
    if (!g_zoomOrig) return;
    int pt = std::clamp(g_zoomNow + delta, 2, std::max(g_zoomOrig, 2));
    if (pt != g_zoomNow) zoomSet(pt, kZoomFont);
}

static void zoomLeave() {
    if (!g_zoomOrig) return;
    zoomSet(g_zoomOrig, g_zoomOrigFont);
    g_zoomOrig = g_zoomNow = 0;
    g_cellAspect = 0.5;
}

static inline void appendInt(std::string& s, int v) {
    char b[12];
    auto r = std::to_chars(b, b + sizeof b, v);
    s.append(b, r.ptr);
}

static inline void moveTo(std::string& s, int row, int col) {
    s += "\x1b[";
    appendInt(s, row);
    s += ';';
    appendInt(s, col);
    s += 'H';
}

static inline void setColor(std::string& s, bool bg, int r, int g, int b) {
    s += bg ? "\x1b[48;2;" : "\x1b[38;2;";
    appendInt(s, r); s += ';';
    appendInt(s, g); s += ';';
    appendInt(s, b); s += 'm';
}

static std::string fmtTime(double t) {
    if (t < 0) t = 0;
    int s = (int)t;
    char b[32];
    if (s >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(b, sizeof b, "%02d:%02d", s / 60, s % 60);
    return b;
}

static double secondsSince(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

// Shows a (possibly multi-line) message and waits for a keypress.
static void showMessage(const std::string& msg) {
    int cols, rows;
    termSize(cols, rows);
    std::string o = "\x1b[0m\x1b[2J";

    int row = 2;
    size_t pos = 0;
    while (pos <= msg.size() && row < rows - 1) {
        size_t e = msg.find('\n', pos);
        if (e == std::string::npos) e = msg.size();
        std::string line = msg.substr(pos, e - pos);
        if (cols > 4 && (int)line.size() > cols - 4) line.resize((size_t)cols - 4);
        moveTo(o, row++, 3);
        o += line;
        if (e >= msg.size()) break;
        pos = e + 1;
    }
    moveTo(o, std::min(row + 1, rows), 3);
    o += "\x1b[38;2;150;150;150m(press any key)\x1b[0m";
    writeAll(o);
    while (!g_quit && readKey(100).k == K::None) {}
}

// ─────────────────────────────── probing ────────────────────────────────────
struct VideoInfo {
    int w = 0, h = 0;
    double fps = 25.0;
    double duration = 0.0;
    bool audio = false;
    int audioIndex = -1;          // stream played: the default audio track, else the first
};

static VideoInfo probe(const std::string& path) {
    VideoInfo vi;
    std::string out = runCmd(
        "ffprobe -v error -select_streams v:0 "
        "-show_entries stream=width,height,r_frame_rate:format=duration "
        "-of default=noprint_wrappers=1 " + shq(path) + " 2>/dev/null");

    size_t pos = 0;
    while (pos < out.size()) {
        size_t e = out.find('\n', pos);
        if (e == std::string::npos) e = out.size();
        std::string line = out.substr(pos, e - pos);
        pos = e + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "width") vi.w = std::atoi(v.c_str());
        else if (k == "height") vi.h = std::atoi(v.c_str());
        else if (k == "duration") vi.duration = std::atof(v.c_str());
        else if (k == "r_frame_rate") {
            int n = 0, d = 1;
            if (std::sscanf(v.c_str(), "%d/%d", &n, &d) == 2 && n > 0 && d > 0)
                vi.fps = (double)n / d;
        }
    }
    if (vi.fps < 1 || vi.fps > 120) vi.fps = 25.0;

    // "index,default" per audio stream, e.g. "1,1\n2,0" for Hindi (default) + English.
    std::string a = runCmd("ffprobe -v error -select_streams a -show_entries stream=index"
                           ":stream_disposition=default -of csv=p=0 " + shq(path) + " 2>/dev/null");
    int idx = -1, def = 0, n = 0;
    for (const char* q = a.c_str(); std::sscanf(q, "%d,%d%n", &idx, &def, &n) == 2; q += n) {
        if (vi.audioIndex < 0 || def == 1) vi.audioIndex = idx;
        if (def == 1) break;
    }
    vi.audio = vi.audioIndex >= 0;
    return vi;
}

// ─────────────────────────────── audio ──────────────────────────────────────
// Audio is decoded by ffmpeg and only *played* by ffplay. ffplay's own -ss jumps
// to a nearby keyframe, and on some files that is a minute early (asked for
// 600 s of a screen recording it played from 537 s); ffmpeg's input seek
// decodes up to the exact time, the same way the video decoder does.
// aresample pads silence where the audio track starts after the seek point, so
// sound and picture share one zero. ffplay's -stats status line reports its
// audio clock ("  1.23 M-A: ...", every ~35 ms) on stderr, which comes back
// to us through errFd so the picture can follow the sound.
static pid_t startAudio(const std::string& path, int stream, double t, int& errFd) {
    errFd = -1;
    int fds[2];
    if (pipe(fds) != 0) return -1;
    char ts[32];
    snprintf(ts, sizeof ts, "%.3f", t);
    // The first line is a watchdog: if the player itself dies (kill -9, crash),
    // it takes the whole audio group down instead of leaving sound playing.
    std::string cmd =
        "(while kill -0 $PPID 2>/dev/null; do sleep 0.5; done; kill -TERM 0) & "
        "ffmpeg -nostdin -loglevel quiet -ss " + std::string(ts) + " -i " + shq(path) +
        " -map 0:" + std::to_string(stream) +
        " -vn -sn -af aresample=async=1:first_pts=0 -f s16le -ac 2 -ar 48000 - 2>/dev/null"
        " | ffplay -nodisp -autoexit -loglevel quiet -stats"
        " -f s16le -sample_rate 48000 -ch_layout stereo -i -";

    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);                        // one group: stopAudio kills sh, ffmpeg and ffplay
        int dn = open("/dev/null", O_RDWR);   // keep the pipeline away from our terminal
        dup2(dn, 0); dup2(dn, 1); dup2(fds[1], 2);
        // Drop every inherited descriptor - above all the read end of the video
        // pipe. If ffplay keeps that open, ffmpeg never sees a broken pipe when
        // we close our end, blocks forever on a full pipe, and pclose() hangs.
        for (int fd = 3; fd < 1024; ++fd) close(fd);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) { close(fds[0]); return -1; }
    setpgid(pid, pid);                        // also here, whichever runs first
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    errFd = fds[0];
    return pid;
}

static void stopAudio(pid_t& pid, int& errFd) {
    if (pid > 0) {
        kill(-pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        pid = -1;
    }
    if (errFd >= 0) { close(errFd); errFd = -1; }
}

// ─────────────────────────────── player ─────────────────────────────────────
enum class Mode { Color, Mono, Shapes, Blocks };

static const char* modeName(Mode m) {
    switch (m) {
        case Mode::Color:  return "RGB ASCII";
        case Mode::Mono:   return "MONO ASCII";
        case Mode::Shapes: return "RGB SHAPES";
        case Mode::Blocks: return "RGB BLOCKS";
    }
    return "";
}

// ── brightness ramps ────────────────────────────────────────────────────────
// Color and Mono: ffmpeg scales the video to one pixel per character cell,
// the pixel's brightness indexes a dark-to-bright ramp, and (in colour) the
// pixel's RGB, quantised to steps of 4, becomes the foreground.
struct Ramp { const char* name; const char* chars; };
static const Ramp kRamps[] = {
    { "classic",  " .:-=+*#%@" },
    { "detailed", " .`^\\,:;Il!i><~+_-?][}{1)(|/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$" },
    { "letters",  ".,:;irsXA253hMHGS#9B&@" },
};
static const int kRampCount = (int)(sizeof(kRamps) / sizeof(kRamps[0]));
static const int kQuant = 4;        // colour quantisation step
// A zoomed-in font means several times more cells, and a terminal only swallows
// so many bytes a second (Terminal.app: about 5 MB/s). Two things keep the
// ramp modes inside that budget without changing what a frame looks like:
//   - only cells that differ from what is already on screen are redrawn;
//   - when a frame still comes out too big, colours within `tol` of the one
//     already active / already on screen are treated as equal. tol is 0 (exact)
//     until the budget is exceeded and creeps back down when there is room.
// How the ramp modes turn a pixel into a cell:
//   Rich  - a plain ramp uses brightness twice: a dark pixel gets a sparse
//           glyph *and* a dark colour, so shadows all but vanish. Here glyph
//           and colour each carry sqrt(brightness), so their product is the
//           real brightness and dark scenes stay readable. The cell background
//           is filled with the pixel's colour at half strength, which closes
//           the gaps between characters and rows.
//   Lift  - the same without the background fill.
//   Exact - the plain mapping: glyph and colour both from raw brightness.
enum class Style { Rich, Lift, Exact };
static const char* styleName(Style s) {
    return s == Style::Rich ? "rich" : s == Style::Lift ? "lift" : "exact";
}

// The budget itself adapts: a write that blocks for most of a frame means the
// terminal is falling behind, so the budget shrinks; quick writes let it grow.
// Terminal.app on an M2 kept 20 fps up to about 300 KB a frame.
static const size_t kFrameBudgetStart = 180000;
static const size_t kFrameBudgetMin = 40000, kFrameBudgetMax = 400000;
static const int kMaxTol = 64;
static const double kRampFps = 20.0;    // fixed ramp-mode frame rate
static const int kRampMaxSkip = 5;      // most frames skipped in a row

// ── glyph shapes ────────────────────────────────────────────────────────────
// One glyph per cell picked from the cell's *average* brightness throws away
// everything happening inside the cell, which is why character mode used to
// look so much coarser than block mode. Instead we sample 8 sub-pixels per
// cell (2 across, 4 down - square, given a ~1:2 character cell) and pick the
// glyph whose real ink distribution best matches that little 2x4 patch. The
// glyph then carries the sub-cell structure: '_' for a bright bottom edge,
// '"' for a bright top, '/' and '\\' for diagonals, and so on.
// Ink coverage of each glyph measured from
// Menlo in a 2-wide x 4-tall grid over the character cell, normalised so
// the densest glyph reads as full brightness. Order: top row first,
// left column first.
struct Glyph { char ch; float ink[8]; };
static const Glyph kGlyphs[] = {
    { ' '   , { 0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f } },   // mean 0.000
    { '`'   , { 0.4648f, 0.0707f, 0.0135f, 0.0543f, 0.0000f, 0.0000f, 0.0000f, 0.0000f } },   // mean 0.075
    { '\''  , { 0.2188f, 0.2881f, 0.3264f, 0.4299f, 0.0000f, 0.0000f, 0.0000f, 0.0000f } },   // mean 0.158
    { '.'   , { 0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.5903f, 0.3298f, 0.2349f, 0.1312f } },   // mean 0.161
    { '-'   , { 0.0000f, 0.0000f, 0.0593f, 0.0631f, 0.7306f, 0.7767f, 0.0000f, 0.0000f } },   // mean 0.204
    { ','   , { 0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.3863f, 0.3112f, 0.8574f, 0.1940f } },   // mean 0.219
    { '_'   , { 0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.0000f, 0.9425f, 0.9303f } },   // mean 0.234
    { ':'   , { 0.0000f, 0.0000f, 0.6064f, 0.4119f, 0.4208f, 0.2859f, 0.1932f, 0.1312f } },   // mean 0.256
    { '^'   , { 0.2613f, 0.2667f, 0.8835f, 0.8802f, 0.0000f, 0.0000f, 0.0000f, 0.0000f } },   // mean 0.286
    { '~'   , { 0.0000f, 0.0000f, 0.7596f, 0.0629f, 0.4114f, 1.0695f, 0.0000f, 0.0000f } },   // mean 0.288
    { '!'   , { 0.1154f, 0.2260f, 0.3651f, 0.7196f, 0.2097f, 0.3976f, 0.1025f, 0.1706f } },   // mean 0.288
    { '"'   , { 0.4824f, 0.4799f, 0.7586f, 0.7546f, 0.0000f, 0.0000f, 0.0000f, 0.0000f } },   // mean 0.309
    { ';'   , { 0.0000f, 0.0000f, 0.6064f, 0.4119f, 0.3872f, 0.3119f, 0.7659f, 0.1926f } },   // mean 0.334
    { 'r'   , { 0.0000f, 0.0000f, 0.8065f, 0.8373f, 1.0086f, 0.0000f, 0.1928f, 0.0000f } },   // mean 0.356
    { '\\'  , { 0.3223f, 0.0000f, 1.0129f, 0.0192f, 0.2068f, 0.8192f, 0.0000f, 0.5200f } },   // mean 0.363
    { '/'   , { 0.0000f, 0.3191f, 0.0818f, 0.9432f, 0.9374f, 0.0940f, 0.5249f, 0.0000f } },   // mean 0.363
    { '+'   , { 0.0000f, 0.0000f, 0.3665f, 0.5464f, 0.9155f, 1.1421f, 0.0000f, 0.0000f } },   // mean 0.371
    { '('   , { 0.1018f, 0.4291f, 0.9361f, 0.0897f, 0.8752f, 0.1412f, 0.0517f, 0.3791f } },   // mean 0.375
    { ')'   , { 0.4761f, 0.0577f, 0.1857f, 0.8375f, 0.2521f, 0.7649f, 0.4120f, 0.0231f } },   // mean 0.376
    { '<'   , { 0.0000f, 0.0000f, 0.5322f, 0.8446f, 0.9077f, 0.8494f, 0.0000f, 0.0000f } },   // mean 0.392
    { '>'   , { 0.0000f, 0.0000f, 0.8425f, 0.5360f, 0.8473f, 0.9095f, 0.0000f, 0.0000f } },   // mean 0.392
    { '|'   , { 0.1034f, 0.2955f, 0.2404f, 0.6871f, 0.2404f, 0.6871f, 0.2404f, 0.6871f } },   // mean 0.398
    { '='   , { 0.0000f, 0.0000f, 0.8075f, 0.8097f, 0.8118f, 0.8141f, 0.0000f, 0.0000f } },   // mean 0.405
    { '?'   , { 0.5190f, 0.5342f, 0.1244f, 1.1330f, 0.4878f, 0.3049f, 0.1328f, 0.0784f } },   // mean 0.414
    { 'l'   , { 0.5688f, 0.2392f, 0.4298f, 0.5632f, 0.3673f, 0.7693f, 0.0000f, 0.4375f } },   // mean 0.422
    { 'c'   , { 0.0000f, 0.0000f, 0.7897f, 0.6585f, 1.1368f, 0.1625f, 0.1960f, 0.4949f } },   // mean 0.430
    { 'i'   , { 0.0242f, 0.3648f, 0.4157f, 0.6312f, 0.1573f, 1.0245f, 0.3922f, 0.5572f } },   // mean 0.446
    { '['   , { 0.1630f, 0.7310f, 0.2782f, 0.7140f, 0.2782f, 0.7140f, 0.1324f, 0.6508f } },   // mean 0.458
    { 'v'   , { 0.0000f, 0.0000f, 0.7003f, 0.6975f, 0.9894f, 0.9902f, 0.1426f, 0.1462f } },   // mean 0.458
    { ']'   , { 0.5629f, 0.3332f, 0.4244f, 0.5686f, 0.4244f, 0.5686f, 0.5145f, 0.2706f } },   // mean 0.458
    { 't'   , { 0.2142f, 0.0000f, 1.3859f, 0.5411f, 0.9591f, 0.1792f, 0.0633f, 0.4194f } },   // mean 0.470
    { 'z'   , { 0.0000f, 0.0000f, 0.5158f, 1.0654f, 0.9011f, 0.4330f, 0.4287f, 0.4411f } },   // mean 0.473
    { 'j'   , { 0.0306f, 0.3584f, 0.4662f, 0.6251f, 0.0780f, 0.9132f, 0.7061f, 0.6218f } },   // mean 0.475
    { '7'   , { 0.7332f, 0.7288f, 0.0000f, 1.0636f, 0.6328f, 0.4770f, 0.2191f, 0.0000f } },   // mean 0.482
    { 'L'   , { 0.3426f, 0.0000f, 1.0988f, 0.0000f, 1.1959f, 0.2582f, 0.4162f, 0.5438f } },   // mean 0.482
    { 'f'   , { 0.1258f, 0.6804f, 1.0381f, 0.8439f, 0.6517f, 0.3425f, 0.1255f, 0.0659f } },   // mean 0.484
    { 'x'   , { 0.0000f, 0.0000f, 0.7444f, 0.7480f, 0.9757f, 0.9824f, 0.2204f, 0.2194f } },   // mean 0.486
    { '*'   , { 0.0000f, 0.0000f, 0.8234f, 0.8654f, 1.0490f, 1.1002f, 0.0365f, 0.0419f } },   // mean 0.490
    { 's'   , { 0.0000f, 0.0000f, 0.8720f, 0.5185f, 0.6814f, 1.1603f, 0.4388f, 0.3191f } },   // mean 0.499
    { '{'   , { 0.1175f, 0.7731f, 0.6459f, 0.5341f, 0.6625f, 0.5320f, 0.0833f, 0.7042f } },   // mean 0.507
    { '}'   , { 0.7196f, 0.1741f, 0.4290f, 0.7499f, 0.4268f, 0.7665f, 0.6623f, 0.1283f } },   // mean 0.507
    { 'Y'   , { 0.3586f, 0.3595f, 1.0518f, 1.0506f, 0.5476f, 0.5566f, 0.1046f, 0.1063f } },   // mean 0.517
    { 'T'   , { 0.8894f, 0.8921f, 0.5326f, 0.5632f, 0.5326f, 0.5632f, 0.1025f, 0.1084f } },   // mean 0.523
    { 'J'   , { 0.3723f, 0.5533f, 0.0000f, 1.0877f, 0.2802f, 1.1385f, 0.5494f, 0.2119f } },   // mean 0.524
    { '1'   , { 0.3597f, 0.2749f, 0.5121f, 0.9132f, 0.3199f, 1.0657f, 0.3588f, 0.4971f } },   // mean 0.538
    { 'u'   , { 0.0000f, 0.0000f, 0.6764f, 0.6765f, 1.0725f, 1.1753f, 0.3662f, 0.3676f } },   // mean 0.542
    { 'n'   , { 0.0000f, 0.0000f, 1.0170f, 0.9466f, 0.9969f, 0.9961f, 0.1917f, 0.1918f } },   // mean 0.543
    { 'C'   , { 0.3654f, 0.6838f, 1.1629f, 0.0403f, 1.2096f, 0.2099f, 0.1969f, 0.5106f } },   // mean 0.547
    { 'y'   , { 0.0000f, 0.0000f, 0.7118f, 0.7075f, 0.9508f, 0.9888f, 0.9450f, 0.2625f } },   // mean 0.571
    { 'I'   , { 0.6522f, 0.6525f, 0.5434f, 0.5524f, 0.6982f, 0.7065f, 0.4307f, 0.4308f } },   // mean 0.583
    { 'F'   , { 0.6118f, 0.7634f, 1.3724f, 0.6632f, 1.0987f, 0.0000f, 0.2115f, 0.0000f } },   // mean 0.590
    { 'o'   , { 0.0000f, 0.0000f, 0.9133f, 0.9130f, 1.1331f, 1.1279f, 0.3250f, 0.3285f } },   // mean 0.593
    { '2'   , { 0.6713f, 0.4784f, 0.0729f, 1.1578f, 0.9846f, 0.5514f, 0.4818f, 0.4609f } },   // mean 0.607
    { 'e'   , { 0.0000f, 0.0000f, 0.8814f, 0.9083f, 1.5186f, 0.8632f, 0.2835f, 0.4833f } },   // mean 0.617
    { 'w'   , { 0.0000f, 0.0000f, 0.7272f, 0.7252f, 1.5601f, 1.5546f, 0.2148f, 0.2138f } },   // mean 0.624
    { 'V'   , { 0.3483f, 0.3466f, 1.0574f, 1.0539f, 0.9621f, 0.9632f, 0.1430f, 0.1466f } },   // mean 0.628
    { 'h'   , { 0.4123f, 0.0000f, 1.3312f, 0.9466f, 0.9969f, 0.9961f, 0.1917f, 0.1918f } },   // mean 0.633
    { '3'   , { 0.6610f, 0.4852f, 0.3505f, 1.2444f, 0.2407f, 1.2591f, 0.5271f, 0.3099f } },   // mean 0.635
    { 'k'   , { 0.4257f, 0.0000f, 1.1029f, 0.7466f, 1.3378f, 1.0456f, 0.1980f, 0.2324f } },   // mean 0.636
    { 'a'   , { 0.0000f, 0.0000f, 0.5959f, 0.8810f, 1.2709f, 1.5657f, 0.4310f, 0.3668f } },   // mean 0.639
    { '%'   , { 0.4053f, 0.0000f, 1.4766f, 0.7641f, 0.7215f, 1.5003f, 0.0000f, 0.3617f } },   // mean 0.654
    { 'Z'   , { 0.6730f, 0.8485f, 0.0667f, 1.0971f, 1.1514f, 0.3595f, 0.4776f, 0.5749f } },   // mean 0.656
    { '4'   , { 0.0065f, 0.4853f, 0.7637f, 1.1901f, 1.0603f, 1.5541f, 0.0000f, 0.2084f } },   // mean 0.659
    { '5'   , { 0.6551f, 0.5348f, 1.2148f, 0.6288f, 0.2232f, 1.2282f, 0.5448f, 0.2679f } },   // mean 0.662
    { 'S'   , { 0.5247f, 0.5870f, 1.3576f, 0.3689f, 0.3043f, 1.3404f, 0.4791f, 0.3514f } },   // mean 0.664
    { 'X'   , { 0.3611f, 0.3576f, 0.9626f, 0.9973f, 1.1156f, 1.0883f, 0.2261f, 0.2228f } },   // mean 0.666
    { '$'   , { 0.0565f, 0.1818f, 1.3405f, 0.8871f, 0.5673f, 1.6686f, 0.3811f, 0.5042f } },   // mean 0.698
    { 'P'   , { 0.6668f, 0.5619f, 1.2056f, 1.3735f, 1.3064f, 0.2832f, 0.2105f, 0.0000f } },   // mean 0.701
    { 'm'   , { 0.0000f, 0.0000f, 1.2994f, 1.1976f, 1.3232f, 1.3926f, 0.2547f, 0.2681f } },   // mean 0.717
    { 'G'   , { 0.4213f, 0.6332f, 1.1642f, 0.2593f, 1.2091f, 1.3474f, 0.2437f, 0.4663f } },   // mean 0.718
    { 'p'   , { 0.0000f, 0.0000f, 1.0736f, 0.9325f, 1.2141f, 1.1085f, 1.0937f, 0.3605f } },   // mean 0.723
    { 'q'   , { 0.0000f, 0.0000f, 0.9127f, 1.0837f, 1.1176f, 1.2010f, 0.3512f, 1.1178f } },   // mean 0.723
    { 'd'   , { 0.0000f, 0.4102f, 0.9373f, 1.3854f, 1.1164f, 1.2040f, 0.3570f, 0.3819f } },   // mean 0.724
    { 'b'   , { 0.4123f, 0.0000f, 1.3818f, 0.9441f, 1.2084f, 1.1114f, 0.3760f, 0.3629f } },   // mean 0.725
    { 'A'   , { 0.2511f, 0.2567f, 1.0014f, 1.0015f, 1.4288f, 1.4322f, 0.2162f, 0.2153f } },   // mean 0.725
    { 'U'   , { 0.3426f, 0.3392f, 1.0987f, 1.0876f, 1.1587f, 1.1499f, 0.3396f, 0.3427f } },   // mean 0.732
    { 'E'   , { 0.6672f, 0.7189f, 1.4238f, 0.6693f, 1.1999f, 0.2400f, 0.4349f, 0.5054f } },   // mean 0.732
    { '&'   , { 0.4704f, 0.3738f, 1.2960f, 0.1157f, 1.1991f, 1.7672f, 0.4122f, 0.4839f } },   // mean 0.765
    { '6'   , { 0.4182f, 0.5783f, 1.4685f, 0.6817f, 1.1943f, 1.1491f, 0.2996f, 0.3540f } },   // mean 0.768
    { '9'   , { 0.5379f, 0.4631f, 1.1148f, 1.2113f, 0.6924f, 1.4563f, 0.4381f, 0.2323f } },   // mean 0.768
    { 'K'   , { 0.3426f, 0.3976f, 1.7431f, 0.7783f, 1.2300f, 1.1923f, 0.2115f, 0.2514f } },   // mean 0.768
    { 'O'   , { 0.4927f, 0.4962f, 1.1410f, 1.1351f, 1.1921f, 1.1836f, 0.3152f, 0.3172f } },   // mean 0.784
    { 'H'   , { 0.3426f, 0.3409f, 1.5196f, 1.5182f, 1.0987f, 1.0931f, 0.2115f, 0.2104f } },   // mean 0.792
    { 'g'   , { 0.0000f, 0.0000f, 0.9407f, 1.0572f, 1.1454f, 1.2239f, 0.8241f, 1.1603f } },   // mean 0.794
    { '#'   , { 0.1575f, 0.2972f, 1.3040f, 1.5098f, 1.5074f, 1.3202f, 0.2062f, 0.1255f } },   // mean 0.803
    { 'D'   , { 0.7607f, 0.3324f, 1.0987f, 1.1928f, 1.2366f, 1.2490f, 0.4903f, 0.1430f } },   // mean 0.813
    { '8'   , { 0.5231f, 0.5258f, 1.2213f, 1.2204f, 1.2274f, 1.2208f, 0.3616f, 0.3637f } },   // mean 0.833
    { 'R'   , { 0.7489f, 0.4509f, 1.3165f, 1.2867f, 1.2880f, 1.1643f, 0.2115f, 0.2234f } },   // mean 0.836
    { 'Q'   , { 0.4927f, 0.4962f, 1.1410f, 1.1351f, 1.1895f, 1.1871f, 0.3143f, 0.7759f } },   // mean 0.841
    { '0'   , { 0.4538f, 0.4569f, 1.2025f, 1.6455f, 1.6292f, 1.1643f, 0.2980f, 0.3008f } },   // mean 0.894
    { 'W'   , { 0.3276f, 0.3258f, 1.4417f, 1.4371f, 1.5995f, 1.5897f, 0.2216f, 0.2205f } },   // mean 0.895
    { 'M'   , { 0.4896f, 0.4891f, 1.8216f, 1.7420f, 1.2612f, 1.1011f, 0.1938f, 0.1938f } },   // mean 0.912
    { 'B'   , { 0.7139f, 0.5079f, 1.4609f, 1.3708f, 1.2130f, 1.2744f, 0.4672f, 0.3018f } },   // mean 0.914
    { '@'   , { 0.1037f, 0.2335f, 1.2463f, 1.5896f, 1.5279f, 1.3595f, 0.7491f, 0.6620f } },   // mean 0.934
    { 'N'   , { 0.5193f, 0.3486f, 2.1323f, 1.1825f, 1.3377f, 1.9410f, 0.2240f, 0.3146f } },   // mean 1.000
};
static const int kGlyphCount = (int)(sizeof(kGlyphs) / sizeof(kGlyphs[0]));

// Brightness and shape have to be scored separately. Matching the raw 8-value
// patch directly sounds right but collapses the tonal range: most glyphs leave
// the top and bottom of the cell empty (leading above the caps, descender space
// below the baseline), so a uniformly-lit cell can't match any of them well and
// every flat region lands on the same two or three glyphs. So:
//
//   level error - how close the glyph's overall ink is to the cell's brightness
//   shape error - how close the glyph's ink *deviations* are to the cell's
//
// Flat cells have no deviations, so level decides and we get a smooth ramp.
// Structured cells have strong deviations, so shape decides and we get '_', '"',
// '/' and friends tracking the real edge inside the cell.
static const int kSubX = 2, kSubY = 4;
static const int kSubN = kSubX * kSubY;

static const int kLevels = 32;      // brightness steps -> smooth flat regions
static const int kPlanes = 3;       // flat, soft and hard contrast targets
static char g_shapeLut[kPlanes][kLevels][256];
static bool g_shapeLutReady = false;

static void buildShapeLut() {
    if (g_shapeLutReady) return;

    // Per glyph: mean ink, and how each sub-cell deviates from that mean.
    static float gMean[256], gDev[256][kSubN];
    for (int g = 0; g < kGlyphCount; ++g) {
        float m = 0.0f;
        for (int i = 0; i < kSubN; ++i) m += kGlyphs[g].ink[i];
        m /= kSubN;
        gMean[g] = m;
        for (int i = 0; i < kSubN; ++i) gDev[g][i] = kGlyphs[g].ink[i] - m;
    }

    // No ASCII glyph is a solid block - they all leave the top and bottom of the
    // cell empty - so a flat cell can never match one on shape. Judging a flat
    // cell on shape therefore rejects every dense glyph and strands the bright
    // end of the ramp. The shape term has to fade out with the cell's contrast:
    // plane 0 is pure brightness (smooth gradients), plane 2 is full shape
    // matching (edges and diagonals), plane 1 sits between them.
    const float kLevelWeight = 4.0f;
    const float kContrast[kPlanes]   = { 0.00f, 0.18f, 0.40f };
    const float kShapeWeight[kPlanes] = { 0.00f, 0.60f, 1.00f };

    for (int plane = 0; plane < kPlanes; ++plane) {
        for (int lvl = 0; lvl < kLevels; ++lvl) {
            float tm = (float)lvl / (kLevels - 1);
            for (int sig = 0; sig < 256; ++sig) {
                float tdev[kSubN];
                for (int i = 0; i < kSubN; ++i)
                    tdev[i] = (sig >> i) & 1 ? kContrast[plane] : -kContrast[plane];

                float best = 1e30f;
                char bestCh = ' ';
                for (int g = 0; g < kGlyphCount; ++g) {
                    float lv = tm - gMean[g];
                    float sum = kLevelWeight * kSubN * lv * lv;
                    if (kShapeWeight[plane] > 0.0f) {
                        float shape = 0.0f;
                        for (int i = 0; i < kSubN; ++i) {
                            float d = tdev[i] - gDev[g][i];
                            shape += d * d;
                        }
                        sum += kShapeWeight[plane] * shape;
                    }
                    if (sum < best) { best = sum; bestCh = kGlyphs[g].ch; }
                }
                g_shapeLut[plane][lvl][sig] = bestCh;
            }
        }
    }
    g_shapeLutReady = true;
}

// Picks the glyph for one cell from its 8 sub-pixel luminances.
static inline char glyphFor(const int* lum) {
    int sum = 0;
    for (int i = 0; i < kSubN; ++i) sum += lum[i];
    int mean = sum / kSubN;

    int sig = 0, spread = 0;
    for (int i = 0; i < kSubN; ++i) {
        if (lum[i] > mean) sig |= 1 << i;
        spread += lum[i] > mean ? lum[i] - mean : mean - lum[i];
    }
    int avgSpread = spread / kSubN;
    int plane = avgSpread > 40 ? 2 : (avgSpread > 14 ? 1 : 0);
    int lvl = mean * (kLevels - 1) / 255;
    return g_shapeLut[plane][lvl][sig];
}

struct Player {
    std::string path;
    VideoInfo vi;
    Mode mode = Mode::Color;          // start in text-glyph ASCII; 'm' cycles to blocks
    int ramp = 0;                     // index into kRamps; 'c' cycles
    Style style = Style::Rich;        // 's' cycles
    bool paused = false, muted = false, audioOk = false;

    FILE* dec = nullptr;          // ffmpeg pipe
    pid_t apid = -1;              // audio pipeline (process group leader)
    int afd = -1;                 // ffplay's stderr: its -stats audio clock
    std::string abuf;             // unparsed tail of afd
    double audioBase = 0.0;       // media time the audio pipeline started from
    bool audioClock = false;      // ffplay has reported a real clock since it started
    Clock::time_point audioSpawned, lastDraw;

    int cols = 80, rows = 24;
    int pw = 0, ph = 0;           // decoded pixel size
    int cellW = 0, cellH = 0;     // size on screen in characters
    int offX = 0, offY = 0;       // centring offset
    int lastCols = -1, lastRows = -1;
    Mode lastMode = Mode::Blocks;  // differs from `mode` so the first frame clears

    std::vector<unsigned char> frame;
    std::vector<int> drawn;           // ramp modes: what each cell shows now (colour<<8 | glyph), -1 = unknown
    std::vector<int> drawnSaved;      // `drawn` before this frame, for redoing an oversized one
    int tol = 0;                      // colour tolerance, see kFrameBudgetStart
    size_t budget = kFrameBudgetStart;
    double startPos = 0.0;
    long framesRead = 0;              // frames since the last (re)open
    long totalFrames = 0;             // frames decoded for this file, ever
    bool decoderEof = false;          // playback ended because the pipe closed
    std::string errLog;               // ffmpeg's stderr, kept for diagnostics
    Clock::time_point t0;
    std::string out;

    Player() {
        std::error_code ec;
        fs::path t = fs::temp_directory_path(ec);
        if (ec) t = "/tmp";
        errLog = (t / ("asciiplayer-" + std::to_string((long)getpid()) + ".log")).string();
    }

    ~Player() {
        closeDecoder();
        stopSound();
        std::error_code ec;
        fs::remove(errLog, ec);
    }

    double fps = 25.0;                // rate the current decoder emits frames at

    double position() const { return startPos + framesRead / fps; }
    bool ramped() const { return mode == Mode::Color || mode == Mode::Mono; }

    void closeDecoder() {
        if (dec) { pclose(dec); dec = nullptr; }
    }

    // Fit the video into the terminal, keeping its aspect ratio.
    void layout() {
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
    void openAt(double t) {
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

    void startSound(double t) {
        stopSound();
        apid = startAudio(path, vi.audioIndex, t, afd);
        audioBase = t;
        audioClock = false;
        abuf.clear();
        audioSpawned = Clock::now();
    }

    void stopSound() {
        stopAudio(apid, afd);
        audioClock = false;
    }

    // True while the picture should wait for the sound to start: right after
    // (re)opening, ffplay needs ~0.1-0.4 s before its first sample is heard.
    bool waitingForSound() const {
        return apid > 0 && !audioClock && audioBase == startPos && secondsSince(audioSpawned) < 2.0;
    }

    // Keep the picture on the sound's clock. ffplay reports how far into the
    // audio it is; the frame due now is the one at that same media time.
    void syncToAudio() {
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

    bool readFrame() {
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
    bool stoppedEarly() const {
        if (vi.duration <= 0) return totalFrames == 0;   // live stream: no end to fall short of
        return position() < vi.duration - 0.5;
    }

    // Clip long paths from the left - the filename is the part worth reading.
    std::string shortPath() const {
        int room = std::max(24, cols - 6);
        if ((int)path.size() <= room) return path;
        return "..." + path.substr(path.size() - (size_t)room + 3);
    }

    std::string decodeError() const {
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

    void drawStatus() {
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
                (g_zoomOrig ? "  -/+ text size" : "") + "  a audio  q back ",
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

    void render() {
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

    void refreshStatus() {
        out.clear();
        drawStatus();
        writeAll(out);
    }

    void showCurrentFrame() {                    // used while paused
        if (readFrame()) render();
        else refreshStatus();
    }

    void seek(double delta) {
        openAt(position() + delta);
        if (paused) showCurrentFrame();
    }

    void togglePause() {
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
    bool handleKey(const Key& k) {
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

    void run() {
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
};

// ─────────────────────────────── file browser ───────────────────────────────
static bool isVideo(const fs::path& p) {
    static const char* exts[] = {".mp4", ".mkv", ".avi", ".mov", ".webm", ".flv", ".wmv",
                                 ".m4v", ".mpg", ".mpeg", ".gif", ".ts", ".3gp", ".ogv"};
    std::string e = p.extension().string();
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    for (const char* x : exts)
        if (e == x) return true;
    return false;
}

struct Entry { std::string name; bool dir; };

static std::vector<Entry> listDir(const fs::path& dir) {
    std::vector<Entry> items, dirs, files;
    if (dir != dir.root_path()) items.push_back({"..", true});
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;          // hide dotfiles
        std::error_code ec2;
        if (it->is_directory(ec2)) dirs.push_back({name, true});
        else if (isVideo(it->path())) files.push_back({name, false});
    }
    auto byName = [](const Entry& a, const Entry& b) { return a.name < b.name; };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    items.insert(items.end(), dirs.begin(), dirs.end());
    items.insert(items.end(), files.begin(), files.end());
    return items;
}

// Returns the chosen file, or "" if the user quits. `dir` is remembered between calls.
static std::string browse(fs::path& dir) {
    int sel = 0, top = 0;
    bool reload = true;
    std::string selectName;
    std::vector<Entry> items;

    while (!g_quit) {
        if (reload) {
            items = listDir(dir);
            sel = 0; top = 0;
            for (int i = 0; i < (int)items.size(); ++i)
                if (items[i].name == selectName) sel = i;
            selectName.clear();
            reload = false;
        }

        int cols, rows;
        termSize(cols, rows);
        int listH = std::max(1, rows - 4);
        sel = std::clamp(sel, 0, std::max(0, (int)items.size() - 1));
        if (sel < top) top = sel;
        if (sel >= top + listH) top = sel - listH + 1;

        std::string o = "\x1b[?2026h\x1b[0m\x1b[2J\x1b[H";
        o += "\x1b[1;38;2;120;200;255m ASCII RGB PLAYER \x1b[0m\x1b[38;2;150;150;150m  select a video\r\n";
        o += " " + dir.string() + "\x1b[0m\r\n\r\n";
        for (int i = top; i < std::min((int)items.size(), top + listH); ++i) {
            o += (i == sel) ? "\x1b[7m > " : "   ";
            if (items[i].dir) o += "\x1b[38;2;100;170;255m" + items[i].name + "/";
            else              o += "\x1b[38;2;235;235;235m" + items[i].name;
            o += "\x1b[0m\r\n";
        }
        if (items.empty()) o += "   (empty)\r\n";
        moveTo(o, rows, 1);
        o += "\x1b[38;2;150;150;150m up/down move   Enter open   left/Backspace parent   ~ home   q quit\x1b[0m";
        o += "\x1b[?2026l";
        writeAll(o);

        Key k = readKey(-1);   // blocks; a resize interrupts it and we just redraw
        switch (k.k) {
            case K::Up:   --sel; break;
            case K::Down: ++sel; break;
            case K::PgUp: sel -= listH; break;
            case K::PgDn: sel += listH; break;
            case K::Left:
            case K::Back:
                if (dir != dir.root_path()) {
                    selectName = dir.filename().string();
                    dir = dir.parent_path();
                    reload = true;
                }
                break;
            case K::Enter:
            case K::Right: {
                if (items.empty()) break;
                const Entry& e = items[sel];
                if (e.dir) {
                    if (e.name == "..") { selectName = dir.filename().string(); dir = dir.parent_path(); }
                    else dir /= e.name;
                    reload = true;
                } else {
                    return (dir / e.name).string();
                }
                break;
            }
            case K::Esc: return "";
            case K::Char:
                if (k.c == 'q' || k.c == 'Q') return "";
                if (k.c == '~') {
                    if (const char* h = std::getenv("HOME")) { dir = h; reload = true; }
                }
                break;
            default: break;
        }
    }
    return "";
}

// ─────────────────────────────── main ───────────────────────────────────────
int main(int argc, char** argv) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::fprintf(stderr, "Please run asciiplayer inside a terminal.\n");
        return 1;
    }
    if (!haveTool("ffmpeg") || !haveTool("ffprobe")) {
        std::fprintf(stderr,
            "ffmpeg/ffprobe not found.\n"
            "  Ubuntu/Debian: sudo apt install ffmpeg\n"
            "  Arch:          sudo pacman -S ffmpeg\n"
            "  macOS:         brew install ffmpeg\n");
        return 1;
    }

    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGWINCH, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    // -g: play in Ghostty instead. Terminal.app draws on the CPU and saturates
    // (~150% CPU) repainting a full-screen 5 pt grid 20 times a second, so it
    // shows fewer frames than we send; Ghostty draws on the GPU and does the
    // same at ~30%. Opens a maximised 5 pt Ghostty window running this player.
    if (argc > 1 && (std::strcmp(argv[1], "-g") == 0 || std::strcmp(argv[1], "--ghostty") == 0)) {
        std::error_code ec;
        fs::path self = fs::canonical(argv[0], ec);
        if (ec) self = fs::absolute(argv[0]);
        int pt = 5;
        if (const char* e = std::getenv("ASCIIPLAYER_ZOOM")) if (std::atoi(e) > 0) pt = std::atoi(e);
        std::string cmd = "open -na Ghostty --args --font-size=" + std::to_string(pt) +
            " --maximize=true --window-save-state=never --quit-after-last-window-closed=true"
            " --working-directory=" + shq(fs::current_path().string()) +
            " -e " + shq(self.string());
        if (argc > 2) cmd += " " + shq(fs::absolute(argv[2]).string());
        if (std::system((cmd + " 2>/dev/null").c_str()) != 0) {
            std::fprintf(stderr, "Could not open Ghostty. Install it from https://ghostty.org\n");
            return 1;
        }
        return 0;
    }

    fs::path dir = fs::current_path();
    std::string file = argc > 1 ? argv[1] : "";
    if (!file.empty()) {
        std::error_code ec;
        fs::path p = fs::absolute(file, ec);
        if (!ec) { file = p.string(); dir = p.parent_path(); }
    }

    enableRaw();
    std::atexit(restoreTerminal);

    while (!g_quit) {
        if (file.empty()) file = browse(dir);
        if (file.empty()) break;
        {
            Player player;
            player.path = file;
            player.run();
        }
        file.clear();                // back to the browser after playback
    }

    restoreTerminal();
    return 0;
}
