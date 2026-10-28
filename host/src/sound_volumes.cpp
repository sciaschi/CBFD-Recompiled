// Sound: Music, Sound Effects and Speech volumes, each apart from the others (and all under
// the Main Volume, which audio_output.cpp applies to the finished mix). Called from hooks in
// conker.toml. After ConkerBFDReloaded's (by dahmedvall95, MIT,
// github.com/DahSidiAbdallah/ConkerBFDReloaded, host/src/sound_mix.cpp), whose findings these are,
// done without a lock (below).
//
// Everything but the speech goes through libultra's synthesizer as voices. The music comes from
// sequence players, which work out each of their notes' volumes in __n_vsVol(voiceState, seqp):
// the voice states passed are theirs, with the voice 4 bytes into each. Every other voice is the
// sound player's: a sound effect. Rare plays some ambience through the sequence players too, on
// channels of the music's sequence: where it's known (ambience_channel, the bar's crowd and glasses),
// it follows the Sound Effects volume, and elsewhere the Music volume. When a voice's volume is set
// (n_alSynStartVoiceParams as a note starts, n_alSynSetVol as it changes), it's scaled by the Music
// or Sound Effects volume.
// A voice state belongs to its player for good (each player's are made with it), so a voice once
// seen in __n_vsVol stays a music voice.
//
// The speech is streamed: the MP3 player (func_151F2E88) reads its volume (D_800E0D8E) to work out
// the left and right levels as a line starts, or as its volume changes, and that's scaled by the
// Speech volume. The chainsaw opening (scene 0x21, before the bar) streams its music that way,
// which follows the Music volume.
//
// These run on the game's audio thread, which must not wait on a lock (a game thread that holds
// one may be paused by the cooperative scheduler: printing from there hung the game): the
// voices are kept in a table of atomics, and the volumes are read from the settings once per game
// frame (game_frame) and kept as numbers. Once a slider settles, the sounds playing are set to the
// new volume (conker_audio_frame; for the speech, the MP3 player works its levels out again).

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "recomp.h"
#include "librecomp/config.hpp"
#include "recompui/config.h"

#include "conker.hpp"

namespace {
    namespace options {
        const std::string music_volume = "music_volume";
        const std::string effects_volume = "effects_volume";
        const std::string speech_volume = "speech_volume";
        const std::string sound_mode = "sound_mode";
    }

    // Sound Mode (issue #159), the game's own Options setting, here too. The game keeps it in its options (the
    // save's first block, also at 0x800E0BDE while playing): the byte at 0x800E0BE9, 0 Stereo, 1 Headphones, 2
    // Mono (found by changing only it in the game's menu: the save changed at that byte and its checksum). Its
    // menu applies a change with func_151E7E9C, which gives func_10017870 the synthesizer's output mode (2 Stereo,
    // 4 Headphones, 1 Mono: flags at D_800428C0 to C2 that func_10017944 works the channels' out from, all in
    // memory), as the save's settings are applied as it loads.
    enum class SoundMode : uint32_t { Stereo, Mono, Headphones };
    constexpr uint32_t game_sound_mode = 0x800E0BE9;
    extern "C" void func_151E7E9C(uint8_t* rdram, recomp_context* ctx);

    int8_t game_byte(SoundMode mode) {
        return mode == SoundMode::Mono ? 2 : mode == SoundMode::Headphones ? 1 : 0;
    }

    SoundMode from_game_byte(int8_t value) {
        return value == 2 ? SoundMode::Mono : value == 1 ? SoundMode::Headphones : SoundMode::Stereo;
    }

    // The volumes, 0 to 1, from game_frame.
    std::atomic<float> music_scale{ 1.0f };
    std::atomic<float> effects_scale{ 1.0f };
    std::atomic<float> speech_scale{ 1.0f };
    bool options_added = false;

