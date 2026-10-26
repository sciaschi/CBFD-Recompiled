// Netplay's handshake hashes (hello.hpp). The ROM and the profile are hashed as for the harness's
// recordings (harness.cpp), so a recording and a match of the same game carry the same ones.

#include "hello.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "librecomp/game.hpp"

#include "../conker.hpp"

#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

std::string conker::netplay::release_url(const std::string& release) {
    // v<digits and dots>, maybe with a suffix of letters, digits, dots and dashes: the release workflow's tags.
    const auto allowed = [](char c) { return std::isalnum((unsigned char)c) || c == '.' || c == '-'; };
    if (release.size() < 2 || (release[0] != 'v' && release[0] != 'V') || !std::isdigit((unsigned char)release[1]) ||
        !std::all_of(release.begin() + 1, release.end(), allowed)) {
        return {};
    }
    return "https://github.com/sciaschi/CBFD-Recompiled/releases/tag/" + release;
}

conker::netplay::Hello conker::netplay::local_hello() {
    return hello_for_rom(recomp::get_rom());
}

conker::netplay::Hello conker::netplay::hello_for_rom(std::span<const uint8_t> rom) {
    Hello hello;
    hello.build = XXH3_64bits(build_id(), std::strlen(build_id()));
    hello.rom = XXH3_64bits(rom.data(), rom.size());
    hello.profile = conker::netplay_profile::hash();
    hello.mods = conker::netplay_profile::mods_hash();
    return hello;
}
