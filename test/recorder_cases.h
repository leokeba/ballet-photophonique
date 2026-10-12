// The recorder and Standard MIDI files (docs/DESIGN.md section 5.6).
#pragma once

#include <map>

#include "midi_cases.h"

using photophonique::BendPoint;
using photophonique::Clip;
using photophonique::ClipEntry;
using photophonique::Note;
using photophonique::Recorder;
namespace smf = photophonique::smf;

namespace {

struct FakeClock : photophonique::PlayerClock {
    uint64_t us = 1000000;  // not 0: a start at 0 hides mistakes
    int wakes = 0;
    uint64_t nowUs() override { return us; }
    void wake() override { wakes++; }
};

struct FakeFiles : photophonique::ClipFiles {
    std::map<std::string, std::string> files;
    int writes = 0;
    std::vector<ClipEntry> list() override {
        std::vector<ClipEntry> out;
        for (auto& f : files) out.push_back({f.first, static_cast<uint32_t>(f.second.size())});
        return out;
    }
    bool read(const std::string& name, std::string& out) override {
        auto it = files.find(name);
        if (it == files.end()) return false;
        out = it->second;
        return true;
    }
    bool write(const std::string& name, std::string_view data) override {
        files[name] = std::string(data);
        writes++;
        return true;
    }
    bool remove(const std::string& name) override { return files.erase(name) > 0; }
    bool rename(const std::string& from, const std::string& to) override {
        auto it = files.find(from);
        if (it == files.end()) return false;
        files[to] = it->second;
        files.erase(from);
        return true;
    }
};

struct RecRig : MidiRig {
    FakeClock clock;
    FakeFiles files;
    Recorder rec{api, midi, files, clock};
    explicit RecRig(bool begin = true) {
        api.mount("recorder", rec).label("Recorder");
        if (begin) rec.begin();
    }
    // Runs the player as its task would, each wake-up at its microsecond,
    // until `ms` from now.
    void run(uint32_t ms) {
        uint64_t end = clock.us + ms * 1000ull;
        for (;;) {
            uint64_t wait = rec.poll();
            if (wait == Recorder::kIdle || clock.us + wait > end) break;
            clock.us += wait ? wait : 1;
        }
        clock.us = end;
        rec.poll();
    }
    void tick(uint32_t ms) { clock.us += ms * 1000ull; }  // time passes, the player asleep
    std::string call(std::string_view path, std::string_view body = "null") {
        auto r = set(path, body);
        return r.status == tesser::Status::Ok ? "ok" : r.body;
    }
    // A note on output 1 (C4, E4), 3 (G4), or none.
    bool sounds(int output) { return out[output - 1].on(); }
};

Clip sampleClip() {
    Clip c;
    c.length = 2000;
    c.bpm = 100;
    c.beats = 3;
    c.notes = {{0, 0, 500, 60, 100}, {0, 250, 250, 67, 64}, {0, 1000, 999, 64, 127}};
    c.bends = {{0, 100, 4096}, {0, 600, -8192}};
    return c;
}

// A track chunk from raw bytes.
std::string chunk(const char* id, const std::string& body) {
    std::string s(id, 4);
    uint32_t n = static_cast<uint32_t>(body.size());
    for (int sh = 24; sh >= 0; sh -= 8) s.push_back(static_cast<char>(n >> sh & 0xFF));
    return s + body;
}
std::string bytes(std::initializer_list<int> b) {
    std::string s;
    for (int v : b) s.push_back(static_cast<char>(v));
    return s;
}

}  // namespace

