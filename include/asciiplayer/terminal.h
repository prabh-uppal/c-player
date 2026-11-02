#pragma once

#include <csignal>
#include <string>

extern volatile sig_atomic_t g_quit;
extern volatile sig_atomic_t g_resized;

void writeAll(const std::string& s);
void restoreTerminal();
void enableRaw();
void termSize(int& cols, int& rows);
void onSignal(int sig);
void installSignalHandlers();
