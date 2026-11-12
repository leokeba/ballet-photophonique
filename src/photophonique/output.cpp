#include "photophonique/output.h"

#include <string.h>

#include "tesser/api.h"

namespace photophonique {

bool Output::begin() {
    if (!pwm_.attach(index_, pin_)) return false;
    begun_ = true;
    pwm_.timing(index_, timing_);
    applyDuty();
    return true;
}

void Output::setOn(bool on) {
    on_ = on;
    applyDuty();
}

void Output::setDuty(float percent) {
    duty_ = percent;
    applyDuty();
}

void Output::setFrequency(double hz) {
    frequency_ = hz;
    retime();
}

void Output::setResolution(uint8_t bits) {
    resolution_ = bits;
    retime();
}

void Output::retime() {
    Timing t = plan(frequency_, resolution_);
    if (t == timing_) return;
    timing_ = t;
    if (!begun_) return;
    pwm_.timing(index_, timing_);
    applyDuty();  // the compare value scales with the resolution
}

void Output::applyDuty() {
    if (begun_) pwm_.duty(index_, counts());
}

bool Outputs::begin() {
    bool ok = true;
    for (int i = 0; i < count; i++) ok = items[i].begin() && ok;
    return ok;
}

void Outputs::allOff() {
    for (int i = 0; i < count; i++) {
        items[i].setOn(false);
        items[i].announce();
    }
}

void Output::wrote() {
    if (api_ && node_) api_->wrote(*node_);
}

void Outputs::trackWriters(tesser::Api& api) {
    for (int i = 0; i < count; i++) {
        Output& out = items[i];
        if (!out.node_) continue;
        out.api_ = &api;
        api.trackWriter(*out.node_, [this, &out](tesser::Node&, tesser::PeerLoss) {
            if (!out.on()) return;
            out.setOn(false);
            out.announce();
            released++;
        });
    }
}

}  // namespace photophonique
