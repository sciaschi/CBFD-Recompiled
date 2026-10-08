// Game Frame Rate: the game's own logic at up to 60 frames a second instead of 30.
//
// The game times everything in ticks of the N64's 60 Hz display (D_800BE9E4: the ticks since the
// last frame, set in func_10004F00), so it plays at the same speed at any rate. Its scheduler
// (func_10004DB0) starts a frame only once at least D_8003B239 ticks have passed, and raises that
// minimum to each frame's length so the pace stays even. On the N64 every frame took at least
// two ticks, so it settled at 2 (30 frames a second); here the game's work takes no time, and
// keeping the minimum at 1 lets it run a frame every tick. RT64 interpolates up to the display's
// rate from there.
//
// TEMP-DEBUG (testing): with CONKER_GAME_60FPS set.

#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <cmath>

#include "recomp.h"

#include "conker.hpp"

namespace {
    constexpr uint32_t minimum_ticks = 0x8003B239; // D_8003B239

    bool enabled() {
        static const bool on = std::getenv("CONKER_GAME_60FPS") != nullptr;
        return on;
    }
}

// func_10004FE0 (swap to the frame drawn, or wait a tick), after its first instruction: nonzero to
// swap now (the hook calls func_10005020 and returns).
extern "C" int conker_frame_rate_swap_now(uint8_t* rdram, recomp_context* ctx) {
    return enabled() ? 1 : 0;
}

// func_10004DB0 (the scheduler's frame start), after its first instruction.
extern "C" void conker_frame_rate_schedule(uint8_t* rdram, recomp_context* ctx) {
    // TEMP-DEBUG: the scheduler's state once a second.
    {
        static auto last = std::chrono::steady_clock::now();
        static unsigned calls = 0;
        calls++;
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(1)) {
            std::fprintf(stderr, "[frame_rate] calls/s %u ticks %u min %u smooth %u state %u frame_ticks %d\n", calls,
                MEM_BU(0, (gpr)(int32_t)0x8003B238), MEM_BU(0, (gpr)(int32_t)0x8003B239), MEM_BU(0, (gpr)(int32_t)0x8003B23A),
                MEM_BU(0, (gpr)(int32_t)0x8003A582), (int32_t)MEM_W(0, (gpr)(int32_t)0x800BE9E4));
            std::fflush(stderr);
            calls = 0;
            last = now;
        }
    }
    if (enabled() && MEM_BU(0, (gpr)(int32_t)minimum_ticks) > 1) {
        MEM_B(0, (gpr)(int32_t)minimum_ticks) = 1;
    }
}

// TEMP-DEBUG (Game Frame Rate: actions sped up at 60): with CONKER_ACTION_LOG set, each game frame
// logs every 32-bit word that changed in the game objects (gObjects[25]) and player 1's state
// (their camera's +0x3D4), next to the executable
// (action_log_<time>.txt), with the frame's ticks: a counter that steps by the same amount every
// frame at 60, where the rest step every other frame, is one that ignores the frame time.
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <SDL.h>

namespace {
    FILE* action_log() {
        static FILE* log = [] {
            if (std::getenv("CONKER_ACTION_LOG") == nullptr) {
                return (FILE*)nullptr;
            }
            const std::time_t now = std::time(nullptr);
            char name[64];
            std::strftime(name, sizeof(name), "action_log_%Y%m%d_%H%M%S.txt", std::localtime(&now));
            char* base = SDL_GetBasePath();
            const std::string path = std::string(base != nullptr ? base : "") + name;
            SDL_free(base);
            return std::fopen(path.c_str(), "w");
        }();
        return log;
    }

    struct Region {
        std::string tag;
        uint32_t address = 0;
        uint32_t size;
        std::vector<uint32_t> words;
    };

    void diff_region(uint8_t* rdram, FILE* log, Region& region, uint32_t address) {
        const uint32_t count = region.size / 4;
        std::vector<uint32_t> now(count);
        const bool valid = address >= 0x80000000u && address + region.size < 0x80800000u;
        for (uint32_t i = 0; valid && i < count; i++) {
            now[i] = (uint32_t)MEM_W(i * 4, (gpr)(int32_t)address);
        }
        if (address != region.address) {
            std::fprintf(log, " %s@%08X", region.tag.c_str(), address);
        }
        else if (valid) {
            for (uint32_t i = 0; i < count; i++) {
                if (now[i] != region.words[i]) {
                    float f;
                    std::memcpy(&f, &now[i], 4);
                    std::fprintf(log, " %s.%03X=%08X(%g)", region.tag.c_str(), i * 4, now[i], (std::fabs(f) < 1e7f && std::fabs(f) > 1e-7f) ? f : 0.0f);
                }
            }
        }
        region.address = address;
        region.words = now;
    }
}

