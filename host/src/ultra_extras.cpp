// libultra functions the recompiled code calls that neither N64Recomp's output
// nor N64ModernRuntime provides.

#include <chrono>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <SDL.h>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "ultramodern/error_handling.hpp"
#include "ultramodern/ultra64.h"

#include "conker.hpp"

namespace {
    constexpr int32_t PFS_ERR_NOPACK = 1;
    constexpr int32_t PFS_ERR_DEVICE = 11;

    // Shared by osPiReadIo and osPiRawReadIo: read one word of cartridge ROM.
    // Rare's anti-piracy checks read ROM header words this way, so this must
    // return the real ROM contents.
    int32_t read_rom_word(uint8_t* rdram, uint32_t dev_addr, gpr data_ptr) {
        uint32_t physical_addr = (0xB0000000u | dev_addr) & 0x1FFFFFFFu;
        uint64_t rom_offset = (uint64_t)physical_addr - recomp::rom_base;
        if (physical_addr < recomp::rom_base || rom_offset + 4 > recomp::get_rom().size()) {
            std::fprintf(stderr, "[ultra_extras] PI read outside ROM: dev_addr=0x%08X\n", dev_addr);
            return -1;
        }
        recomp::do_rom_pio(rdram, data_ptr, physical_addr);
        return 0;
    }
}

// A pass of a busy-wait loop in the game (issue #66): func_10008CE8, which starts a
// song on a sequence player, stops the player and then counts up to 2,000,000 (then
// 4,000,000) while it waits for the audio thread to report it stopped. On the N64
// the audio thread preempts the loop; here game threads switch only when one waits
// or yields, so the loop ran out without the audio thread running, and the new song
// went to a player still playing the old one (the bar's music played on after
// loading a save from the menu the game over leads to). conker.toml calls this at
// the head of both loops: it yields for up to 1 ms, letting the audio thread run,
// and counts that as the passes the N64 would have made in the time (about 3,000;
// a pass is some 30 cycles at 93.75 MHz), so the loop still gives up after about as
// long as it would there. The count ($s0) is kept at or under the loop's bound
// ($s1), which both loops end on.
extern "C" void yield_self_1ms(uint8_t* rdram);
extern "C" void conker_spin_wait_pass(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t passes_per_ms = 3000;
    // TEMP-DEBUG (issues #66, #21): CONKER_SPIN_NO_YIELD turns this fix off, the loop as before it.
    static const bool off = std::getenv("CONKER_SPIN_NO_YIELD") != nullptr;
    if (off) {
        return;
    }
    yield_self_1ms(rdram);
    const uint32_t count = (uint32_t)ctx->r16;
    const uint32_t bound = (uint32_t)ctx->r17;
    ctx->r16 = (count < bound && bound - count > passes_per_ms) ? count + passes_per_ms : bound;
}

// A song just started still reads as stopped (issue #66). Starting a song (func_10008CE8) only queues
// an event for the audio thread, and the player says it's stopped (its state, +0x2C, AL_STOPPED)
// until the audio thread has handled it. On the N64 the audio thread runs before the game looks
// again; here it can run later, as threads switch only when one waits. The music manager
// (func_1000D2F8) asked the player a frame later (func_1000853C), found it stopped, took the song
// for finished and freed its channel while it played on: the next song went to that player as if it
// were free, the old one was never stopped (the wind outside the bar went on inside it, after
// skipping the walk in), and the mix-up carried on (the stone dragon's mouth faded the wrong player
// and the level's music played on, very loud). Waiting for the audio thread there doesn't work: it
// takes the song up only once the game goes on. So until it has, the player reads as playing:
// marked as just started when func_10008CE8 starts its song, and the mark cleared once the player
// plays, when the game stops it, or after 500 ms (should the song never start).
namespace {
    constexpr int sequence_players = 3; // D_8003C900
    std::chrono::steady_clock::time_point song_started_at[sequence_players];
    bool song_just_started[sequence_players] = {};
}

// func_10008CE8 at 0x10008EC4, just after it starts the song: its player number is its first
// argument, the byte at $sp + 0x43.
extern "C" void conker_song_started(uint8_t* rdram, recomp_context* ctx) {
    // TEMP-DEBUG (issue #66): CONKER_SPIN_NO_YIELD turns this off too.
    static const bool off = std::getenv("CONKER_SPIN_NO_YIELD") != nullptr;
    const uint32_t player = MEM_BU(0x43, ctx->r29);
    if (off || player >= sequence_players) {
        return;
    }
    song_just_started[player] = true;
    song_started_at[player] = std::chrono::steady_clock::now();
}

