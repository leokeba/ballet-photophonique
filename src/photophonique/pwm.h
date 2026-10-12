#pragma once

// What the outputs need from the chip (docs/DESIGN.md section 4.1): one timer
// and one channel per output, so each has its own frequency and resolution.
// platform/esp/pwm_esp.cpp drives the classic ESP32's LEDC; the tests use a
// fake that records what it was given.

#include <stdint.h>

#include "photophonique/timing.h"

namespace photophonique {

class Pwm {
public:
    virtual ~Pwm() = default;
    // Output `index` (0 to kMaxOutputs - 1) on `pin`, low until a duty comes.
    virtual bool attach(int index, int pin) = 0;
    // The output's timer: clock, divider and resolution, applied together.
    virtual void timing(int index, const Timing& t) = 0;
    // The compare value, 0 to 2^bits of the last timing.
    virtual void duty(int index, uint32_t counts) = 0;
};

// The classic ESP32 has 8 timers (4 high-speed, 4 low-speed): outputs 0–3 on
// the high-speed ones, the rest on the low-speed ones.
constexpr int kMaxOutputs = 8;

#if defined(ESP_PLATFORM)
// The chip's LEDC.
Pwm& defaultPwm();
#endif

}  // namespace photophonique
