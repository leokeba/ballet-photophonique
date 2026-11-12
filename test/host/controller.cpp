// A controller on this computer, for the elements' browser tests without a
// board (docs/DESIGN.md section 10): the tree as the firmware mounts it
// (/outputs on a fake PWM, /midi with its controllers, /ensemble with no
// peers, /recorder with its files in memory and the player on this
// computer's clock), behind TesserAPI's line transport (the serial API's
// envelopes, one per line) on a TCP port of localhost.
// test/recorder/serial_bridge.py reaches it as socket://127.0.0.1:<port>.
//
//   ./build/photophonique_host 8140
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <map>
#include <string>

#include "Photophonique.h"
#include "TesserAPI.h"
#include "tesser/line_transport.h"
#include "views.h"  // firmware/main

namespace {

using namespace photophonique;

struct HostPwm : Pwm {
    bool attach(int, int) override { return true; }
    void timing(int, const Timing&) override {}
    void duty(int, uint32_t) override {}
};

struct MemoryFiles : ClipFiles {
    std::map<std::string, std::string> files;
    std::vector<ClipEntry> list() override {
        std::vector<ClipEntry> out;
        for (auto& f : files) out.push_back({f.first, static_cast<uint32_t>(f.second.size())});
        return out;
    }
    bool read(const std::string& name, std::string& out) override {
        auto it = files.find(name);
        if (it == files.end()) return false;
        out = it->second;
        return true;
    }
    bool write(const std::string& name, std::string_view data) override {
        files[name] = std::string(data);
        return true;
    }
    bool remove(const std::string& name) override { return files.erase(name) > 0; }
    bool rename(const std::string& from, const std::string& to) override {
        auto it = files.find(from);
        if (it == files.end()) return false;
        files[to] = it->second;
        files.erase(it);
        return true;
    }
};

uint64_t nowUs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

struct HostClock : PlayerClock {
    uint64_t nowUs() override { return ::nowUs(); }
    void wake() override {}  // the loop polls every millisecond
};

}  // namespace

int main(int argc, char** argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8140;
    signal(SIGPIPE, SIG_IGN);

    static HostPwm pwm;
    static Output out[5] = {{pwm, 0, 18}, {pwm, 1, 16}, {pwm, 2, 12}, {pwm, 3, 4}, {pwm, 4, 2}};
    static Outputs outputs{out, 5};
    static tesser::Api api;
    static Midi midi(outputs);
    static Ensemble ensemble;
    static MemoryFiles files;
    static HostClock clock;
    static Recorder recorder(api, midi, files, clock);

    api.mount("outputs", outputs).label("Outputs").ui("icon", "bulb");
    ensemble.peers(api.object("peers"));
    midi.remote(&ensemble);
    midi.controllers.attach(api);
    api.mount("midi", midi).label("MIDI").ui("icon", "play");
    api.mount("ensemble", ensemble).label("Ensemble").ui("icon", "link");
    api.mount("recorder", recorder).label("Recorder").ui("icon", "target");
    addViews(api);
    outputs.begin();
    {
        tesser::Lock lock(api);
        recorder.begin();
    }

    int server = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) || listen(server, 1)) {
        perror("photophonique_host");
        return 1;
    }
    printf("photophonique_host: socket://127.0.0.1:%d\n", port);
    fflush(stdout);

    int client = -1;
    tesser::LineTransport line(api, [&client](const char* data, size_t len) {
        while (client >= 0 && len) {
            ssize_t n = write(client, data, len);
            if (n <= 0) return;
            data += n;
            len -= static_cast<size_t>(n);
        }
    });
    uint64_t start = nowUs();
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server, &fds);
        int top = server;
        if (client >= 0) {
            FD_SET(client, &fds);
            if (client > top) top = client;
        }
        timeval tv = {0, 1000};
        if (select(top + 1, &fds, nullptr, nullptr, &tv) > 0) {
            if (FD_ISSET(server, &fds)) {
                int c = accept(server, nullptr, nullptr);
                if (client >= 0) close(client);
                line.reset();
                client = c;
            }
            if (client >= 0 && FD_ISSET(client, &fds)) {
                char buf[4096];
                ssize_t n = read(client, buf, sizeof(buf));
                if (n <= 0) {
                    close(client);
                    client = -1;
                    line.reset();
                } else {
                    line.feed(buf, static_cast<size_t>(n));
                }
            }
        }
        uint64_t now = nowUs();
        uint32_t ms = static_cast<uint32_t>((now - start) / 1000);
        tesser::Lock lock(api);
        recorder.poll();  // every millisecond: what is due plays within one
        midi.controllers.tick(ms);
        api.poll(ms);
    }
}
