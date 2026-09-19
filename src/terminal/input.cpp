// input.cpp — see include/asciiplayer/input.h
#include "asciiplayer/input.h"

#include <poll.h>
#include <unistd.h>

static bool readByte(char& c, int timeoutMs) {
    pollfd p{STDIN_FILENO, POLLIN, 0};
    if (poll(&p, 1, timeoutMs) <= 0) return false;
    return ::read(STDIN_FILENO, &c, 1) == 1;
}

Key readKey(int timeoutMs) {
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