    // The voices seen (their RDRAM addresses), open addressing with linear probing, 0 an empty slot:
    // the players have a few dozen between them. With each, its sequence player (0: the sound
    // player's, a sound effect), and the volume the game last set it to, before it was scaled.
    constexpr size_t voice_slots = 512;
    std::array<std::atomic<uint32_t>, voice_slots> voices{};
    std::array<std::atomic<uint32_t>, voice_slots> voice_players{};
    std::array<std::atomic<int32_t>, voice_slots> voice_volumes{};
    constexpr size_t no_slot = voice_slots;

    // The Music and Sound Effects volumes the playing voices were last set to (conker_audio_frame).
    float applied_music = 1.0f;
    float applied_effects = 1.0f;

    // The MP3 player works its left and right levels out again when this is set (D_800E0DA4, an s16).
    constexpr uint32_t stream_levels_dirty = 0x800E0DA4;

    // A voice's slot, taken for it if it has none (no_slot if the table is full, or for no voice).
    size_t voice_slot(uint32_t voice) {
        if (voice == 0) {
            return no_slot;
        }
        size_t slot = (size_t)((voice >> 2) * 2654435761u) % voice_slots;
        for (size_t probes = 0; probes < voice_slots; probes++) {
            uint32_t held = voices[slot].load(std::memory_order_relaxed);
            if (held == 0 && voices[slot].compare_exchange_strong(held, voice, std::memory_order_relaxed)) {
                return slot;
            }
            if (held == voice) {
                return slot;
            }
            slot = (slot + 1) % voice_slots;
        }
        return no_slot;
    }

    // The sequence player of a music voice, 0 for a sound effect's.
    uint32_t music_voice_player(uint32_t voice) {
        const size_t slot = voice_slot(voice);
        return slot == no_slot ? 0 : voice_players[slot].load(std::memory_order_relaxed);
    }

    // Ambience a sequence plays on channels of its own, apart from its music's: sound effects, not music.
    // The bar's (scene 0x1D) plays on its sequence player 0x8007B820, the music on channels 0 to 4 and the
    // crowd's chatter and the glasses clinking on 5 to 11 (found by muting them: the music played on whole).
    // A sequence's channels are its own, so this is by the scene and the player.
    bool ambience_channel(uint8_t* rdram, uint32_t player, uint32_t channel) {
        constexpr uint32_t scene_in_play = 0x800BE9F0; // D_800BE9F0
        constexpr uint32_t bar_scene = 0x1D;
        constexpr uint32_t bar_player = 0x8007B820;
        return player == bar_player && channel >= 5 && channel <= 11 && (uint32_t)MEM_W(0, (gpr)(int32_t)scene_in_play) == bar_scene;
    }

    // The scale for a voice's volume: the Music or the Sound Effects volume.
    float voice_scale(uint8_t* rdram, uint32_t voice) {
        const uint32_t player = music_voice_player(voice);
        if (player == 0) {
            return effects_scale.load(std::memory_order_relaxed);
        }
        // The voice's channel: 0x35 into its state, 0x31 into the voice.
        if (ambience_channel(rdram, player, MEM_BU(0x31, (gpr)(int32_t)voice))) {
            return effects_scale.load(std::memory_order_relaxed);
        }
        return music_scale.load(std::memory_order_relaxed);
    }

    // A volume (an s16, the synthesizer's 0 to 0x7FFF) scaled by one of the settings.
    int32_t scaled(int32_t volume, float scale) {
        return (int32_t)(int16_t)((float)(int16_t)volume * scale);
    }

    float percent(recomp::config::Config& config, const std::string& id) {
        const recomp::config::ConfigValueVariant value = config.get_option_value(id);
        const double* number = std::get_if<double>(&value);
        return number != nullptr ? (float)(*number / 100.0) : 1.0f;
    }
}

