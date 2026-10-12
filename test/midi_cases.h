// MIDI onto the outputs (docs/DESIGN.md section 5.5).
#pragma once

#include "rig.h"

using photophonique::Midi;

namespace {

struct MidiRig : Rig {
    Midi midi{outputs};
    MidiRig() {
        api.mount("midi", midi).label("MIDI");
        midi.outputs[60] = 1;  // C4
        midi.outputs[64] = 1;  // E4, on the same output
        midi.outputs[67] = 3;  // G4
        midi.frequencies[67] = 1000;
    }
    std::string send(std::string_view body) {
        auto r = set("/midi/send", body);
        return r.status == tesser::Status::Ok ? "ok" : r.body;
    }
};

uint32_t pack(int status, int d1, int d2) { return static_cast<uint32_t>(status | d1 << 8 | d2 << 16); }
std::string msg(int status, int d1, int d2) { return std::to_string(pack(status, d1, d2)); }

}  // namespace

TEST(midi_note_on_sets_frequency_duty_and_on) {
    MidiRig r;
    r.midi.message(0x90, 60, 127);
    CHECK(r.out[0].on());
    CHECK(near(r.out[0].frequency(), 261.6256, 1e-3));
    CHECK(near(r.out[0].duty(), 100, 1e-4));
    CHECK_EQ(r.midi.voice(1), 60);
    // Velocity 64 is about half.
    r.midi.message(0x92, 67, 64);  // channel 3: channels are ignored
    CHECK(r.out[2].on());
    CHECK(near(r.out[2].frequency(), 1000, 1e-9));  // the key's own frequency
    CHECK(near(r.out[2].duty(), 50.394, 1e-3));
}

TEST(midi_unmapped_keys_do_nothing) {
    MidiRig r;
    r.midi.message(0x90, 61, 100);
    for (auto& o : r.out) CHECK(!o.on());
    // A key mapped beyond the outputs plays nothing either.
    r.midi.outputs[61] = 7;
    r.midi.message(0x90, 61, 100);
    for (auto& o : r.out) CHECK(!o.on());
}

TEST(midi_new_note_takes_the_output) {
    MidiRig r;
    r.midi.message(0x90, 60, 100);
    r.midi.message(0x90, 64, 100);  // E4 takes output 1 from C4
    CHECK_EQ(r.midi.voice(1), 64);
    CHECK(near(r.out[0].frequency(), 329.6276, 1e-3));
    // C4's release no longer turns it off.
    r.midi.message(0x80, 60, 0);
    CHECK(r.out[0].on());
    r.midi.message(0x90, 64, 0);  // velocity 0: note off
    CHECK(!r.out[0].on());
    CHECK_EQ(r.midi.voice(1), -1);
}

TEST(midi_note_off_follows_a_remapped_key) {
    MidiRig r;
    r.midi.message(0x90, 60, 100);
    r.midi.outputs[60] = 2;  // remapped while held
    r.midi.message(0x80, 60, 0);
    CHECK(!r.out[0].on());
}

TEST(midi_bend_frequency) {
    MidiRig r;
    r.midi.message(0x90, 69, 100);  // unmapped: nothing
    r.midi.outputs[69] = 2;
    r.midi.message(0x90, 69, 100);
    r.midi.message(0xE0, 0x7F, 0x7F);  // full up: 2 semitones
    CHECK(near(r.midi.bend(), 1, 1e-6));
    CHECK(near(r.out[1].frequency(), 440 * pow(2.0, 2 / 12.0), 1e-6));
    r.midi.message(0xE0, 0, 0);  // full down
    CHECK(near(r.out[1].frequency(), 440 * pow(2.0, -2 / 12.0), 1e-6));
    // A new note starts bent.
    r.midi.message(0x90, 60, 100);
    CHECK(near(r.out[0].frequency(), Midi::noteHz(60) * pow(2.0, -2 / 12.0), 1e-6));
    r.midi.message(0xE0, 0, 0x40);  // centre
    CHECK(near(r.out[1].frequency(), 440, 1e-9));
    CHECK(near(r.out[0].frequency(), Midi::noteHz(60), 1e-9));
}

TEST(midi_bend_duty) {
    MidiRig r;
    r.midi.bendTarget = Midi::Bend::Duty;
    r.midi.bendDuty = 30;
    r.midi.message(0x90, 60, 127);  // 100 %
    r.midi.message(0xE0, 0, 0);      // full down: -30 points
    CHECK(near(r.out[0].duty(), 70, 1e-4));
    CHECK(near(r.out[0].frequency(), Midi::noteHz(60), 1e-9));  // untouched
    r.midi.message(0xE0, 0x7F, 0x7F);  // full up: clamped
    CHECK(near(r.out[0].duty(), 100, 1e-4));
}

TEST(midi_all_notes_off) {
    MidiRig r;
    r.midi.message(0x90, 60, 100);
    r.midi.message(0x90, 67, 100);
    r.midi.message(0xB0, 123, 0);
    CHECK(!r.out[0].on());
    CHECK(!r.out[2].on());
    r.midi.message(0x90, 60, 100);
    CHECK(r.set("/midi/off", "null").status == tesser::Status::Ok);
    CHECK(!r.out[0].on());
}

TEST(midi_send_through_the_tree) {
    MidiRig r;
    CHECK_EQ(r.send(msg(0x90, 60, 100)), "ok");
    CHECK(r.out[0].on());
    CHECK_EQ(r.get("/outputs/1/on").body, "true");
    CHECK_EQ(r.get("/midi/keys/voices").body, "[60,-1,-1,-1,-1,-1,-1,-1]");
    // A batch, in order.
    CHECK_EQ(r.send("[" + msg(0x80, 60, 0) + "," + msg(0x90, 67, 127) + "," + msg(0xE0, 0x7F, 0x7F) + "]"), "ok");
    CHECK(!r.out[0].on());
    CHECK(r.out[2].on());
    CHECK_EQ(r.get("/midi/bend/value").body, "1");
    // Not messages: refused whole.
    CHECK(r.send("[" + msg(0x80, 67, 0) + ", 60]") != "ok");
    CHECK(r.out[2].on());
    CHECK(r.send("\"note\"") != "ok");
    CHECK(r.send("16777216") != "ok");
}

TEST(midi_keys_persist) {
    std::string stored;
    {
        MidiRig r;
        CHECK(r.set("/midi/keys/outputs/72", "5").status == tesser::Status::Ok);
        CHECK(r.set("/midi/keys/frequencies/72", "12345.5").status == tesser::Status::Ok);
        CHECK(r.set("/midi/bend", R"({"target":"duty","duty":20})").status == tesser::Status::Ok);
        CHECK(r.set("/midi/keys/outputs/72", "6").status == tesser::Status::InvalidValue);
        stored = r.api.persistedState();
    }
    CHECK(stored.find("voices") == std::string::npos);
    MidiRig r;
    std::vector<std::string> skipped;
    CHECK(r.api.restore(stored, &skipped) == tesser::Status::Ok);
    CHECK(skipped.empty());
    CHECK_EQ(r.midi.outputs[72], 5);
    CHECK(near(r.midi.frequencies[72], 12345.5, 1e-3));
    CHECK(r.midi.bendTarget == Midi::Bend::Duty);
    CHECK(near(r.midi.bendDuty, 20, 1e-6));
}
