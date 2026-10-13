// The /outputs tree (docs/DESIGN.md section 6).
#include "photophonique/output.h"
#include "tesser/status.h"

namespace photophonique {

namespace {

using tesser::Access;
using tesser::Object;

// Names outlive the tree: TesserAPI never copies them.
const char* const kNames[kMaxOutputs] = {"1", "2", "3", "4", "5", "6", "7", "8"};
const char* const kLabels[kMaxOutputs] = {"Output 1", "Output 2", "Output 3", "Output 4",
                                          "Output 5", "Output 6", "Output 7", "Output 8"};

}  // namespace

void describe(Object& o, Output& out) {
    out.node(&o);
    o.value("on", [&out] { return out.on(); }, [&out](bool v) {
        out.setOn(v);
        out.wrote();
    })
        .persist()
        .doc("Off holds the output low");
    o.value("duty", [&out] { return out.duty(); }, [&out](float v) {
        out.setDuty(v);
        out.wrote();
    })
        .range(0, 100)
        .step(0.1)
        .unit("%")
        .persist()
        .doc("The share of each period the output is high");
    o.value("frequency", [&out] { return out.frequency(); }, [&out](double v) {
        out.setFrequency(v);
        out.wrote();
    })
        .range(kMinHz, kMaxHz)
        .unit("Hz")
        .persist()
        .doc("Periods per second. The timer reaches it in steps that grow with the resolution: see Applied")
        .ui("widget", "number");
    o.value("resolution", [&out] { return out.resolution(); }, [&out](uint8_t v) {
        out.setResolution(v);
        out.wrote();
    })
        .range(kMinBits, kMaxBits)
        .unit("bits")
        .persist()
        .doc("Duty steps per period, as a power of two. Lowered when the frequency is too high for it, raised "
             "when too low");

    auto& applied = o.object("applied").label("Applied").doc("What the timer runs").ui("inline");
    applied.value("frequency", [&out] { return out.timing().frequency; }).unit("Hz").ui("precision", 3).watch();
    applied.value("bits", [&out] { return out.timing().bits; }).unit("bits").watch();
    applied.value("clock", [&out] { return out.timing().clock; }, {"apb", "refTick"})
        .ui("labels", "80 MHz,1 MHz")
        .ui("advanced")
        .watch();
    o.value("gpio", [&out] { return out.pin(); }).label("GPIO").ui("advanced");
}

void describe(Object& o, Outputs& outs) {
    o.writeAccess(Access::User);
    o.action("off", [&outs] { outs.allOff(); }).label("All off").ui("style", "danger").ui("icon", "power");
    for (int i = 0; i < outs.count && i < kMaxOutputs; i++) {
        auto& c = o.object(kNames[i]).label(kLabels[i]);
        describe(c, outs.items[i]);
    }
}

}  // namespace photophonique
