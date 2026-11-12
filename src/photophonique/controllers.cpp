#include "photophonique/controllers.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <ArduinoJson.h>

#include "photophonique/midi.h"
#include "tesser/api.h"
#include "tesser/client.h"

namespace photophonique {

using tesser::Check;
using tesser::Status;

Controllers::Controllers() {
    for (auto& v : values_) v = -1;
}

Controllers::~Controllers() = default;

void Controllers::attach(tesser::Api& api) {
    api_ = &api;
    client_.reset(new tesser::LocalClient(api));
    client_->setAccess(tesser::Access::User);
}

std::string Controllers::mapped(const CcMapping& m, uint8_t value, Kind kind) {
    if (m.threshold) return value >= m.threshold ? "true" : "false";
    if (kind == Kind::Boolean) return value >= 64 ? "true" : "false";
    double curve = m.curve > 0 ? m.curve : 1;
    double x;
    if (m.centred) {
        // -1 at 0, 0 at 64 (and within the dead band), 1 at 127.
        int d = static_cast<int>(value) - 64;
        int band = m.deadband;
        int reach = (d > 0 ? 63 : 64) - band;
        double s = 0;
        if (abs(d) > band && reach > 0) s = static_cast<double>(abs(d) - band) / reach;
        s = pow(s, curve);
        x = (1 + (d < 0 ? -s : s)) / 2;
    } else {
        x = pow(value / 127.0, curve);
    }
    double v = m.min + x * (m.max - m.min);
    char buf[32];
    if (kind == Kind::Integer) {
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(llround(v)));
    } else {
        snprintf(buf, sizeof(buf), "%.7g", v);
    }
    return buf;
}

Controllers::Kind Controllers::kindOf(const CcMapping& m) const {
    // Another controller's node isn't known here: a number.
    if (m.slot || !api_) return Kind::Number;
    const tesser::Node* node = api_;
    std::string_view path = m.path;
    size_t start = !path.empty() && path[0] == '/' ? 1 : 0;
    while (node && start < path.size()) {
        if (node->type() != tesser::NodeType::Object) return Kind::Number;
        size_t end = path.find('/', start);
        if (end == std::string_view::npos) end = path.size();
        node = static_cast<const tesser::Object*>(node)->child(path.substr(start, end - start));
        start = end + 1;
    }
    if (!node || node->type() != tesser::NodeType::Value) return Kind::Number;
    switch (static_cast<const tesser::ValueNode*>(node)->kind()) {
        case tesser::ValueKind::Boolean:
            return Kind::Boolean;
        case tesser::ValueKind::Integer:
            return Kind::Integer;
        default:
            return Kind::Number;
    }
}

void Controllers::sync() {
    uint32_t g = nodes.mappings ? nodes.mappings->generation() : 0;
    if (sent_.size() == mappings.size() && g == generation_) return;
    // Mappings changed: each writes again at its controller's next move.
    sent_.assign(mappings.size(), Sent());
    generation_ = g;
    if (!error_.empty()) {
        error_.clear();
        if (nodes.error) nodes.error->changed();
    }
}

void Controllers::moved(uint8_t cc, uint8_t value) {
    if (cc >= kControllers) return;
    value &= 0x7F;
    bool changed = values_[cc] != static_cast<int8_t>(value);
    values_[cc] = static_cast<int8_t>(value);
    if (changed && nodes.values) nodes.values->changed();
    if (last_ != cc) {
        last_ = cc;
        if (nodes.last) nodes.last->changed();
    }
    sync();
    for (size_t i = 0; i < mappings.size(); i++) {
        const CcMapping& m = mappings[i];
        if (m.cc != cc || m.path.empty()) continue;
        std::string json = mapped(m, value, kindOf(m));
        if (json == sent_[i].json) continue;
        write(i, json);
    }
}

void Controllers::rest() {
    sync();
    for (size_t i = 0; i < mappings.size(); i++) {
        if (!mappings[i].rest.empty() && !mappings[i].path.empty()) write(i, mappings[i].rest);
    }
}

void Controllers::tick(uint32_t nowMs) {
    nowMs_ = nowMs;
    if (client_) client_->process();
    sync();
    for (size_t i = 0; i < mappings.size(); i++) {
        const CcMapping& m = mappings[i];
        Sent& s = sent_[i];
        if (!m.renew || s.json.empty() || m.path.empty()) continue;
        if (static_cast<uint32_t>(nowMs - s.atMs) >= m.renew) write(i, s.json);
    }
}