void conker::frame_rate::log_game_frame(uint8_t* rdram) {
    FILE* log = action_log();
    if (log == nullptr) {
        return;
    }
    static unsigned frame = 0;
    static std::vector<Region> objects = [] {
        std::vector<Region> list;
        for (int i = 0; i < 25; i++) {
            list.push_back(Region{ "O" + std::to_string(i), 0, 0x32C, {} });
        }
        return list;
    }();
    static Region state{ "S", 0, 0x400, {} };
    const uint32_t camera = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
    std::fprintf(log, "%u ticks %d f30 %u", frame++, (int32_t)MEM_W(0, (gpr)(int32_t)0x800BEA08), MEM_BU(0, (gpr)(int32_t)0x800BE9A0));
    for (int i = 0; i < 25; i++) {
        diff_region(rdram, log, objects[i], 0x800CC2D0 + i * 0x32C);
    }
    if (camera >= 0x80000000u && camera < 0x80800000u) {
        diff_region(rdram, log, state, (uint32_t)MEM_W(0x3D4, (gpr)(int32_t)camera));
    }
    std::fprintf(log, "\n");
    std::fflush(log);
}

// TEMP-DEBUG (Game Frame Rate: things that move once a frame): with CONKER_MEMORY_SCAN set, every
// 32-bit word of RDRAM is compared each game frame, and a word that changes by the same step for
// scan_run frames in a row (a swinging door moved a set angle each frame) is logged next to the
// executable (memory_scan_<time>.txt) as its run starts and when it ends: the frame, its address,
// the step (as a float and as an integer) and the value.
namespace {
    constexpr uint32_t rdram_words = 0x800000 / 4;
    constexpr uint16_t scan_run = 20;

    FILE* scan_log() {
        static FILE* log = [] {
            if (std::getenv("CONKER_MEMORY_SCAN") == nullptr) {
                return (FILE*)nullptr;
            }
            const std::time_t now = std::time(nullptr);
            char name[64];
            std::strftime(name, sizeof(name), "memory_scan_%Y%m%d_%H%M%S.txt", std::localtime(&now));
            char* base = SDL_GetBasePath();
            const std::string path = std::string(base != nullptr ? base : "") + name;
            SDL_free(base);
            return std::fopen(path.c_str(), "w");
        }();
        return log;
    }

    float as_float(uint32_t word) {
        float f;
        std::memcpy(&f, &word, 4);
        return f;
    }

    // TEMP-DEBUG: the addresses watched for stores (claude-scratch/scripts/watch_stores.py adds a
    // conker_watch_store call after every float store in RecompiledFuncs/), and the stores seen
    // this frame (one per address and code address).
    uint32_t watched[2] = {};
    struct WatchHit {
        uint32_t address, pc;
    };
    WatchHit watch_hits[16];
    int watch_count = 0;

    // The memory around a run's word, written once per address: the doors behind Birdy's lever
    // (0x80153074 and 0x80153114, swinging 0.075 a frame each way) and what spins at 0x8011F800,
    // or any two words that start the same frame with opposite steps.
    void dump_around(FILE* log, const uint32_t* words, uint32_t index) {
        static std::vector<uint32_t> dumped;
        for (uint32_t d : dumped) {
            if (d == index) {
                return;
            }
        }
        if (dumped.size() >= 24) {
            return;
        }
        dumped.push_back(index);
        const uint32_t first = index >= 0x60 ? (index - 0x60) & ~3u : 0;
        std::fprintf(log, "dump around %08X:\n", 0x80000000u + index * 4);
        for (uint32_t i = first; i + 3 < rdram_words && i < first + 0xC0; i += 4) {
            std::fprintf(log, "  %08X: %08X %08X %08X %08X  (%g %g %g %g)\n", 0x80000000u + i * 4, words[i], words[i + 1], words[i + 2], words[i + 3],
                as_float(words[i]), as_float(words[i + 1]), as_float(words[i + 2]), as_float(words[i + 3]));
        }
    }
}

