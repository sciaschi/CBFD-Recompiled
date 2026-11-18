// Trigonometry that gives the same result on every platform, for the host code that writes into the game's
// memory in netplay (through game_trig.hpp: widescreen.cpp's frustum and backdrop, field_of_view.cpp). The
// C library's sin, cos, tan and atan
// aren't required to be correctly rounded, and glibc's and Microsoft's round some results' last bit
// differently: a netplay match between Linux and Windows then had different frustum planes for a frame
// (0x3F57F095's sine came out ...C7 on one and ...C8 on the other), which can change what the game culls.
//
// These use only addition, subtraction, multiplication, division and square roots, which IEEE 754 rounds
// exactly the same everywhere (with no fused multiply-adds: host/CMakeLists.txt), in double precision,
// and round to float once at the end. They're accurate to about 1e-15 for the angles used here (a few
// turns at most), far below a float's precision. Inputs that aren't finite, or beyond a million radians (none
// of the callers' come near), give NaN rather than whatever converting them to an integer would give.

#pragma once

#include <cmath>
#include <cstdint>

namespace conker::same_math {
    namespace detail {
        constexpr double half_pi_hi = 1.5707963267948966192;     // pi/2, split so k * pi/2 is exact enough
        constexpr double half_pi_lo = 6.123233995736766e-17;
        constexpr double pi = 3.14159265358979323846;
        constexpr double sixth_pi = 0.52359877559829887308;      // pi/6
        constexpr double sqrt3 = 1.73205080756887729353;
        constexpr double tan_twelfth_pi = 0.26794919243112270647; // tan(pi/12)

        // sin and cos of r in [-pi/4, pi/4]: Taylor series to their x^15 and x^16 terms.
        inline double sin_small(double r) {
            const double r2 = r * r;
            double term = r, sum = r;
            for (int n = 1; n <= 7; n++) {
                term = -term * r2 / (double)((2 * n) * (2 * n + 1));
                sum = sum + term;
            }
            return sum;
        }

        inline double cos_small(double r) {
            const double r2 = r * r;
            double term = 1.0, sum = 1.0;
            for (int n = 1; n <= 8; n++) {
                term = -term * r2 / (double)((2 * n - 1) * (2 * n));
                sum = sum + term;
            }
            return sum;
        }

        // x as k quarter turns and the rest, r in [-pi/4, pi/4].
        inline double reduce(double x, int64_t& k) {
            const double q = x / half_pi_hi;
            k = (int64_t)(q >= 0.0 ? q + 0.5 : q - 0.5);
            return (x - (double)k * half_pi_hi) - (double)k * half_pi_lo;
        }

        inline double sin_d(double x) {
            int64_t k;
            const double r = reduce(x, k);
            switch (((k % 4) + 4) % 4) {
                case 0: return sin_small(r);
                case 1: return cos_small(r);
                case 2: return -sin_small(r);
                default: return -cos_small(r);
            }
        }

        inline double cos_d(double x) {
            int64_t k;
            const double r = reduce(x, k);
            switch (((k % 4) + 4) % 4) {
                case 0: return cos_small(r);
                case 1: return -sin_small(r);
                case 2: return -cos_small(r);
                default: return sin_small(r);
            }
        }

        // atan of x in [-tan(pi/12), tan(pi/12)]: its series to the x^23 term.
        inline double atan_small(double x) {
            const double x2 = x * x;
            double power = x, sum = x;
            for (int n = 1; n <= 11; n++) {
                power = -power * x2;
                sum = sum + power / (double)(2 * n + 1);
            }
            return sum;
        }

        inline double atan_d(double x) {
            const bool negative = x < 0.0;
            double a = negative ? -x : x;
            const bool inverted = a > 1.0;
            if (inverted) a = 1.0 / a;
            // atan(a) = pi/6 + atan((a*sqrt3 - 1) / (sqrt3 + a)), for a above tan(pi/12).
            const double result = a > tan_twelfth_pi ? sixth_pi + atan_small((a * sqrt3 - 1.0) / (sqrt3 + a)) : atan_small(a);
            const double whole = inverted ? half_pi_hi - result : result;
            return negative ? -whole : whole;
        }
    }

    namespace detail {
        inline bool in_range(float x) { return std::isfinite(x) && std::fabs(x) <= 1.0e6f; }
    }
    inline float sin(float x) { return detail::in_range(x) ? (float)detail::sin_d((double)x) : NAN; }
    inline float cos(float x) { return detail::in_range(x) ? (float)detail::cos_d((double)x) : NAN; }
    inline float tan(float x) { return detail::in_range(x) ? (float)(detail::sin_d((double)x) / detail::cos_d((double)x)) : NAN; }
    inline float atan(float x) { return (float)detail::atan_d((double)x); }
    // atan2's quadrants, from atan; (0, 0) gives 0, as the C library's does.
    inline float atan2(float y, float x) {
        if (!std::isfinite(x) || !std::isfinite(y)) return NAN;
        const double dy = (double)y, dx = (double)x;
        if (dx > 0.0) return (float)detail::atan_d(dy / dx);
        if (dx < 0.0) return (float)(detail::atan_d(dy / dx) + (dy < 0.0 ? -detail::pi : detail::pi));
        if (dy > 0.0) return (float)detail::half_pi_hi;
        if (dy < 0.0) return (float)-detail::half_pi_hi;
        return 0.0f;
    }
    inline float asin(float x) {
        const double d = (double)x;
        if (d >= 1.0) return (float)detail::half_pi_hi;
        if (d <= -1.0) return (float)-detail::half_pi_hi;
        return (float)detail::atan_d(d / std::sqrt(1.0 - d * d));
    }
}
