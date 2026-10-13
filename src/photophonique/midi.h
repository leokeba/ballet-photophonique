#pragma once

// MIDI onto the outputs (docs/DESIGN.md section 5.5): each key is mapped to
// an output, or to none, with its own frequency or the note's. The output
// may be another controller's (section 5.7: RemoteOutputs). A note on sets
// its output's frequency, its duty from the velocity, and turns it on, taking
// the output from whichever note had it; its note off turns it off. Pitch
// bend moves the sounding outputs' frequency or duty. Messages come from any
// client through /midi/send (the browser's Web MIDI, in the x-midi element).
// Under the API lock, like the outputs' setters.

#include <stdint.h>

#include <string>

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

// Other controllers' outputs (docs/DESIGN.md section 5.7), played by MIDI
// like this controller's: `slot` 1 to kSlots names the controller, `output`
// 1 to kMaxOutputs its output. Each call gives the output's whole state.
class RemoteOutputs {
public:
    static constexpr int kSlots = 9;
    virtual void play(int slot, int output, bool on, double hz, float duty) = 0;
    // How the key map names each slot's controller, "" for an empty slot;
    // `node` is announced when they change.
    using Names = std::string[kSlots];
    virtual const Names& names() const = 0;
    virtual void namesNode(tesser::Node* node) = 0;

protected:
    ~RemoteOutputs() = default;
};

class Midi {
public:
    explicit Midi(Outputs& outs) : outputs_(outs) {
        for (auto& v : local_) v = -1;
    }

    static constexpr int kNotes = 128;
    enum class Bend : uint8_t { Frequency, Duty };
    // A key's output: 0 none, 1 to kMaxOutputs this controller's, 10 * slot
    // + output another controller's (RemoteOutputs).
    static constexpr int kTargets = 10 * (RemoteOutputs::kSlots + 1);

    // Settings, bound by the tree. outputs[n]: 0 (none) or a target (above);
    // frequencies[n]: 0 (the note's own) or a frequency in Hz.
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
    // The note target `t` (an output, 1 to count, or another controller's,
    // 10 * slot + output) plays, or -1.
    int8_t voice(int t) const { return t >= 0 && t < kTargets ? voices_[t].note : -1; }
    // This controller's outputs' notes, for the tree.
    const int8_t (&voices() const)[kMaxOutputs] { return local_; }
    float bend() const { return bend_; }  // -1 to 1
    int count() const { return outputs_.count; }
    // The highest key map value: the outputs' count, or the last remote
    // output once other controllers can be played.
    int maxTarget() const { return remote_ ? kTargets - 2 : outputs_.count; }

    // Other controllers' outputs, played from then on (before the tree is
    // described, which gives the key map its range).
    void remote(RemoteOutputs* r) { remote_ = r; }
    RemoteOutputs* remote() const { return remote_; }

    // The tree's voices and bend value, announced when they change.
    void nodes(tesser::Node* voices, tesser::Node* bend) {
        voicesNode_ = voices;
        bendNode_ = bend;
    }

private:
    struct Voice {
        int8_t note = -1;
        double hz = 0;
        float duty = 0;
    };

    void noteOn(uint8_t note, uint8_t velocity);
    void noteOff(uint8_t note);
    // Whether target `t` can play: an output here, or another controller's.
    bool playable(int t);
    // Plays target `t`'s voice, its frequency and duty bent; `start` turns
    // it on (a note on) rather than only moving it (a bend).
    void sound(int t, bool start);

    Outputs& outputs_;
    RemoteOutputs* remote_ = nullptr;
    Voice voices_[kTargets];
    int8_t local_[kMaxOutputs];  // voices_[1 … kMaxOutputs]'s notes
    float bend_ = 0;
    tesser::Node* voicesNode_ = nullptr;
    tesser::Node* bendNode_ = nullptr;
    MidiListener* listener_ = nullptr;
};

// The /midi tree (docs/DESIGN.md section 6): api.mount("midi", midi).
void describe(tesser::Object& o, Midi& midi);

}  // namespace photophonique
