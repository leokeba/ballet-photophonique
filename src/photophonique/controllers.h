#pragma once

// MIDI control change (docs/DESIGN.md section 5.5): each controller's last
// value, and mappings that write a controller to any node of the tree, this
// controller's or another's (an ensemble slot's, section 5.7): an output's
// frequency, a stepper's velocity. A mapping is TesserKIT's Art-Net mapping
// for MIDI (a range, a gate), with a curve, a centred mode (64 the middle,
// both directions on one controller), a renewal for nodes that stop when
// they aren't rewritten (deadmen), and a rest value written when playing
// stops. Under the API lock, like Midi.

#include <stdint.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tesser/node.h"
#include "tesser/status.h"

namespace tesser {
class Api;
class LocalClient;
}  // namespace tesser

namespace photophonique {

class RemoteOutputs;

struct CcMapping {
    uint8_t cc = 1;    // 0–119
    uint8_t slot = 0;  // 0: this controller; 1–9: the ensemble slot's controller
    std::string path;  // the node written, from that controller's root
    double min = 0, max = 1;
    float curve = 1;        // the value's exponent: 1 linear, 2 or 3 finer near the low end (the centre if centred)
    bool centred = false;   // 64 is the middle of the range: 0 gives min, 127 max
    uint8_t deadband = 0;   // centred: values this close to 64 count as 64
    uint8_t threshold = 0;  // 1–127: a gate, true while the value is at or above it
    uint16_t renew = 0;     // ms: the last value written again this often; 0 never
    std::string rest;       // JSON written when playing stops (all notes off, replay's stop); "" nothing
};

class Controllers {
public:
    static constexpr int kControllers = 120;  // 120–127 are channel mode messages
    static constexpr size_t kMaxMappings = 16;
    static constexpr size_t kMaxPath = 96;
    static constexpr size_t kMaxRest = 32;

    // Out of line, where LocalClient is complete.
    Controllers();
    ~Controllers();

    // Persisted, bound by the tree.
    std::vector<CcMapping> mappings;

    // Local targets are written through `api` from then on, at User level
    // (the /midi tree is User's: a mapping mustn't reach admin settings).
    void attach(tesser::Api& api);
    // Other controllers' nodes, through the ensemble's slots.
    void remote(RemoteOutputs* r) { remote_ = r; }

    // A controller moved: its value, then the mappings that use it, each
    // written when what it writes changes.
    void moved(uint8_t cc, uint8_t value);
    // Playing stopped: each mapping with a rest value writes it.
    void rest();
    // Renewals, and the local writes' replies: from the device's tick, under
    // the API lock.
    void tick(uint32_t nowMs);

    // What a mapping writes for a value, as JSON: a number (an integer if
    // `integer`), or a boolean for a gate (or a boolean node).
    enum class Kind : uint8_t { Number, Integer, Boolean };
    static std::string mapped(const CcMapping& m, uint8_t value, Kind kind);

    const int8_t (&values() const)[kControllers] { return values_; }
    int last() const { return last_; }
    uint32_t writes() const { return writes_; }
    uint32_t failures() const { return failures_; }
    const std::string& error() const { return error_; }

    // The tree's nodes, announced when they change.
    struct Nodes {
        tesser::Node* values = nullptr;
        tesser::Node* last = nullptr;
        tesser::Node* error = nullptr;
        tesser::Node* mappings = nullptr;
        tesser::EventNode* refused = nullptr;
    };
    Nodes nodes;

private:
    void write(size_t i, const std::string& json);
    Kind kindOf(const CcMapping& m) const;
    void failed(const std::string& error);
    void sync();  // `sent_` follows `mappings`

    tesser::Api* api_ = nullptr;
    std::unique_ptr<tesser::LocalClient> client_;
    RemoteOutputs* remote_ = nullptr;
    int8_t values_[kControllers];
    int last_ = -1;
    struct Sent {
        std::string json;  // "" none yet
        uint32_t atMs = 0;
    };
    std::vector<Sent> sent_;
    uint32_t generation_ = 0;  // the mappings' node, as of sent_
    uint32_t nowMs_ = 0;
    uint32_t writes_ = 0;
    uint32_t failures_ = 0;
    std::string error_;
};

// /midi/cc (docs/DESIGN.md section 6).
void describe(tesser::Object& o, Controllers& c);

}  // namespace photophonique
