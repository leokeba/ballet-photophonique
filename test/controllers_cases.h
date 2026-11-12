// MIDI control change (docs/DESIGN.md section 5.5): values, and mappings
// onto nodes of the tree.
#pragma once

#include "midi_cases.h"
#include "tesser/storage.h"

using photophonique::CcMapping;
using photophonique::Controllers;

namespace {

std::string cc(int controller, int value) { return msg(0xB0, controller, value); }

CcMapping mapping(int controller, const char* path, double min, double max) {
    CcMapping m;
    m.cc = static_cast<uint8_t>(controller);
    m.path = path;
    m.min = min;
    m.max = max;
    return m;
}

}  // namespace

TEST(cc_values_and_the_last_controller) {
    MidiRig r;
    CHECK(r.get("/midi/cc/last").body == "-1");
    CHECK(r.send(cc(7, 100)) == "ok");
    CHECK(r.send("[" + cc(1, 5) + "," + cc(7, 101) + "]") == "ok");
    CHECK(r.midi.controllers.values()[7] == 101);
    CHECK(r.midi.controllers.values()[1] == 5);
    CHECK(r.midi.controllers.values()[2] == -1);
    CHECK(r.get("/midi/cc/last").body == "7");
    // Channel mode messages aren't controllers; 123 still ends the notes.
    CHECK(r.send(msg(0x90, 60, 100)) == "ok");
    CHECK(r.send(cc(123, 0)) == "ok");
    CHECK(!r.out[0].on());
    CHECK(r.get("/midi/cc/last").body == "7");
}

TEST(cc_mapped_values) {
    using K = Controllers::Kind;
    CcMapping m = mapping(1, "/x", 100, 1100);
    CHECK(Controllers::mapped(m, 0, K::Number) == "100");
    CHECK(Controllers::mapped(m, 127, K::Number) == "1100");
    CHECK(Controllers::mapped(m, 127, K::Integer) == "1100");
    CHECK(Controllers::mapped(m, 64, K::Integer) == "604");  // 100 + 64/127 · 1000
    CHECK(Controllers::mapped(m, 64, K::Boolean) == "true");
    CHECK(Controllers::mapped(m, 63, K::Boolean) == "false");
    // A curve: finer near min.
    m.curve = 2;
    CHECK(Controllers::mapped(m, 127, K::Number) == "1100");
    CHECK(Controllers::mapped(m, 0, K::Number) == "100");
    CHECK(Controllers::mapped(m, 64, K::Integer) == "354");  // 100 + (64/127)² · 1000
    // Centred: both ways on one controller, 64 the middle.
    CcMapping v = mapping(1, "/x", -2000, 2000);
    v.centred = true;
    CHECK(Controllers::mapped(v, 64, K::Number) == "0");
    CHECK(Controllers::mapped(v, 0, K::Number) == "-2000");
    CHECK(Controllers::mapped(v, 127, K::Number) == "2000");
    CHECK(Controllers::mapped(v, 96, K::Integer) == "1016");  // 32/63 of the way up
    CHECK(Controllers::mapped(v, 32, K::Integer) == "-1000");
    v.deadband = 2;
    CHECK(Controllers::mapped(v, 66, K::Number) == "0");
    CHECK(Controllers::mapped(v, 62, K::Number) == "0");
    CHECK(Controllers::mapped(v, 67, K::Integer) == "33");  // 1/61
    CHECK(Controllers::mapped(v, 127, K::Number) == "2000");
    CHECK(Controllers::mapped(v, 0, K::Number) == "-2000");
    v.curve = 2;
    CHECK(Controllers::mapped(v, 127, K::Number) == "2000");
    CHECK(Controllers::mapped(v, 0, K::Number) == "-2000");
    CHECK(Controllers::mapped(v, 3, K::Integer) == "-1811");  // -(59/62)² of 2000
    // A gate.
    m.threshold = 100;
    CHECK(Controllers::mapped(m, 99, K::Number) == "false");
    CHECK(Controllers::mapped(m, 100, K::Number) == "true");
}