TEST(smf_round_trip) {
    Clip c = sampleClip();
    std::string file = smf::write(c);
    CHECK_EQ(file.substr(0, 4), "MThd");
    Clip back;
    CHECK(smf::read(file, back) == nullptr);
    CHECK_EQ(back.notes.size(), 3u);
    CHECK_EQ(back.bends.size(), 2u);
    for (size_t i = 0; i < 3; i++) {
        CHECK(near(back.notes[i].t, c.notes[i].t, 1));
        CHECK(near(back.notes[i].d, c.notes[i].d, 1));
        CHECK_EQ(back.notes[i].n, c.notes[i].n);
        CHECK_EQ(back.notes[i].v, c.notes[i].v);
    }
    CHECK_EQ(back.bends[0].v, 4096);
    CHECK_EQ(back.bends[1].v, -8192);
    CHECK(near(back.bends[1].t, 600, 1));
    CHECK(near(back.length, 2000, 1));
    CHECK(near(back.bpm, 100, 1e-3));
    CHECK_EQ(back.beats, 3);
    // Notes past the end stay, and so does the length: the "end" marker.
    c.length = 1500;
    CHECK(smf::read(smf::write(c), back) == nullptr);
    CHECK(near(back.length, 1500, 1));
    CHECK(near(back.notes[2].d, 999, 1));
}

TEST(smf_reads_a_type_1_file) {
    // 480 ticks per quarter; track 0: 3/4, 120 BPM, then 240 BPM from beat 2.
    std::string header = chunk("MThd", bytes({0, 1, 0, 2, 0x01, 0xE0}));
    std::string t0 = chunk("MTrk", bytes({0x00, 0xFF, 0x58, 0x04, 3, 2, 24, 8,         //
                                          0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,    // 500000 µs
                                          0x83, 0x60, 0xFF, 0x51, 0x03, 0x03, 0xD0, 0x90,  // at 480: 250000
                                          0x00, 0xFF, 0x2F, 0x00}));
    std::string t1 = chunk("MTrk", bytes({0x00, 0xC0, 5,                     // program change: skipped
                                          0x00, 0xF0, 0x03, 0x7E, 0x00, 0xF7,  // system exclusive: skipped
                                          0x00, 0x91, 60, 100,               // note on, channel 2
                                          0x83, 0x60, 60, 0,                 // at 480, running status, velocity 0: off
                                          0x00, 64, 90,                      // E4 on at 480
                                          0x83, 0x60, 0x81, 64, 0,           // at 960: off
                                          0x00, 0xE1, 0x00, 0x60,            // bend at 960
                                          0x83, 0x60, 0xFF, 0x2F, 0x00}));   // ends at 1440
    Clip c;
    CHECK(smf::read(header + t0 + t1, c) == nullptr);
    CHECK_EQ(c.notes.size(), 2u);
    CHECK_EQ(c.notes[0].n, 60);
    CHECK_EQ(c.notes[0].t, 0u);
    CHECK_EQ(c.notes[0].d, 500u);  // a beat at 120 BPM
    CHECK_EQ(c.notes[1].n, 64);
    CHECK_EQ(c.notes[1].t, 500u);
    CHECK_EQ(c.notes[1].d, 250u);  // a beat at 240 BPM
    CHECK_EQ(c.bends.size(), 1u);
    CHECK_EQ(c.bends[0].t, 750u);
    CHECK_EQ(c.bends[0].v, (0x60 << 7) - 8192);
    CHECK_EQ(c.length, 1000u);
    CHECK(near(c.bpm, 120, 1e-3));  // the first tempo
    CHECK_EQ(c.beats, 3);
}

TEST(smf_reads_smpte_time_and_ends_open_notes) {
    // 25 frames of 40 ticks: a tick is a millisecond. A note on on a note
    // already sounding ends it; a note never released ends with the track.
    std::string header = chunk("MThd", bytes({0, 0, 0, 1, 0xE7, 40}));
    std::string t = chunk("MTrk", bytes({0x00, 0x90, 60, 100,  //
                                         0x64, 0x90, 60, 80,   // at 100: again
                                         0x64, 0x90, 62, 1,    // at 200
                                         0x83, 0x60, 0xB0, 123, 0,  // at 680: all notes off
                                         0x64, 0x90, 65, 50,   // at 780, never released
                                         0x64, 0xFF, 0x2F, 0x00}));  // ends at 880
    Clip c;
    CHECK(smf::read(header + t, c) == nullptr);
    CHECK_EQ(c.notes.size(), 4u);
    CHECK_EQ(c.notes[0].d, 100u);
    CHECK_EQ(c.notes[1].t, 100u);
    CHECK_EQ(c.notes[1].d, 580u);  // to all notes off
    CHECK_EQ(c.notes[2].d, 480u);
    CHECK_EQ(c.notes[3].t, 780u);
    CHECK_EQ(c.notes[3].d, 100u);
    CHECK_EQ(c.length, 880u);
}

