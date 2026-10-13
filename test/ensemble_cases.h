// The ensemble (docs/DESIGN.md section 5.7): a master playing another
// controller's outputs from MIDI, through TesserAPI's remote nodes over an
// in-memory link, and the controller turning off what a lost master held.
#pragma once

#include <deque>
#include <string>

#include "photophonique/ensemble.h"
#include "rig.h"
#include "tesser/datagram.h"
#include "tesser/remote.h"

using photophonique::Ensemble;

namespace {

tesser::PeerAddress mac(uint8_t last) {
    const uint8_t b[6] = {0x20, 0x50, 0x0d, 0x1b, 0x02, last};
    return tesser::PeerAddress::fromBytes(b, sizeof(b));
}

// Two controllers on one link: the master (Rig's outputs, MIDI, the
// ensemble, NowTP-like addresses: MACs) and a node (Rig's outputs and its own
// MIDI). The master mounts the node under /peers and names it in slot 1.
struct EnsembleRig {
    struct Packet {
        tesser::PeerAddress from, to;
        std::string data;
    };
    std::deque<Packet> wire;
    bool cut = false;  // the link drops everything
    uint32_t clock = 0;
    tesser::PeerAddress masterMac = mac(0x01), nodeMac = mac(0xbc);

    Rig master;
    photophonique::Midi midi{master.outputs};
    Ensemble ensemble;
    tesser::DatagramEndpoint masterLink;

    Rig node;
    photophonique::Midi nodeMidi{node.outputs};
    tesser::DatagramEndpoint nodeLink;
    int released = 0;

    tesser::DatagramEndpoint::SendFn sender(tesser::PeerAddress from) {
        return [this, from](const tesser::PeerAddress& to, const std::string& m, tesser::Delivery) {
            if (!cut) wire.push_back({from, to, m});
            return true;
        };
    }

    EnsembleRig()
        : masterLink(&master.api, tesser::TransportKind::NowTP, sender(masterMac), [this] { return clock; }),
          nodeLink(&node.api, tesser::TransportKind::NowTP, sender(nodeMac), [this] { return clock; }) {
        ensemble.peers(master.api.object("peers"));
        midi.remote(&ensemble);
        master.api.mount("midi", midi);
        master.api.mount("ensemble", ensemble);
        node.api.mount("midi", nodeMidi);
        node.outputs.trackWriters(node.api);
        nodeLink.setPeerAccess(tesser::Access::User);  // as TesserKIT's UdpApi has it
        nodeLink.onPeerLost([this](const tesser::PeerAddress&, const tesser::PeerAddress& device) {
            tesser::Lock lock(node.api);
            released += node.outputs.release(device.bytes, device.length);
        });
        masterLink.mountPeers(master.api.object("peers"));
        masterLink.peerSeen(nodeMac, "stage-left", node.api.schemaHash());
        run();
        CHECK(master.set("/ensemble/1/device", "\"20500d1b02bc\"").status == tesser::Status::Ok);
        midi.outputs[60] = 11;  // C4: the node's output 1
        midi.outputs[62] = 13;  // D4: its output 3
        midi.outputs[64] = 1;   // E4: the master's own output 1
    }

    // Delivers what is on the wire, and what that sends, until it settles.
    void run() {
        for (int i = 0; i < 20; i++) {
            masterLink.process();
            nodeLink.process();
            while (!wire.empty()) {
                Packet p = wire.front();
                wire.pop_front();
                tesser::DatagramEndpoint& to = p.to == masterMac ? masterLink : nodeLink;
                to.receive(p.from, p.data.data(), p.data.size(), true);
            }
        }
    }
    void play(int status, int d1, int d2) {
        {
            tesser::Lock lock(master.api);
            midi.message(static_cast<uint8_t>(status), static_cast<uint8_t>(d1), static_cast<uint8_t>(d2));
        }
        run();
    }
    FakePwm::Channel& pwm(int index) { return node.pwm.ch[index]; }
    tesser::RemoteNode* remote() { return tesser::RemoteNode::byDevice(master.api.object("peers"), nodeMac); }
};

}  // namespace

TEST(ensemble_note_plays_another_controller) {
    EnsembleRig r;
    CHECK(r.remote() != nullptr);
    CHECK_EQ(r.master.get("/ensemble/1/name").body, "\"stage-left\"");
    CHECK_EQ(r.master.get("/ensemble/1/online").body, "true");
    {
        tesser::Lock lock(r.master.api);
        r.ensemble.tick();
    }
    // The key map's editor names the slots.
    CHECK_EQ(r.master.get("/midi/keys/nodes").body, "[\"stage-left\",\"\",\"\",\"\",\"\",\"\",\"\",\"\",\"\"]");
    r.play(0x90, 60, 127);
    CHECK(r.node.out[0].on());
    CHECK(near(r.node.out[0].frequency(), 261.6256, 1e-3));
    CHECK(near(r.node.out[0].duty(), 100, 1e-3));
    CHECK_EQ(r.midi.voice(11), 60);
    CHECK(!r.master.out[0].on());  // the master's own output 1 stays off
    r.play(0x90, 64, 64);          // and plays its own notes as before
    CHECK(r.master.out[0].on());
    CHECK(!r.node.out[2].on());
    // Note off turns it off, keeping its frequency and duty.
    r.play(0x80, 60, 0);
    CHECK(!r.node.out[0].on());
    CHECK(near(r.node.out[0].frequency(), 261.6256, 1e-3));
    CHECK_EQ(r.midi.voice(11), -1);
    CHECK_EQ(r.ensemble.dropped(), 0u);
}

