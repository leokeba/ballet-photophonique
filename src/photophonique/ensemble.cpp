#include "photophonique/ensemble.h"

#include <ArduinoJson.h>

#include "tesser/remote.h"

namespace photophonique {

using tesser::Check;
using tesser::Status;

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

Check Ensemble::setDevice(int n, const std::string& id) {
    if (n < 1 || n > kSlots) return Check::fail(Status::NotFound, "no such slot");
    Slot& s = slots_[n - 1];
    tesser::PeerAddress address;
    std::string lower;
    if (!id.empty()) {
        if (id.size() != kIdLength) return Check::fail(Status::InvalidValue, "a device id is 12 hex digits");
        uint8_t bytes[kIdLength / 2];
        for (size_t i = 0; i < kIdLength; i += 2) {
            int hi = hexDigit(id[i]), lo = hexDigit(id[i + 1]);
            if (hi < 0 || lo < 0) return Check::fail(Status::InvalidValue, "a device id is 12 hex digits");
            bytes[i / 2] = static_cast<uint8_t>(hi << 4 | lo);
        }
        address = tesser::PeerAddress::fromBytes(bytes, sizeof(bytes));
        static const char kHex[] = "0123456789abcdef";
        for (uint8_t b : bytes) {
            lower += kHex[b >> 4];
            lower += kHex[b & 0xF];
        }
    }
    if (lower == s.device) return Check::ok();
    // Another controller: what was played on the previous one isn't sent
    // there, and the new one gets what is played from now on.
    s.device = lower;
    s.address = address;
    s.online = false;
    for (Held& h : s.held) h = Held();
    return Check::ok();
}

tesser::RemoteNode* Ensemble::node(int n) {
    if (!peers_ || n < 1 || n > kSlots || slots_[n - 1].device.empty()) return nullptr;
    return tesser::RemoteNode::byDevice(*peers_, slots_[n - 1].address);
}

void Ensemble::play(int slot, int output, bool on, double hz, float duty) {
    if (slot < 1 || slot > kSlots || output < 1 || output > kMaxOutputs) return;
    Held& h = slots_[slot - 1].held[output - 1];
    h.known = true;
    h.on = on;
    if (on) {
        h.hz = hz;
        h.duty = duty;
    }
    send(slot, output);
}

void Ensemble::send(int slot, int output) {
    tesser::RemoteNode* n = node(slot);
    if (!n) {
        dropped_++;
        return;
    }
    static const char* const kNames[kMaxOutputs] = {"1", "2", "3", "4", "5", "6", "7", "8"};
    const Slot& s = slots_[slot - 1];
    JsonDocument doc;
    for (int o = 1; o <= kMaxOutputs; o++) {
        const Held& h = s.held[o - 1];
        if (!h.known || (output && o != output)) continue;
        JsonObject out = doc[kNames[o - 1]].to<JsonObject>();
        out["on"] = h.on;
        // Off leaves the frequency and duty as the controller has them.
        if (h.on) {
            out["frequency"] = h.hz;
            out["duty"] = h.duty;
        }
    }
    if (doc.isNull()) return;
    std::string body;
    serializeJson(doc, body);
    // Offline: dropped, and sent again when it is back (tick()).
    if (n->stream("/outputs", body) == Status::Ok) {
        sent_++;
    } else {
        dropped_++;
    }
}

void Ensemble::tick() {
    bool renamed = false;
    for (int i = 1; i <= kSlots; i++) {
        Slot& s = slots_[i - 1];
        tesser::RemoteNode* n = node(i);
        bool online = n && n->online();
        if (online && !s.online) send(i, 0);
        s.online = online;
        std::string name = n ? std::string(n->name()) : s.device;
        if (name != names_[i - 1]) {
            names_[i - 1] = name;
            renamed = true;
        }
    }
    if (renamed && namesNode_) namesNode_->changed();
}

namespace {

const char* const kSlotNames[RemoteOutputs::kSlots] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
const char* const kSlotLabels[RemoteOutputs::kSlots] = {"Node 1", "Node 2", "Node 3", "Node 4", "Node 5",
                                                       "Node 6", "Node 7", "Node 8", "Node 9"};

}  // namespace

void describe(tesser::Object& o, Ensemble& e) {
    o.writeAccess(tesser::Access::User);
    for (int i = 1; i <= RemoteOutputs::kSlots; i++) {
        auto& slot = o.object(kSlotNames[i - 1]).label(kSlotLabels[i - 1]);
        slot.value("device", [&e, i] { return e.device(i); },
                   [&e, i](const std::string& id) { return e.setDevice(i, id); })
            .persist()
            .label("Controller")
            .doc("The controller whose outputs keys mapped to 10 × this slot + output play, by its device id")
            .ui("options", "/peers")
            .ui("optionsValue", "id");
        slot.value("name", [&e, i] {
                tesser::RemoteNode* n = e.node(i);
                return std::string(n ? n->name() : "");
            })
            .doc("Its name under /peers; empty while it isn't found")
            .watch();
        slot.value("online", [&e, i] {
                tesser::RemoteNode* n = e.node(i);
                return n && n->online();
            })
            .watch();
    }
}

}  // namespace photophonique