TEST(cc_mapping_writes_a_node_unsaved) {
    MidiRig r;
    tesser::MemoryStorage storage;
    r.api.persistence(storage, 100);
    r.api.poll(10000);
    CHECK(r.set("/midi/cc/mappings",
                R"([{"cc": 1, "path": "/outputs/2/frequency", "min": 100, "max": 1100},
                    {"cc": 1, "path": "/outputs/2/on", "threshold": 1},
                    {"cc": 2, "path": "/outputs/2/resolution", "min": 8, "max": 16}])")
              .status == tesser::Status::Ok);
    r.api.poll(10100);
    r.api.poll(11000);
    CHECK(storage.saves == 1);  // the mappings themselves are saved
    int saves = storage.saves;
    CHECK(r.send(cc(1, 127)) == "ok");
    CHECK(near(r.out[1].frequency(), 1100, 1e-9));
    CHECK(r.out[1].on());
    CHECK(r.send(cc(2, 64)) == "ok");
    CHECK(r.out[1].resolution() == 12);  // 8 + 64/127 · 8, rounded for an integer node
    uint32_t writes = r.midi.controllers.writes();
    CHECK(r.send(cc(1, 127)) == "ok");  // nothing changed: nothing written
    CHECK(r.midi.controllers.writes() == writes);
    CHECK(r.send(cc(1, 0)) == "ok");
    CHECK(near(r.out[1].frequency(), 100, 1e-9));
    CHECK(!r.out[1].on());
    CHECK(r.midi.controllers.writes() == writes + 2);
    r.api.poll(12000);
    r.api.poll(13000);
    CHECK(storage.saves == saves);  // the outputs' record isn't rewritten
    CHECK(r.midi.controllers.error().empty());
}

TEST(cc_mapping_errors_and_access) {
    MidiRig r;
    int admin = 0;
    auto& secret = r.api.object("secret").writeAccess(tesser::Access::Admin);
    secret.value("level", admin);
    CHECK(r.set("/midi/cc/mappings", R"([{"cc": 3, "path": "/secret/level", "max": 100},
                                         {"cc": 4, "path": "/nothing/here"}])")
              .status == tesser::Status::Ok);
    CHECK(r.send(cc(3, 127)) == "ok");
    r.midi.controllers.tick(0);
    CHECK(admin == 0);  // a mapping writes as User: /midi is User's
    CHECK(r.midi.controllers.error().find("/secret/level") == 0);
    CHECK(r.send(cc(4, 127)) == "ok");
    r.midi.controllers.tick(0);
    CHECK(r.midi.controllers.error().find("/nothing/here") == 0);
    CHECK(r.midi.controllers.failures() == 2);
    // Validated: a path's length, a rest value's JSON.
    CHECK(r.set("/midi/cc/mappings", R"([{"cc": 3, "path": "/a", "rest": "{nope"}])").status != tesser::Status::Ok);
    CHECK(r.set("/midi/cc/mappings", R"([{"cc": 120, "path": "/a"}])").status != tesser::Status::Ok);
}

TEST(cc_rest_on_all_notes_off) {
    MidiRig r;
    CHECK(r.set("/midi/cc/mappings", R"([{"cc": 10, "path": "/outputs/4/duty", "max": 100, "rest": "0"},
                                         {"cc": 11, "path": "/outputs/5/duty", "max": 100}])")
              .status == tesser::Status::Ok);
    CHECK(r.send("[" + cc(10, 127) + "," + cc(11, 127) + "]") == "ok");
    CHECK(near(r.out[3].duty(), 100, 1e-6));
    CHECK(r.set("/midi/off", "null").status == tesser::Status::Ok);
    CHECK(near(r.out[3].duty(), 0, 1e-6));
    CHECK(near(r.out[4].duty(), 100, 1e-6));  // no rest value: left as it is
    // The controller moving to where it was writes again.
    CHECK(r.send(cc(10, 127)) == "ok");
    CHECK(near(r.out[3].duty(), 100, 1e-6));
    CHECK(r.send(cc(120, 0)) == "ok");  // all sound off
    CHECK(near(r.out[3].duty(), 0, 1e-6));
}

