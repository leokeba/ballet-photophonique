// Output and Outputs on the fake PWM (docs/DESIGN.md section 5).
#pragma once

#include "rig.h"

TEST(output_begins_low_on_its_pin) {
    Rig r;
    for (int i = 0; i < 5; i++) {
        CHECK(r.pwm.ch[i].attached);
        CHECK_EQ(r.pwm.ch[i].pin, kPins[i]);
        CHECK(r.pwm.ch[i].timing == photophonique::plan(1000, 12));
        CHECK_EQ(r.pwm.ch[i].counts, 0u);  // off by default
    }
}

TEST(output_stores_until_begun) {
    Rig r(false);
    r.out[0].setFrequency(20000);
    r.out[0].setOn(true);
    CHECK_EQ(r.pwm.ch[0].timings, 0);
    CHECK_EQ(r.pwm.ch[0].duties, 0);
    CHECK(r.outputs.begin());
    CHECK(near(r.pwm.ch[0].timing.frequency, 20000, 1));
    // 20 kHz leaves 4000 steps per period: 11 bits, not the default 12.
    CHECK_EQ(r.pwm.ch[0].timing.bits, 11);
    CHECK_EQ(r.pwm.ch[0].counts, 1024u);  // 50 %
}

TEST(output_on_and_duty) {
    Rig r;
    r.out[1].setOn(true);
    CHECK_EQ(r.pwm.ch[1].counts, 2048u);
    r.out[1].setDuty(25);
    CHECK_EQ(r.pwm.ch[1].counts, 1024u);
    r.out[1].setDuty(100);
    CHECK_EQ(r.pwm.ch[1].counts, 4096u);
    r.out[1].setOn(false);
    CHECK_EQ(r.pwm.ch[1].counts, 0u);
    // The others untouched.
    CHECK_EQ(r.pwm.ch[0].duties, 1);
}

TEST(output_retimes_and_rescales_duty) {
    Rig r;
    r.out[2].setOn(true);
    int timings = r.pwm.ch[2].timings;
    r.out[2].setResolution(8);
    CHECK_EQ(r.pwm.ch[2].timing.bits, 8);
    CHECK_EQ(r.pwm.ch[2].counts, 128u);  // still 50 %
    CHECK_EQ(r.pwm.ch[2].timings, timings + 1);
    // A frequency the resolution can't reach lowers it, and the duty follows.
    r.out[2].setFrequency(1e6);
    CHECK_EQ(r.out[2].timing().bits, 6);
    CHECK_EQ(r.out[2].resolution(), 8);  // as asked
    CHECK_EQ(r.pwm.ch[2].counts, 32u);
    // Back down: the asked resolution again.
    r.out[2].setFrequency(1000);
    CHECK_EQ(r.out[2].timing().bits, 8);
    // The same timing isn't written again.
    timings = r.pwm.ch[2].timings;
    r.out[2].setFrequency(1000.0001);
    CHECK_EQ(r.pwm.ch[2].timings, timings);
}

TEST(output_all_off) {
    Rig r;
    for (auto& o : r.out) o.setOn(true);
    r.outputs.allOff();
    for (int i = 0; i < 5; i++) {
        CHECK(!r.out[i].on());
        CHECK_EQ(r.pwm.ch[i].counts, 0u);
    }
}

TEST(output_reports_a_refused_pin) {
    Rig r(false);
    r.pwm.refuse = true;
    CHECK(!r.outputs.begin());
    r.out[0].setOn(true);  // not begun: nothing written
    CHECK_EQ(r.pwm.ch[0].duties, 0);
}
