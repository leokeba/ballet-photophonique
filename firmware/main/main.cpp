// Ballet Photophonique (docs/DESIGN.md): five independent PWM outputs on a
// Gledopto GL-C-611WL, each with its own duty, frequency and resolution,
// under /outputs, played from a MIDI keyboard under /midi, recorded and
// replayed under /recorder; TesserKIT for
// Ethernet and Wi-Fi, time, updates from this repository's releases, files
// and backups, timers, accounts, Art-Net and the API over UDP (the other
// controllers on the LAN, under /peers) under /sys.
//
// Flash it and plug in a cable, or join the "ballet-photophonique-xxxx"
// network from a phone, and the interface opens; or reach it from the
// launcher (https://leokeba.github.io/ballet-photophonique/): over USB, or by
// its address. A fresh controller boots with every output off.
//
// For development, a gitignored main/secrets.h may define WIFI_SSID and
// WIFI_PASSWORD, and WIFI_TX_POWER (dBm) for boards with a weak supply.
#include "Photophonique.h"
#include "photophonique/elements.h"
#include "TesserKIT.h"
#include "board.h"
#include "clip_files.h"
#include "tesserui/views.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

namespace {

tesserkit::Device dev("Ballet Photophonique");
tesserkit::System sys(dev);
tesserkit::Network net(dev);
tesserkit::NowTp espNow(dev);  // the controllers nearby over ESP-NOW, for testing (after Network)
tesserkit::Time timekeeper(dev);
tesserkit::Firmware firmware(dev);
tesserkit::Files files(dev);  // LittleFS: the stored backups, the recordings
tesserkit::Backup backup(dev);
tesserkit::Timers timers(dev);
tesserkit::Auth auth(dev);  // open until an admin account exists
tesserkit::ArtNet artnet(dev);
tesserkit::UdpApi udp(dev);  // the controllers on the LAN, under /peers
tesserkit::Peers peers(dev);  // how they are reached (UDP, then NowTP), and signed in to
// The launcher's USB way: envelopes on the console's UART, one per line, with
// Admin access (physical access). Logs stay at warnings, so that few land in
// the middle of a reply (sdkconfig.defaults).
tesser::UartTransport serialApi(dev.api(), UART_NUM_0);

photophonique::Output out[5] = {{photophonique::defaultPwm(), 0, board::kOutputs[0]},
                                {photophonique::defaultPwm(), 1, board::kOutputs[1]},
                                {photophonique::defaultPwm(), 2, board::kOutputs[2]},
                                {photophonique::defaultPwm(), 3, board::kOutputs[3]},
                                {photophonique::defaultPwm(), 4, board::kOutputs[4]}};
photophonique::Outputs outputs{out, 5};
photophonique::Midi midi(outputs);
// Other controllers' outputs, played by keys mapped to them (DESIGN 5.7).
photophonique::Ensemble ensemble;
// Recordings in recordings/*.mid, replayed by the player's task.
ClipFilesOnFiles clipFiles(files);
photophonique::Recorder recorder(dev.api(), midi, clipFiles, photophonique::defaultClock());

// The first page (docs/DESIGN.md section 7): the keyboard, which forwards
// MIDI only while it is on screen, beside what each output does.
void addViews(tesser::Api& api) {
    static tesserui::Views views(api);
    auto& play = views.page("play", "Play").icon("play");
    play.card("Keyboard").widget("x-midi", "/midi/keys").hint("script", photophonique::elements::kMidiPath);
    static const char* const kLabels[5] = {"Output 1", "Output 2", "Output 3", "Output 4", "Output 5"};
    static const char* const kOn[5] = {"/outputs/1/on", "/outputs/2/on", "/outputs/3/on", "/outputs/4/on",
                                       "/outputs/5/on"};
    static const char* const kHz[5] = {"/outputs/1/applied/frequency", "/outputs/2/applied/frequency",
                                       "/outputs/3/applied/frequency", "/outputs/4/applied/frequency",
                                       "/outputs/5/applied/frequency"};
    static const char* const kDuty[5] = {"/outputs/1/duty", "/outputs/2/duty", "/outputs/3/duty",
                                         "/outputs/4/duty", "/outputs/5/duty"};
    auto& card = play.card("Outputs");
    for (int i = 0; i < 5; i++) {
        card.row()
            .widget("toggle", kOn[i], kLabels[i])
            .widget("readout", kHz[i], "Frequency")
            .widget("readout", kDuty[i], "Duty");
    }
    card.row().node("/outputs/off").node("/midi/off");
    play.card("Pitch bend").node("/midi/bend");

    // Recording needs the keyboard on screen too. The keyboard and the
    // recordings side by side, the piano roll across the page under them
    // ("wide", TesserUI e87ff4d: a hint on the page right after card() goes
    // on that card; older renderers ignore it).
    auto& record = views.page("record", "Record").icon("target");
    record.card("Keyboard").widget("x-midi", "/midi/keys").hint("script", photophonique::elements::kMidiPath);
    record.card("Recordings").node("/recorder/recordings").node("/recorder/upload");
    record.card("Recorder")
        .widget("x-recorder", "/recorder")
        .hint("script", photophonique::elements::kRecorderPath);
    record.hint("wide", true);
}

}  // namespace