TEST(smf_refuses_what_it_cant_read) {
    Clip c = sampleClip();
    std::string good = smf::write(c);
    Clip out;
    CHECK(smf::read("", out) != nullptr);
    CHECK(smf::read("RIFF....", out) != nullptr);
    CHECK(smf::read(good.substr(0, good.size() - 5), out) != nullptr);  // truncated
    std::string type2 = good;
    type2[9] = 2;
    CHECK(smf::read(type2, out) != nullptr);
    std::string noStatus = chunk("MThd", bytes({0, 0, 0, 1, 0x01, 0xE0})) + chunk("MTrk", bytes({0x00, 60, 100}));
    CHECK(smf::read(noStatus, out) != nullptr);
    // Too many notes: refused, not cut.
    Clip big;
    big.length = 100000;
    for (uint32_t i = 0; i < photophonique::kMaxNotes + 1; i++) big.notes.push_back({0, i * 10, 5, 60, 100});
    CHECK(smf::read(smf::write(big), out) != nullptr);
    CHECK_EQ(out.notes.size(), 0u);  // untouched
}

TEST(recorder_takes_start_at_their_first_note) {
    RecRig r;
    CHECK_EQ(r.call("/recorder/record"), "ok");
    CHECK_EQ(r.get("/recorder/state").body, "\"armed\"");
    CHECK_EQ(r.get("/recorder/recording").body, "\"Take 1\"");
    r.tick(5000);  // silence: not recorded
    r.midi.input(0x80, 50, 0);  // a release from before: doesn't start it
    CHECK_EQ(r.get("/recorder/state").body, "\"armed\"");
    CHECK_EQ(r.send(msg(0x90, 60, 100)), "ok");
    CHECK_EQ(r.get("/recorder/state").body, "\"recording\"");
    CHECK(r.sounds(1));  // played as it is recorded
    r.tick(300);
    // A batch: E4 played 100 ms before the request, C4 released 40 ms before.
    CHECK_EQ(r.send("[[" + msg(0x90, 64, 80) + ",100],[" + msg(0x80, 60, 0) + ",40]]"), "ok");
    r.tick(200);
    CHECK_EQ(r.send(msg(0x80, 64, 0)), "ok");
    r.tick(100);
    CHECK_EQ(r.call("/recorder/stop"), "ok");
    const Clip& c = r.rec.clip();
    CHECK_EQ(c.notes.size(), 2u);
    CHECK_EQ(c.notes[0].n, 60);
    CHECK_EQ(c.notes[0].t, 0u);
    CHECK_EQ(c.notes[0].d, 260u);
    CHECK_EQ(c.notes[1].n, 64);
    CHECK_EQ(c.notes[1].t, 200u);
    CHECK_EQ(c.notes[1].d, 300u);
    CHECK_EQ(c.length, 600u);
    CHECK(r.files.files.count("Take 1") == 1);
    CHECK_EQ(r.get("/recorder/recordings").body, "[{\"name\":\"Take 1\",\"size\":" +
                                                     std::to_string(r.files.files["Take 1"].size()) + "}]");
    // The next take is a new recording; nothing heard, it is dropped.
    CHECK_EQ(r.call("/recorder/record"), "ok");
    CHECK_EQ(r.get("/recorder/recording").body, "\"Take 2\"");
    CHECK_EQ(r.call("/recorder/stop"), "ok");
    CHECK_EQ(r.get("/recorder/recording").body, "\"Take 1\"");
    CHECK_EQ(r.rec.clip().notes.size(), 2u);
    CHECK_EQ(r.files.files.size(), 1u);
}

