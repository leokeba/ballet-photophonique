#pragma once

// The ensemble (docs/DESIGN.md section 5.7): this controller, the master,
// plays other controllers' outputs from MIDI, live and replayed. Each slot
// names a controller by its device id (its Wi-Fi station MAC), mounted under
// /peers by TesserKIT's transports (UDP, NowTP) with TesserAPI's routes. A
// note on one of its outputs becomes a patch of its /outputs, streamed
// (TesserAPI's RemoteNode::stream()): never waiting, applied in order, merged
// with the patches waiting behind it. Under the API lock.

#include <stdint.h>

#include <string>

#include "photophonique/midi.h"
#include "tesser/datagram.h"
#include "tesser/node.h"
#include "tesser/status.h"

namespace tesser {
class RemoteNode;
}

namespace photophonique {

class Ensemble : public RemoteOutputs {
public:
    // The object the transports mount the controllers under (/peers),
    // once the tree has it. Until then, nothing is found.
    void peers(tesser::Object& peers) { peers_ = &peers; }

    static constexpr size_t kIdLength = 12;  // hex digits

    // The controller slot `n` (1 to kSlots) names: its device id, 12
    // lowercase hex digits, or "" for none. Bound by the tree, persisted.
    const std::string& device(int n) const { return slots_[n - 1].device; }
    tesser::Check setDevice(int n, const std::string& id);
    // The controller slot `n` names, if mounted.
    tesser::RemoteNode* node(int n);

    // RemoteOutputs: an output's state, sent at once; the slots' names
    // (the controller's under /peers, its id while it isn't found).
    void play(int slot, int output, bool on, double hz, float duty) override;
    const Names& names() const override { return names_; }
    void namesNode(tesser::Node* node) override { namesNode_ = node; }

    // A controller back online gets the state of the outputs played there,
    // and the names follow the controllers: from the device's tick, under
    // the API lock.
    void tick();

    // Patches streamed, and patches for a controller that wasn't mounted
    // or reachable (dropped).
    uint32_t sent() const { return sent_; }
    uint32_t dropped() const { return dropped_; }

private:
    struct Held {
        bool known = false;  // played since the slot named its controller
        bool on = false;
        double hz = 0;
        float duty = 0;
    };
    struct Slot {
        std::string device;
        tesser::PeerAddress address;  // the device id's bytes
        bool online = false;          // as of the last tick()
        Held held[kMaxOutputs];
    };

    // Streams output `output`'s held state, or every known one's (0).
    void send(int slot, int output);

    tesser::Object* peers_ = nullptr;
    Slot slots_[kSlots];
    std::string names_[kSlots];
    tesser::Node* namesNode_ = nullptr;
    uint32_t sent_ = 0;
    uint32_t dropped_ = 0;
};

// The /ensemble tree (docs/DESIGN.md section 6): api.mount("ensemble", e).
void describe(tesser::Object& o, Ensemble& e);

}  // namespace photophonique