void conker::frame_rate::scan_game_frame(uint8_t* rdram) {
    FILE* log = scan_log();
    if (log == nullptr) {
        return;
    }
    static std::vector<uint32_t> last(rdram_words), step(rdram_words);
    static std::vector<uint16_t> run(rdram_words);
    static unsigned frame = 0;
    static bool primed = false;
    frame++;
    const uint32_t* words = reinterpret_cast<const uint32_t*>(rdram);
    // Runs that started this frame, for pairs with opposite steps.
    std::vector<std::pair<uint32_t, float>> started;
    for (uint32_t i = 0; i < rdram_words; i++) {
        // RDRAM is kept word-swapped: a word reads as it does on the N64.
        const uint32_t now = words[i];
        if (!primed) {
            last[i] = now;
            continue;
        }
        const uint32_t was = last[i];
        if (now == was) {
            if (run[i] >= scan_run) {
                std::fprintf(log, "%u end %08X after %u frames value %g (%08X)\n", frame, 0x80000000u + i * 4, run[i], as_float(now), now);
            }
            run[i] = 0;
            continue;
        }
        const float d = as_float(now) - as_float(was);
        uint32_t s;
        std::memcpy(&s, &d, 4);
        const bool is_float = std::isfinite(as_float(now)) && std::fabs(as_float(now)) < 1e6f && std::fabs(as_float(now)) > 1e-4f;
        const uint32_t key = is_float ? s : now - was;
        if (run[i] > 0 && key == step[i]) {
            if (run[i] < 0xFFFF) {
                run[i]++;
            }
            if (run[i] == scan_run) {
                std::fprintf(log, "%u start %08X step %g (int %d) value %g (%08X)\n", frame - scan_run, 0x80000000u + i * 4,
                    is_float ? d : 0.0f, (int32_t)(now - was), as_float(now), now);
                const uint32_t address = 0x80000000u + i * 4;
                if ((address >= 0x80153000u && address < 0x80153200u) || (address >= 0x8011F700u && address < 0x8011F900u)) {
                    dump_around(log, words, i);
                }
                if (is_float && d != 0.0f) {
                    started.emplace_back(i, d);
                }
            }
        }
        else {
            if (run[i] >= scan_run) {
                std::fprintf(log, "%u end %08X after %u frames value %g (%08X)\n", frame, 0x80000000u + i * 4, run[i], as_float(now), now);
            }
            run[i] = 1;
            step[i] = key;
        }
        last[i] = now;
    }
    for (size_t a = 0; a < started.size(); a++) {
        for (size_t b = a + 1; b < started.size(); b++) {
            if (started[a].second == -started[b].second && std::fabs(started[a].second) < 1.0f &&
                started[b].first - started[a].first < 0x400 / 4) {
                dump_around(log, words, started[a].first);
                dump_around(log, words, started[b].first);
                watched[0] = 0x80000000u + started[a].first * 4;
                watched[1] = 0x80000000u + started[b].first * 4;
                std::fprintf(log, "%u watching %08X and %08X\n", frame, watched[0], watched[1]);
            }
        }
    }
    for (int w = 0; w < watch_count; w++) {
        std::fprintf(log, "%u written %08X by code at %08X\n", frame, watch_hits[w].address, watch_hits[w].pc);
    }
    watch_count = 0;
    primed = true;
    std::fflush(log);
}

// TEMP-DEBUG: called after every float store in the instrumented RecompiledFuncs/ (see watched).
extern "C" void conker_watch_store(uint32_t address, uint32_t pc) {
    if (address != watched[0] && address != watched[1]) {
        return;
    }
    for (int i = 0; i < watch_count; i++) {
        if (watch_hits[i].address == address && watch_hits[i].pc == pc) {
            return;
        }
    }
    if (watch_count < 16) {
        watch_hits[watch_count++] = WatchHit{ address, pc };
    }
}

// Code that steps something once a frame without the frame's length plays twice as fast at 60. Each
// spot found is scaled by frame_scale(): the frame's length in 30-a-second frames (D_800BE9A4, the
// ticks over 2), 1 at 30, so the game's own rate is untouched.
namespace {
    float frame_scale(uint8_t* rdram) {
        uint32_t word = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800BE9A4);
        float scale;
        std::memcpy(&scale, &word, 4);
        return (scale > 0.0f && scale <= 4.0f) ? scale : 1.0f;
    }
}

// func_151189AC (swinging props, such as the doors Birdy's lever opens), at 0x15118B6C: the angle
// ($f6, degrees) is about to have the speed ($f16, degrees a frame) added. Taking speed x (1 - scale)
// off first makes it angle + speed x scale.
extern "C" void conker_frame_rate_prop_angle(uint8_t* rdram, recomp_context* ctx) {
    ctx->f6.fl -= ctx->f16.fl * (1.0f - frame_scale(rdram));
}

// func_151189AC, at 0x15118C44: the speed ($f16) is about to have the acceleration ($f18, from the
// prop's +0x80) added or taken off; scaled for the frame. $f18 isn't used after on this path.
extern "C" void conker_frame_rate_prop_speed(uint8_t* rdram, recomp_context* ctx) {
    ctx->f18.fl *= frame_scale(rdram);
}
