#pragma once

#include <chrono>
#include <string>

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point t);
std::string fmtTime(double t);
