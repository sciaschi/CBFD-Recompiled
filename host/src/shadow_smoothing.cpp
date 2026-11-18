// Shadow smoothing: a character's shadow eased toward the light it's cast from (called from a hook in conker.toml).
//
// func_15184368 lays each character's shadow away from a light: it walks the dynamic lights at D_800DCD78, keeps
// the nearest within 5000 units (D_800A7350) that's switched on (+4 and +9 clear; its position in the shorts at
// +0xE), and moves the shadow's light point (D_800DDFB0, 12 bytes for each of the 8 shadows) 0.3 of the way to it
// (D_800A7354) each time the shadow is drawn. With D_800DBFF4 set, or D_800DE038 at 2 or more, the point is put
// on the light outright instead. The shadow is then cast from that point onto the ground under the character.
//
// Many lights move: a fire's light hops about by a couple of hundred units every few frames. Measured (the light
// each shadow took, logged as Conker stood still by one): the light's x went 2260, 2460, 2299, 2334, 2447, 2252
// within a few seconds, 175 to 415 units from him, and 0.3 of each hop left the shadow snapping from side to side,
// in 4:3 as on the N64. Walking past lights, the shadow also swung across in a few frames when another became the
// nearest. The point now moves 0.06 of the way each time instead (as the game, once each game frame, so about
// half a second to settle at 30 frames a second): the hops average out into a small sway, and the shadow turns
// over to the next light instead of snapping to it. 0.06 was picked by trying it against the game's 0.3 in play.

#include <cstdint>

#include "recomp.h"

namespace {
    // How far the shadow's light point moves toward the light each time (the game's own is 0.3).
    constexpr float smooth_step = 0.06f;
}

// func_15184368 at 0x15184658: $f14 has just been loaded with the step (D_800A7354), about to move the point
// toward the light. Only reached when the game eases the point (see the top).
extern "C" void conker_shadow_light_step(uint8_t* rdram, recomp_context* ctx) {
    ctx->f14.fl = smooth_step;
}