TEST(recorder_thins_bend_and_ends_held_notes) {
    RecRig r;
    r.call("/recorder/record");
    r.midi.input(0xE0, 0, 0x40);  // centre: starts the take
    for (int i = 1; i <= 10; i++) {
        r.tick(3);
        r.midi.input(0xE0, 0, static_cast<uint8_t>(0x40 + i));
    }
    r.tick(20);
    r.midi.input(0x90, 60, 100);
    r.midi.input(0x90, 67, 90);
    r.tick(50);
    r.midi.input(0xB0, 123, 0);  // all notes off ends both
    r.midi.input(0x90, 64, 70);
    r.tick(100);
    r.call("/recorder/stop");  // ends E4
    const Clip& c = r.rec.clip();
    // 0, 3, 6, 9 → one point; 12…21 → another; 24, 27, 30 → a third.
    CHECK_EQ(c.bends.size(), 3u);
    CHECK_EQ(c.bends[0].t, 0u);
    CHECK_EQ(c.bends[0].v, 3 << 7);  // the last value within 10 ms
    CHECK_EQ(c.bends[1].t, 12u);
    CHECK_EQ(c.bends[2].v, 10 << 7);
    CHECK_EQ(c.notes.size(), 3u);
    CHECK_EQ(c.notes[0].d, 50u);
    CHECK_EQ(c.notes[1].d, 50u);
    CHECK_EQ(c.notes[2].n, 64);
    CHECK_EQ(c.notes[2].d, 100u);
}

TEST(recorder_replays_on_the_millisecond) {
    RecRig r;
    Clip c;
    c.length = 1000;
    c.notes = {{0, 100, 200, 60, 127}, {0, 300, 100, 60, 64}, {0, 500, 600, 67, 100}};  // the last runs past the end
    r.files.files["Song"] = smf::write(c);
    r.rec.begin();
    CHECK_EQ(r.call("/recorder/recording", "\"Song\""), "ok");
    CHECK_EQ(r.call("/recorder/play"), "ok");
    CHECK_EQ(r.get("/recorder/playing").body, "true");
    r.run(99);
    CHECK(!r.sounds(1));
    r.run(1);
    CHECK(r.sounds(1));
    CHECK(near(r.out[0].duty(), 100, 1e-3));
    r.run(199);
    CHECK(r.sounds(1));
    r.run(1);  // at 300: off, then on again (velocity 64)
    CHECK(r.sounds(1));
    CHECK(near(r.out[0].duty(), 50.394, 1e-3));
    r.run(100);
    CHECK(!r.sounds(1));
    r.run(100);
    CHECK(r.sounds(3));
    r.run(499);
    CHECK(r.sounds(3));
    r.run(1);  // the end: G4 cut, replay stops
    CHECK(!r.sounds(3));
    CHECK_EQ(r.get("/recorder/state").body, "\"stopped\"");
    // Each wake-up was asked for: the player sleeps between events.
    r.rec.loop = true;
    r.call("/recorder/play");
    r.run(1100);  // the end, then 100 into the second pass
    CHECK(r.sounds(1));
    CHECK_EQ(r.get("/recorder/position").body, "0");
    CHECK_EQ(r.get("/recorder/state").body, "\"playing\"");
}

TEST(recorder_stop_releases_only_its_notes) {
    RecRig r;
    Clip c;
    c.length = 1000;
    c.notes = {{0, 0, 900, 60, 100}};
    r.files.files["Song"] = smf::write(c);
    r.call("/recorder/new", "\"x\"");
    r.rec.choose("Song");
    r.call("/recorder/play");
    r.run(10);
    r.send(msg(0x90, 67, 100));  // played live, output 3
    CHECK(r.sounds(1));
    CHECK_EQ(r.call("/recorder/playing", "false"), "ok");
    CHECK(!r.sounds(1));
    CHECK(r.sounds(3));
}

TEST(recorder_starts_with_its_bend) {
    RecRig r;
    Clip c;
    c.length = 1000;
    c.notes = {{0, 0, 900, 69, 100}};
    c.bends = {{0, 500, 8191}};
    r.files.files["Song"] = smf::write(c);
    r.midi.outputs[69] = 2;
    r.rec.begin();
    r.rec.choose("Song");
    r.midi.message(0xE0, 0, 0);  // the wheel left down
    r.call("/recorder/play");
    r.run(0);
    CHECK(near(r.out[1].frequency(), 440, 1e-6));  // the centre: the recording has a bend
    r.run(500);
    CHECK(near(r.out[1].frequency(), 440 * pow(2.0, 2 / 12.0), 1e-3));
}

