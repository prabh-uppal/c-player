#pragma once

#include <string>
#include <sys/types.h>

pid_t startAudio(const std::string& path, int stream, double t, int& errFd);
void stopAudio(pid_t& pid, int& errFd);
