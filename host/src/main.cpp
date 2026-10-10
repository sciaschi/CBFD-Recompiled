// Host application for Conker's Bad Fur Day (US), recompiled with N64Recomp.
//
// With RT64 (CONKER_RT64) it opens RecompFrontend's launcher and menus with
// remappable keyboard/controller input (frontend.cpp) and plays sound
// (audio_output.cpp); otherwise, or with --headless, it runs with a null renderer,
// no input and no sound output.
// Usage: ConkerRecomp [--rom <baserom.us.z64>] [--seconds N] [--headless] [--window WxH]
//   --rom PATH   the US ROM (a bare path works too, e.g. a ROM dropped onto the exe);
//                only needed once, it is then kept with the game's data. The window
//                build can also load it from the launcher.
//   --seconds N  start the game right away (no launcher) and quit after N seconds
//   --headless   null renderer, no window, input or sound
//   --window WxH open the window at this size, e.g. 2520x1080 to try a 21:9 screen
//                (the window mode, windowed or fullscreen, is still the setting's)

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultramodern.hpp"

#include "conker.hpp"

#if defined(CONKER_RT64)
#include "nfd.h"
#include "recompui/program_config.h"
#include "util/file.h"
#endif

// The game's RDRAM, for reporting fault addresses as N64 addresses.
static uint8_t* crash_rdram = nullptr;

#if defined(__linux__) || defined(__APPLE__)
#include <csignal>
#include <execinfo.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

// Debugging aid: report where a crash happened (the recompiled functions are
// named after their vram, so the backtrace maps straight back to game code).
static void crash_handler(int sig, siginfo_t* info, void*) {
    char buf[96];
    int n = std::snprintf(buf, sizeof(buf), "[host] signal %d at address %p\n", sig, info->si_addr);
    write(STDERR_FILENO, buf, n);
    void* frames[48];
    int count = backtrace(frames, 48);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    _exit(128 + sig);
}

static void install_crash_handler() {
    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}
#elif defined(_WIN32)
#include <Windows.h>
#include <DbgHelp.h>
#include <atomic>
#include <csignal>
#include <cstdarg>
#include <exception>

// Debugging aid: report where the game died (the recompiled functions are named
// after their vram, and their source lines give the MIPS instruction), using the
// PDB next to the exe.
using CrashReport = std::string;

static void crash_add(CrashReport& report, const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    report += line;
}

static void crash_add_stack(CrashReport& report, void* const* frames, USHORT count) {
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, nullptr, TRUE);
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    for (USHORT i = 0; i < count; i++) {
        DWORD64 offset = 0;
        if (SymFromAddr(process, (DWORD64)frames[i], &offset, symbol)) {
            // The source line: in RecompiledFuncs/ the line's comment gives the MIPS instruction.
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD displacement = 0;
            if (SymGetLineFromAddr64(process, (DWORD64)frames[i], &displacement, &line)) {
                crash_add(report, "  %s+0x%llx (%s:%lu)\n", symbol->Name, (unsigned long long)offset,
                    std::filesystem::path(line.FileName).filename().string().c_str(), line.LineNumber);
            }
            else {
                crash_add(report, "  %s+0x%llx\n", symbol->Name, (unsigned long long)offset);
            }
        }
        else {
            crash_add(report, "  %p\n", frames[i]);
        }
    }
}

