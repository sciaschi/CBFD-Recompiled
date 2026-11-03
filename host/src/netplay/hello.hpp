// What this game says in netplay's handshake: a joiner whose Hello differs from the
// host's is refused (session.hpp).

#pragma once

#include <span>
#include <string>

#include "net/packets.hpp"

namespace conker::netplay {
    // The build: the git commit, with "-dirty-" and a hash of any uncommitted changes (build_id.cmake).
    const char* build_id();
    // The release's name, for players to read: e.g. "v0.1.6-netplay.15", or "0.1.6-dev-1a2b3c4" for a local
    // build (build_id.cmake). Printable, no spaces, 1 to signal::max_release characters.
    const char* release_name();
    // Where a release (e.g. "v0.1.7") can be downloaded; empty for any other name (a local build's).
    std::string release_url(const std::string& release);
    // XXH3-64s of the build id, the ROM loaded and the netplay settings profile. Once the ROM is loaded.
    Hello local_hello();
    // The same with `rom` for the ROM: the stored one, before the game has loaded it (the menu's).
    Hello hello_for_rom(std::span<const uint8_t> rom);
}
