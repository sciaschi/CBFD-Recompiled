// The sound's timing in deterministic mode (ultramodern/timeline.hpp: replays and netplay).
//
// The game's audio thread (func_100095A0) sizes each buffer by how much is left of the one playing
// (osAiGetLength): normally that's the sound device's queue (audio_output.cpp), whose pace differs from
// run to run and machine to machine, and headless has none (0). So while the timeline is on, the
// buffers play on the timeline instead, as the N64's audio interface would play them: each ends at a
// fixed time after the one before it (or after it was handed over, if the one before had finished),
// and the length reported is what's left of the buffer playing at the timeline's time. Headless and in
// a window alike; the device only plays what it's given (and paces nothing).
//
// Times are kept as ticks (46,875,000 a second) × the sample rate, so a buffer's length is a whole
// number of them and every machine gets the same.

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "ultramodern/timeline.hpp"

#include "conker.hpp"

namespace {
    constexpr uint64_t ticks_per_second = 46'875'000;
    constexpr int channels = 2;

    ultramodern::audio_callbacks_t device{};

    std::mutex mutex;
    uint32_t frequency = 48000;
    // The end of each buffer not yet played, oldest first, in ticks × frequency.
    std::deque<uint64_t> ends;

    uint64_t scaled_now() {
        return ultramodern::timeline::delivery_time() * frequency;
    }

    void drop_played(uint64_t now) {
        while (!ends.empty() && ends.front() <= now) {
            ends.pop_front();
        }
    }

    void queue_samples(int16_t* samples, size_t sample_count) {
        if (ultramodern::timeline::enabled()) {
            std::lock_guard lock{mutex};
            const uint64_t now = scaled_now();
            drop_played(now);
            const uint64_t start = ends.empty() ? now : ends.back();
            ends.push_back(start + (uint64_t)(sample_count / channels) * ticks_per_second);
        }
        // Not heard while replaying (a rollback playing reads again): those reads' sound was played already.
        if (device.queue_samples != nullptr && !ultramodern::timeline::replaying()) {
            device.queue_samples(samples, sample_count);
        }
    }

    size_t get_frames_remaining() {
        if (!ultramodern::timeline::enabled()) {
            return device.get_frames_remaining != nullptr ? device.get_frames_remaining() : 0;
        }
        std::lock_guard lock{mutex};
        const uint64_t now = scaled_now();
        drop_played(now);
        return ends.empty() ? 0 : (size_t)((ends.front() - now) / ticks_per_second);
    }

    void set_frequency(uint32_t new_frequency) {
        {
            std::lock_guard lock{mutex};
            if (ultramodern::timeline::enabled() && new_frequency != 0 && new_frequency != frequency) {
                // What's left of each buffer plays on at the new rate's scale (the same time).
                const uint64_t now = scaled_now();
                drop_played(now);
                const uint64_t new_now = ultramodern::timeline::delivery_time() * new_frequency;
                for (uint64_t& end : ends) {
                    end = new_now + (end - now) * new_frequency / frequency;
                }
            }
            if (new_frequency != 0) {
                frequency = new_frequency;
            }
        }
        if (device.set_frequency != nullptr) {
            device.set_frequency(new_frequency);
        }
    }
}

ultramodern::audio_callbacks_t conker::audio_clock::wrap(const ultramodern::audio_callbacks_t& device_callbacks) {
    device = device_callbacks;
    return { queue_samples, get_frames_remaining, set_frequency };
}

std::vector<uint32_t> conker::audio_clock::state() {
    std::lock_guard lock{mutex};
    std::vector<uint32_t> words{ frequency };
    for (uint64_t end : ends) {
        words.push_back((uint32_t)end);
        words.push_back((uint32_t)(end >> 32));
    }
    return words;
}

void conker::audio_clock::set_state(const std::vector<uint32_t>& words) {
    std::lock_guard lock{mutex};
    ends.clear();
    if (words.empty()) {
        return;
    }
    // The ends are on the state's rate's scale. (Conker sets one rate, 22020 Hz, so the device's is the same.)
    frequency = words[0] != 0 ? words[0] : frequency;
    for (size_t i = 1; i + 1 < words.size(); i += 2) {
        ends.push_back((uint64_t)words[i] | ((uint64_t)words[i + 1] << 32));
    }
}
