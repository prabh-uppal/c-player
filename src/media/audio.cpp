// audio.cpp — see include/asciiplayer/audio.h
#include "asciiplayer/audio.h"

#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "asciiplayer/shell.h"

pid_t startAudio(const std::string& path, int stream, double t, int& errFd) {
    errFd = -1;
    int fds[2];
    if (pipe(fds) != 0) return -1;
    char ts[32];
    snprintf(ts, sizeof ts, "%.3f", t);
    // The first line is a watchdog: if the player itself dies (kill -9, crash),
    // it takes the whole audio group down instead of leaving sound playing.
    std::string cmd =
        "(while kill -0 $PPID 2>/dev/null; do sleep 0.5; done; kill -TERM 0) & "
        "ffmpeg -nostdin -loglevel quiet -ss " + std::string(ts) + " -i " + shq(path) +
        " -map 0:" + std::to_string(stream) +
        " -vn -sn -af aresample=async=1:first_pts=0 -f s16le -ac 2 -ar 48000 - 2>/dev/null"
        " | ffplay -nodisp -autoexit -loglevel quiet -stats"
        " -f s16le -sample_rate 48000 -ch_layout stereo -i -";

    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);                        // one group: stopAudio kills sh, ffmpeg and ffplay
        int dn = open("/dev/null", O_RDWR);   // keep the pipeline away from our terminal
        dup2(dn, 0); dup2(dn, 1); dup2(fds[1], 2);
        // Drop every inherited descriptor - above all the read end of the video
        // pipe. If ffplay keeps that open, ffmpeg never sees a broken pipe when
        // we close our end, blocks forever on a full pipe, and pclose() hangs.
        for (int fd = 3; fd < 1024; ++fd) close(fd);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) { close(fds[0]); return -1; }
    setpgid(pid, pid);                        // also here, whichever runs first
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    errFd = fds[0];
    return pid;
}

void stopAudio(pid_t& pid, int& errFd) {
    if (pid > 0) {
        kill(-pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        pid = -1;
    }
    if (errFd >= 0) { close(errFd); errFd = -1; }
}

