// message.cpp — see include/asciiplayer/message.h
#include "asciiplayer/message.h"

#include <algorithm>

#include "asciiplayer/ansi.h"
#include "asciiplayer/input.h"
#include "asciiplayer/terminal.h"

// Shows a (possibly multi-line) message and waits for a keypress.
void showMessage(const std::string& msg) {
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

