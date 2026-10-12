#pragma once

// A PWM output (docs/DESIGN.md section 5): on, duty, frequency and resolution,
// applied at once to its own timer. The tree's setters call these, under the
// API lock; nothing else touches an output.

#include <stdint.h>

#include "photophonique/pwm.h"
#include "photophonique/timing.h"
#include "tesser/node.h"

namespace photophonique {

class Output {
public:
    Output(Pwm& pwm, int index, int pin) : pwm_(pwm), index_(index), pin_(pin) {}

    // Factory defaults, until the stored settings say otherwise.
    static constexpr float kDuty = 50;
    static constexpr double kFrequency = 1000;
    static constexpr uint8_t kResolution = 12;

    // Attaches the pin and applies the settings. Before it, setters only store.
    bool begin();

    bool on() const { return on_; }
    float duty() const { return duty_; }                // percent
    double frequency() const { return frequency_; }     // Hz, as asked
    uint8_t resolution() const { return resolution_; }  // bits, as asked
    void setOn(bool on);
    void setDuty(float percent);
    void setFrequency(double hz);
    void setResolution(uint8_t bits);

    // What the timer runs: the resolution may differ from the one asked when
    // the frequency doesn't allow it (timing.h).
    const Timing& timing() const { return timing_; }
    // The compare value applied: 0 while off.
    uint32_t counts() const { return on_ ? dutyCounts(duty_, timing_.bits) : 0; }
    int pin() const { return pin_; }
    int index() const { return index_; }

    // The output's object in the tree, so that changes made by code (All
    // off, MIDI) are announced to subscribers: announce() after them.
    void node(tesser::Node* n) { node_ = n; }
    void announce() {
        if (node_) node_->changed();
    }

private:
    void retime();
    void applyDuty();

    Pwm& pwm_;
    int index_;
    int pin_;
    bool begun_ = false;
    bool on_ = false;
    float duty_ = kDuty;
    double frequency_ = kFrequency;
    uint8_t resolution_ = kResolution;
    Timing timing_ = plan(kFrequency, kResolution);
    tesser::Node* node_ = nullptr;
};

// The /outputs tree (docs/DESIGN.md section 6): one object per output, named
// "1" to "n", and an All off action. Found by argument-dependent lookup:
// api.mount("outputs", outputs).
struct Outputs {
    Output* items;
    int count;

    bool begin();
    void allOff();
    // Output `n` (1 to count), or null.
    Output* at(int n) { return n >= 1 && n <= count ? &items[n - 1] : nullptr; }
};

void describe(tesser::Object& o, Output& out);
void describe(tesser::Object& o, Outputs& outs);

}  // namespace photophonique