TEST(recorder_overdubs_across_the_loop) {
    RecRig r;
    Clip c;
    c.length = 1000;
    c.notes = {{0, 0, 100, 60, 100}};
    r.files.files["Loop"] = smf::write(c);
    r.rec.begin();
    r.rec.choose("Loop");
    r.rec.loop = true;
    r.rec.overdub = true;
    CHECK_EQ(r.call("/recorder/record"), "ok");
    CHECK_EQ(r.get("/recorder/state").body, "\"recording\"");
    r.run(400);
    r.send(msg(0x90, 67, 100));
    r.run(400);
    r.send(msg(0x80, 67, 0));
    r.run(100);
    r.send(msg(0x90, 64, 90));  // at 900, held past the end
    r.run(150);                 // into the second pass: ends at 1000
    CHECK(r.sounds(1));         // C4 replayed in the second pass
    r.run(150);
    // A note played 50 ms before the request: at 150.
    r.send("[[" + msg(0x90, 72, 80) + ",50]]");
    r.run(100);
    r.send(msg(0x80, 72, 0));
    CHECK_EQ(r.call("/recorder/stop"), "ok");
    const Clip& k = r.rec.clip();
    CHECK_EQ(k.length, 1000u);
    CHECK_EQ(k.notes.size(), 4u);
    CHECK_EQ(k.notes[0].n, 60);
    CHECK_EQ(k.notes[1].n, 72);
    CHECK_EQ(k.notes[1].t, 150u);
    CHECK_EQ(k.notes[1].d, 150u);
    CHECK_EQ(k.notes[2].n, 67);
    CHECK_EQ(k.notes[2].t, 400u);
    CHECK_EQ(k.notes[2].d, 400u);
    CHECK_EQ(k.notes[3].n, 64);
    CHECK_EQ(k.notes[3].d, 100u);
    // Saved at once.
    Clip saved;
    CHECK(smf::read(r.files.files["Loop"], saved) == nullptr);
    CHECK_EQ(saved.notes.size(), 4u);
}

TEST(recorder_edits) {
    RecRig r;
    CHECK_EQ(r.call("/recorder/new", "\"Piece\""), "ok");
    CHECK_EQ(r.get("/recorder/length").body, "8000");
    CHECK_EQ(r.call("/recorder/edit", R"({"add":[{"t":500,"d":250,"n":60,"v":90},{"t":0,"d":100,"n":67}],"addBends":[{"t":10,"v":-100}]})"),
             "ok");
    const Clip& c = r.rec.clip();
    CHECK_EQ(c.notes.size(), 2u);
    CHECK_EQ(c.notes[0].n, 67);  // sorted
    CHECK_EQ(c.notes[0].v, 100);  // the default velocity
    uint32_t g = c.notes[0].id, cn = c.notes[1].id, b = c.bends[0].id;
    std::string notes = r.get("/recorder/notes").body;
    CHECK(notes.find("{\"id\":" + std::to_string(cn) + ",\"t\":500,\"d\":250,\"n\":60,\"v\":90}") != std::string::npos);
    // Validated whole: one bad field changes nothing.
    CHECK(r.call("/recorder/edit", "{\"set\":[{\"id\":" + std::to_string(g) + ",\"t\":1000},{\"id\":" +
                                       std::to_string(cn) + ",\"n\":128}]}") != "ok");
    CHECK_EQ(c.notes[0].t, 0u);
    CHECK(r.call("/recorder/edit", "{\"set\":[{\"id\":" + std::to_string(b) + ",\"n\":3}]}") != "ok");
    CHECK(r.call("/recorder/edit", R"({"move":[]})") != "ok");
    CHECK(r.call("/recorder/edit", R"({"add":[{"t":0,"n":60}]})") != "ok");  // no duration
    CHECK_EQ(r.call("/recorder/edit", "{\"set\":[{\"id\":" + std::to_string(g) + ",\"t\":1000,\"v\":20},{\"id\":" +
                                          std::to_string(b) + ",\"v\":8191}],\"remove\":[" + std::to_string(cn) +
                                          ",99999]}"),
             "ok");
    CHECK_EQ(c.notes.size(), 1u);
    CHECK_EQ(c.notes[0].t, 1000u);
    CHECK_EQ(c.notes[0].v, 20);
    CHECK_EQ(c.bends[0].v, 8191);
    // Saved 2 s after the last edit.
    int writes = r.files.writes;
    r.run(1999);
    CHECK_EQ(r.files.writes, writes);
    r.run(1);
    CHECK_EQ(r.files.writes, writes + 1);
    Clip saved;
    CHECK(smf::read(r.files.files["Piece"], saved) == nullptr);
    CHECK_EQ(saved.notes.size(), 1u);
    // Length, tempo and metre, written to the file too.
    CHECK_EQ(r.call("/recorder", R"({"length":4000,"bpm":90,"beats":7})"), "ok");
    CHECK(r.call("/recorder/bpm", "10") != "ok");
    r.run(2000);
    CHECK(smf::read(r.files.files["Piece"], saved) == nullptr);
    CHECK_EQ(saved.length, 4000u);
    CHECK_EQ(saved.beats, 7);
    // Too many notes: refused.
    std::string add = "{\"add\":[";
    for (int i = 0; i < 1024; i++) add += std::string(i ? "," : "") + "{\"t\":0,\"d\":1,\"n\":1}";
    CHECK(r.call("/recorder/edit", add + "]}") != "ok");
    // Not while recording.
    r.call("/recorder/record");
    CHECK(r.call("/recorder/edit", R"({"remove":[]})") != "ok");
}