TEST(cc_renews_for_deadmen) {
    MidiRig r;
    CHECK(r.set("/midi/cc/mappings", R"([{"cc": 1, "path": "/outputs/1/duty", "max": 100, "renew": 400},
                                         {"cc": 1, "path": "/outputs/2/duty", "max": 100}])")
              .status == tesser::Status::Ok);
    r.midi.controllers.tick(1000);
    CHECK(r.midi.controllers.writes() == 0);  // nothing to renew before a first value
    CHECK(r.send(cc(1, 64)) == "ok");
    uint32_t w = r.midi.controllers.writes();
    CHECK(w == 2);
    r.midi.controllers.tick(1399);
    CHECK(r.midi.controllers.writes() == w);
    r.midi.controllers.tick(1400);
    CHECK(r.midi.controllers.writes() == w + 1);  // only the mapping that renews
    r.midi.controllers.tick(1500);
    CHECK(r.midi.controllers.writes() == w + 1);
    r.midi.controllers.tick(1800);
    CHECK(r.midi.controllers.writes() == w + 2);
    // Changed mappings forget what they wrote.
    CHECK(r.set("/midi/cc/mappings/0/renew", "200").status == tesser::Status::Ok);
    r.midi.controllers.tick(3000);
    CHECK(r.midi.controllers.writes() == w + 2);
}

TEST(cc_mappings_persist) {
    std::string stored;
    {
        MidiRig r;
        CHECK(r.set("/midi/cc/mappings", R"([{"cc": 74, "slot": 2, "path": "/stepper/velocity", "min": -51200,
                                             "max": 51200, "centred": true, "deadband": 2, "renew": 400,
                                             "rest": "0"}])")
                  .status == tesser::Status::Ok);
        stored = r.api.persistedState();
    }
    MidiRig r;
    CHECK(r.api.restore(stored) == tesser::Status::Ok);
    CHECK(r.midi.controllers.mappings.size() == 1);
    const CcMapping& m = r.midi.controllers.mappings[0];
    CHECK(m.cc == 74 && m.slot == 2 && m.path == "/stepper/velocity" && m.centred && m.deadband == 2);
    CHECK(m.renew == 400 && m.rest == "0" && m.min == -51200);
    // No ensemble here: the slot's writes fail and say so.
    CHECK(r.send(cc(74, 127)) == "ok");
    CHECK(r.midi.controllers.error().find("slot 2") == 0);
}

TEST(cc_refused_rest_is_reported) {
    MidiRig r;
    CHECK(r.set("/midi/cc/mappings", R"([{"cc": 74, "path": "/outputs/1/frequency", "min": 100, "max": 2000,
                                         "rest": "0"}])")
              .status == tesser::Status::Ok);
    CHECK(r.send(cc(74, 127)) == "ok");
    CHECK(r.set("/midi/off", "null").status == tesser::Status::Ok);
    r.midi.controllers.tick(0);
    CHECK(near(r.out[0].frequency(), 2000, 1e-6));  // 0 Hz is out of range: refused
    CHECK_EQ(r.midi.controllers.error().substr(0, 21), std::string("/outputs/1/frequency:"));
}

TEST(cc_errors_stand_per_mapping) {
    MidiRig r;
    CHECK(r.set("/midi/cc/mappings", R"([{"cc": 1, "slot": 2, "path": "/stepper/velocity", "renew": 400},
                                         {"cc": 2, "path": "/nothing/here"},
                                         {"cc": 3, "path": "/outputs/1/duty", "max": 100}])")
              .status == tesser::Status::Ok);
    CHECK(r.send(cc(1, 64)) == "ok");
    CHECK(r.midi.controllers.error().find("slot 2") == 0);
    CHECK(r.send(cc(2, 64)) == "ok");
    r.midi.controllers.tick(10);
    CHECK(r.midi.controllers.error().find("/nothing/here") == 0);
    // The slot's renewal failing again doesn't take `error` back: it is
    // the same failure, still standing (and toasted once).
    r.midi.controllers.tick(400);
    r.midi.controllers.tick(800);
    CHECK(r.midi.controllers.failures() == 4);
    CHECK(r.midi.controllers.error().find("/nothing/here") == 0);
    // A mapping that works again: the other failure shows.
    CHECK(r.set("/midi/cc/mappings/1/path", "\"/outputs/2/duty\"").status == tesser::Status::Ok);
    CHECK(r.midi.controllers.error().empty());  // changed mappings start afresh
    CHECK(r.send(cc(2, 10)) == "ok");
    r.midi.controllers.tick(900);
    CHECK(r.midi.controllers.error().empty());
    CHECK(r.send(cc(1, 10)) == "ok");
    CHECK(r.midi.controllers.error().find("slot 2") == 0);
}
