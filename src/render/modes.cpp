// modes.cpp — see include/asciiplayer/modes.h
//
// Just the tables and the two name lookups. Kept out of the header so the
// ramp strings exist exactly once in the program.
#include "asciiplayer/modes.h"

const char* modeName(Mode m) {
    switch (m) {
        case Mode::Color:  return "RGB ASCII";
        case Mode::Mono:   return "MONO ASCII";
        case Mode::Shapes: return "RGB SHAPES";
        case Mode::Blocks: return "RGB BLOCKS";
    }
    return "";
}

const char* styleName(Style s) {
    return s == Style::Rich ? "rich" : s == Style::Lift ? "lift" : "exact";
}

// Dark -> bright. "classic" is the one everybody recognises; "detailed" has
// the finest tonal steps; "letters" looks like text from a distance.
const Ramp kRamps[] = {
    { "classic",  " .:-=+*#%@" },
    { "detailed", " .`^\\,:;Il!i><~+_-?][}{1)(|/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$" },
    { "letters",  ".,:;irsXA253hMHGS#9B&@" },
};
const int kRampCount = (int)(sizeof(kRamps) / sizeof(kRamps[0]));