TEST(recorder_edits_during_replay) {
    RecRig r;
    r.call("/recorder/new", "\"Live\"");
    r.call("/recorder/edit", R"({"add":[{"t":0,"d":1000,"n":60},{"t":500,"d":200,"n":67}]})");
    uint32_t c4 = r.rec.clip().notes[0].id;
    r.call("/recorder/play");
    r.run(300);
    CHECK(r.sounds(1));
    // C4 shortened to end before the position: it ends at once.
    CHECK_EQ(r.call("/recorder/edit", "{\"set\":[{\"id\":" + std::to_string(c4) + ",\"d\":200}]}"), "ok");
    r.run(0);
    CHECK(!r.sounds(1));
    // A note added under the position waits for its next start; one ahead plays.
    CHECK_EQ(r.call("/recorder/edit", R"({"add":[{"t":100,"d":500,"n":64},{"t":400,"d":50,"n":60}]})"), "ok");
    r.run(50);
    CHECK(!r.sounds(1));
    r.run(50);  // 400
    CHECK(r.sounds(1));
    r.run(100);  // 500
    CHECK(r.sounds(3));
}

TEST(recorder_quantizes) {
    RecRig r;
    r.call("/recorder/new", "\"Q\"");  // 120 BPM: a beat is 500 ms
    r.call("/recorder/edit", R"({"add":[{"t":130,"d":240,"n":60},{"t":610,"d":100,"n":62},{"t":990,"d":30,"n":64}]})");
    uint32_t second = r.rec.clip().notes[1].id;
    CHECK(r.call("/recorder/quantize", R"({"grid":0})") != "ok");
    CHECK_EQ(r.call("/recorder/quantize", "{\"grid\":0.25,\"ids\":[" + std::to_string(second) + "]}"), "ok");
    const Clip& c = r.rec.clip();
    CHECK_EQ(c.notes[0].t, 130u);  // not named
    CHECK_EQ(c.notes[1].t, 625u);
    CHECK_EQ(c.notes[1].d, 100u);
    CHECK_EQ(r.call("/recorder/quantize", R"({"grid":0.25,"ends":true})"), "ok");
    CHECK_EQ(c.notes[0].t, 125u);
    CHECK_EQ(c.notes[0].d, 250u);
    CHECK_EQ(c.notes[2].t, 1000u);
    CHECK_EQ(c.notes[2].d, 125u);  // at least a grid step
}