// func_10008F24 (stop a player) at its start: $a0 the player number.
extern "C" void conker_song_stopped(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t player = (uint32_t)ctx->r4 & 0xFF;
    if (player < sequence_players) {
        song_just_started[player] = false;
    }
}

// func_1000853C (a player's state) at 0x10008560, after reading it: $v0 the state, $a1 the player.
extern "C" void conker_song_state(uint8_t* rdram, recomp_context* ctx) {
    constexpr auto start_limit = std::chrono::milliseconds(500);
    const uint32_t player = (uint32_t)ctx->r5 & 0xFF;
    if (player >= sequence_players || !song_just_started[player]) {
        return;
    }
    if ((int32_t)ctx->r2 != 0 || std::chrono::steady_clock::now() - song_started_at[player] > start_limit) {
        song_just_started[player] = false;
        return;
    }
    ctx->r2 = 1; // AL_PLAYING
}

// TEMP-DEBUG (issues #66, #21: loud or missing music after a song change): with CONKER_MUSIC_LOG set,
// the game's music calls are logged next to the executable (a music_log_<time>_fix-on/off.txt per launch), with the time and the
// scene: starting a song on a sequence player (func_10008CE8: player, song, and how many passes its
// wait for the player to stop took, 2,000,000 or more being a timeout), and the calls that set a
// player's volume (func_10008EE0), stop it (func_10008F24), func_10008F58, func_10008BC0 (two floats)
// and func_10008B60.
namespace {
    FILE* music_log() {
        static FILE* log = [] {
            if (std::getenv("CONKER_MUSIC_LOG") == nullptr) {
                return (FILE*)nullptr;
            }
            // A file per launch, named for when and with the fix on or off, so no run overwrites another.
            const bool fix_off = std::getenv("CONKER_SPIN_NO_YIELD") != nullptr;
            const std::time_t now = std::time(nullptr);
            char name[64];
            std::strftime(name, sizeof(name), "music_log_%Y%m%d_%H%M%S", std::localtime(&now));
            char* base = SDL_GetBasePath();
            const std::string path = std::string(base != nullptr ? base : "") + name + (fix_off ? "_fix-off.txt" : "_fix-on.txt");
            SDL_free(base);
            FILE* f = std::fopen(path.c_str(), "w");
            if (f != nullptr) {
                std::fprintf(f, "wait fix %s\n", std::getenv("CONKER_SPIN_NO_YIELD") != nullptr ? "OFF" : "on");
            }
            return f;
        }();
        return log;
    }

    void music_line(uint8_t* rdram, const char* what, int player, const char* details) {
        FILE* log = music_log();
        if (log == nullptr) {
            return;
        }
        static const auto start = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::fprintf(log, "%9.3f  scene %02X  %-6s player %d  %s\n", seconds, (uint32_t)MEM_W(0, (gpr)(int32_t)0x800BE9F0),
            what, player, details);
        std::fflush(log);
    }
    uint32_t music_song = 0;
}

void conker_music_log_text(uint8_t* rdram, const char* text) {
    music_line(rdram, "note", -1, text);
}

// Each sequence player's state (+0x2C of D_8003C900[i]: 0 stopped, 1 playing, 2 stopping), logged
// when it changes; checked from the music manager's calls, every frame.
void conker_music_log_player_states(uint8_t* rdram) {
    static int32_t last[3] = { -1, -1, -1 };
    for (int i = 0; i < 3; i++) {
        const gpr player = (gpr)MEM_W(i * 4, (gpr)(int32_t)0x8003C900);
        const int32_t state = player != 0 ? MEM_W(0x2C, player) : -2;
        if (state != last[i]) {
            last[i] = state;
            char text[64];
            std::snprintf(text, sizeof(text), "player %d (%08X) state now %d", i, (uint32_t)player, state);
            music_line(rdram, "pstate", i, text);
        }
    }
}

extern "C" void conker_music_log_play(uint8_t* rdram, recomp_context* ctx) {
    music_song = (uint32_t)ctx->r5 & 0xFFFF;
}

