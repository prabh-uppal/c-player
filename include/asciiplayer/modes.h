#pragma once

#include <cstddef>

enum class Mode { Color, Mono, Shapes, Blocks };
enum class Style { Rich, Lift, Exact };

struct Ramp {
    const char* name;
    const char* chars;
};

extern const Ramp kRamps[];
extern const int kRampCount;

inline constexpr int kQuant = 4;
inline constexpr size_t kFrameBudgetStart = 180000;
inline constexpr size_t kFrameBudgetMin = 40000;
inline constexpr size_t kFrameBudgetMax = 400000;
inline constexpr int kMaxTol = 64;
inline constexpr double kRampFps = 20.0;
inline constexpr int kRampMaxSkip = 5;

const char* modeName(Mode m);
const char* styleName(Style s);
