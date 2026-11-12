#pragma once

// A recording in memory (docs/DESIGN.md section 5.6): its notes, pitch bend
// points and controller points (control change), in milliseconds from its
// start, its length, and its tempo and metre, which only the grid,
// quantizing and its file use.

#include <stdint.h>

#include <vector>

namespace photophonique {

struct Note {
    uint32_t id = 0;
    uint32_t t = 0;  // start, ms
    uint32_t d = 1;  // duration, ms
    uint8_t n = 60;  // pitch
    uint8_t v = 100; // velocity, 1–127
};

struct BendPoint {
    uint32_t id = 0;
    uint32_t t = 0;   // ms
    int16_t v = 0;    // -8192 to 8191, 0 the centre
};

// A controller's value from then on (a step, as MIDI has it).
struct CcPoint {
    uint32_t id = 0;
    uint32_t t = 0;  // ms
    uint8_t c = 1;   // controller, 0–119
    uint8_t v = 0;   // 0–127
};

struct Clip {
    std::vector<Note> notes;       // by t, then n
    std::vector<BendPoint> bends;  // by t
    std::vector<CcPoint> ccs;      // by t
    uint32_t length = 0;           // ms
    float bpm = 120;
    uint8_t beats = 4;  // per bar

    void sort();
};

// Limits (docs/DESIGN.md section 5.6).
constexpr size_t kMaxNotes = 1024;
constexpr size_t kMaxBends = 1024;
constexpr size_t kMaxCcs = 1024;  // controller points, all controllers together
constexpr size_t kMaxFile = 65536;          // bytes of a .mid
constexpr uint32_t kMaxLength = 14400000;  // 4 h, ms
constexpr float kMinBpm = 20, kMaxBpm = 300;

}  // namespace photophonique