extern "C" void conker_music_log_played(uint8_t* rdram, recomp_context* ctx) {
    char details[96];
    const uint32_t passes = (uint32_t)ctx->r16;
    std::snprintf(details, sizeof(details), "song %u  waited %u passes%s  (song loaded %u)", music_song, passes,
        passes >= 2000000 ? " TIMEOUT" : "", (uint32_t)MEM_HU(((uint32_t)MEM_BU(0x43, ctx->r29)) * 2, (gpr)(int32_t)0x8003CA3C));
    music_line(rdram, "play", (int)MEM_BU(0x43, ctx->r29), details);
}

extern "C" void conker_music_log_volume(uint8_t* rdram, recomp_context* ctx) {
    char details[48];
    std::snprintf(details, sizeof(details), "value %d (0x%X)", (int32_t)ctx->r5, (uint32_t)ctx->r5);
    music_line(rdram, "volume", (int)(ctx->r4 & 0xFF), details);
}

extern "C" void conker_music_log_stop(uint8_t* rdram, recomp_context* ctx) {
    music_line(rdram, "stop", (int)(ctx->r4 & 0xFF), "");
}

extern "C" void conker_music_log_f58(uint8_t* rdram, recomp_context* ctx) {
    music_line(rdram, "F58", (int)(ctx->r4 & 0xFF), "");
}

extern "C" void conker_music_log_bc0(uint8_t* rdram, recomp_context* ctx) {
    // The first argument is an integer, so the floats come in $a1 and $a2.
    char details[64];
    uint32_t words[2] = { (uint32_t)ctx->r5, (uint32_t)ctx->r6 };
    float values[2];
    std::memcpy(values, words, sizeof(values));
    std::snprintf(details, sizeof(details), "%.3f %.3f", values[0], values[1]);
    music_line(rdram, "BC0", (int)(ctx->r4 & 0xFF), details);
}

extern "C" void conker_music_log_b60(uint8_t* rdram, recomp_context* ctx) {
    char details[64];
    std::snprintf(details, sizeof(details), "%u %u %u %d", (uint32_t)ctx->r5 & 0xFF, (uint32_t)ctx->r6 & 0xFF,
        (uint32_t)ctx->r7 & 0xFF, (int32_t)MEM_W(0x10, ctx->r29));
    music_line(rdram, "B60", (int)(ctx->r4 & 0xFF), details);
}

extern "C" void conker_music_log_c6c(uint8_t* rdram, recomp_context* ctx) {
    char details[48];
    std::snprintf(details, sizeof(details), "arg %u", (uint32_t)ctx->r5 & 0xFF);
    music_line(rdram, "C6C", (int)(ctx->r4 & 0xFF), details);
}

extern "C" void conker_music_log_c04(uint8_t* rdram, recomp_context* ctx) {
    char details[48];
    std::snprintf(details, sizeof(details), "arg %u %d", (uint32_t)ctx->r5 & 0xFF, (int32_t)ctx->r6);
    music_line(rdram, "C04", (int)(ctx->r4 & 0xFF), details);
}

// func_1000D2F8, the music manager's update for one channel ($a0): its state (D_800417B0[channel]),
// the song it plays (+0x4), its pending request (+0x60, that request's song at +0x4) and +0x20/+0x24.
// Logged only when any of them changed since the channel's last call, as it runs every frame.
extern "C" void conker_music_log_manager(uint8_t* rdram, recomp_context* ctx) {
    if (music_log() == nullptr) {
        return;
    }
    conker_music_log_player_states(rdram);
    const uint32_t channel = (uint32_t)ctx->r4 & 0xFF;
    if (channel >= 8) {
        return;
    }
    const uint32_t state = (uint32_t)MEM_W(channel * 4, (gpr)(int32_t)0x800417B0);
    uint32_t now[6] = { state, 0, 0, 0, 0, 0 };
    if (state != 0) {
        const gpr base = (gpr)(int32_t)state;
        now[1] = (uint32_t)MEM_W(0x4, base);
        now[2] = (uint32_t)MEM_W(0x60, base);
        now[3] = now[2] != 0 ? (uint32_t)MEM_W(0x4, (gpr)(int32_t)now[2]) : 0;
        now[4] = (uint32_t)MEM_W(0x20, base);
        now[5] = (uint32_t)MEM_W(0x24, base);
    }
    static uint32_t last[8][6] = {};
    if (std::memcmp(last[channel], now, sizeof(now)) == 0) {
        return;
    }
    std::memcpy(last[channel], now, sizeof(now));
    char details[128];
    std::snprintf(details, sizeof(details), "state %08X  song %d  pending %08X (song %d)  +20 %d  +24 %d", now[0],
        (int32_t)now[1], now[2], (int32_t)now[3], (int32_t)now[4], (int32_t)now[5]);
    music_line(rdram, "manage", (int)channel, details);
}

