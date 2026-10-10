// TEMP-DEBUG (issue #98): a report on Rare's heap, printed as a level allocates its display lists
// (render_fixes.cpp) and when the game halts (ultra_extras.cpp), to tell a heap that ran out from one
// that was overwritten.
//
// The heap (func_10003BD0 sets it up, func_10003C6C allocates, func_10004074 frees) is one run of
// blocks from D_800380B4 up to D_80038098, each with a 0xC-byte header: +0 the next block, +4 the
// one before, +8 the block's kind in its top byte (0 free) and its size in the low 24 bits. The free
// blocks are also on a list of their own, from D_800380B8 through each one's +0xC. The report walks
// both, and says where either goes wrong (a link outside the heap, out of order, or a free-list block
// that isn't free): an overwritten header shows there.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include "recomp.h"

#include "conker.hpp"

namespace {
    constexpr uint32_t heap_first = 0x800380B4;      // D_800380B4: the first block
    constexpr uint32_t heap_end = 0x80038098;        // D_80038098: where the heap ends
    constexpr uint32_t heap_free_first = 0x800380B8; // D_800380B8: the first free block

    uint32_t word(uint8_t* rdram, uint32_t address) {
        return (uint32_t)MEM_W(0, (gpr)(int32_t)address);
    }
}

std::string conker::heap::describe(uint8_t* rdram) {
    const uint32_t first = word(rdram, heap_first);
    const uint32_t end = word(rdram, heap_end);
    if (first < 0x80000000u || end > 0x80800000u || first >= end) {
        char line[96];
        std::snprintf(line, sizeof(line), "heap: not set up (first block 0x%08X, end 0x%08X)", first, end);
        return line;
    }
    auto in_heap = [&](uint32_t block) { return block >= first && block + 0xC <= end && (block & 3) == 0; };

    uint32_t blocks = 0, free_bytes = 0, largest_free = 0, used_bytes = 0;
    std::string problem;
    for (uint32_t block = first; block != 0 && blocks < 100000;) {
        if (!in_heap(block)) {
            char line[96];
            std::snprintf(line, sizeof(line), "; the block chain breaks: a link to 0x%08X", block);
            problem += line;
            break;
        }
        const uint32_t info = word(rdram, block + 8);
        const uint32_t size = info & 0xFFFFFF;
        if ((info >> 24) == 0) {
            free_bytes += size;
            largest_free = std::max(largest_free, size);
        }
        else {
            used_bytes += size;
        }
        blocks++;
        const uint32_t next = word(rdram, block);
        if (next != 0 && next <= block) {
            char line[96];
            std::snprintf(line, sizeof(line), "; the block chain breaks: 0x%08X links back to 0x%08X", block, next);
            problem += line;
            break;
        }
        block = next;
    }

    uint32_t free_blocks = 0, free_list_bytes = 0;
    for (uint32_t block = word(rdram, heap_free_first); block != 0 && free_blocks < 100000;) {
        if (!in_heap(block)) {
            char line[96];
            std::snprintf(line, sizeof(line), "; the free list breaks: a link to 0x%08X", block);
            problem += line;
            break;
        }
        const uint32_t info = word(rdram, block + 8);
        if ((info >> 24) != 0) {
            char line[96];
            std::snprintf(line, sizeof(line), "; the free list has a used block, 0x%08X", block);
            problem += line;
            break;
        }
        free_list_bytes += info & 0xFFFFFF;
        free_blocks++;
        block = word(rdram, block + 0xC);
    }

    char line[256];
    std::snprintf(line, sizeof(line), "heap: %u KB, %u blocks, %u KB used, %u KB free (largest %u KB), free list %u blocks, %u KB",
        (end - first) / 1024, blocks, used_bytes / 1024, free_bytes / 1024, largest_free / 1024, free_blocks, free_list_bytes / 1024);
    return std::string(line) + problem;
}