// The console window closes with the process, so also keep the report in
// crash.log next to the executable, and show it with where it was saved.
static void crash_publish(const CrashReport& report) {
    std::fputs(report.c_str(), stderr);
    std::fflush(stderr);
    wchar_t exe[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path log_path = (length > 0 && length < MAX_PATH)
        ? std::filesystem::path(exe).parent_path() / "crash.log" : std::filesystem::path("crash.log");
    if (FILE* log = _wfopen(log_path.c_str(), L"a")) {
        std::fputs(report.c_str(), log);
        std::fputs("\n", log);
        std::fclose(log);
    }
    std::string message = "Conker's Bad Fur Day (recompiled) crashed. The report below was saved to " +
        log_path.string() + ".\n\n" + report;
    MessageBoxA(nullptr, message.c_str(), "Conker's Bad Fur Day (recompiled)", MB_OK | MB_ICONERROR);
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    CrashReport report;
    crash_add(report, "[host] exception 0x%08lX at %p\n", code, info->ExceptionRecord->ExceptionAddress);
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        // The faulting address, and the N64 address it stands for (rdram is the KSEG0 base).
        uintptr_t fault = (uintptr_t)info->ExceptionRecord->ExceptionInformation[1];
        crash_add(report, "[host] %s %p", info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", (void*)fault);
        if (crash_rdram != nullptr) {
            crash_add(report, " (N64 address 0x%08X)", (uint32_t)(fault - (uintptr_t)crash_rdram + 0x80000000u));
        }
        crash_add(report, "\n");
    }
    // The first frame is the faulting instruction itself.
    void* frames[32];
    frames[0] = info->ExceptionRecord->ExceptionAddress;
    USHORT count = 1 + CaptureStackBackTrace(0, 31, frames + 1, nullptr);
    crash_add_stack(report, frames, count);
    crash_publish(report);
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort() (the runtime's fatal errors, e.g. switch_error, and std::terminate) ends
// the process with a fast fail that skips the exception filter, so report it here.
// The runtime prints its reason to the console just before; the stack shows where.
static std::atomic<bool> crash_reported{ false };

static void abort_handler(int) {
    if (crash_reported.exchange(true)) {
        return;
    }
    CrashReport report;
    crash_add(report, "[host] abort() called (the console output just before it gives the reason)\n");
    void* frames[40];
    USHORT count = CaptureStackBackTrace(1, 40, frames, nullptr);
    crash_add_stack(report, frames, count);
    crash_publish(report);
}

// An exception nothing caught (in any thread): name it, then abort as usual.
static void on_terminate() {
    CrashReport report;
    crash_add(report, "[host] uncaught C++ exception");
    if (std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        }
        catch (const std::exception& e) {
            crash_add(report, ": %s", e.what());
        }
        catch (...) {
        }
    }
    crash_add(report, "\n");
    void* frames[40];
    USHORT count = CaptureStackBackTrace(1, 40, frames, nullptr);
    crash_add_stack(report, frames, count);
    if (!crash_reported.exchange(true)) {
        crash_publish(report);
    }
    std::abort();
}

static void install_crash_handler() {
    // Only exceptions nothing else handles: libraries like DXC raise and catch their own.
    SetUnhandledExceptionFilter(crash_handler);
    std::signal(SIGABRT, abort_handler);
    std::set_terminate(on_terminate);
    // No "abort() has been called" dialog or error report on top of ours.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}
#else
static void install_crash_handler() {}
#endif

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
// Conker's audio microcode, recompiled by RSPRecomp (recomp/audio_ucode.toml).
RspExitReason conker_audio_ucode(uint8_t* rdram, uint32_t ucode_addr);

namespace {
    const std::u8string game_id = u8"conker.n64.us.1.0";

    std::atomic<uint32_t> vi_count{0};

    // Conker runs with Status.FR set: 32 independent FPRs, which the hand-written
    // math code relies on. The boot code's own Status write is patched out in
    // conker.toml, so put every context into FR=1 mode here. cop0_status_write
    // also points ctx->f_odd at the odd registers themselves, which the
    // recompiled lwc1/mtc1 to odd FPRs go through.
    void set_fr_mode(recomp_context* ctx) {
        constexpr uint32_t STATUS_FR = 0x04000000;
        cop0_status_write(ctx, ctx->status_reg | STATUS_FR);
    }

    void on_thread_create(uint8_t*, recomp_context* ctx) {
        set_fr_mode(ctx);
    }

    void on_init(uint8_t* rdram, recomp_context* ctx) {
        crash_rdram = rdram;
        // The ROM is loaded by now and the game hasn't read it yet.
        conker::roms::fix_data();
        set_fr_mode(ctx);
        conker::register_tlb_mapped_code();
        conker::map_tlb_code_pages(rdram);

        // osCicId, normally left by IPL3; librecomp's init doesn't set it. Conker's
        // idle thread only starts the main thread if it reads 6105 (CIC-NUS-6105).
        constexpr int32_t osCicId = 0x80000310;
        MEM_W(osCicId, 0) = 6105;

        // Game code reads libultra's __osRunningThread directly (e.g. func_10004514
        // reads its id); have the runtime keep it pointing at the running thread.
        constexpr int32_t osRunningThread = 0x8002BE00;
        ultramodern::set_running_thread_variable(osRunningThread);
    }

    RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
        if (task->t.type == M_AUDTASK) {
            return conker_audio_ucode;
        }
        std::fprintf(stderr, "[host] no RSP microcode for task type %u\n", (unsigned)task->t.type);
        return nullptr;
    }

    void queue_samples(int16_t*, size_t) {}
    size_t get_frames_remaining() { return 0; }
    void set_frequency(uint32_t) {}

    bool headless = true;

    void poll_input() {}
    bool get_input(int, uint16_t*, float*, float*) { return false; }
    void set_rumble(int, bool) {}

    ultramodern::renderer::WindowHandle create_window(void*) {
        return ultramodern::renderer::WindowHandle{};
    }

    void vi_callback() {
        ++vi_count;
#if defined(CONKER_RT64)
        if (!headless) {
            conker::frontend::on_vi();
        }
#endif
    }

    void message_box(const char* msg) {
        std::fprintf(stderr, "[host] %s\n", msg);
    }
}

ultramodern::input::connected_device_info_t conker::get_connected_device_info(int controller_num) {
#if defined(CONKER_RT64)
    if (!headless) {
        return conker::frontend::get_connected_device_info(controller_num);
    }
#endif
    if (controller_num == 0) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

namespace {
    // conker_data/ (the stored ROM and saves) lives next to the executable, so it's
    // found however the game is started: double-click, shortcut or another directory.
    std::filesystem::path exe_directory(const char* argv0) {
#if defined(_WIN32)
        wchar_t buffer[MAX_PATH];
        DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            return std::filesystem::path(buffer).parent_path();
        }
#elif defined(__linux__)
        std::error_code error;
        std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", error);
        if (!error) {
            return exe.parent_path();
        }
#elif defined(__APPLE__)
        char buffer[4096];
        uint32_t size = sizeof(buffer);
        if (_NSGetExecutablePath(buffer, &size) == 0) {
            std::error_code error;
            std::filesystem::path exe = std::filesystem::canonical(buffer, error);
            if (!error) {
                return exe.parent_path();
            }
        }
#endif
        return std::filesystem::absolute(argv0).parent_path();
    }
}

const char* conker::rom_error_text(recomp::RomValidationError error) {
    switch (error) {
        case recomp::RomValidationError::FailedToOpen:
            return "The file couldn't be opened.";
        case recomp::RomValidationError::NotARom:
            return "The file isn't an N64 ROM.";
        case recomp::RomValidationError::IncorrectRom:
            return "This isn't the US version of Conker's Bad Fur Day, the only one supported.";
        case recomp::RomValidationError::IncorrectVersion:
            return "This is Conker's Bad Fur Day, but not the US version, or a ROM hack that changes the "
                "game's code (hacks that only change its assets, such as its audio or textures, work).";
        default:
            return "The ROM couldn't be loaded.";
    }
}

namespace {
    std::string path_text(const std::filesystem::path& path) {
        std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    // A ROM from the command line is stored with the game's data; without one, an
    // earlier run's stored copy is used. The window build's launcher can also load
    // one, so there it's only required when starting the game directly.
    bool select_rom(const std::filesystem::path& rom_path, bool required) {
        if (!rom_path.empty()) {
            recomp::RomValidationError result = recomp::select_rom(rom_path, game_id);
            if (result != recomp::RomValidationError::Good) {
                std::fprintf(stderr, "[host] %s: %s\n", path_text(rom_path).c_str(), conker::rom_error_text(result));
                return false;
            }
            return true;
        }
        recomp::check_all_stored_roms();
        if (!required || recomp::is_rom_valid(game_id)) {
            return true;
        }
        std::fprintf(stderr, "[host] No ROM yet: run once with --rom <path to the US ROM>.\n");
        return false;
    }

    // Earlier builds kept the ROM and saves in conker_data/ next to the executable.
    // Copy them to the window build's data folder once, if it doesn't have them yet.
    void migrate_old_data(const std::filesystem::path& old_dir, const std::filesystem::path& new_dir) {
        std::error_code error;
        if (!std::filesystem::is_directory(old_dir, error) || old_dir == new_dir) {
            return;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(old_dir, error)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            std::filesystem::path target = new_dir / std::filesystem::relative(entry.path(), old_dir);
            if (!std::filesystem::exists(target)) {
                std::filesystem::create_directories(target.parent_path(), error);
                std::filesystem::copy_file(entry.path(), target, error);
            }
        }
    }
}

int main(int argc, char** argv) {
    // Unbuffered, so diagnostics (e.g. RT64's microcode hashes) survive a crash.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    install_crash_handler();
    std::filesystem::path rom_path;
    int seconds = 0;
#if defined(CONKER_RT64)
    headless = false;
#endif
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            rom_path = argv[++i];
        }
        else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--headless") == 0) {
            headless = true;
        }
#if defined(CONKER_RT64)
        else if (std::strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
            int width = 0, height = 0;
            if (std::sscanf(argv[++i], "%dx%d", &width, &height) == 2 && width > 0 && height > 0) {
                conker::frontend::window_width = width;
                conker::frontend::window_height = height;
            }
        }
#endif
        else if (argv[i][0] != '-' && rom_path.empty()) {
            rom_path = argv[i]; // e.g. a ROM dropped onto the executable
        }
    }

    std::filesystem::path old_data_dir = exe_directory(argv[0]) / "conker_data";
#if defined(CONKER_RT64)
    if (!headless) {
        // NFD_Init() is called once SDL is up (frontend.cpp's create_gfx).
        // recompui loads assets/ (and looks for portable.txt) relative to the working
        // directory: make that the executable's folder, wherever the game is started from.
        std::filesystem::current_path(exe_directory(argv[0]));
        recompui::programconfig::set_program_id(conker::program_id());
        recomp::register_config_path(recompui::file::get_app_folder_path());
        migrate_old_data(old_data_dir, recomp::get_config_path());
    }
    else
#endif
    {
        recomp::register_config_path(old_data_dir);
    }
    std::filesystem::create_directories(recomp::get_config_path());
    // --seconds (test runs) starts the game directly instead of opening the launcher.
    bool start_directly = headless || seconds > 0;

    recomp::GameEntry game{};
    game.rom_hash = conker::roms::us_rom_hash;
    // The US ROM, or a ROM hack that only changes the game's assets.
    game.accept_rom = conker::roms::accept;
    game.internal_name = "CONKER BFD";
    game.display_name = "Conker's Bad Fur Day";
    game.game_id = game_id;
    game.mod_game_id = "conker";
    game.save_type = recomp::SaveType::Eep16k;
    game.is_enabled = true;
    // .game's code is compressed in the ROM (Rare's own format). Mods' hooks
    // (RECOMP_HOOK) rebuild game functions from their original instructions, which
    // decompress_rom provides from the copy of the code the exe already carries.
    game.has_compressed_code = true;
    game.decompression_routine = conker::decompress_rom;
    game.entrypoint_address = (gpr)(int32_t)0x80001000u;
    game.entrypoint = recomp_entrypoint;
    game.on_init_callback = on_init;
    game.thread_create_callback = on_thread_create;
#if defined(CONKER_RT64)
    if (!headless) {
        conker::frontend::init(game);
    }
#endif
    recomp::register_game(game);

    conker::register_overlays();
    conker::register_mod_exports();

    if (!select_rom(rom_path, start_directly)) {
        return EXIT_FAILURE;
    }

    // librecomp starts a game named on the command line with --game; otherwise the
    // launcher does.
    std::vector<char*> runtime_argv{ argv[0] };
    if (start_directly) {
        runtime_argv.push_back((char*)"--game");
        runtime_argv.push_back((char*)"conker");
    }

    recomp::Configuration cfg{};
    cfg.argc = (int)runtime_argv.size();
    cfg.argv = runtime_argv.data();
    cfg.project_version = recomp::Version{ 0, 1, 5 };
    cfg.rsp_callbacks.get_rsp_microcode = get_rsp_microcode;
    cfg.audio_callbacks = { queue_samples, get_frames_remaining, set_frequency };
    cfg.renderer_callbacks.create_render_context = conker::create_null_renderer;
    cfg.input_callbacks = { poll_input, get_input, set_rumble, conker::get_connected_device_info };
    cfg.gfx_callbacks = { nullptr, create_window, nullptr };
#if defined(CONKER_RT64)
    if (!headless) {
        cfg.audio_callbacks = { conker::audio::queue_samples, conker::audio::get_frames_remaining,
                                conker::audio::set_frequency };
    }
#endif
    cfg.events_callbacks = { vi_callback, nullptr };
    cfg.error_handling_callbacks = { message_box };
#if defined(CONKER_RT64)
    if (!headless) {
        conker::frontend::set_callbacks(cfg);
    }
#endif

    std::thread timer;
    if (seconds > 0) {
        timer = std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            std::printf("[host] %d seconds elapsed, %u VIs; quitting\n", seconds, vi_count.load());
            ultramodern::quit();
        });
    }

    recomp::start(cfg);

#if defined(CONKER_RT64)
    if (!headless) {
        NFD_Quit();
    }
#endif

    if (timer.joinable()) {
        timer.join();
    }
    return EXIT_SUCCESS;
}