void conker::sound_volumes::add_options(recomp::config::Config& config) {
    config.add_percent_number_option(options::music_volume, "Music Volume",
        "The volume of the <recomp-color primary>music</recomp-color>, and in some places of background sounds the game plays "
        "with it.", 100.0);
    config.add_percent_number_option(options::effects_volume, "Sound Effects Volume",
        "The volume of the <recomp-color primary>sound effects</recomp-color>, Conker's grunts and the characters' short "
        "voice bits during play among them.", 100.0);
    config.add_percent_number_option(options::speech_volume, "Speech Volume",
        "The volume of the characters' <recomp-color primary>spoken lines</recomp-color> in cutscenes.", 100.0);
    static const std::vector<recomp::config::ConfigOptionEnumOption> mode_choices = {
        {SoundMode::Stereo, "Stereo", "Stereo"},
        {SoundMode::Mono, "Mono", "Mono"},
        {SoundMode::Headphones, "Headphones", "Headphones"},
    };
    config.add_enum_option(options::sound_mode, "Sound Mode",
        "How the game mixes its sound, the same setting as the game's own Options menu (it follows that menu, and is "
        "saved with your game as the game saves it). <recomp-color primary>Stereo</recomp-color> for speakers, "
        "<recomp-color primary>Mono</recomp-color> for a single speaker, and <recomp-color primary>Headphones"
        "</recomp-color> for headphones.",
        mode_choices, SoundMode::Stereo);
    options_added = true;
}

// Sound Mode, from the game thread at the start of each frame's display list ($ctx: that hook's, for calling the
// game). Kept the same both ways: a change here is written to the game's setting and applied as its menu does
// (func_151E7E9C, on a stack of its own below the hook's); a change the game made (its menu, a save loaded) is
// shown here. The game only changes it in its own menu or as a save loads, while this menu is closed.
void conker::sound_volumes::sound_mode_frame(uint8_t* rdram, recomp_context* ctx) {
    // The netplay settings profile: the game's own (its memory is the same on every machine).
    if (!options_added || conker::netplay_profile::active()) {
        return;
    }
    static bool synced = false;
    static SoundMode last_setting = SoundMode::Stereo;
    static int8_t last_game = 0;
    recomp::config::Config& config = recompui::config::get_config(recompui::config::sound::id);
    const SoundMode setting = static_cast<SoundMode>(std::get<uint32_t>(config.get_option_value(options::sound_mode)));
    const int8_t game = (int8_t)MEM_B(0, (gpr)(int32_t)game_sound_mode);
    if (synced && setting != last_setting) {
        // Changed here: the game's setting, applied.
        MEM_B(0, (gpr)(int32_t)game_sound_mode) = game_byte(setting);
        recomp_context call = *ctx;
        call.r29 = ADD32(ctx->r29, -0x40);
        func_151E7E9C(rdram, &call);
        last_setting = setting;
        last_game = game_byte(setting);
        return;
    }
    if (!synced || game != last_game) {
        // Changed in the game (or the first frame): shown here.
        last_game = game;
        last_setting = from_game_byte(game);
        synced = true;
        if (setting != last_setting) {
            config.update_option_value(options::sound_mode, static_cast<uint32_t>(last_setting));
        }
    }
}

void conker::sound_volumes::game_frame(uint8_t* rdram) {
    // The netplay settings profile: 100%, as without the settings (the sounds' volumes are in game memory).
    if (!options_added || conker::netplay_profile::active()) {
        return;
    }
    // A new volume is taken once its slider has been left alone for a few frames, not on every frame
    // it's dragged through: the playing sounds are each set to it then (conker_audio_frame, and the
    // MP3 player below), and setting them over and over as the slider moved made them skip.
    struct Settled {
        float pending = 1.0f;
        int frames_unchanged = -1; // -1: nothing waiting
        // Whether the volume has settled on a new value this frame.
        bool update(float value) {
            if (value != pending) {
                pending = value;
                frames_unchanged = 0;
            }
            else if (frames_unchanged >= 0 && ++frames_unchanged >= 8) {
                frames_unchanged = -1;
                return true;
            }
            return false;
        }
    };
    static Settled music, effects, speech;
    recomp::config::Config& config = recompui::config::get_config(recompui::config::sound::id);
    if (music.update(percent(config, options::music_volume))) {
        music_scale.store(music.pending, std::memory_order_relaxed);
    }
    if (effects.update(percent(config, options::effects_volume))) {
        effects_scale.store(effects.pending, std::memory_order_relaxed);
    }
    // The MP3 player works its levels out from the volume only as a line starts or its volume changes,
    // and a cutscene's lines are often one long stream: a new Speech volume has it work them out again
    // (which also sets the stream's volume afresh for the audio microcode, starting a new ramp).
    if (speech.update(percent(config, options::speech_volume))) {
        speech_scale.store(speech.pending, std::memory_order_relaxed);
        MEM_H(0, (gpr)(int32_t)stream_levels_dirty) = 1;
    }
}

