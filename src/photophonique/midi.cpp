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
            for (int t = 0; t < kTargets; t++) {
                if (voices_[t].note >= 0) sound(t, false);
            }
            if (bendNode_) bendNode_->changed();
            break;
        }
        default:
            break;
    }
}

bool Midi::playable(int t) {
    if (t < 10) return outputs_.at(t) != nullptr;
    int o = t % 10;
    return remote_ && t < kTargets && o >= 1 && o <= kMaxOutputs;
}

void Midi::noteOn(uint8_t note, uint8_t velocity) {
    int t = outputs[note];
    if (!playable(t)) return;
    Voice& v = voices_[t];
    v.note = static_cast<int8_t>(note);
    v.hz = frequencies[note] > 0 ? frequencies[note] : noteHz(note);
    v.duty = static_cast<float>(velocity) * 100.0f / 127.0f;
    sound(t, true);
}

void Midi::noteOff(uint8_t note) {
    // Wherever it sounds: its key may have been mapped elsewhere since.
    for (int t = 0; t < kTargets; t++) {
        if (voices_[t].note != note) continue;
        voices_[t].note = -1;
        if (t >= 10) {
            remote_->play(t / 10, t % 10, false, 0, 0);
            continue;
        }
        local_[t - 1] = -1;
        outputs_.at(t)->setOn(false);
        outputs_.at(t)->wrote();
        outputs_.at(t)->announce();
        if (voicesNode_) voicesNode_->changed();
    }
}

void Midi::allNotesOff() {
    for (int t = 0; t < kTargets; t++) {
        if (voices_[t].note >= 0) noteOff(static_cast<uint8_t>(voices_[t].note));
    }
}

void Midi::sound(int t, bool start) {
    const Voice& v = voices_[t];
    double hz = v.hz;
    float duty = v.duty;
    if (bendTarget == Bend::Frequency) {
        hz *= pow(2.0, static_cast<double>(bend_ * bendSemitones) / 12.0);
    } else {
        duty += bend_ * bendDuty;
        duty = duty < 0 ? 0 : duty > 100 ? 100 : duty;
    }
    hz = hz < kMinHz ? kMinHz : hz > kMaxHz ? kMaxHz : hz;
    if (t >= 10) {
        // Another controller's output gets its whole state each time.
        remote_->play(t / 10, t % 10, true, hz, duty);
        return;
    }
    // A bend leaves an output turned off by hand off.
    Output* out = outputs_.at(t);
    out->setFrequency(hz);
    out->setDuty(duty);
    if (start) {
        out->setOn(true);
        out->wrote();  // this note's client's, not a lost master's to turn off
        local_[t - 1] = v.note;
        if (voicesNode_) voicesNode_->changed();
    }
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
        .range(0, midi.maxTarget())
        .persist()
        .ui("outputs", midi.count())  // this controller's, for the editor
        .doc(midi.maxTarget() > midi.count()
                 ? "Per MIDI note (0–127): the output it plays, 0 for none; 10 × slot + output for another "
                   "controller's (11 is slot 1's output 1)"
                 : "Per MIDI note (0–127): the output it plays, 0 for none");
    if (midi.remote()) {
        auto& names = keys.array("nodes", midi.remote()->names())
                          .doc("Per slot (1–9): its controller's name, empty for none (/ensemble)");
        midi.remote()->namesNode(&names);
    }
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
