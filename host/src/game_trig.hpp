// The trigonometry for host code that writes into the game's memory (widescreen.cpp, field_of_view.cpp):
// same_math.hpp's while the netplay settings profile is active (a netplay match, or the harness's replays and
// hash logs), where every machine has to compute the same values; the C library's otherwise, so normal play
// is as it always was.

#pragma once

#include <cmath>

#include "conker.hpp"
#include "same_math.hpp"

namespace conker::game_trig {
    inline bool same() { return conker::netplay_profile::active(); }
    inline float sin(float x) { return same() ? same_math::sin(x) : std::sin(x); }
    inline float cos(float x) { return same() ? same_math::cos(x) : std::cos(x); }
    inline float tan(float x) { return same() ? same_math::tan(x) : std::tan(x); }
    inline float atan(float x) { return same() ? same_math::atan(x) : std::atan(x); }
    inline float asin(float x) { return same() ? same_math::asin(x) : std::asin(x); }
    inline float atan2(float y, float x) { return same() ? same_math::atan2(y, x) : std::atan2(y, x); }
    // hypot isn't required to be correctly rounded either; a square root is.
    inline float hypot(float x, float y) { return same() ? std::sqrt(x * x + y * y) : std::hypot(x, y); }
}