void Controllers::write(size_t i, const std::string& json) {
    const CcMapping& m = mappings[i];
    sent_[i].json = json;
    sent_[i].atMs = nowMs_;
    writes_++;
    if (m.slot) {
        Status s = remote_ ? remote_->write(m.slot, m.path, json) : Status::NotFound;
        result(i, generation_, s == Status::Ok ? std::string()
                                               : "slot " + std::to_string(m.slot) + " " + m.path + ": " +
                                                     (s == Status::NotFound  ? "no controller"
                                                      : s == Status::Timeout ? "offline"
                                                                             : tesser::toString(s)));
        return;
    }
    if (!client_) return;
    tesser::Query unsaved;
    // Not saved: a controller moving a persisted node would rewrite the
    // settings every 2 s for as long as it moves (TesserKIT's mappings).
    unsaved.persist = false;
    std::string path = m.path;
    uint32_t generation = generation_;
    client_->set(path, json, [this, i, generation, path](Status s, JsonVariantConst body) {
        std::string e;
        if (s != Status::Ok) {
            e = path + ": " + tesser::toString(s);
            if (const char* msg = body["message"]) e = e + ": " + msg;
        }
        result(i, generation, e);
    }, unsaved);
}

void Controllers::result(size_t i, uint32_t generation, const std::string& error) {
    if (!error.empty()) failures_++;
    // A reply for mappings since changed: counted, not shown.
    if (generation != generation_ || i >= sent_.size()) return;
    std::string& was = sent_[i].error;
    if (error == was) return;
    // A mapping starting to fail toasts once; renewals failing again don't.
    if (!error.empty() && nodes.refused) nodes.refused->emit(error);
    was = error;
    // `error`: the latest failure still standing.
    std::string shown = error;
    if (shown.empty()) {
        for (const Sent& x : sent_) {
            if (!x.error.empty()) shown = x.error;
        }
    }
    if (shown == error_) return;
    error_ = shown;
    if (nodes.error) nodes.error->changed();
}

namespace {

Check bounded(const std::string& v, size_t max) {
    return v.size() <= max ? Check::ok() : Check::fail(Status::InvalidValue, "too long");
}

}  // namespace

void describe(tesser::Object& o, Controllers& c) {
    c.nodes.values = &o.array("values", c.values())
                          .doc("Each controller's last value (0–127), -1 until it moves");
    c.nodes.last = &o.value("last", [&c] { return c.last(); })
                        .doc("The last controller moved, -1 none: move a knob to learn its number");
    c.nodes.mappings =
        &o.list("mappings", c.mappings,
                [](tesser::Object& e, CcMapping& m) {
                    e.value("cc", m.cc).range(0, Controllers::kControllers - 1).label("Controller");
                    e.value("slot", m.slot)
                        .range(0, RemoteOutputs::kSlots)
                        .label("Controller slot")
                        .doc("0: this controller; 1–9: the ensemble slot's (/ensemble)");
                    e.value("path", m.path)
                        .validate([](const std::string& v) { return bounded(v, Controllers::kMaxPath); })
                        .doc("The node written, from its controller's root")
                        .ui("placeholder", "/outputs/1/frequency");
                    e.value("min", m.min).doc("Written at 0");
                    e.value("max", m.max).doc("Written at 127");
                    e.value("curve", m.curve)
                        .range(0.25, 4)
                        .doc("The value's exponent: 1 linear, above 1 finer near min (near the middle if centred)")
                        .ui("advanced");
                    e.value("centred", m.centred).doc("Both ways on one controller: 64 writes the middle of min–max");
                    e.value("deadband", m.deadband)
                        .range(0, 32)
                        .doc("Centred: values this close to 64 write the middle")
                        .ui("visibleWhen", "centred=true");
                    e.value("threshold", m.threshold)
                        .range(0, 127)
                        .label("Gate")
                        .doc("1–127: writes true while the value is at or above it; 0: a value from min to max")
                        .ui("advanced");
                    e.value("renew", m.renew)
                        .range(0, 60000)
                        .unit("ms")
                        .doc("Writes the last value again this often, for nodes that stop when not rewritten; 0 never")
                        .ui("advanced");
                    e.value("rest", m.rest)
                        .validate([](const std::string& v) {
                            if (v.size() > Controllers::kMaxRest) return Check::fail(Status::InvalidValue, "too long");
                            JsonDocument doc;
                            if (!v.empty() && deserializeJson(doc, v)) {
                                return Check::fail(Status::InvalidValue, "a JSON value");
                            }
                            return Check::ok();
                        })
                        .doc("JSON written when playing stops (All notes off, the recorder's stop); empty: nothing")
                        .ui("placeholder", "0");
                })
             .maxSize(Controllers::kMaxMappings)
             .persist()
             .doc("Each mapping writes its controller's value to a node, when what it writes changes");
    c.nodes.error = &o.value("error", [&c] { return c.error(); }).label("Last error").ui("advanced");
    c.nodes.refused = &o.event("refused").label("Controller write refused").ui("toast", "warn").ui("hidden");
}

}  // namespace photophonique
