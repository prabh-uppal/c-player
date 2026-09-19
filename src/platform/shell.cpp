// shell.cpp — see include/asciiplayer/shell.h
#include "asciiplayer/shell.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

std::string shq(const std::string& s) {           // safe shell quoting
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else r += c;
    }
    return r + "'";
}

std::string runCmd(const std::string& cmd) {
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

bool haveTool(const char* name) {
    return std::system((std::string("command -v ") + name + " >/dev/null 2>&1").c_str()) == 0;
}
