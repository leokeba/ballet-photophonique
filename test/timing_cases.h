// plan() and dutyCounts() (docs/DESIGN.md section 5.2).
#pragma once

#include "rig.h"

using photophonique::Clock;
using photophonique::dutyCounts;
using photophonique::plan;

TEST(timing_exact_on_apb) {
    // 80 MHz / (1000 Hz · 4096) = 19.53125: 5000 in eighths of 256ths.
    auto t = plan(1000, 12);
    CHECK(t.clock == Clock::Apb);
    CHECK_EQ(t.bits, 12);
    CHECK_EQ(t.divider, 5000u);
    CHECK(near(t.frequency, 1000, 1e-9));
    // ESP-IDF's own example: 5 kHz at 13 bits.
    t = plan(5000, 13);
    CHECK_EQ(t.divider, 500u);
    CHECK(near(t.frequency, 5000, 1e-9));
}

TEST(timing_fractional_hertz) {
    // Two outputs a half hertz apart beat at 0.5 Hz: at 8 bits the divider's
    // step near 440 Hz is about 0.002 Hz.
    auto a = plan(440, 8);
    auto b = plan(440.5, 8);
    CHECK(a.clock == Clock::Apb);
    CHECK(near(a.frequency, 440, 0.002));
    CHECK(near(b.frequency, 440.5, 0.002));
    CHECK(near(b.frequency - a.frequency, 0.5, 0.004));
    // At 16 bits the steps are about 0.6 Hz there: the nearest one.
    auto c = plan(440, 16);
    CHECK_EQ(c.bits, 16);
    CHECK(near(c.frequency, 440, 0.4));
}

TEST(timing_lowers_bits_when_too_fast) {
    // 1 MHz leaves 80 steps per period: 6 bits.
    auto t = plan(1e6, 12);
    CHECK_EQ(t.bits, 6);
    CHECK(t.clock == Clock::Apb);
    CHECK(near(t.frequency, 1e6, 1e6 * 0.002));
    // The top: 40 MHz at 1 bit (divider 1).
    t = plan(40e6, 20);
    CHECK_EQ(t.bits, 1);
    CHECK_EQ(t.divider, photophonique::kMinDivider);
    CHECK(near(t.frequency, 40e6, 1e-3));
}

TEST(timing_ref_tick_for_low_frequencies) {
    // 10 Hz at 8 bits: APB would need a divider of 31250, the 1 MHz tick
    // 390.6.
    auto t = plan(10, 8);
    CHECK(t.clock == Clock::RefTick);
    CHECK_EQ(t.bits, 8);
    CHECK(near(t.frequency, 10, 0.001));
}

TEST(timing_raises_bits_when_too_slow) {
    // 0.5 Hz at 4 bits is beyond both clocks: 11 bits on the tick reach it.
    auto t = plan(0.5, 4);
    CHECK_EQ(t.bits, 11);
    CHECK(t.clock == Clock::RefTick);
    CHECK(near(t.frequency, 0.5, 0.001));
    // The bottom: 0.001 Hz needs all 20 bits on the tick.
    t = plan(0.001, 1);
    CHECK_EQ(t.bits, 20);
    CHECK(t.clock == Clock::RefTick);
    CHECK(near(t.frequency, 0.001, 1e-5));
}

TEST(timing_every_request_lands_in_range) {
    for (double hz = photophonique::kMinHz; hz <= photophonique::kMaxHz; hz *= 1.37) {
        for (int bits = photophonique::kMinBits; bits <= photophonique::kMaxBits; bits++) {
            auto t = plan(hz, static_cast<uint8_t>(bits));
            if (!CHECK(t.divider >= photophonique::kMinDivider && t.divider <= photophonique::kMaxDivider)) return;
            if (!CHECK(t.bits >= photophonique::kMinBits && t.bits <= photophonique::kMaxBits)) return;
            // Within one divider step (1/256 of the divider, at most 1/256).
            if (!CHECK(fabs(t.frequency - hz) / hz < 1.0 / 256 + 1e-9)) return;
            // The resolution asked when the frequency allows it.
            double apbDiv = photophonique::kApbHz * 256 / (hz * (1u << bits));
            double tickDiv = photophonique::kRefTickHz * 256 / (hz * (1u << bits));
            bool fits = (apbDiv >= 256 && apbDiv <= photophonique::kMaxDivider) ||
                        (tickDiv >= 256 && tickDiv <= photophonique::kMaxDivider);
            if (fits && !CHECK_EQ(t.bits, bits)) return;
        }
    }
}

TEST(timing_clamps) {
    CHECK(plan(0, 8) == plan(photophonique::kMinHz, 8));
    CHECK(plan(1e9, 8) == plan(photophonique::kMaxHz, 8));
    CHECK(plan(1000, 0) == plan(1000, 1));
    CHECK(plan(1000, 30) == plan(1000, 20));
}

TEST(timing_duty_counts) {
    CHECK_EQ(dutyCounts(0, 8), 0u);
    CHECK_EQ(dutyCounts(100, 8), 256u);  // always on
    CHECK_EQ(dutyCounts(50, 8), 128u);
    CHECK_EQ(dutyCounts(50, 1), 1u);
    CHECK_EQ(dutyCounts(0.1f, 20), 1049u);
    CHECK_EQ(dutyCounts(-5, 8), 0u);
    CHECK_EQ(dutyCounts(250, 8), 256u);
    CHECK_EQ(dutyCounts(NAN, 8), 0u);
}
