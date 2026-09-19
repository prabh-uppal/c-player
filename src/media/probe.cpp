// probe.cpp — see include/asciiplayer/probe.h
//
// ffprobe prints lines like "width=1920". We ask for exactly the fields we
// need with -show_entries so the parsing stays this simple.
#include "asciiplayer/probe.h"

#include <cstdio>
#include <cstdlib>

#include "asciiplayer/shell.h"

VideoInfo probe(const std::string& path) {
    VideoInfo vi;
    std::string out = runCmd(
        "ffprobe -v error -select_streams v:0 "
        "-show_entries stream=width,height,r_frame_rate:format=duration "
        "-of default=noprint_wrappers=1 " + shq(path) + " 2>/dev/null");

    size_t pos = 0;
    while (pos < out.size()) {
        size_t e = out.find('\n', pos);
        if (e == std::string::npos) e = out.size();
        std::string line = out.substr(pos, e - pos);
        pos = e + 1;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "width") vi.w = std::atoi(v.c_str());
        else if (k == "height") vi.h = std::atoi(v.c_str());
        else if (k == "duration") vi.duration = std::atof(v.c_str());
        else if (k == "r_frame_rate") {
            int n = 0, d = 1;
            if (std::sscanf(v.c_str(), "%d/%d", &n, &d) == 2 && n > 0 && d > 0)
                vi.fps = (double)n / d;
        }
    }
    if (vi.fps < 1 || vi.fps > 120) vi.fps = 25.0;

    // "index,default" per audio stream, e.g. "1,1\n2,0" for Hindi (default) + English.
    std::string a = runCmd("ffprobe -v error -select_streams a -show_entries stream=index"
                           ":stream_disposition=default -of csv=p=0 " + shq(path) + " 2>/dev/null");
    int idx = -1, def = 0, n = 0;
    for (const char* q = a.c_str(); std::sscanf(q, "%d,%d%n", &idx, &def, &n) == 2; q += n) {
        if (vi.audioIndex < 0 || def == 1) vi.audioIndex = idx;
        if (def == 1) break;
    }
    vi.audio = vi.audioIndex >= 0;
    return vi;
}
