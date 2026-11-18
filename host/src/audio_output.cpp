// SDL2 sound output for the RT64 build.
//
// The game's audio thread hands over each buffer the audio microcode mixed
// (osAiSetNextBuffer) and paces itself by how much is still queued
// (osAiGetLength), so the SDL device is used in queue mode: the reported
// remaining frames are what SDL hasn't played yet.
//
// In deterministic mode (replays, netplay) the game is paced by the timeline
// instead (audio_clock.cpp), and the device only plays: it waits for a cushion
// before it starts (and again whenever it runs dry), and drops what comes while
// it's far behind, so its own pace never reaches the game.

#include <cstdio>
#include <mutex>
#include <vector>

#include <SDL.h>

#include "ultramodern/timeline.hpp"

#include "recompui/config.h"

#include "conker.hpp"

namespace {
    constexpr int channels = 2;
    // Deterministic mode: the frames queued before the device starts playing (about 70 ms at 22 kHz),
    // and the most it holds (about 270 ms), past which buffers are dropped.
    constexpr uint32_t cushion_frames = 2 * 736;
    constexpr uint32_t most_frames = 8 * 736;

    std::mutex audio_mutex;
    SDL_AudioDeviceID device = 0;
    std::vector<float> convert_buffer;

    void open_device(uint32_t frequency) {
        if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            std::fprintf(stderr, "[audio] SDL audio init failed: %s\n", SDL_GetError());
            return;
        }
        if (device != 0) {
            SDL_CloseAudioDevice(device);
            device = 0;
        }
        SDL_AudioSpec want{};
        want.freq = (int)frequency;
        want.format = AUDIO_F32SYS;
        want.channels = channels;
        want.samples = 256; // ~12 ms at 22 kHz: SDL pulls from the queue in chunks this size
        // No allowed changes: SDL converts to whatever the output device uses, so the
        // queue stays in the game's sample rate.
        device = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
        if (device == 0) {
            std::fprintf(stderr, "[audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
            return;
        }
        SDL_PauseAudioDevice(device, 0);
    }
}

void conker::audio::set_frequency(uint32_t frequency) {
    std::lock_guard lock{audio_mutex};
    open_device(frequency);
}

void conker::audio::queue_samples(int16_t* samples, size_t sample_count) {
    std::lock_guard lock{audio_mutex};
    if (device == 0) {
        return;
    }
    // RDRAM is kept as native-endian 32-bit words, so each stereo frame's two 16-bit
    // samples read back swapped: [i + 1] is the left channel and [i] the right.
    // Scaled by the main volume from the Sound settings (0-100).
    const float scale = static_cast<float>(recompui::config::sound::get_main_volume()) / (100.0f * 32768.0f);
    if (ultramodern::timeline::enabled()) {
        const uint32_t queued = SDL_GetQueuedAudioSize(device) / (channels * sizeof(float));
        if (queued + sample_count / channels > most_frames) {
            return;
        }
        if (queued == 0) {
            SDL_PauseAudioDevice(device, 1);
        }
    }
    convert_buffer.resize(sample_count);
    for (size_t i = 0; i + 1 < sample_count; i += channels) {
        convert_buffer[i + 0] = samples[i + 1] * scale;
        convert_buffer[i + 1] = samples[i + 0] * scale;
    }
    SDL_QueueAudio(device, convert_buffer.data(), (Uint32)(sample_count * sizeof(float)));
    if (ultramodern::timeline::enabled() && SDL_GetAudioDeviceStatus(device) == SDL_AUDIO_PAUSED &&
        SDL_GetQueuedAudioSize(device) / (channels * sizeof(float)) >= cushion_frames) {
        SDL_PauseAudioDevice(device, 0);
    }
}

size_t conker::audio::get_frames_remaining() {
    std::lock_guard lock{audio_mutex};
    if (device == 0) {
        return 0;
    }
    // On hardware AI_LEN only counts what is left of the buffer playing now, not the
    // one queued behind it. Conker's audio thread (func_100095A0) makes its next
    // buffer short (552 frames instead of 736) whenever 249 or more samples are
    // left, so reporting the whole SDL queue makes it underrun and pop. Leave one
    // buffer out, so the queue settles at about a buffer and a half.
    constexpr uint32_t one_buffer = 736;
    uint32_t queued = SDL_GetQueuedAudioSize(device) / (channels * sizeof(float));
    return queued > one_buffer ? queued - one_buffer : 0;
}
