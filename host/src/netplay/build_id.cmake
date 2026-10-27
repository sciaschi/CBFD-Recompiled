# Writes the game's build id for netplay's handshake (hello.cpp) into OUT, a C++ file, at every build:
# the game's version (host/src/main.cpp's project_version), the git commit, and if there are uncommitted
# changes "-dirty-" and a hash of them (git diff, with the submodules' patched trees left out: their patches
# are in the repo; and the untracked files, names and contents). Two builds of the same commit from clean
# trees have the same id, on any platform: a match can be between Windows, Linux and macOS, as long as their
# compilers turn the game's code into the same arithmetic (host/CMakeLists.txt keeps them from fusing
# multiply-adds); the memory hashes the peers compare catch it if not. Without git (a source download) the
# commit is "unknown", and only the version tells builds apart.
# And the release's name, for players to read (the room list, a refusal): CONKER_RELEASE from the environment
# (the release workflow sets its tag, e.g. v0.1.6-netplay.15), or else "<version>-dev-<short commit>", with
# "-dirty" if there are changes. It isn't part of the id: a release and a local build of its commit play
# together.
# A release build fails if the tree has changes: an id with "-dirty-" wouldn't match another platform's.
# The file is rewritten only when the id or the name changes, so nothing is rebuilt otherwise.
#   cmake -DSOURCE_DIR=<repo> -DOUT=<file> -P build_id.cmake
file(READ "${SOURCE_DIR}/host/src/main.cpp" main_source)
string(REGEX MATCH "project_version = recomp::Version\\{ *([0-9]+), *([0-9]+), *([0-9]+)(, *\"([^\"]*)\")? *\\}" version_match "${main_source}")
if (version_match)
    set(version "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}${CMAKE_MATCH_5}")
else()
    set(version "unknown")
endif()

execute_process(COMMAND git rev-parse HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}" OUTPUT_VARIABLE commit RESULT_VARIABLE failed
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if (failed OR commit STREQUAL "")
    set(id "${version} unknown")
    set(release "${version}-dev")
else()
    set(id "${version} ${commit}")
    string(SUBSTRING "${commit}" 0 7 short_commit)
    set(release "${version}-dev-${short_commit}")
    execute_process(COMMAND git diff HEAD --ignore-submodules=dirty
        WORKING_DIRECTORY "${SOURCE_DIR}" OUTPUT_VARIABLE changes ERROR_QUIET)
    # Files not yet added: each one's name and contents.
    execute_process(COMMAND git ls-files --others --exclude-standard
        WORKING_DIRECTORY "${SOURCE_DIR}" OUTPUT_VARIABLE untracked ERROR_QUIET)
    string(REPLACE "\n" ";" untracked_files "${untracked}")
    foreach (untracked_file IN LISTS untracked_files)
        if (NOT untracked_file STREQUAL "" AND EXISTS "${SOURCE_DIR}/${untracked_file}" AND NOT IS_DIRECTORY "${SOURCE_DIR}/${untracked_file}")
            file(SHA256 "${SOURCE_DIR}/${untracked_file}" file_hash)
            string(APPEND changes "\nuntracked ${untracked_file} ${file_hash}")
        endif()
    endforeach()
    if (NOT changes STREQUAL "")
        string(SHA256 changes_hash "${changes}")
        string(SUBSTRING "${changes_hash}" 0 16 changes_hash)
        string(APPEND id "-dirty-${changes_hash}")
        string(APPEND release "-dirty")
        # A release (CONKER_RELEASE set, below) is built from its commit alone: with changes, each
        # platform's build would have its own id and refuse the others'.
        if (DEFINED ENV{CONKER_RELEASE})
            execute_process(COMMAND git status --short --untracked-files=all --ignore-submodules=dirty
                WORKING_DIRECTORY "${SOURCE_DIR}" OUTPUT_VARIABLE changed ERROR_QUIET)
            message(FATAL_ERROR "A release's tree has changes, so its build id would be its own:\n${changed}")
        endif()
    endif()
endif()
# Printable, no spaces, 1 to 40 characters (signal_packets.hpp's max_release): anything else isn't used.
if (DEFINED ENV{CONKER_RELEASE} AND "$ENV{CONKER_RELEASE}" MATCHES "^[!-~]+$")
    string(LENGTH "$ENV{CONKER_RELEASE}" release_length)
    if (release_length LESS_EQUAL 40)
        set(release "$ENV{CONKER_RELEASE}")
    endif()
endif()
set(content "// Written by host/src/netplay/build_id.cmake at each build.\nnamespace conker::netplay {\n    const char* build_id() { return \"${id}\"; }\n    const char* release_name() { return \"${release}\"; }\n}\n")
if (EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if (NOT old STREQUAL content)
    file(WRITE "${OUT}" "${content}")
endif()
