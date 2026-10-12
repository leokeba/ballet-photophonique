#include "photophonique/recorder.h"

#include <math.h>

#include <algorithm>

#include "photophonique/smf.h"
#include "tesser/platform.h"

namespace photophonique {

using tesser::Check;
using tesser::Status;

Recorder::Recorder(tesser::Api& api, Midi& midi, ClipFiles& files, PlayerClock& clock)
    : api_(api), midi_(midi), files_(files), clock_(clock) {
    midi_.listen(this);
}

void Recorder::begin() {
    begun_ = true;
    refreshEntries();
    if (!name_.empty() && !load(name_).isOk()) {
        name_.clear();
        clip_ = Clip();
    }
    announce(nodes.recording);
    changedClip();
}

// The transport.

Check Recorder::record() {
    if (recording_()) return Check::ok();
    uint64_t now = clock_.nowUs();
    if (overdub && !name_.empty() && clip_.length > 0) {
        // Onto the current recording: from where it plays, else from its start.
        if (state_ != State::Playing) {
            releaseHeld();
            startPass(now);
        }
        for (auto& o : open_) o = 0;
        lastBend_ = 0;
        fullSaid_ = false;
        replaying_ = true;
        setState(State::Recording);
        clock_.wake();
        return Check::ok();
    }
    stop();
    flush();
    Clip c;
    c.bpm = clip_.bpm;
    c.beats = clip_.beats;
    previous_ = name_;
    name_ = takeName();
    take_ = true;
    clip_ = std::move(c);
    eventsDirty_ = true;
    for (auto& o : open_) o = 0;
    lastBend_ = 0;
    fullSaid_ = false;
    setPosition(0);
    setState(State::Armed);
    announce(nodes.recording);
    changedClip();
    clock_.wake();
    return Check::ok();
}

Check Recorder::play() {
    if (recording_()) stop();
    if (name_.empty()) return Check::fail(Status::NotAllowed, "no recording chosen");
    if (clip_.length == 0) return Check::fail(Status::NotAllowed, "the recording is empty");
    releaseHeld();
    startPass(clock_.nowUs());
    replaying_ = true;
    setState(State::Playing);
    clock_.wake();
    return Check::ok();
}

void Recorder::stop() {
    uint32_t at = state_ == State::Armed ? 0 : now();
    switch (state_) {
        case State::Armed:
            dropTake();
            break;
        case State::Recording:
            if (take_) {
                finishTake(at);
            } else {
                closeOpen(at);
                save();  // the overdub, at once
            }
            break;
        default:
            break;
    }
    releaseHeld();
    replaying_ = false;
    if (state_ != State::Stopped) {
        setPosition(at);
        setState(State::Stopped);
    }
    clock_.wake();
}

Check Recorder::setPlaying(bool on) {
    if (!on) {
        stop();
        return Check::ok();
    }
    return state_ == State::Playing ? Check::ok() : play();
}

Check Recorder::cue(int n) {
    if (n < 0 || n > kMaxCue) return Check::fail(Status::InvalidValue, "0 to 64");
    if (static_cast<size_t>(n) > entries_.size()) return Check::fail(Status::NotFound, "no such recording");
    cue_ = n;
    if (n == 0) {
        stop();
        return Check::ok();
    }
    Check c = choose(entries_[static_cast<size_t>(n) - 1].name);
    return c.isOk() ? play() : c;
}

Check Recorder::choose(const std::string& name) {
    if (!begun_) {
        name_ = name;  // a stored setting, loaded by begin()
        return Check::ok();
    }
    if (name == name_ && !take_) return Check::ok();
    if (!name.empty() && !exists(name)) return Check::fail(Status::NotFound, "no such recording");
    stop();
    flush();
    if (name.empty()) {
        name_.clear();
        clip_ = Clip();
        eventsDirty_ = true;
    } else {
        Check c = load(name);
        if (!c.isOk()) return c;
        name_ = name;
    }
    setPosition(0);
    announce(nodes.recording);
    changedClip();
    return Check::ok();
}

// Time.

int64_t Recorder::msSince(uint64_t us) const {
    int64_t d = static_cast<int64_t>(us - startUs_);
    return d >= 0 ? d / 1000 : -((-d + 999) / 1000);
}

uint32_t Recorder::timeOf(uint64_t us) const {
    int64_t ms = msSince(us);
    if (take_) return static_cast<uint32_t>(std::min<int64_t>(std::max<int64_t>(ms, 0), kMaxLength));
    int64_t length = clip_.length;
    if (length <= 0) return 0;
    if (loop) {
        ms %= length;
        if (ms < 0) ms += length;  // played before the pass began
        return static_cast<uint32_t>(ms);
    }
    return static_cast<uint32_t>(std::min<int64_t>(std::max<int64_t>(ms, 0), length));
}

uint32_t Recorder::now() {
    if (state_ == State::Stopped || state_ == State::Armed) return position_;
    return timeOf(clock_.nowUs());
}

// Replay.

void Recorder::startPass(uint64_t nowUs) {
    startUs_ = nowUs;
    next_ = 0;
    playedTo_ = -1;
    eventsDirty_ = true;
    setPosition(0);
}

uint64_t Recorder::poll() {
    uint64_t now = clock_.nowUs();
    uint64_t wait = kIdle;
    if (replaying_) wait = playDue(now);
    if (state_ == State::Recording && take_) {
        int64_t ms = msSince(now);
        if (ms >= kMaxLength) {
            stop();
        } else {
            wait = std::min<uint64_t>(wait, static_cast<uint64_t>(kMaxLength - ms) * 1000);
        }
    }
    if (dirty_ && !recording_()) {
        uint64_t due = dirtyAtUs_ + kSaveDelayMs * 1000ull;
        if (now >= due) {
            save();
        } else {
            wait = std::min<uint64_t>(wait, due - now);
        }
    }
    return wait;
}

uint64_t Recorder::playDue(uint64_t now) {
    if (clip_.length == 0) {
        stop();
        return kIdle;
    }
    if (eventsDirty_) rebuild();
    for (;;) {
        int64_t pos = msSince(now);
        int64_t limit = std::min<int64_t>(pos, clip_.length);
        while (next_ < events_.size() && events_[next_].t <= limit) fire(events_[next_++]);
        if (limit > playedTo_) playedTo_ = limit;
        if (pos < clip_.length) break;
        // The end of a pass.
        if (state_ == State::Recording) closeOpen(clip_.length);
        releaseHeld();
        if (!loop) {
            stop();
            return kIdle;
        }
        startUs_ += clip_.length * 1000ull;
        next_ = 0;
        playedTo_ = -1;
        lastBend_ = 0;
        if (eventsDirty_) rebuild();
        setPosition(0);
    }
    uint32_t t = next_ < events_.size() ? events_[next_].t : clip_.length;
    uint64_t due = startUs_ + t * 1000ull;
    return due > now ? due - now : 0;
}

void Recorder::rebuild() {
    events_.clear();
    const uint32_t length = clip_.length;
    size_t firstBend = 0;
    if (!clip_.bends.empty()) {
        // The bend at the start of each pass: the centre unless a point is there.
        int16_t v = 0;
        if (clip_.bends[0].t == 0) {
            v = clip_.bends[0].v;
            firstBend = 1;
        }
        uint16_t u = static_cast<uint16_t>(v + 8192);
        events_.push_back({0, 1, 0xE0, static_cast<uint8_t>(u & 0x7F), static_cast<uint8_t>(u >> 7)});
    }
    for (const Note& n : clip_.notes) {
        if (n.t >= length) continue;
        events_.push_back({n.t, 2, 0x90, n.n, n.v});
        events_.push_back({std::min(n.t + n.d, length), 0, 0x80, n.n, 0});
    }
    for (size_t i = firstBend; i < clip_.bends.size(); i++) {
        const BendPoint& b = clip_.bends[i];
        if (b.t >= length) continue;
        uint16_t u = static_cast<uint16_t>(b.v + 8192);
        events_.push_back({b.t, 1, 0xE0, static_cast<uint8_t>(u & 0x7F), static_cast<uint8_t>(u >> 7)});
    }
    std::stable_sort(events_.begin(), events_.end(),
                     [](const Event& x, const Event& y) { return x.t != y.t ? x.t < y.t : x.order < y.order; });
    // What is past isn't replayed.
    next_ = 0;
    while (next_ < events_.size() && static_cast<int64_t>(events_[next_].t) <= playedTo_) next_++;
    // A note replay holds ends unless a note still covers the position.
    for (uint8_t p = 0; p < 128; p++) {
        if (!held_[p]) continue;
        uint8_t covering = 0;
        for (const Note& n : clip_.notes) {
            if (n.n == p && n.t <= playedTo_ && playedTo_ < std::min(n.t + n.d, length) && covering < 255) covering++;
        }
        if (!covering) midi_.message(0x80, p, 0);
        held_[p] = covering;
    }
    eventsDirty_ = false;
}

void Recorder::fire(const Event& e) {
    if (e.status == 0x90) {
        midi_.message(0x90, e.a, e.b);
        if (held_[e.a] < 255) held_[e.a]++;
    } else if (e.status == 0x80) {
        // Overlapping notes of one pitch: the last to end releases it.
        if (held_[e.a] && --held_[e.a] == 0) midi_.message(0x80, e.a, 0);
    } else {
        midi_.message(e.status, e.a, e.b);
    }
}

void Recorder::releaseHeld() {
    for (uint8_t p = 0; p < 128; p++) {
        if (held_[p]) midi_.message(0x80, p, 0);
        held_[p] = 0;
    }
}

// Recording.

void Recorder::heard(uint8_t status, uint8_t data1, uint8_t data2, uint32_t ageMs) {
    if (!recording_()) return;
    uint8_t kind = status & 0xF0;
    bool on = kind == 0x90 && data2 > 0;
    bool off = kind == 0x80 || (kind == 0x90 && data2 == 0);
    bool bend = kind == 0xE0;
    bool all = kind == 0xB0 && (data1 == 120 || data1 == 123);
    if (!on && !off && !bend && !all) return;
    uint64_t now = clock_.nowUs();
    uint64_t age = static_cast<uint64_t>(ageMs) * 1000;
    uint64_t at = now - std::min(age, now);
    if (state_ == State::Armed) {
        // A take starts at its first note or bend.
        if (!on && !bend) return;
        startUs_ = at;
        setState(State::Recording);
    }
    uint32_t t = timeOf(at);
    if (on) {
        addNote(t, data1, data2);
    } else if (off) {
        closeNote(data1, t);
    } else if (bend) {
        addBend(t, static_cast<int16_t>((data2 << 7 | data1) - 8192));
    } else {
        closeOpen(t);
    }
    eventsDirty_ = true;
    dirty_ = true;
    changedClip();
}

void Recorder::addNote(uint32_t t, uint8_t n, uint8_t v) {
    closeNote(n, t);
    if (clip_.notes.size() >= kMaxNotes) {
        full();
        return;
    }
    Note note;
    note.id = nextId_++;
    note.t = t;
    note.d = 1;
    note.n = n;
    note.v = v;
    auto it = std::upper_bound(clip_.notes.begin(), clip_.notes.end(), note,
                               [](const Note& a, const Note& b) { return a.t != b.t ? a.t < b.t : a.n < b.n; });
    clip_.notes.insert(it, note);
    open_[n] = note.id;
}

void Recorder::closeNote(uint8_t n, uint32_t t) {
    if (!open_[n]) return;
    if (Note* note = findNote(open_[n])) note->d = t > note->t ? t - note->t : 1;
    open_[n] = 0;
}

void Recorder::closeOpen(uint32_t t) {
    for (uint8_t n = 0; n < 128; n++) closeNote(n, t);
}

void Recorder::addBend(uint32_t t, int16_t v) {
    if (BendPoint* last = lastBend_ ? findBend(lastBend_) : nullptr) {
        if (t >= last->t && t - last->t < kThinMs) {
            last->v = v;
            return;
        }
    }
    if (clip_.bends.size() >= kMaxBends) {
        full();
        return;
    }
    BendPoint b;
    b.id = nextId_++;
    b.t = t;
    b.v = v;
    auto it = std::upper_bound(clip_.bends.begin(), clip_.bends.end(), b,
                               [](const BendPoint& x, const BendPoint& y) { return x.t < y.t; });
    clip_.bends.insert(it, b);
    lastBend_ = b.id;
}

void Recorder::full() {
    if (fullSaid_) return;
    fullSaid_ = true;
    if (nodes.full) nodes.full->emit("the recording is full: 1024 notes or bend points");
}

void Recorder::finishTake(uint32_t at) {
    closeOpen(at);
    if (clip_.notes.empty() && clip_.bends.empty()) {
        dropTake();
        return;
    }
    uint32_t end = std::max<uint32_t>(at, 1);
    for (const Note& n : clip_.notes) end = std::max(end, n.t + n.d);
    for (const BendPoint& b : clip_.bends) end = std::max(end, b.t);
    clip_.length = std::min(end, kMaxLength);
    take_ = false;
    previous_.clear();
    save();
    changedClip();
}

void Recorder::dropTake() {
    take_ = false;
    dirty_ = false;
    std::string previous = std::move(previous_);
    previous_.clear();
    name_.clear();
    clip_ = Clip();
    if (!previous.empty() && !load(previous).isOk()) previous.clear();
    name_ = previous;
    eventsDirty_ = true;
    announce(nodes.recording);
    changedClip();
}

// Editing.

Check Recorder::editable() const {
    if (recording_()) return Check::fail(Status::Busy, "recording");
    if (name_.empty()) return Check::fail(Status::NotAllowed, "no recording chosen");
    return Check::ok();
}

namespace {

// An optional unsigned field of an edit within a range.
bool field(JsonObjectConst o, const char* key, int64_t min, int64_t max, int64_t& out, bool& present) {
    JsonVariantConst v = o[key];
    present = !v.isNull();
    if (!present) return true;
    if (!v.is<int64_t>()) return false;
    out = v.as<int64_t>();
    return out >= min && out <= max;
}

bool ids(JsonVariantConst v, std::vector<uint32_t>& out) {
    if (v.isNull()) return true;
    if (!v.is<JsonArrayConst>()) return false;
    for (JsonVariantConst id : v.as<JsonArrayConst>()) {
        if (!id.is<uint32_t>()) return false;
        out.push_back(id.as<uint32_t>());
    }
    return true;
}

}  // namespace

Check Recorder::edit(JsonVariantConst body) {
    Check c = editable();
    if (!c.isOk()) return c;
    if (!body.is<JsonObjectConst>()) return Check::fail(Status::InvalidValue, "{remove, set, add, addBends}");
    JsonObjectConst o = body.as<JsonObjectConst>();
    for (JsonPairConst kv : o) {
        const char* k = kv.key().c_str();
        if (strcmp(k, "remove") && strcmp(k, "set") && strcmp(k, "add") && strcmp(k, "addBends")) {
            return Check::fail(Status::InvalidValue, "unknown field: remove, set, add, addBends");
        }
    }
    const int64_t maxT = kMaxLength;

    // Validated whole before anything changes. Ids already gone (another
    // client's edit) are skipped.
    std::vector<uint32_t> removed;
    if (!ids(o["remove"], removed)) return Check::fail(Status::InvalidValue, "remove: an array of ids");
    size_t notesLeft = clip_.notes.size(), bendsLeft = clip_.bends.size();
    std::sort(removed.begin(), removed.end());
    removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
    for (uint32_t id : removed) {
        if (findNote(id)) notesLeft--;
        if (findBend(id)) bendsLeft--;
    }
    JsonVariantConst sets = o["set"];
    if (!sets.isNull() && !sets.is<JsonArrayConst>()) return Check::fail(Status::InvalidValue, "set: an array");
    for (JsonVariantConst s : sets.as<JsonArrayConst>()) {
        if (!s.is<JsonObjectConst>() || !s["id"].is<uint32_t>()) return Check::fail(Status::InvalidValue, "set: {id, ...}");
        JsonObjectConst e = s.as<JsonObjectConst>();
        int64_t v;
        bool has;
        bool bend = findBend(e["id"].as<uint32_t>()) != nullptr;
        if (!field(e, "t", 0, maxT, v, has)) return Check::fail(Status::InvalidValue, "set: t, 0 to 14400000 ms");
        if (bend) {
            if (!e["d"].isNull() || !e["n"].isNull()) return Check::fail(Status::InvalidValue, "set: a bend point has t and v");
            if (!field(e, "v", -8192, 8191, v, has)) return Check::fail(Status::InvalidValue, "set: a bend, -8192 to 8191");
        } else {
            if (!field(e, "d", 1, maxT, v, has)) return Check::fail(Status::InvalidValue, "set: d, 1 to 14400000 ms");
            if (!field(e, "n", 0, 127, v, has)) return Check::fail(Status::InvalidValue, "set: n, 0 to 127");
            if (!field(e, "v", 1, 127, v, has)) return Check::fail(Status::InvalidValue, "set: v, 1 to 127");
        }
    }
    JsonVariantConst adds = o["add"];
    if (!adds.isNull() && !adds.is<JsonArrayConst>()) return Check::fail(Status::InvalidValue, "add: an array");
    for (JsonVariantConst a : adds.as<JsonArrayConst>()) {
        if (!a.is<JsonObjectConst>()) return Check::fail(Status::InvalidValue, "add: {t, d, n, v}");
        JsonObjectConst e = a.as<JsonObjectConst>();
        int64_t v;
        bool t, d, n, vel;
        if (!field(e, "t", 0, maxT, v, t) || !field(e, "d", 1, maxT, v, d) || !field(e, "n", 0, 127, v, n) ||
            !field(e, "v", 1, 127, v, vel) || !t || !d || !n) {
            return Check::fail(Status::InvalidValue, "add: {t: 0–14400000, d: 1–14400000, n: 0–127, v: 1–127}");
        }
        notesLeft++;
    }
    JsonVariantConst addBends = o["addBends"];
    if (!addBends.isNull() && !addBends.is<JsonArrayConst>()) return Check::fail(Status::InvalidValue, "addBends: an array");
    for (JsonVariantConst a : addBends.as<JsonArrayConst>()) {
        if (!a.is<JsonObjectConst>()) return Check::fail(Status::InvalidValue, "addBends: {t, v}");
        JsonObjectConst e = a.as<JsonObjectConst>();
        int64_t v;
        bool t, has;
        if (!field(e, "t", 0, maxT, v, t) || !field(e, "v", -8192, 8191, v, has) || !t || !has) {
            return Check::fail(Status::InvalidValue, "addBends: {t: 0–14400000, v: -8192–8191}");
        }
        bendsLeft++;
    }
    if (notesLeft > kMaxNotes) return Check::fail(Status::TooLarge, "more than 1024 notes");
    if (bendsLeft > kMaxBends) return Check::fail(Status::TooLarge, "more than 1024 bend points");

    // Applied.
    auto gone = [&removed](uint32_t id) { return std::binary_search(removed.begin(), removed.end(), id); };
    clip_.notes.erase(std::remove_if(clip_.notes.begin(), clip_.notes.end(), [&](const Note& n) { return gone(n.id); }),
                      clip_.notes.end());
    clip_.bends.erase(
        std::remove_if(clip_.bends.begin(), clip_.bends.end(), [&](const BendPoint& b) { return gone(b.id); }),
        clip_.bends.end());
    for (JsonVariantConst s : sets.as<JsonArrayConst>()) {
        uint32_t id = s["id"].as<uint32_t>();
        if (Note* n = findNote(id)) {
            if (!s["t"].isNull()) n->t = s["t"].as<uint32_t>();
            if (!s["d"].isNull()) n->d = s["d"].as<uint32_t>();
            if (!s["n"].isNull()) n->n = s["n"].as<uint8_t>();
            if (!s["v"].isNull()) n->v = s["v"].as<uint8_t>();
        } else if (BendPoint* b = findBend(id)) {
            if (!s["t"].isNull()) b->t = s["t"].as<uint32_t>();
            if (!s["v"].isNull()) b->v = s["v"].as<int16_t>();
        }
    }
    for (JsonVariantConst a : adds.as<JsonArrayConst>()) {
        Note n;
        n.id = nextId_++;
        n.t = a["t"].as<uint32_t>();
        n.d = a["d"].as<uint32_t>();
        n.n = a["n"].as<uint8_t>();
        n.v = a["v"].isNull() ? 100 : a["v"].as<uint8_t>();
        clip_.notes.push_back(n);
    }
    for (JsonVariantConst a : addBends.as<JsonArrayConst>()) {
        BendPoint b;
        b.id = nextId_++;
        b.t = a["t"].as<uint32_t>();
        b.v = a["v"].as<int16_t>();
        clip_.bends.push_back(b);
    }
    clip_.sort();
    eventsDirty_ = true;
    markDirty();
    changedClip();
    return Check::ok();
}

Check Recorder::quantize(JsonVariantConst body) {
    Check c = editable();
    if (!c.isOk()) return c;
    if (!body.is<JsonObjectConst>() || !body["grid"].is<double>()) {
        return Check::fail(Status::InvalidValue, "{grid: beats, ids?: [id], ends?: bool}");
    }
    double grid = body["grid"].as<double>();
    if (!(grid >= 1.0 / 64 && grid <= 4)) return Check::fail(Status::InvalidValue, "grid: 1/64 to 4 beats");
    std::vector<uint32_t> only;
    if (!ids(body["ids"], only)) return Check::fail(Status::InvalidValue, "ids: an array of ids");
    JsonVariantConst endsV = body["ends"];
    if (!endsV.isNull() && !endsV.is<bool>()) return Check::fail(Status::InvalidValue, "ends: a boolean");
    bool ends = endsV.as<bool>();
    std::sort(only.begin(), only.end());
    double step = grid * 60000.0 / clip_.bpm;
    auto snap = [step](double ms) { return static_cast<uint32_t>(lround(round(ms / step) * step)); };
    for (Note& n : clip_.notes) {
        if (!body["ids"].isNull() && !std::binary_search(only.begin(), only.end(), n.id)) continue;
        uint32_t t = std::min(snap(n.t), kMaxLength);
        if (ends) {
            uint32_t e = snap(n.t + n.d);
            uint32_t least = std::max<uint32_t>(1, static_cast<uint32_t>(lround(step)));
            n.d = e > t + least ? e - t : least;
        }
        n.t = t;
    }
    clip_.sort();
    eventsDirty_ = true;
    markDirty();
    changedClip();
    return Check::ok();
}

Check Recorder::setLength(uint32_t ms) {
    Check c = editable();
    if (!c.isOk()) return c;
    clip_.length = ms;
    eventsDirty_ = true;
    markDirty();
    announce(nodes.length);
    return Check::ok();
}

Check Recorder::setBpm(float bpm) {
    Check c = editable();
    if (!c.isOk()) return c;
    clip_.bpm = bpm;
    markDirty();
    announce(nodes.bpm);
    return Check::ok();
}

Check Recorder::setBeats(uint8_t beats) {
    Check c = editable();
    if (!c.isOk()) return c;
    clip_.beats = beats;
    markDirty();
    announce(nodes.beats);
    return Check::ok();
}

// Recordings.

Check Recorder::checkName(std::string_view name) {
    if (name.empty() || name.size() > kMaxName) return Check::fail(Status::InvalidValue, "1 to 28 characters");
    if (name[0] == '.') return Check::fail(Status::InvalidValue, "can't start with a dot");
    for (char ch : name) {
        auto u = static_cast<unsigned char>(ch);
        if (ch == '/' || u < 0x20 || u == 0x7F) return Check::fail(Status::InvalidValue, "no '/' or control characters");
    }
    return Check::ok();
}

Check Recorder::create(const std::string& name) {
    Check c = checkName(name);
    if (!c.isOk()) return c;
    if (exists(name)) return Check::fail(Status::InvalidValue, "a recording has that name");
    stop();
    flush();
    Clip clip;
    clip.length = kNewLength;
    if (!files_.write(name, smf::write(clip))) return Check::fail(Status::Internal, "the file couldn't be written");
    name_ = name;
    adopt(std::move(clip));
    setPosition(0);
    refreshEntries();
    announce(nodes.recording);
    changedClip();
    return Check::ok();
}

Check Recorder::rename(const std::string& from, const std::string& to) {
    Check c = checkName(to);
    if (!c.isOk()) return c;
    if (from == to) return Check::ok();
    if (exists(to)) return Check::fail(Status::InvalidValue, "a recording has that name");
    if (from == name_) flush();
    if (!files_.rename(from, to)) return Check::fail(Status::Internal, "the file couldn't be renamed");
    if (from == name_ && !take_) {
        name_ = to;
        announce(nodes.recording);
    }
    refreshEntries();
    return Check::ok();
}

Check Recorder::remove(const std::string& name) {
    if (name == name_ && !take_) {
        stop();
        dirty_ = false;
        name_.clear();
        clip_ = Clip();
        eventsDirty_ = true;
        setPosition(0);
        announce(nodes.recording);
        changedClip();
    }
    bool ok = files_.remove(name);
    refreshEntries();
    return ok ? Check::ok() : Check::fail(Status::NotFound, "no such recording");
}

Check Recorder::store(const std::string& name, std::string_view data) {
    Check c = checkName(name);
    if (!c.isOk()) return c;
    Clip clip;
    if (const char* why = smf::read(data, clip)) return Check::fail(Status::InvalidValue, why);
    bool current = name == name_ && !take_;
    if (current) stop();
    if (!files_.write(name, data)) return Check::fail(Status::Internal, "the file couldn't be written");
    if (current) {
        dirty_ = false;
        adopt(std::move(clip));
        setPosition(0);
        changedClip();
    }
    refreshEntries();
    return Check::ok();
}

std::string Recorder::file(const std::string& name) {
    if (name == name_) flush();
    std::string out;
    if (!files_.read(name, out)) out.clear();
    return out;
}

// Files and announcements.

Check Recorder::load(const std::string& name) {
    std::string data;
    if (!files_.read(name, data)) return Check::fail(Status::NotFound, "no such recording");
    Clip clip;
    if (const char* why = smf::read(data, clip)) return Check::fail(Status::InvalidValue, why);
    adopt(std::move(clip));
    return Check::ok();
}

void Recorder::adopt(Clip&& c) {
    clip_ = std::move(c);
    for (Note& n : clip_.notes) n.id = nextId_++;
    for (BendPoint& b : clip_.bends) b.id = nextId_++;
    eventsDirty_ = true;
    dirty_ = false;
}

void Recorder::save() {
    dirty_ = false;
    if (name_.empty() || take_) return;
    if (!files_.write(name_, smf::write(clip_))) {
        tesser::logWarning("photophonique: recording \"%s\" couldn't be saved", name_.c_str());
    }
    refreshEntries();
}

void Recorder::flush() {
    if (dirty_ && !recording_()) save();
}

void Recorder::markDirty() {
    dirty_ = true;
    dirtyAtUs_ = clock_.nowUs();
    clock_.wake();
}

void Recorder::refreshEntries() {
    entries_ = files_.list();
    announce(nodes.entries);
}

bool Recorder::exists(const std::string& name) const {
    for (const ClipEntry& e : entries_) {
        if (e.name == name) return true;
    }
    return false;
}

std::string Recorder::takeName() const {
    for (int n = 1;; n++) {
        std::string name = "Take " + std::to_string(n);
        if (!exists(name) && name != name_) return name;
    }
}

Note* Recorder::findNote(uint32_t id) {
    for (Note& n : clip_.notes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

BendPoint* Recorder::findBend(uint32_t id) {
    for (BendPoint& b : clip_.bends) {
        if (b.id == id) return &b;
    }
    return nullptr;
}

void Recorder::changedClip() {
    announce(nodes.notes);
    announce(nodes.bends);
    announce(nodes.length);
    announce(nodes.bpm);
    announce(nodes.beats);
    if (replaying_) clock_.wake();
}

void Recorder::setPosition(uint32_t ms) {
    position_ = ms;
    announce(nodes.position);
}

void Recorder::setState(State s) {
    if (s == state_) return;
    state_ = s;
    announce(nodes.state);
    announce(nodes.playing);
}

void Recorder::announce(tesser::Node* n) {
    if (n) n->changed();
}

}  // namespace photophonique
