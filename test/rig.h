// Five outputs on a fake PWM, mounted at /outputs as on the device, and
// requests through the API as a client would send them.
#pragma once

#include <math.h>

#include <string>
#include <string_view>

#include <ArduinoJson.h>

#include "Photophonique.h"
#include "TesserAPI.h"
#include "check.h"

namespace {

// What the chip was last told, per output.
struct FakePwm : photophonique::Pwm {
    struct Channel {
        bool attached = false;
        int pin = -1;
        photophonique::Timing timing;
        uint32_t counts = 0;
        int timings = 0;  // how many times the timer was written
        int duties = 0;
    };
    Channel ch[photophonique::kMaxOutputs];
    bool refuse = false;

    bool attach(int index, int pin) override {
        if (refuse) return false;
        ch[index].attached = true;
        ch[index].pin = pin;
        ch[index].counts = 0;
        return true;
    }
    void timing(int index, const photophonique::Timing& t) override {
        ch[index].timing = t;
        ch[index].timings++;
    }
    void duty(int index, uint32_t counts) override {
        ch[index].counts = counts;
        ch[index].duties++;
    }
};

// The Gledopto GL-C-611WL's outputs, in terminal order.
constexpr int kPins[5] = {18, 16, 12, 4, 2};

struct Rig {
    FakePwm pwm;
    photophonique::Output out[5] = {{pwm, 0, kPins[0]},
                                    {pwm, 1, kPins[1]},
                                    {pwm, 2, kPins[2]},
                                    {pwm, 3, kPins[3]},
                                    {pwm, 4, kPins[4]}};
    photophonique::Outputs outputs{out, 5};
    tesser::Api api;

    explicit Rig(bool begin = true) {
        api.mount("outputs", outputs).label("Outputs");
        if (begin) outputs.begin();
    }

    struct Reply {
        tesser::Status status;
        std::string body;
    };
    Reply request(tesser::Op op, std::string_view path, std::string_view body = {}) {
        JsonDocument doc;
        if (!body.empty()) deserializeJson(doc, body.data(), body.size());
        tesser::Request req;
        req.op = op;
        req.path = path;
        req.body = doc.as<JsonVariantConst>();
        req.client.authenticated = true;
        tesser::StringReply reply;
        api.handle(req, reply);
        return {reply.status, reply.body};
    }
    Reply get(std::string_view path) { return request(tesser::Op::Get, path); }
    Reply set(std::string_view path, std::string_view body) { return request(tesser::Op::Set, path, body); }
};

bool near(double a, double b, double tolerance) { return fabs(a - b) <= tolerance; }

}  // namespace
