// terminal.cpp — see include/asciiplayer/terminal.h
#include "asciiplayer/terminal.h"

#include <cerrno>
#include <csignal>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "asciiplayer/zoom.h"

// The terminal settings as we found them, so restoreTerminal() can put them
// back exactly. `static` = private to this file; nothing else may touch them.
static termios g_orig{};
static bool g_rawOn = false;

volatile sig_atomic_t g_quit = 0;
volatile sig_atomic_t g_resized = 0;

void writeAll(const std::string& s) {
    const char* p = s.data();
    size_t left = s.size();
    while (left > 0) {
        ssize_t n = ::write(STDOUT_FILENO, p, left);
        if (n < 0) { if (errno == EINTR) continue; return; }
        p += n;
        left -= (size_t)n;
    }
}

void restoreTerminal() {
    zoomLeave();
    if (!g_rawOn) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
    g_rawOn = false;
    // reset colours, show cursor, re-enable wrap, leave alternate screen
    writeAll("\x1b[0m\x1b[?25h\x1b[?7h\x1b[?1049l");
}

void enableRaw() {
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

void termSize(int& cols, int& rows) {
    winsize w{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
        cols = w.ws_col;
        rows = w.ws_row;
    } else {
        cols = 80;
        rows = 24;
    }
}

void onSignal(int sig) {
    if (sig == SIGWINCH) g_resized = 1;
    else g_quit = 1;
}


// Installed once, from main(). sigaction is preferred over signal(): it does
// not reset the handler after the first signal, and its behaviour is portable.
void installSignalHandlers() {
    struct sigaction sa{};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGWINCH, &sa, nullptr);   // sent when the window is resized
    // Ignore SIGPIPE: when we close the ffmpeg pipe, a write to it must return
    // an error, not kill the process.
    std::signal(SIGPIPE, SIG_IGN);
}
