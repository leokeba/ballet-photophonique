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
    const tesser::Client* c = api_ ? api_->currentClient() : nullptr;
    writerLength_ = c && c->deviceLength <= sizeof(writer_) ? c->deviceLength : 0;
    if (writerLength_) memcpy(writer_, c->device, writerLength_);
}

bool Output::writtenBy(const uint8_t* device, uint8_t length) const {
    return length && length == writerLength_ && memcmp(device, writer_, length) == 0;
}

void Outputs::trackWriters(const tesser::Api& api) {
    for (int i = 0; i < count; i++) items[i].track(&api);
}

int Outputs::release(const uint8_t* device, uint8_t length) {
    int n = 0;
    for (int i = 0; i < count; i++) {
        if (!items[i].writtenBy(device, length) || !items[i].on()) continue;
        items[i].setOn(false);
        items[i].announce();
        n++;
    }
    return n;
}

}  // namespace photophonique