// Reads a word through a KSEG1 (uncached) address, for game code that reads
// cartridge ROM or RDRAM that way directly. The runtime maps only KSEG0 RDRAM,
// so conker.toml replaces those loads with a hook that calls this.
extern "C" int32_t conker_kseg1_read32(uint8_t* rdram, uint32_t vaddr) {
    uint32_t physical_addr = vaddr & 0x1FFFFFFFu;
    if (physical_addr >= recomp::rom_base) {
        auto rom = recomp::get_rom();
        uint64_t offset = (uint64_t)physical_addr - recomp::rom_base;
        if (offset + 4 > rom.size()) {
            std::fprintf(stderr, "[ultra_extras] KSEG1 read past the end of ROM: 0x%08X\n", vaddr);
            return 0;
        }
        return (int32_t)(((uint32_t)rom[offset] << 24) | ((uint32_t)rom[offset + 1] << 16) |
                         ((uint32_t)rom[offset + 2] << 8) | (uint32_t)rom[offset + 3]);
    }
    if (physical_addr < 0x00800000u) {
        return MEM_W(0, (gpr)(int32_t)(0x80000000u | physical_addr));
    }
    std::fprintf(stderr, "[ultra_extras] unhandled KSEG1 read: 0x%08X\n", vaddr);
    return 0;
}

// Where the script interpreter (func_150ADAF0) returns to when a script aborts
// (func_150AE280); see conker.toml. Only the game's main thread runs scripts.
extern "C" {
    jmp_buf conker_interpreter_exit;
}

// libultra's osContInit creates __osEepromTimerQ, which libultra's EEPROM
// functions and Rare's EEPROM code (func_151DCFD8) put their timer messages on.
// The runtime's osContInit doesn't, so conker.toml calls this after the game's
// osContInit. Like osContInit, only the first call creates it.
extern "C" void conker_create_eeprom_timer_queue(uint8_t* rdram) {
    constexpr int32_t osEepromTimerQ = 0x80042A78;
    constexpr int32_t osEepromTimerMsg = 0x80042A90;
    OSMesgQueue* queue = TO_PTR(OSMesgQueue, osEepromTimerQ);
    if (queue->msgCount == 0) {
        osCreateMesgQueue(rdram, osEepromTimerQ, osEepromTimerMsg, 1);
    }
}

// s32 osPiRawReadIo(u32 devAddr, u32 *data)
extern "C" void osPiRawReadIo_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = read_rom_word(rdram, (uint32_t)ctx->r4, ctx->r5);
}

// s32 osPiReadIo(u32 devAddr, u32 *data): osPiRawReadIo under the PI access lock,
// which the runtime doesn't need.
extern "C" void osPiReadIo_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = read_rom_word(rdram, (uint32_t)ctx->r4, ctx->r5);
}

// Conker halts on fatal errors with a bare `syscall` (func_10007DA0,
// func_150AD770), called from e.g. the memory allocator and an anti-piracy
// check. Report where it came from and stop.
extern "C" void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t instruction_vram) {
    char msg[160];
    std::snprintf(msg, sizeof(msg), "The game halted: syscall at 0x%08X, return address 0x%08X",
        (uint32_t)instruction_vram, (uint32_t)ctx->r31);
    std::fprintf(stderr, "[ultra_extras] %s\n", msg);
    ultramodern::error_handling::message_box(msg);
    ULTRAMODERN_QUICK_EXIT();
}

// s32 osPfsInit(OSMesgQueue *mq, OSPfs *pfs, int channel)
// Conker only calls this to find out what kind of pak is attached
// (func_15006234): PFS_ERR_ID_FATAL or PFS_ERR_DEVICE means "not a Controller
// Pak", and it then tries osMotorInit for a Rumble Pak. The runtime has no
// Controller Pak support, so report either a non-Controller-Pak device or no pak.
extern "C" void osPfsInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    int channel = (int)ctx->r6;
    auto info = conker::get_connected_device_info(channel);
    bool has_pak = info.connected_device == ultramodern::input::Device::Controller &&
                   info.connected_pak != ultramodern::input::Pak::None;
    ctx->r2 = has_pak ? PFS_ERR_DEVICE : PFS_ERR_NOPACK;
}
