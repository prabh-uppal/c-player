#pragma once

#include <cstdio>
#include <string>
#include <vector>
#include <sys/types.h>

#include "asciiplayer/common.h"
#include "asciiplayer/input.h"
#include "asciiplayer/modes.h"
#include "asciiplayer/probe.h"

class Player {
public:
    Player();
    ~Player();

    std::string path;
    void run();

private:
    VideoInfo vi;
    Mode mode = Mode::Color;
    int ramp = 0;
    Style style = Style::Rich;
    bool paused = false;
    bool muted = false;
    bool audioOk = false;

    FILE* dec = nullptr;
    pid_t apid = -1;
    int afd = -1;
    std::string abuf;
    double audioBase = 0.0;
    bool audioClock = false;
    Clock::time_point audioSpawned;
    Clock::time_point lastDraw;

    int cols = 80;
    int rows = 24;
    int pw = 0;
    int ph = 0;
    int cellW = 0;
    int cellH = 0;
    int offX = 0;
    int offY = 0;
    int lastCols = -1;
    int lastRows = -1;
    Mode lastMode = Mode::Blocks;

    std::vector<unsigned char> frame;
    std::vector<int> drawn;
    std::vector<int> drawnSaved;
    int tol = 0;
    size_t budget = kFrameBudgetStart;
    double startPos = 0.0;
    long framesRead = 0;
    long totalFrames = 0;
    bool decoderEof = false;
    std::string errLog;
    Clock::time_point t0;
    std::string out;
    double fps = 25.0;

    double position() const { return startPos + framesRead / fps; }
    bool ramped() const { return mode == Mode::Color || mode == Mode::Mono; }
    void closeDecoder();
    void layout();
    void openAt(double t);
    void startSound(double t);
    void stopSound();
    bool waitingForSound() const;
    void syncToAudio();
    bool readFrame();
    bool stoppedEarly() const;
    std::string shortPath() const;
    std::string decodeError() const;
    void drawStatus();
    void render();
    void refreshStatus();
    void showCurrentFrame();
    void seek(double delta);
    void togglePause();
    bool handleKey(const Key& k);
};
