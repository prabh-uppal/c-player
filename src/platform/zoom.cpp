// zoom.cpp — see include/asciiplayer/zoom.h
//
// Everything here is macOS Terminal.app-specific and driven through
// AppleScript (osascript). On any other terminal these functions do nothing,
// which is why the rest of the player can call them unconditionally.
#include "asciiplayer/zoom.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>

#include "asciiplayer/shell.h"

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
double cellAspect() {
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

void zoomEnter() {
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

void zoomStep(int delta) {                 // '-' / '+' while playing
    if (!g_zoomOrig) return;
    int pt = std::clamp(g_zoomNow + delta, 2, std::max(g_zoomOrig, 2));
    if (pt != g_zoomNow) zoomSet(pt, kZoomFont);
}

void zoomLeave() {
    if (!g_zoomOrig) return;
    zoomSet(g_zoomOrig, g_zoomOrigFont);
    g_zoomOrig = g_zoomNow = 0;
    g_cellAspect = 0.5;
}

// Exposed so the status bar knows whether to advertise the -/+ keys.
bool zoomActive() {
    return g_zoomOrig != 0;
}
