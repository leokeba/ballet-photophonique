#include "photophonique/timing.h"

#include <math.h>

#include <initializer_list>

namespace photophonique {

namespace {

// The divider for `hz` at `bits` from `clock`, rounded to the nearest step,
// or 0 when it is out of the hardware's range.
uint32_t divider(Clock clock, double hz, uint8_t bits) {
    double d = clockHz(clock) * (1 << kFractionBits) / (hz * static_cast<double>(1u << bits));
    double rounded = floor(d + 0.5);
    if (rounded < kMinDivider || rounded > kMaxDivider) return 0;
    return static_cast<uint32_t>(rounded);
}

bool fit(double hz, uint8_t bits, Timing& t) {
    for (Clock c : {Clock::Apb, Clock::RefTick}) {
        uint32_t d = divider(c, hz, bits);
        if (!d) continue;
        t.clock = c;
        t.divider = d;
        t.bits = bits;
        t.frequency = clockHz(c) * (1 << kFractionBits) / (static_cast<double>(d) * static_cast<double>(1u << bits));
        return true;
    }
    return false;
}

}  // namespace

double clockHz(Clock c) { return c == Clock::Apb ? kApbHz : kRefTickHz; }

Timing plan(double hz, uint8_t bits) {
    hz = hz < kMinHz ? kMinHz : hz > kMaxHz ? kMaxHz : hz;
    bits = bits < kMinBits ? kMinBits : bits > kMaxBits ? kMaxBits : bits;
    Timing t;
    if (fit(hz, bits, t)) return t;
    // Too fast for this resolution when even the fastest clock's divider is
    // under 1; else too slow.
    bool tooFast = kApbHz / (hz * static_cast<double>(1u << bits)) < 1;
    if (tooFast) {
        for (int b = bits - 1; b >= kMinBits; b--) {
            if (fit(hz, static_cast<uint8_t>(b), t)) return t;
        }
    } else {
        for (int b = bits + 1; b <= kMaxBits; b++) {
            if (fit(hz, static_cast<uint8_t>(b), t)) return t;
        }
    }
    // Unreachable within kMinHz..kMaxHz; the closest end otherwise.
    fit(tooFast ? kMaxHz : kMinHz, tooFast ? kMinBits : kMaxBits, t);
    return t;
}

uint32_t dutyCounts(float percent, uint8_t bits) {
    uint32_t full = 1u << bits;
    if (!(percent > 0)) return 0;  // NaN too
    if (percent >= 100) return full;
    auto c = static_cast<uint32_t>(floor(static_cast<double>(percent) / 100.0 * full + 0.5));
    return c > full ? full : c;
}

}  // namespace photophonique
