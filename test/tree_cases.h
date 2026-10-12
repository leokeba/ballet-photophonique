// The /outputs tree (docs/DESIGN.md section 6): writes, ranges, persistence.
#pragma once

#include "rig.h"

using tesser::Status;

TEST(tree_declares_cleanly) {
    Rig r;
    CHECK_EQ(tesser::declarationErrors(), 0);
    CHECK(r.get("/outputs").status == Status::Ok);
    CHECK_EQ(r.get("/outputs/1/frequency").body, "1000");
    CHECK_EQ(r.get("/outputs/5/gpio").body, "2");
    CHECK_EQ(r.get("/outputs/3/applied/clock").body, "\"apb\"");
    CHECK(r.get("/outputs/6").status == Status::NotFound);
}

TEST(tree_writes_reach_the_pwm) {
    Rig r;
    CHECK(r.set("/outputs/4", R"({"on":true,"duty":12.5,"frequency":440.5,"resolution":8})").status == Status::Ok);
    CHECK_EQ(r.pwm.ch[3].timing.bits, 8);
    CHECK(near(r.pwm.ch[3].timing.frequency, 440.5, 0.002));
    CHECK_EQ(r.pwm.ch[3].counts, 32u);
    CHECK_EQ(r.get("/outputs/4/applied/bits").body, "8");
    // Frequency and resolution in one patch, whatever their order: no
    // combination is refused, the timer takes the nearest.
    CHECK(r.set("/outputs/4", R"({"frequency":2000000,"resolution":16})").status == Status::Ok);
    CHECK_EQ(r.get("/outputs/4/applied/bits").body, "5");
    CHECK_EQ(r.get("/outputs/4/resolution").body, "16");
}

TEST(tree_refuses_out_of_range) {
    Rig r;
    CHECK(r.set("/outputs/1/duty", "101").status == Status::InvalidValue);
    CHECK(r.set("/outputs/1/duty", "-1").status == Status::InvalidValue);
    CHECK(r.set("/outputs/1/resolution", "0").status == Status::InvalidValue);
    CHECK(r.set("/outputs/1/resolution", "21").status == Status::InvalidValue);
    CHECK(r.set("/outputs/1/frequency", "0").status == Status::InvalidValue);
    CHECK(r.set("/outputs/1/frequency", "50000000").status == Status::InvalidValue);
    CHECK(r.set("/outputs/1/applied/bits", "8").status == Status::ReadOnly);
    // A refused patch changes nothing.
    CHECK(r.set("/outputs/1", R"({"on":true,"duty":200})").status == Status::InvalidValue);
    CHECK_EQ(r.get("/outputs/1/on").body, "false");
}

TEST(tree_all_off) {
    Rig r;
    CHECK(r.set("/outputs", R"({"1":{"on":true},"2":{"on":true}})").status == Status::Ok);
    CHECK(r.request(tesser::Op::Set, "/outputs/off").status == Status::Ok);
    CHECK_EQ(r.get("/outputs/1/on").body, "false");
    CHECK_EQ(r.pwm.ch[1].counts, 0u);
}

TEST(tree_persists_and_restores) {
    std::string stored;
    {
        Rig r;
        CHECK(r.set("/outputs/2", R"({"on":true,"duty":33.3,"frequency":7000.25,"resolution":10})").status ==
              Status::Ok);
        stored = r.api.persistedState();
    }
    // The applied readouts aren't stored.
    CHECK(stored.find("applied") == std::string::npos);
    Rig r(false);
    std::vector<std::string> skipped;
    CHECK(r.api.restore(stored, &skipped) == Status::Ok);
    CHECK(skipped.empty());
    CHECK(r.outputs.begin());
    CHECK(r.out[1].on());
    CHECK(near(r.out[1].frequency(), 7000.25, 1e-9));
    CHECK_EQ(r.pwm.ch[1].timing.bits, 10);
    CHECK_EQ(r.pwm.ch[1].counts, photophonique::dutyCounts(33.3f, 10));
}
