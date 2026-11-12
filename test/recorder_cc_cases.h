// The recorder's controller points (docs/DESIGN.md section 5.6): recorded,
// replayed through the mappings, edited, in Standard MIDI files.
#pragma once

#include "recorder_cases.h"

using photophonique::CcPoint;

TEST(smf_round_trip_with_controllers) {
    Clip c = sampleClip();
    c.ccs = {{0, 0, 1, 10}, {0, 500, 74, 127}, {0, 500, 1, 0}};
    Clip back;
    CHECK(smf::read(smf::write(c), back) == nullptr);
    CHECK_EQ(back.ccs.size(), 3u);
    CHECK_EQ(back.ccs[0].c, 1);
    CHECK_EQ(back.ccs[0].v, 10);
    CHECK(near(back.ccs[1].t, 500, 1));
    CHECK_EQ(back.ccs[1].c, 74);
    CHECK_EQ(back.ccs[1].v, 127);
    CHECK_EQ(back.ccs[2].c, 1);  // the file's order at one time
    CHECK_EQ(back.bends.size(), 2u);
    CHECK_EQ(back.notes.size(), 3u);
    // From elsewhere: any channel; 121 (reset all controllers) isn't a point;
    // a file of controllers alone has their length.
    std::string track = bytes({0x00, 0xB3, 7, 100, 0x83, 0x60, 0xB3, 7, 50, 0x00, 121, 0, 0x00, 0xFF, 0x2F, 0x00});
    std::string file = chunk("MThd", bytes({0, 0, 0, 1, 0x01, 0xE0})) + chunk("MTrk", track);
    CHECK(smf::read(file, back) == nullptr);
    CHECK_EQ(back.ccs.size(), 2u);
    CHECK_EQ(back.ccs[1].v, 50);
    CHECK(near(back.ccs[1].t, 500, 1));  // 480 ticks at 120 BPM
    CHECK(near(back.length, 500, 1));
}

TEST(recorder_records_controllers) {
    RecRig r;
    r.call("/recorder/record");
    r.tick(1000);
    r.midi.input(0xB0, 74, 64);  // a controller starts the take
    CHECK_EQ(r.get("/recorder/state").body, "\"recording\"");
    for (int i = 1; i <= 5; i++) {
        r.tick(3);
        r.midi.input(0xB0, 74, static_cast<uint8_t>(64 + i));
    }
    r.tick(20);                   // at 35
    r.midi.input(0xB0, 1, 10);    // another controller, its own points
    r.tick(20);                   // at 55
    r.midi.input(0xB0, 74, 69);   // the value it has: no point
    r.midi.input(0xB0, 121, 0);   // not a controller
    r.tick(20);                   // at 75
    r.midi.input(0xB0, 74, 0);
    r.tick(25);
    r.call("/recorder/stop");
    const Clip& c = r.rec.clip();
    // 0, 3, 6, 9 thinned into one point; 12, 15 another.
    CHECK_EQ(c.ccs.size(), 4u);
    CHECK_EQ(c.ccs[0].t, 0u);
    CHECK_EQ(c.ccs[0].v, 67);
    CHECK_EQ(c.ccs[1].t, 12u);
    CHECK_EQ(c.ccs[1].v, 69);
    CHECK_EQ(c.ccs[2].c, 1);
    CHECK_EQ(c.ccs[2].t, 35u);
    CHECK_EQ(c.ccs[3].t, 75u);
    CHECK_EQ(c.ccs[3].v, 0);
    CHECK_EQ(c.length, 100u);
    CHECK(r.get("/recorder/ccs").body.find("\"c\":74") != std::string::npos);
}

TEST(recorder_replays_controllers_through_their_mappings) {
    RecRig r;
    CHECK(r.set("/midi/cc/mappings", R"([{"cc": 74, "path": "/outputs/2/duty", "max": 100, "rest": "50"}])").status ==
          tesser::Status::Ok);
    Clip c;
    c.length = 1000;
    c.ccs = {{0, 200, 74, 127}, {0, 600, 74, 0}};
    r.files.files["Song"] = smf::write(c);
    r.rec.begin();
    r.rec.choose("Song");
    r.midi.message(0xB0, 74, 64);
    CHECK(near(r.out[1].duty(), 50.394, 1e-3));
    r.rec.loop = true;
    r.call("/recorder/play");
    r.run(0);
    CHECK(near(r.out[1].duty(), 100, 1e-3));  // before its first point, a controller has that point's value
    r.run(599);
    CHECK(near(r.out[1].duty(), 100, 1e-3));
    r.run(1);
    CHECK(near(r.out[1].duty(), 0, 1e-3));
    r.run(400);  // the next pass starts where the first did
    CHECK(near(r.out[1].duty(), 100, 1e-3));
    CHECK_EQ(r.midi.controllers.values()[74], 127);
    r.run(700);
    CHECK(near(r.out[1].duty(), 0, 1e-3));
    // Replay's stop: the mapping's rest value.
    r.call("/recorder/stop");
    CHECK(near(r.out[1].duty(), 50, 1e-3));
    // A stop with nothing replayed writes no rest.
    r.midi.message(0xB0, 74, 127);
    r.call("/recorder/stop");
    CHECK(near(r.out[1].duty(), 100, 1e-3));
}

TEST(recorder_edits_controllers) {
    RecRig r;
    r.call("/recorder/new", "\"x\"");
    CHECK_EQ(r.call("/recorder/edit", R"({"addCcs": [{"t": 100, "c": 74, "v": 10}, {"t": 50, "c": 1, "v": 0}]})"),
             "ok");
    const Clip& c = r.rec.clip();
    CHECK_EQ(c.ccs.size(), 2u);
    CHECK_EQ(c.ccs[0].c, 1);  // by time
    uint32_t id = c.ccs[1].id;
    CHECK_EQ(r.call("/recorder/edit", "{\"set\": [{\"id\": " + std::to_string(id) + ", \"t\": 10, \"v\": 127, \"c\": 7}]}"),
             "ok");
    CHECK_EQ(c.ccs[0].c, 7);
    CHECK_EQ(c.ccs[0].v, 127);
    CHECK_EQ(c.ccs[0].t, 10u);
    // Refused whole: a value out of range, a controller that isn't one, a
    // note's field on a controller point.
    CHECK(r.call("/recorder/edit", R"({"addCcs": [{"t": 1, "c": 1, "v": 128}]})") != "ok");
    CHECK(r.call("/recorder/edit", R"({"addCcs": [{"t": 1, "c": 120, "v": 0}]})") != "ok");
    CHECK(r.call("/recorder/edit", R"({"addCcs": [{"t": 1, "v": 0}]})") != "ok");
    CHECK(r.call("/recorder/edit", "{\"set\": [{\"id\": " + std::to_string(id) + ", \"n\": 60}]}") != "ok");
    CHECK_EQ(c.ccs.size(), 2u);
    CHECK_EQ(r.call("/recorder/edit", "{\"remove\": [" + std::to_string(id) + "]}"), "ok");
    CHECK_EQ(c.ccs.size(), 1u);
    // Saved with the recording.
    r.run(Recorder::kSaveDelayMs);
    Clip back;
    CHECK(smf::read(r.files.files["x"], back) == nullptr);
    CHECK_EQ(back.ccs.size(), 1u);
    CHECK_EQ(back.ccs[0].c, 1);
}
