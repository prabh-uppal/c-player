#pragma once

struct Glyph {
    char ch;
    float ink[8];
};

inline constexpr int kSubX = 2;
inline constexpr int kSubY = 4;
inline constexpr int kSubN = kSubX * kSubY;

extern const Glyph kGlyphs[];
extern const int kGlyphCount;
