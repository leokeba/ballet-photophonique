#pragma once

// MIDI onto the outputs (docs/DESIGN.md section 5.5): each key is mapped to
// an output, or to none, with its own frequency or the note's. A note on sets
// its output's frequency, its duty from the velocity, and turns it on, taking
// the output from whichever note had it; its note off turns it off. Pitch
// bend moves the sounding outputs' frequency or duty. Messages come from any
// client through /midi/send (the browser's Web MIDI, in the x-midi element).
// Under the API lock, like the outputs' setters.

#include <stdint.h>

#include "photophonique/output.h"
#include "tesser/node.h"

namespace photophonique {

// What clients play (/midi/send), heard by the recorder (docs/DESIGN.md
// section 5.6): each message after it is applied, with how long ago it was
// played.
class MidiListener {
public:
    virtual void heard(uint8_t status, uint8_t data1, uint8_t data2, uint32_t ageMs) = 0;

protected:
    ~MidiListener() = default;
};

class Midi {
public:
    explicit Midi(Outputs& outs) : outputs_(outs) {
        for (auto& v : voices_) v = -1;
    }

    static constexpr int kNotes = 128;
    enum class Bend : uint8_t { Frequency, Duty };

    // Settings, bound by the tree. outputs[n]: 0 (none) or 1 to the outputs'
    // count; frequencies[n]: 0 (the note's own) or a frequency in Hz.
    uint8_t outputs[kNotes] = {};
    float frequencies[kNotes] = {};
    Bend bendTarget = Bend::Frequency;
    float bendSemitones = 2;  // the frequency at full bend
    float bendDuty = 50;      // percentage points of duty at full bend

    // One message: a status byte and its data bytes. Channels are ignored.
    void message(uint8_t status, uint8_t data1 = 0, uint8_t data2 = 0);
    // A message from a client, played `ageMs` ago: applied, then heard by
    // the listener. Replay calls message() instead, so it isn't recorded.
    void input(uint8_t status, uint8_t data1, uint8_t data2, uint32_t ageMs = 0) {
        message(status, data1, data2);
        if (listener_) listener_->heard(status, data1, data2, ageMs);
    }
    void listen(MidiListener* listener) { listener_ = listener; }
    static constexpr uint32_t kMaxAgeMs = 60000;
    // Turns off every output a note holds.
    void allNotesOff();

    // Equal temperament, A4 (69) at 440 Hz.
    static double noteHz(int note);
    // The note output `n` (1 to count) plays, or -1.
    int8_t voice(int n) const { return n >= 1 && n <= kMaxOutputs ? voices_[n - 1] : -1; }
    const int8_t (&voices() const)[kMaxOutputs] { return voices_; }
    float bend() const { return bend_; }  // -1 to 1
    int count() const { return outputs_.count; }

    // The tree's voices and bend value, announced when they change.
    void nodes(tesser::Node* voices, tesser::Node* bend) {
        voicesNode_ = voices;
        bendNode_ = bend;
    }

private:
    void noteOn(uint8_t note, uint8_t velocity);
    void noteOff(uint8_t note);
    // Applies a voice's frequency and duty, bent, to its output.
    void apply(int n);

    Outputs& outputs_;
    int8_t voices_[kMaxOutputs];
    double hz_[kMaxOutputs] = {};
    float duty_[kMaxOutputs] = {};
    float bend_ = 0;
    tesser::Node* voicesNode_ = nullptr;
    tesser::Node* bendNode_ = nullptr;
    MidiListener* listener_ = nullptr;
};

// The /midi tree (docs/DESIGN.md section 6): api.mount("midi", midi).
void describe(tesser::Object& o, Midi& midi);

}  // namespace photophonique
