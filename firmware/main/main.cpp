// Ballet Photophonique (docs/DESIGN.md): five independent PWM outputs on a
// Gledopto GL-C-611WL, each with its own duty, frequency and resolution,
// under /outputs; TesserKIT for Wi-Fi, time, updates, files and backups,
// timers, accounts and Art-Net under /sys.
//
// Flash it, join the "ballet-photophonique-xxxx" network from a phone, and
// the interface opens; or reach it from TesserUI's launcher
// (https://leokeba.github.io/TesserKIT/): over USB, or by its address. A fresh
// controller boots with every output off.
//
// For development, a gitignored main/secrets.h may define WIFI_SSID and
// WIFI_PASSWORD, and WIFI_TX_POWER (dBm) for boards with a weak supply.
#include "Photophonique.h"
#include "TesserKIT.h"
#include "board.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

namespace {

tesserkit::Device dev("Ballet Photophonique");
tesserkit::System sys(dev);
tesserkit::Network net(dev);
tesserkit::Time timekeeper(dev);
tesserkit::Firmware firmware(dev);
tesserkit::Files files(dev);  // LittleFS: the stored backups
tesserkit::Backup backup(dev);
tesserkit::Timers timers(dev);
tesserkit::Auth auth(dev);  // open until an admin account exists
tesserkit::ArtNet artnet(dev);
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

}  // namespace

extern "C" void app_main() {
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
    net.defaultNetwork(WIFI_SSID, WIFI_PASSWORD);
#endif
#if defined(WIFI_TX_POWER)
    net.txPower(WIFI_TX_POWER);
#endif
    timekeeper.defaultTz("CET-1CEST,M3.5.0,M10.5.0/3");  // Paris
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
    // Duties that waited for their channel's period (pwm.h), in TesserKIT's
    // task, under the lock the tree's setters hold.
    dev.onTick([&api](uint32_t) {
        tesser::Lock lock(api);
        photophonique::defaultPwm().poll();
    });

    dev.cors("https://leokeba.github.io");  // the launcher, reaching the device by its address
    dev.begin();  // loads the stored settings
    outputs.begin();
    serialApi.begin();
}