TEST(recorder_recordings_through_the_tree) {
    RecRig r;
    CHECK_EQ(r.call("/recorder/new", "\"A\""), "ok");
    CHECK(r.call("/recorder/new", "\"A\"") != "ok");
    CHECK(r.call("/recorder/new", "\".hidden\"") != "ok");
    CHECK(r.call("/recorder/new", "\"a/b\"") != "ok");
    CHECK(r.call("/recorder/new", "\"12345678901234567890123456789\"") != "ok");
    CHECK_EQ(r.call("/recorder/new", "\"B\""), "ok");
    CHECK_EQ(r.get("/recorder/recording").body, "\"B\"");
    CHECK(r.call("/recorder/recording", "\"Nope\"") != "ok");
    CHECK_EQ(r.call("/recorder/recordings/A/rename", "\"Z\""), "ok");
    CHECK_EQ(r.get("/recorder/recordings").body.find("\"Z\"") != std::string::npos, true);
    CHECK(r.call("/recorder/recordings/Z/rename", "\"B\"") != "ok");
    // The current one renamed follows.
    CHECK_EQ(r.call("/recorder/recordings/B/rename", "\"C\""), "ok");
    CHECK_EQ(r.get("/recorder/recording").body, "\"C\"");
    // An upload replaces the current recording, once checked.
    Clip c = sampleClip();
    CHECK(r.rec.store("C", "nonsense") .status != tesser::Status::Ok);
    CHECK_EQ(r.rec.clip().notes.size(), 0u);
    CHECK(r.rec.store("C", smf::write(c)).isOk());
    CHECK_EQ(r.rec.clip().notes.size(), 3u);
    CHECK(r.rec.store("New one", smf::write(c)).isOk());
    CHECK_EQ(r.files.files.size(), 3u);
    // Cues: in the list's order (C, New one, Z).
    CHECK_EQ(r.call("/recorder/cue", "2"), "ok");
    CHECK_EQ(r.get("/recorder/recording").body, "\"New one\"");
    CHECK_EQ(r.get("/recorder/state").body, "\"playing\"");
    CHECK(r.call("/recorder/cue", "9") != "ok");
    CHECK_EQ(r.call("/recorder/cue", "0"), "ok");
    CHECK_EQ(r.get("/recorder/state").body, "\"stopped\"");
    // A gate held high doesn't restart: G4 (from 250) keeps sounding.
    r.call("/recorder/playing", "true");
    r.run(300);
    CHECK(r.sounds(3));
    r.call("/recorder/playing", "true");
    r.run(0);
    CHECK(r.sounds(3));
    // Deleting the current one leaves none.
    CHECK_EQ(r.call("/recorder/recordings/New one/delete"), "ok");
    CHECK_EQ(r.get("/recorder/recording").body, "\"\"");
    CHECK_EQ(r.get("/recorder/state").body, "\"stopped\"");
    CHECK(r.call("/recorder/play") != "ok");
    CHECK_EQ(r.files.files.size(), 2u);
}

TEST(recorder_persists_its_choice) {
    std::string stored;
    FakeFiles files;
    {
        RecRig r;
        r.call("/recorder/new", "\"Show\"");
        r.call("/recorder", R"({"loop":true,"overdub":true})");
        stored = r.api.persistedState();
        files = r.files;
    }
    CHECK(stored.find("notes") == std::string::npos);
    RecRig r(false);
    r.files = files;
    std::vector<std::string> skipped;
    CHECK(r.api.restore(stored, &skipped) == tesser::Status::Ok);
    CHECK(skipped.empty());
    r.rec.begin();
    CHECK_EQ(r.get("/recorder/recording").body, "\"Show\"");
    CHECK_EQ(r.rec.clip().length, 8000u);
    CHECK(r.rec.loop);
    CHECK(r.rec.overdub);
    // A stored name whose file is gone: none.
    RecRig gone(false);
    CHECK(gone.api.restore(stored) == tesser::Status::Ok);
    gone.rec.begin();
    CHECK_EQ(gone.get("/recorder/recording").body, "\"\"");
}

TEST(recorder_fills_up) {
    RecRig r;
    r.call("/recorder/record");
    for (int i = 0; i < 1100; i++) {
        r.midi.input(0x90, 60, 100);
        r.tick(1);
        r.midi.input(0x80, 60, 0);
        r.tick(1);
    }
    CHECK_EQ(r.rec.clip().notes.size(), photophonique::kMaxNotes);
    r.call("/recorder/stop");
    CHECK(r.files.files.count("Take 1") == 1);
}
