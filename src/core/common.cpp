// common.cpp — see include/asciiplayer/common.h
#include "asciiplayer/common.h"

#include <cstdio>

double secondsSince(Clock::time_point t) {
    // duration<double> means "seconds as a double"; .count() unwraps it.
    return std::chrono::duration<double>(Clock::now() - t).count();
}

std::string fmtTime(double t) {
    if (t < 0) t = 0;
    int s = (int)t;
    char b[32];
    // Only show hours when there are hours to show — "01:23" reads better
    // than "00:01:23" for a two-minute clip.
    if (s >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else           snprintf(b, sizeof b, "%02d:%02d", s / 60, s % 60);
    return b;
}
