#if defined(ESP_PLATFORM)

// The classic ESP32's LEDC (docs/DESIGN.md section 4.1): output i gets timer
// and channel i % 4 of the high-speed group for i < 4, of the low-speed group
// after. ledc_timer_config() takes whole hertz and picks its own divider, so
// it only sets a timer up; plan()'s divider, resolution and clock are then
// written to the timer's register through the low-level layer, which the
// driver reads back for its duty checks (ledc_get_max_duty()).

#include "driver/ledc.h"
#include "esp_log.h"
#include "hal/ledc_ll.h"
#include "photophonique/pwm.h"

namespace photophonique {

namespace {

const char* kTag = "photophonique";

struct Slot {
    ledc_mode_t mode;
    ledc_timer_t timer;
    ledc_channel_t channel;
};

Slot slot(int index) {
    int n = index % 4;
    return {index < 4 ? LEDC_HIGH_SPEED_MODE : LEDC_LOW_SPEED_MODE, static_cast<ledc_timer_t>(n),
            static_cast<ledc_channel_t>(n)};
}

class EspPwm : public Pwm {
public:
    bool attach(int index, int pin) override {
        if (index < 0 || index >= kMaxOutputs) return false;
        Slot s = slot(index);
        // APB at 1 kHz, 10 bits; the low-speed group's slow clock becomes APB.
        ledc_timer_config_t t = {};
        t.speed_mode = s.mode;
        t.timer_num = s.timer;
        t.duty_resolution = LEDC_TIMER_10_BIT;
        t.freq_hz = 1000;
        t.clk_cfg = LEDC_USE_APB_CLK;
        if (ledc_timer_config(&t) != ESP_OK) {
            ESP_LOGE(kTag, "output %d: timer setup failed", index + 1);
            return false;
        }
        ledc_channel_config_t c = {};
        c.gpio_num = pin;
        c.speed_mode = s.mode;
        c.channel = s.channel;
        c.timer_sel = s.timer;
        c.duty = 0;
        if (ledc_channel_config(&c) != ESP_OK) {
            ESP_LOGE(kTag, "output %d: GPIO %d refused", index + 1, pin);
            return false;
        }
        return true;
    }

    void timing(int index, const Timing& t) override {
        if (index < 0 || index >= kMaxOutputs) return;
        Slot s = slot(index);
        ledc_dev_t* hw = LEDC_LL_GET_HW(0);
        uint32_t bits = 0;
        ledc_ll_get_duty_resolution(hw, s.mode, s.timer, &bits);
        ledc_ll_set_clock_divider(hw, s.mode, s.timer, t.divider);
        ledc_ll_set_duty_resolution(hw, s.mode, s.timer, t.bits);
        ledc_ll_set_clock_source(hw, s.mode, s.timer, t.clock == Clock::Apb ? LEDC_APB_CLK : LEDC_REF_TICK);
        if (s.mode == LEDC_LOW_SPEED_MODE) ledc_ll_ls_timer_update(hw, s.mode, s.timer);
        // A counter already past the new top would run on to the 20-bit wrap:
        // minutes on a slow clock. A new period starts instead.
        if (bits != t.bits) ledc_timer_rst(s.mode, s.timer);
    }

    // ESP32's LEDC applies a duty at the end of the period, and
    // ledc_update_duty() spins in a critical section until the previous one
    // was applied: at 0.5 Hz, longer than the interrupt watchdog allows. So a
    // duty is only written once the channel's previous one is done; until
    // then the latest waits here, for poll().
    void duty(int index, uint32_t counts) override {
        if (index < 0 || index >= kMaxOutputs) return;
        waiting_[index] = true;
        counts_[index] = counts;
        flush(index);
    }

    void poll() override {
        for (int i = 0; i < kMaxOutputs; i++) flush(i);
    }

private:
    void flush(int index) {
        if (!waiting_[index]) return;
        Slot s = slot(index);
        if (LEDC_LL_GET_HW(0)->channel_group[s.mode].channel[s.channel].conf1.duty_start) return;
        ledc_set_duty(s.mode, s.channel, counts_[index]);
        ledc_update_duty(s.mode, s.channel);
        waiting_[index] = false;
    }

    bool waiting_[kMaxOutputs] = {};
    uint32_t counts_[kMaxOutputs] = {};
};

}  // namespace

Pwm& defaultPwm() {
    static EspPwm pwm;
    return pwm;
}

}  // namespace photophonique

#endif
