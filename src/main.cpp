// ============================================================================
//  main.cpp — start-up, argument handling, and the browse -> play -> browse loop
//
//  This is the shortest file in the project on purpose. All it does is:
//    1. refuse to run outside a terminal, or without ffmpeg
//    2. install signal handlers
//    3. handle the one special flag, -g (relaunch inside Ghostty)
//    4. enter raw mode, then loop: pick a file, play it, come back
// ============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "asciiplayer/browser.h"
#include "asciiplayer/player.h"
#include "asciiplayer/shell.h"
#include "asciiplayer/terminal.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::fprintf(stderr, "Please run asciiplayer inside a terminal.\n");
        return 1;
    }
    if (!haveTool("ffmpeg") || !haveTo. l("ffprobe")) {
        std::fprintf(stderr,
            "ffmpeg/ffprobe not found.\n"
            "  Ubuntu/Debian: sudo apt install ffmpeg\n"
            "  Arch:          sudo pacman -S ffmpeg\n"
            "  macOS:         brew install ffmpeg\n");
        return 1;
    }

    installSignalHandlers();

    // -g: play in Ghostty instead. Terminal.app draws on the CPU and saturates
    // (~150% CPU) repainting a full-screen 5 pt grid 20 times a second, so it
    // shows fewer frames than we send; Ghostty draws on the GPU and does the
    // same at ~30%. Opens a maximised 5 pt Ghostty window running this player.
    if (argc > 1 && (std::strcmp(argv[1], "-g") == 0 || std::strcmp(argv[1], "--ghostty") == 0)) {
        std::error_code ec;
        fs::path self = fs::canonical(argv[0], ec);
        if (ec) self = fs::absolute(argv[0]);
        int pt = 5;
        if (const char* e = std::getenv("ASCIIPLAYER_ZOOM")) if (std::atoi(e) > 0) pt = std::atoi(e);
        std::string cmd = "open -na Ghostty --args --font-size=" + std::to_string(pt) +
            " --maximize=true --window-save-state=never --quit-after-last-window-closed=true"
            " --working-directory=" + shq(fs::current_path().string()) +
            " -e " + shq(self.string());
        if (argc > 2) cmd += " " + shq(fs::absolute(argv[2]).string());
        if (std::system((cmd + " 2>/dev/null").c_str()) != 0) {
            std::fprintf(stderr, "Could not open Ghostty. Install it from https://ghostty.org\n");
            return 1;
        }
        return 0;
    }

    fs::path dir = fs::current_path();
    std::string file = argc > 1 ? argv[1] : "";
    if (!file.empty()) {
        std::error_code ec;
        fs::path p = fs::absolute(file, ec);
        if (!ec) { file = p.string(); dir = p.parent_path(); }
    }

    enableRaw();
    std::atexit(restoreTerminal);

    while (!g_quit) {
        if (file.empty()) file = browse(dir);
        if (file.empty()) break;
        {
            Player player;
            player.path = file;
            player.run();
        }
        file.clear();                // back to the browser after playback
    }

    restoreTerminal();
    return 0;
}
