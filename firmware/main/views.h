#pragma once

// The interface's views (docs/DESIGN.md section 7), shared by the firmware
// and the host controller (test/host/controller.cpp).
#include "photophonique/elements.h"
#include "tesser/api.h"
#include "tesserui/views.h"

namespace {

// The first page (docs/DESIGN.md section 7): the keyboard, which forwards
// MIDI only while it is on screen, beside what each output does.
inline void addViews(tesser::Api& api) {
    static tesserui::Views views(api);
    auto& play = views.page("play", "Play").icon("play");
    play.card("Keyboard").widget("x-midi", "/midi/keys").hint("script", photophonique::elements::kMidiPath);
    static const char* const kLabels[5] = {"Output 1", "Output 2", "Output 3", "Output 4", "Output 5"};
    static const char* const kOn[5] = {"/outputs/1/on", "/outputs/2/on", "/outputs/3/on", "/outputs/4/on",
                                       "/outputs/5/on"};
    static const char* const kHz[5] = {"/outputs/1/applied/frequency", "/outputs/2/applied/frequency",
                                       "/outputs/3/applied/frequency", "/outputs/4/applied/frequency",
                                       "/outputs/5/applied/frequency"};
    static const char* const kDuty[5] = {"/outputs/1/duty", "/outputs/2/duty", "/outputs/3/duty",
                                         "/outputs/4/duty", "/outputs/5/duty"};
    auto& card = play.card("Outputs");
    for (int i = 0; i < 5; i++) {
        card.row()
            .widget("toggle", kOn[i], kLabels[i])
            .widget("readout", kHz[i], "Frequency")
            .widget("readout", kDuty[i], "Duty");
    }
    card.row().node("/outputs/off").node("/midi/off");
    play.card("Pitch bend").node("/midi/bend");

    // Recording needs the keyboard on screen too. The keyboard and the
    // recordings side by side, the piano roll across the page under them
    // ("wide", TesserUI e87ff4d: a hint on the page right after card() goes
    // on that card; older renderers ignore it).
    auto& record = views.page("record", "Record").icon("target");
    record.card("Keyboard").widget("x-midi", "/midi/keys").hint("script", photophonique::elements::kMidiPath);
    record.card("Recordings").node("/recorder/recordings").node("/recorder/upload");
    record.card("Recorder")
        .widget("x-recorder", "/recorder")
        .hint("script", photophonique::elements::kRecorderPath);
    record.hint("wide", true);
}

}  // namespace
