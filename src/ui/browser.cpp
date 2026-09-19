// browser.cpp — see include/asciiplayer/browser.h
#include "asciiplayer/browser.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "asciiplayer/ansi.h"
#include "asciiplayer/input.h"
#include "asciiplayer/terminal.h"

// ── Which files are worth listing ───────────────────────────────────────────
// Extension matching, not content sniffing: we only need a reasonable filter
// for the list, and ffmpeg gives the real answer when you press Enter.
static bool isVideo(const fs::path& p) {
    static const char* exts[] = {".mp4", ".mkv", ".avi", ".mov", ".webm", ".flv", ".wmv",
                                 ".m4v", ".mpg", ".mpeg", ".gif", ".ts", ".3gp", ".ogv"};
    std::string e = p.extension().string();
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    for (const char* x : exts)
        if (e == x) return true;
    return false;
}

struct Entry { std::string name; bool dir; };

static std::vector<Entry> listDir(const fs::path& dir) {
    std::vector<Entry> items, dirs, files;
    if (dir != dir.root_path()) items.push_back({"..", true});
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;          // hide dotfiles
        std::error_code ec2;
        if (it->is_directory(ec2)) dirs.push_back({name, true});
        else if (isVideo(it->path())) files.push_back({name, false});
    }
    auto byName = [](const Entry& a, const Entry& b) { return a.name < b.name; };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    items.insert(items.end(), dirs.begin(), dirs.end());
    items.insert(items.end(), files.begin(), files.end());
    return items;
}


std::string browse(fs::path& dir) {
    int sel = 0, top = 0;
    bool reload = true;
    std::string selectName;
    std::vector<Entry> items;

    while (!g_quit) {
        if (reload) {
            items = listDir(dir);
            sel = 0; top = 0;
            for (int i = 0; i < (int)items.size(); ++i)
                if (items[i].name == selectName) sel = i;
            selectName.clear();
            reload = false;
        }

        int cols, rows;
        termSize(cols, rows);
        int listH = std::max(1, rows - 4);
        sel = std::clamp(sel, 0, std::max(0, (int)items.size() - 1));
        if (sel < top) top = sel;
        if (sel >= top + listH) top = sel - listH + 1;

        std::string o = "\x1b[?2026h\x1b[0m\x1b[2J\x1b[H";
        o += "\x1b[1;38;2;120;200;255m ASCII RGB PLAYER \x1b[0m\x1b[38;2;150;150;150m  select a video\r\n";
        o += " " + dir.string() + "\x1b[0m\r\n\r\n";
        for (int i = top; i < std::min((int)items.size(), top + listH); ++i) {
            o += (i == sel) ? "\x1b[7m > " : "   ";
            if (items[i].dir) o += "\x1b[38;2;100;170;255m" + items[i].name + "/";
            else              o += "\x1b[38;2;235;235;235m" + items[i].name;
            o += "\x1b[0m\r\n";
        }
        if (items.empty()) o += "   (empty)\r\n";
        moveTo(o, rows, 1);
        o += "\x1b[38;2;150;150;150m up/down move   Enter open   left/Backspace parent   ~ home   q quit\x1b[0m";
        o += "\x1b[?2026l";
        writeAll(o);

        Key k = readKey(-1);   // blocks; a resize interrupts it and we just redraw
        switch (k.k) {
            case K::Up:   --sel; break;
            case K::Down: ++sel; break;
            case K::PgUp: sel -= listH; break;
            case K::PgDn: sel += listH; break;
            case K::Left:
            case K::Back:
                if (dir != dir.root_path()) {
                    selectName = dir.filename().string();
                    dir = dir.parent_path();
                    reload = true;
                }
                break;
            case K::Enter:
            case K::Right: {
                if (items.empty()) break;
                const Entry& e = items[sel];
                if (e.dir) {
                    if (e.name == "..") { selectName = dir.filename().string(); dir = dir.parent_path(); }
                    else dir /= e.name;
                    reload = true;
                } else {
                    return (dir / e.name).string();
                }
                break;
            }
            case K::Esc: return "";
            case K::Char:
                if (k.c == 'q' || k.c == 'Q') return "";
                if (k.c == '~') {
                    if (const char* h = std::getenv("HOME")) { dir = h; reload = true; }
                }
                break;
            default: break;
        }
    }
    return "";
}
