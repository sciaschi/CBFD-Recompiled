// conker.log: when what the game prints would go nowhere (on Windows started from Explorer, or from a desktop or
// Finder elsewhere: no terminal, no file, no pipe), stdout and stderr go to a file instead, so a player can send it.
// Started from a terminal, or redirected, nothing changes. The streams themselves are reopened on the file (no thread,
// no pipe: what a crash handler prints as the process dies is in it too). Each start keeps the last run's as
// conker.prev.log and begins a new one, and a run keeps at most log_most_bytes (a loop printing every frame mustn't
// fill the disk): checked as the frontend updates (check()), then the rest goes nowhere.

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "conker.hpp"

#if defined(_WIN32)
#include <io.h>
#define log_dup2 _dup2
#define log_fileno _fileno
#else
#include <unistd.h>
#define log_dup2 dup2
#define log_fileno fileno
#endif

namespace {
#if defined(_WIN32)
    const std::filesystem::path nowhere = "NUL";
#else
    const std::filesystem::path nowhere = "/dev/null";
#endif
    constexpr uintmax_t log_most_bytes = 10ull * 1024 * 1024;
    std::filesystem::path log_path;
    bool logging = false;
    int checks = 0;

    // (On Windows not _wfreopen_s: it opens the file for this stream alone.)
    bool reopen(const std::filesystem::path& path, const char* mode, std::FILE* stream) {
#if defined(_WIN32)
        const std::wstring wide_mode(mode, mode + std::char_traits<char>::length(mode));
#pragma warning(suppress: 4996)
        return _wfreopen(path.c_str(), wide_mode.c_str(), stream) != nullptr;
#else
        return std::freopen(path.c_str(), mode, stream) != nullptr;
#endif
    }

    // A file of its own in `folder` (another copy of the game started from it has its own: on Windows, its log open
    // can't be moved aside, and this one goes elsewhere).
    bool open_in(const std::filesystem::path& folder) {
        std::error_code error;
        const std::filesystem::path path = folder / "conker.log";
        const std::filesystem::path previous = folder / "conker.prev.log";
        if (std::filesystem::exists(path, error)) {
            std::filesystem::remove(previous, error);
            std::filesystem::rename(path, previous, error);
            if (error) return false;
        }
        // Begun empty, then both streams appending to it (each write at its end, so they don't overwrite each other).
        {
            std::FILE* created = nullptr;
#if defined(_WIN32)
            created = _wfopen(path.c_str(), L"wb");
#else
            created = std::fopen(path.c_str(), "wb");
#endif
            if (created == nullptr) return false;
            std::fclose(created);
        }
        std::fflush(stdout);
        std::fflush(stderr);
        if (!reopen(path, "ab", stdout)) {
            reopen(nowhere, "wb", stdout);
            return false;
        }
        // stderr on the same open file as stdout (its descriptor made stdout's), so the two share one position: on
        // Windows two handles appending each seek to the end then write, and lines printed at once overwrote each
        // other.
        reopen(nowhere, "wb", stderr);
        log_dup2(log_fileno(stdout), log_fileno(stderr));
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        log_path = path;
        return true;
    }
}

void conker::log_file::start(const std::filesystem::path& folder, const std::filesystem::path& otherwise) {
    logging = open_in(folder) || open_in(otherwise);
}

void conker::log_file::check() {
    if (!logging || ++checks % 300 != 0) return;
    std::error_code error;
    if (std::filesystem::file_size(log_path, error) <= log_most_bytes || error) return;
    std::fputs("\n[log] this run's log reached 10 MB: the rest isn't kept\n", stdout);
    reopen(nowhere, "wb", stdout);
    reopen(nowhere, "wb", stderr);
    logging = false;
}
