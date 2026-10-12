// The /recorder tree (docs/DESIGN.md section 6).
#include <string.h>

#include <memory>

#include "photophonique/elements.h"
#include "photophonique/recorder.h"

namespace photophonique {

namespace {

using tesser::Check;
using tesser::FileRequest;
using tesser::Object;
using tesser::Status;

// A download from memory.
class StringSource : public tesser::FileSource {
public:
    explicit StringSource(std::string data) : data_(std::move(data)) {}
    int64_t size() const override { return static_cast<int64_t>(data_.size()); }
    int read(uint8_t* buf, size_t cap) override {
        size_t n = std::min(cap, data_.size() - at_);
        memcpy(buf, data_.data() + at_, n);
        at_ += n;
        return static_cast<int>(n);
    }

private:
    std::string data_;
    size_t at_ = 0;
};

// An upload collected in memory, then stored (outside the API lock until
// then, as file transfers run).
class ClipSink : public tesser::FileSink {
public:
    ClipSink(Recorder& r, std::string name) : recorder_(r), name_(std::move(name)) {}
    Check write(const uint8_t* data, size_t len) override {
        if (data_.size() + len > kMaxFile) return Check::fail(Status::TooLarge, "at most 64 KB");
        data_.append(reinterpret_cast<const char*>(data), len);
        return Check::ok();
    }
    Check finish(tesser::JsonWriter& reply) override {
        (void)reply;
        tesser::Lock lock(recorder_.api());
        return recorder_.store(name_, data_);
    }

private:
    Recorder& recorder_;
    std::string name_;
    std::string data_;
};

// "Take 1.mid" → "Take 1".
std::string withoutExtension(const std::string& filename) {
    size_t dot = filename.rfind('.');
    if (dot == std::string::npos || dot == 0) return filename;
    std::string ext = filename.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return ext == "mid" || ext == "midi" ? filename.substr(0, dot) : filename;
}

}  // namespace

void describe(Object& o, Recorder& r) {
    o.writeAccess(tesser::Access::User)
        .ui("widget", "x-recorder")
        .ui("script", elements::kRecorderPath)
        .doc("Records what is played, replays it, edits it: Standard MIDI files in the controller's files");

    // The transport.
    r.nodes.state = &o.value("state", [&r] { return r.state(); }, {"stopped", "armed", "recording", "playing"})
                         .doc("Armed: a new take waits for its first note");
    r.nodes.position = &o.value("position", [&r] { return r.position(); })
                            .unit("ms")
                            .doc("In the recording, as of the last start, stop, loop or edit");
    r.nodes.playing = &o.value("playing", [&r] { return r.state() == Recorder::State::Playing; },
                               [&r](bool on) { return r.setPlaying(on); })
                           .doc("True plays the current recording, unless it plays; false stops");
    r.nodes.recording = &o.value("recording", [&r] { return r.recording(); },
                                 [&r](const std::string& name) { return r.choose(name); })
                             .persist()
                             .doc("The current recording's name, empty for none");
    o.value("cue", [&r] { return r.lastCue(); }, [&r](int n) { return r.cue(n); })
        .range(0, Recorder::kMaxCue)
        .doc("n plays the n-th recording of the list, 0 stops: for Art-Net")
        .ui("advanced");
    o.value("loop", r.loop).persist().doc("Replay starts over at the end");
    o.value("overdub", r.overdub).persist().doc("Record onto the current recording instead of a new take");
    o.action("record", [&r] { return r.record(); }).ui("icon", "target");
    o.action("play", [&r] { return r.play(); }).ui("icon", "play");
    o.action("stop", [&r] { r.stop(); }).ui("icon", "stop");

    // The current recording.
    r.nodes.length = &o.value("length", [&r] { return r.clip().length; }, [&r](uint32_t ms) { return r.setLength(ms); })
                          .range(1, kMaxLength)
                          .unit("ms");
    r.nodes.bpm = &o.value("bpm", [&r] { return r.clip().bpm; }, [&r](float bpm) { return r.setBpm(bpm); })
                       .range(kMinBpm, kMaxBpm)
                       .label("Tempo")
                       .unit("BPM");
    r.nodes.beats = &o.value("beats", [&r] { return r.clip().beats; }, [&r](uint8_t b) { return r.setBeats(b); })
                         .range(1, 16)
                         .label("Beats per bar");
    r.nodes.notes = &o.list("notes", r.clip_.notes,
                            [](Object& e, Note& n) {
                                e.value("id", n.id);
                                e.value("t", n.t).unit("ms");
                                e.value("d", n.d).unit("ms");
                                e.value("n", n.n);
                                e.value("v", n.v);
                            })
                         .readOnly()
                         .maxSize(kMaxNotes)
                         .doc("Start t and duration d (ms), pitch n, velocity v; changed through edit");
    r.nodes.bends = &o.list("bends", r.clip_.bends,
                            [](Object& e, BendPoint& b) {
                                e.value("id", b.id);
                                e.value("t", b.t).unit("ms");
                                e.value("v", b.v);
                            })
                         .readOnly()
                         .maxSize(kMaxBends)
                         .doc("Pitch bend points: time t (ms), value v (-8192 to 8191)");
    o.action("edit", [&r](JsonVariantConst body) { return r.edit(body); })
        .doc("{remove: [id], set: [{id, t?, d?, n?, v?}], add: [{t, d, n, v}], addBends: [{t, v}]}")
        .ui("hidden");
    o.action("quantize", [&r](JsonVariantConst body) { return r.quantize(body); })
        .doc("{grid: beats, ids?: [id], ends?: bool}: note starts (and ends) to the grid")
        .ui("hidden");
    r.nodes.full = &o.event("full").label("Recording full").ui("toast", "warn").ui("hidden");

    // The recordings.
    r.nodes.entries =
        &o.list("recordings", r.entries_,
                [&r](Object& e, ClipEntry& c) {
                    std::string name = c.name;  // by value: the element is temporary
                    e.value("name", c.name);
                    e.value("size", c.size).unit("B");
                    e.file("content")
                        .download([&r, name](FileRequest& req) -> std::unique_ptr<tesser::FileSource> {
                            std::string data = r.file(name);
                            if (data.empty()) {
                                req.refuse(Status::NotFound, "no such recording");
                                return nullptr;
                            }
                            req.filename = name + ".mid";
                            return std::unique_ptr<tesser::FileSource>(new StringSource(std::move(data)));
                        })
                        .upload([&r, name](FileRequest&) {
                            return std::unique_ptr<tesser::FileSink>(new ClipSink(r, name));
                        })
                        .maxSize(kMaxFile)
                        .contentType("audio/midi")
                        .accept(".mid,.midi")
                        .label("Standard MIDI file")
                        .ui("confirm", "Replace this recording?");
                    e.action("play", [&r, name] {
                        Check chosen = r.choose(name);
                        return chosen.isOk() ? r.play() : chosen;
                    }).ui("icon", "play");
                    e.action("rename", [&r, name](const std::string& to) { return r.rename(name, to); });
                    e.action("delete", [&r, name] { return r.remove(name); }).ui("confirm", "Delete this recording?");
                })
             .key("name")
             .readOnly()
             .maxSize(Recorder::kMaxCue)
             .label("Recordings");
    o.action("new", [&r](const std::string& name) { return r.create(name); }).label("New recording");
    o.file("upload")
        .upload([&r](FileRequest& req) -> std::unique_ptr<tesser::FileSink> {
            std::string name = withoutExtension(req.filename);
            Check c = Recorder::checkName(name);
            if (!c.isOk()) {
                req.refuse(c.status, req.filename.empty() ? "the file has no name" : c.message);
                return nullptr;
            }
            return std::unique_ptr<tesser::FileSink>(new ClipSink(r, name));
        })
        .maxSize(kMaxFile)
        .accept(".mid,.midi")
        .label("Upload a recording")
        .doc("A Standard MIDI file, named after its file; one of the same name is replaced");
}

}  // namespace photophonique
