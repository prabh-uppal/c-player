cp -r /mnt/e/c-player/include/asciiplayer /mnt/c/WINDOWS/system32/c++project/c-player/include/
cp /mnt/e/c-player/src/main.cpp /mnt/c/WINDOWS/system32/c++project/c-player/src/main.cpp
cd /mnt/c/WINDOWS/system32/c++project/c-player
make#pragma once

#include <string>

struct VideoInfo {
    int w = 0;
    int h = 0;
    double fps = 25.0;
    double duration = 0.0;
    bool audio = false;
    int audioIndex = -1;
};

VideoInfo probe(const std::string& path);
