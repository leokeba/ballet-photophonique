#include "photophonique/midi.h"

#include <math.h>

#include <ArduinoJson.h>

#include "photophonique/elements.h"
#include "tesser/status.h"

namespace photophonique {

double Midi::noteHz(int note) { return 440.0 * pow(2.0, (note - 69) / 12.0); }

void Midi::message(uint8_t status, uint8_t data1, uint8_t data2) {
    data1 &= 0x7F;
    data2 &= 0x7F;
    switch (status & 0xF0) {
        case 0x90:
            if (data2) {
                noteOn(data1, data2);
            } else {
                noteOff(data1);  // velocity 0: a note off
            }
            break;
        case 0x80:
            noteOff(data1);
            break;
        case 0xB0:
            if (data1 == 120 || data1 == 123) allNotesOff();  // all sound off, all notes off
            break;
        case 0xE0: {
            int v = (data2 << 7 | data1) - 8192;
            bend_ = v < 0 ? static_cast<float>(v) / 8192 : static_cast<float>(v) / 8191;
            for (int n = 1; n <= outputs_.count; n++) {
                if (voices_[n - 1] >= 0) apply(n);
            }
            if (bendNode_) bendNode_->changed();
            break;
        }
        default:
            break;
    }
}

void Midi::noteOn(uint8_t note, uint8_t velocity) {
    int n = outputs[note];
    if (!outputs_.at(n)) return;
    voices_[n - 1] = static_cast<int8_t>(note);
    hz_[n - 1] = frequencies[note] > 0 ? frequencies[note] : noteHz(note);
    duty_[n - 1] = static_cast<float>(velocity) * 100.0f / 127.0f;
    apply(n);
    outputs_.at(n)->setOn(true);
    outputs_.at(n)->announce();
    if (voicesNode_) voicesNode_->changed();
}

void Midi::noteOff(uint8_t note) {
    // Wherever it sounds: its key may have been mapped elsewhere since.
    for (int n = 1; n <= outputs_.count; n++) {
        if (voices_[n - 1] != note) continue;
        voices_[n - 1] = -1;
        outputs_.at(n)->setOn(false);
        outputs_.at(n)->announce();
        if (voicesNode_) voicesNode_->changed();
    }
}

void Midi::allNotesOff() {
    for (int n = 1; n <= outputs_.count; n++) {
        if (voices_[n - 1] >= 0) noteOff(static_cast<uint8_t>(voices_[n - 1]));
    }
}

void Midi::apply(int n) {
    Output* out = outputs_.at(n);
    double hz = hz_[n - 1];
    float duty = duty_[n - 1];
    if (bendTarget == Bend::Frequency) {
        hz *= pow(2.0, static_cast<double>(bend_ * bendSemitones) / 12.0);
    } else {
        duty += bend_ * bendDuty;
        duty = duty < 0 ? 0 : duty > 100 ? 100 : duty;
    }
    out->setFrequency(hz < kMinHz ? kMinHz : hz > kMaxHz ? kMaxHz : hz);
    out->setDuty(duty);
    out->announce();
}

namespace {

using tesser::Check;
using tesser::Object;
using tesser::Status;

// A message packed as status | data1 << 8 | data2 << 16, or a pair of one and
// how long ago it was played, [message, ms].
bool unpack(JsonVariantConst v, uint32_t& m, uint32_t& age) {
    age = 0;
    if (v.is<JsonArrayConst>()) {
        JsonArrayConst pair = v.as<JsonArrayConst>();
        if (pair.size() != 2 || !pair[1].is<uint32_t>()) return false;
        age = pair[1].as<uint32_t>();
        if (age > Midi::kMaxAgeMs) return false;
        v = pair[0];
    }
    if (!v.is<uint32_t>()) return false;
    m = v.as<uint32_t>();
    return (m & 0x80) && m <= 0xFFFFFF;
}

void play(Midi& midi, uint32_t m, uint32_t age) {
    midi.input(static_cast<uint8_t>(m), static_cast<uint8_t>(m >> 8), static_cast<uint8_t>(m >> 16), age);
}

constexpr const char* kFormat = "status | data1 << 8 | data2 << 16, or [message, ms ago]";

}  // namespace

void describe(Object& o, Midi& midi) {
    o.writeAccess(tesser::Access::User);

    auto& keys = o.object("keys")
                     .label("Keys")
                     .doc("Each key's output and frequency")
                     .ui("widget", "x-midi")
                     .ui("script", elements::kMidiPath);
    keys.array("outputs", midi.outputs)
        .range(0, midi.count())
        .persist()
        .doc("Per MIDI note (0–127): the output it plays, 0 for none");
    keys.array("frequencies", midi.frequencies)
        .range(0, kMaxHz)
        .persist()
        .unit("Hz")
        .doc("Per MIDI note: its frequency, 0 for the note's own (A4 = 440 Hz)");
    auto& voices = keys.array("voices", midi.voices())
                       .doc("Per output: the note it plays, -1 for none");

    auto& bend = o.object("bend").label("Pitch bend");
    bend.value("target", midi.bendTarget, {"frequency", "duty"})
        .persist()
        .ui("labels", "Frequency,Duty")
        .doc("What the bend wheel moves on the outputs that sound");
    bend.value("semitones", midi.bendSemitones)
        .range(0, 48)
        .step(1)
        .unit("st")
        .persist()
        .doc("The frequency at full bend, in semitones")
        .ui("visibleWhen", "target=frequency");
    bend.value("duty", midi.bendDuty)
        .range(0, 100)
        .step(1)
        .unit("%")
        .persist()
        .doc("The duty at full bend, in percentage points")
        .ui("visibleWhen", "target=duty");
    auto& value = bend.value("value", [&midi] { return midi.bend(); }).range(-1, 1).ui("precision", 2);
    midi.nodes(&voices, &value);

    o.action("send",
             [&midi](JsonVariantConst body) {
                 uint32_t m, age;
                 // An array is a batch: a pair is only ever one of its items.
                 if (body.is<JsonArrayConst>()) {
                     JsonArrayConst a = body.as<JsonArrayConst>();
                     for (JsonVariantConst v : a) {
                         if (!unpack(v, m, age)) return Check::fail(Status::InvalidValue, kFormat);
                     }
                     for (JsonVariantConst v : a) {
                         unpack(v, m, age);
                         play(midi, m, age);
                     }
                     return Check::ok();
                 }
                 if (!unpack(body, m, age)) return Check::fail(Status::InvalidValue, kFormat);
                 play(midi, m, age);
                 return Check::ok();
             })
        .doc("MIDI messages, each packed as status | data1 << 8 | data2 << 16 (or [message, ms ago]), or an array of them")
        .ui("hidden");
    o.action("off", [&midi] { midi.allNotesOff(); }).label("All notes off").ui("icon", "stop");
}

}  // namespace photophonique
