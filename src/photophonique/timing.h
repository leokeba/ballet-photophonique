#pragma once

// An output's timer settings (docs/DESIGN.md section 5.2): the classic ESP32's
// LEDC divides a source clock by an 18-bit divider with 8 fractional bits,
// then counts 2^bits steps per period. So frequency and resolution share one
// budget: clock / (divider · 2^bits) = frequency, with 1 ≤ divider < 1024.
// plan() finds the settings closest to what was asked; platform-free.

#include <stdint.h>

namespace photophonique {

enum class Clock : uint8_t {
    Apb,      // 80 MHz
    RefTick,  // 1 MHz: lower frequencies at a given resolution
};

constexpr double kApbHz = 80e6;
constexpr double kRefTickHz = 1e6;
constexpr int kFractionBits = 8;
constexpr uint32_t kMinDivider = 1u << kFractionBits;  // 1.0
constexpr uint32_t kMaxDivider = 0x3FFFF;               // 1023.996
constexpr uint8_t kMinBits = 1;
constexpr uint8_t kMaxBits = 20;
// Every frequency in this range has a resolution that reaches it.
constexpr double kMinHz = 0.001;
constexpr double kMaxHz = 40e6;

double clockHz(Clock c);

struct Timing {
    Clock clock = Clock::Apb;
    uint32_t divider = kMinDivider;  // fixed point, 8 fractional bits
    uint8_t bits = kMinBits;
    double frequency = 0;  // what the divider gives, not what was asked

    bool operator==(const Timing& o) const {
        return clock == o.clock && divider == o.divider && bits == o.bits;
    }
    bool operator!=(const Timing& o) const { return !(*this == o); }
};

// The settings for `hz` at `bits` of resolution: APB if its divider fits,
// else the reference tick. When neither does, the nearest resolution that
// can: fewer bits for a frequency too high, more for one too low. `hz` and
// `bits` are clamped to their ranges first.
Timing plan(double hz, uint8_t bits);

// The compare value for `percent` (0–100) of a period at `bits`: 0 to 2^bits,
// 2^bits being always on.
uint32_t dutyCounts(float percent, uint8_t bits);

}  // namespace photophonique
