#pragma once

#include <charconv>
#include <string>

inline void appendInt(std::string& s, int value) {
    char buffer[12];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    s.append(buffer, result.ptr);
}

inline void moveTo(std::string& s, int row, int col) {
    s += "\x1b[";
    appendInt(s, row);
    s += ';';
    appendInt(s, col);
    s += 'H';
}

inline void setColor(std::string& s, bool bg, int r, int g, int b) {
    s += bg ? "\x1b[48;2;" : "\x1b[38;2;";
    appendInt(s, r);
    s += ';';
    appendInt(s, g);
    s += ';';
    appendInt(s, b);
    s += 'm';
}