// __n_vsVol, at its first instruction: $a0 is a voice state of $a1's, a sequence player.
extern "C" void conker_music_note_volume(uint8_t* rdram, recomp_context* ctx) {
    // The voice is 4 bytes into its state.
    const size_t slot = voice_slot((uint32_t)ctx->r4 + 4);
    if (slot != no_slot) {
        voice_players[slot].store((uint32_t)ctx->r5, std::memory_order_relaxed);
    }
}

// n_alSynStartVoiceParams(voice, wavetable, pitch, volume, ...), at its first instruction: $a3 is the volume.
extern "C" void conker_voice_start_volume(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t voice = (uint32_t)ctx->r4;
    const size_t slot = voice_slot(voice);
    if (slot != no_slot) {
        voice_volumes[slot].store((int16_t)ctx->r7, std::memory_order_relaxed);
    }
    ctx->r7 = scaled((int32_t)ctx->r7, voice_scale(rdram, voice));
}

// n_alSynSetVol(voice, volume, time), at its first instruction: $a1 is the volume.
extern "C" void conker_voice_set_volume(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t voice = (uint32_t)ctx->r4;
    const size_t slot = voice_slot(voice);
    if (slot != no_slot) {
        voice_volumes[slot].store((int16_t)ctx->r5, std::memory_order_relaxed);
    }
    ctx->r5 = scaled((int32_t)ctx->r5, voice_scale(rdram, voice));
}

extern "C" void n_alSynSetVol(uint8_t* rdram, recomp_context* ctx);

// n_alAudioFrame, at its first instruction, as the synthesizer makes each audio frame: when the Music or
// Sound Effects volume has changed, every playing voice (one with a physical voice, 0x8 into it) is set to
// the volume the game last gave it, scaled by the new one (n_alSynSetVol, through the hook above), with
// a short ramp. Otherwise a change was only heard from each sound's next note, the sounds coming back in
// one by one. Called with a stack of its own below n_alAudioFrame's caller's.
extern "C" void conker_audio_frame(uint8_t* rdram, recomp_context* ctx) {
    const float music = music_scale.load(std::memory_order_relaxed);
    const float effects = effects_scale.load(std::memory_order_relaxed);
    if (music == applied_music && effects == applied_effects) {
        return;
    }
    applied_music = music;
    applied_effects = effects;
    constexpr int32_t ramp_microseconds = 20000;
    for (size_t slot = 0; slot < voice_slots; slot++) {
        const uint32_t voice = voices[slot].load(std::memory_order_relaxed);
        if (voice == 0 || MEM_W(0x8, (gpr)(int32_t)voice) == 0) {
            continue;
        }
        recomp_context call = *ctx;
        call.r29 = ADD32(ctx->r29, -0x40);
        call.r4 = (gpr)(int32_t)voice;
        call.r5 = (gpr)voice_volumes[slot].load(std::memory_order_relaxed);
        call.r6 = (gpr)ramp_microseconds;
        n_alSynSetVol(rdram, &call);
    }
}

// func_151F2E88 (the MP3 player), just after it reads the stream's volume (D_800E0D8E) into a register to
// work out the left or the right level: returns it scaled by the Speech volume, or in the chainsaw opening
// scene, which streams its music, by the Music volume.
extern "C" int32_t conker_stream_volume(uint8_t* rdram, int32_t volume) {
    constexpr uint32_t scene_in_play = 0x800BE9F0; // D_800BE9F0
    constexpr uint32_t opening_scene = 0x21;
    const bool music = (uint32_t)MEM_W(0, (gpr)(int32_t)scene_in_play) == opening_scene;
    return scaled(volume, music ? music_scale.load(std::memory_order_relaxed) : speech_scale.load(std::memory_order_relaxed));
}