extern "C" void app_main() {
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
    net.defaultNetwork(WIFI_SSID, WIFI_PASSWORD);
#endif
#if defined(WIFI_TX_POWER)
    net.txPower(WIFI_TX_POWER);
#endif
    // The board's LAN8720, preferred to Wi-Fi while it has an address.
    net.ethernet(tesserkit::boards::gledopto());
    timekeeper.defaultTz("CET-1CEST,M3.5.0,M10.5.0/3");  // Paris
    // The ensemble (DESIGN 5.7): the controllers by UDP over Ethernet for the
    // show, by NowTP for testing (off until turned on), each streamed to by
    // a master with nine slots.
    espNow.enable(false).mountPeers(true).maxCalls(10);
    // A node handles its master's writes in UDP's own task: in TesserKIT's
    // (its default since v0.1.14) they wait about 10 ms more.
    udp.maxCalls(10).ownTask(true);
    peers.order("udp, nowtp");
    // A master gone from every transport, or restarted: what it turned on
    // here goes off.
    peers.onDeviceLost([](const tesser::PeerAddress& device, tesser::PeerLoss) {
        outputs.release(device.bytes, device.length);
    });
    // Updates from this repository's releases (.github/workflows/release.yml),
    // and from any URL.
    firmware.github("leokeba/ballet-photophonique", "ballet-photophonique-{target}.bin");
    // Art-Net: each output's duty on two channels (16 bits), 1–2 for output
    // 1 to 9–10 for output 5. Off until turned on, so a console on the
    // network can't take the outputs over.
    artnet.enable(false).universe(0);
    {
        static const char* const kDutyPaths[5] = {"/outputs/1/duty", "/outputs/2/duty", "/outputs/3/duty",
                                                  "/outputs/4/duty", "/outputs/5/duty"};
        for (int i = 0; i < 5; i++) {
            artnet.map({kDutyPaths[i], static_cast<uint16_t>(2 * i + 1), static_cast<uint16_t>(2 * i + 2), 0, 100});
        }
    }

    auto& api = dev.api();
    api.mount("outputs", outputs).label("Outputs").ui("icon", "bulb");
    // MIDI from the browser (x-midi, served from flash here and from the
    // launcher's own page on GitHub Pages), onto this controller's outputs
    // and the ensemble's.
    midi.remote(&ensemble);
    api.mount("midi", midi).label("MIDI").ui("icon", "play");
    api.mount("ensemble", ensemble).label("Ensemble").ui("icon", "link");
    namespace el = photophonique::elements;
    dev.web().asset(el::kMidiPath, el::kMidi, sizeof(el::kMidi), "text/javascript", true);
    api.mount("recorder", recorder).label("Recorder").ui("icon", "target");
    dev.web().asset(el::kRecorderPath, el::kRecorder, sizeof(el::kRecorder), "text/javascript", true);
    addViews(api);
    // Duties that waited for their channel's period (pwm.h), in TesserKIT's
    // task, under the lock the tree's setters hold.
    // The ensemble's controllers back online get what they should be
    // playing, in the same tick.
    dev.onTick([&api](uint32_t) {
        tesser::Lock lock(api);
        photophonique::defaultPwm().poll();
        ensemble.tick();
    });

    dev.cors("https://leokeba.github.io");  // the launcher, reaching the device by its address
    dev.begin();  // loads the stored settings
    outputs.begin();
    // Outputs remember which controller wrote them, so a lost master's are
    // turned off (peers.onDeviceLost above). The controllers are mounted
    // under /peers.
    outputs.trackWriters(api);
    {
        tesser::Lock lock(api);
        ensemble.peers(peers.nodes());
    }
    {
        tesser::Lock lock(api);
        recorder.begin();  // the current recording, from the files mounted by dev.begin()
    }
    photophonique::startPlayer(api, recorder);
    serialApi.begin();
}