TEST(ensemble_bend_and_all_notes_off) {
    EnsembleRig r;
    r.play(0x90, 60, 100);
    r.play(0x90, 62, 100);
    r.play(0xE0, 0x7F, 0x7F);  // full bend up: two semitones
    CHECK(near(r.node.out[0].frequency(), 261.6256 * pow(2.0, 2.0 / 12), 1e-2));
    CHECK(near(r.node.out[2].frequency(), 293.6648 * pow(2.0, 2.0 / 12), 1e-2));
    r.play(0xB0, 123, 0);
    CHECK(!r.node.out[0].on());
    CHECK(!r.node.out[2].on());
}

TEST(ensemble_burst_lands_in_its_latest_state) {
    EnsembleRig r;
    // Played faster than the link answers: one patch in flight, the rest
    // merged behind it, the last state wins.
    {
        tesser::Lock lock(r.master.api);
        for (int v = 1; v <= 40; v++) {
            r.midi.message(0x90, 60, static_cast<uint8_t>(v));
            r.midi.message(0x90, 62, static_cast<uint8_t>(v));
        }
        r.midi.message(0x80, 62, 0);
    }
    r.run();
    CHECK(r.node.out[0].on());
    CHECK(near(r.node.out[0].duty(), 40 * 100.0 / 127, 1e-3));
    CHECK(!r.node.out[2].on());
    // A note on and its note off waiting behind a patch in flight merge:
    // the note never sounds, but the output ends off (DESIGN 5.7).
    r.play(0x80, 60, 0);
    {
        tesser::Lock lock(r.master.api);
        r.midi.message(0x90, 62, 90);  // in flight
        r.midi.message(0x90, 60, 90);  // these two wait, merged
        r.midi.message(0x80, 60, 0);
    }
    r.run();
    CHECK(r.node.out[2].on());
    CHECK(!r.node.out[0].on());
    CHECK(near(r.node.out[0].duty(), 90 * 100.0 / 127, 1e-3));  // the merged patch, off
    CHECK_EQ(r.pwm(0).counts, 0u);
}

TEST(ensemble_offline_controller_gets_its_state_back) {
    EnsembleRig r;
    r.remote()->setOnline(false);
    r.play(0x90, 60, 127);  // dropped: offline
    CHECK(!r.node.out[0].on());
    CHECK(r.ensemble.dropped() > 0);
    CHECK_EQ(r.master.get("/ensemble/1/online").body, "false");
    {
        tesser::Lock lock(r.master.api);
        r.ensemble.tick();
        r.remote()->setOnline(true);
        r.ensemble.tick();  // back: what it should be playing
    }
    r.run();
    CHECK(r.node.out[0].on());
    CHECK(near(r.node.out[0].duty(), 100, 1e-3));
}

TEST(ensemble_slots_by_device_id) {
    EnsembleRig r;
    CHECK(r.master.set("/ensemble/2/device", "\"12345\"").status == tesser::Status::InvalidValue);
    CHECK(r.master.set("/ensemble/2/device", "\"20500D1B02zz\"").status == tesser::Status::InvalidValue);
    CHECK(r.master.set("/ensemble/2/device", "\"A0B0C0D0E0F0\"").status == tesser::Status::Ok);
    CHECK_EQ(r.master.get("/ensemble/2/device").body, "\"a0b0c0d0e0f0\"");
    // Not mounted: nothing is found, and what it would play is dropped.
    CHECK_EQ(r.master.get("/ensemble/2/name").body, "\"\"");
    CHECK_EQ(r.master.get("/ensemble/2/online").body, "false");
    {
        tesser::Lock lock(r.master.api);
        r.ensemble.tick();
    }
    CHECK_EQ(r.master.get("/midi/keys/nodes/1").body, "\"a0b0c0d0e0f0\"");  // its id until found
    r.midi.outputs[65] = 21;
    r.play(0x90, 65, 100);
    CHECK_EQ(r.ensemble.dropped(), 1u);
    CHECK(r.master.set("/ensemble/2/device", "\"\"").status == tesser::Status::Ok);
    // The key map's range reaches the last slot's last output.
    CHECK(r.master.set("/midi/keys/outputs/10", "98").status == tesser::Status::Ok);
    CHECK(r.master.set("/midi/keys/outputs/10", "99").status != tesser::Status::Ok);
    CHECK(r.node.set("/midi/keys/outputs/10", "6").status != tesser::Status::Ok);  // no ensemble: its own
}

TEST(ensemble_lost_master_turns_off_what_it_held) {
    EnsembleRig r;
    r.play(0x90, 60, 127);  // the master turns the node's output 1 on
    r.node.set("/outputs/2/on", "true");  // its own page, output 2
    r.play(0x90, 62, 127);  // the master, output 3
    {
        // The node's own keyboard takes output 3 from the master.
        tesser::Lock lock(r.node.api);
        r.nodeMidi.outputs[70] = 3;
        r.nodeMidi.message(0x90, 70, 100);
    }
    r.nodeLink.forgetPeer(r.masterMac);  // gone: a restart, silence, a goodbye
    r.run();
    CHECK_EQ(r.released, 1);
    CHECK(!r.node.out[0].on());
    CHECK(r.node.out[1].on());
    CHECK(r.node.out[2].on());
}
