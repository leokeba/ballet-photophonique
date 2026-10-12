#include "photophonique/smf.h"

#include <math.h>

#include <string.h>

#include <algorithm>

namespace photophonique {

void Clip::sort() {
    std::stable_sort(notes.begin(), notes.end(),
                     [](const Note& a, const Note& b) { return a.t != b.t ? a.t < b.t : a.n < b.n; });
    std::stable_sort(bends.begin(), bends.end(), [](const BendPoint& a, const BendPoint& b) { return a.t < b.t; });
}

namespace smf {

namespace {

constexpr uint16_t kPpq = 480;

void put32(std::string& s, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) s.push_back(static_cast<char>(v >> shift & 0xFF));
}

void putVlq(std::string& s, uint32_t v) {
    uint8_t buf[5];
    int n = 0;
    buf[n++] = static_cast<uint8_t>(v & 0x7F);
    while (v >>= 7) buf[n++] = static_cast<uint8_t>(0x80 | (v & 0x7F));
    while (n) s.push_back(static_cast<char>(buf[--n]));
}

struct Out {
    uint32_t tick;
    uint8_t order;  // at one tick: offs, then bends, then ons, then the end marker
    uint8_t bytes[3];
};

// The recording's length, when notes run past it: a Marker named "end".
constexpr const char kEndMarker[] = "end";

}  // namespace

std::string write(const Clip& clip) {
    double bpm = clip.bpm >= kMinBpm && clip.bpm <= kMaxBpm ? clip.bpm : 120.0;
    double ticksPerMs = bpm * kPpq / 60000.0;
    auto tick = [ticksPerMs](uint32_t ms) { return static_cast<uint32_t>(lround(ms * ticksPerMs)); };

    std::vector<Out> events;
    events.reserve(clip.notes.size() * 2 + clip.bends.size());
    for (const Note& n : clip.notes) {
        events.push_back({tick(n.t), 2, {0x90, n.n, n.v}});
        events.push_back({tick(n.t + n.d), 0, {0x80, n.n, 64}});
    }
    for (const BendPoint& b : clip.bends) {
        uint16_t v = static_cast<uint16_t>(b.v + 8192);
        events.push_back({tick(b.t), 1, {0xE0, static_cast<uint8_t>(v & 0x7F), static_cast<uint8_t>(v >> 7)}});
    }
    events.push_back({tick(clip.length), 3, {0xFF, 0x06, 3}});  // the end marker's head; its text follows
    std::stable_sort(events.begin(), events.end(), [](const Out& a, const Out& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });

    std::string track;
    uint32_t tempo = static_cast<uint32_t>(lround(60000000.0 / bpm));
    track.append("\x00\xFF\x51\x03", 4);
    track.push_back(static_cast<char>(tempo >> 16 & 0xFF));
    track.push_back(static_cast<char>(tempo >> 8 & 0xFF));
    track.push_back(static_cast<char>(tempo & 0xFF));
    track.append("\x00\xFF\x58\x04", 4);
    track.push_back(static_cast<char>(clip.beats ? clip.beats : 4));
    track.append("\x02\x18\x08", 3);  // a quarter note per beat
    uint32_t at = 0;
    for (const Out& e : events) {
        putVlq(track, e.tick - at);
        at = e.tick;
        track.append(reinterpret_cast<const char*>(e.bytes), 3);
        if (e.order == 3) track.append(kEndMarker, 3);
    }
    uint32_t end = std::max(at, tick(clip.length));
    putVlq(track, end - at);
    track.append("\xFF\x2F\x00", 3);

    std::string s("MThd", 4);
    put32(s, 6);
    s.append("\x00\x00\x00\x01", 4);  // type 0, one track
    s.push_back(static_cast<char>(kPpq >> 8));
    s.push_back(static_cast<char>(kPpq & 0xFF));
    s.append("MTrk", 4);
    put32(s, static_cast<uint32_t>(track.size()));
    s += track;
    return s;
}

namespace {

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;

    size_t left() const { return static_cast<size_t>(end - p); }
    uint8_t byte() {
        if (p >= end) {
            ok = false;
            return 0;
        }
        return *p++;
    }
    uint32_t be(int bytes) {
        uint32_t v = 0;
        for (int i = 0; i < bytes; i++) v = v << 8 | byte();
        return v;
    }
    uint32_t vlq() {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t b = byte();
            v = v << 7 | (b & 0x7F);
            if (!(b & 0x80)) return v;
        }
        ok = false;  // longer than 4 bytes
        return 0;
    }
    void skip(uint32_t n) {
        if (n > left()) {
            ok = false;
            p = end;
        } else {
            p += n;
        }
    }
};

struct In {
    uint32_t tick;
    uint32_t seq;  // the file's order, for a stable merge
    uint8_t status, a, b;
    uint32_t tempo;  // a tempo event's µs per quarter note
};

}  // namespace

const char* read(std::string_view data, Clip& clip) {
    if (data.size() > kMaxFile) return "file too large";
    Reader r{reinterpret_cast<const uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(data.data()) + data.size()};
    if (r.left() < 14 || data.substr(0, 4) != "MThd") return "not a Standard MIDI file";
    r.skip(4);
    uint32_t headerLength = r.be(4);
    if (headerLength < 6) return "not a Standard MIDI file";
    uint32_t format = r.be(2);
    uint32_t tracks = r.be(2);
    uint32_t division = r.be(2);
    r.skip(headerLength - 6);
    if (!r.ok) return "not a Standard MIDI file";
    if (format > 1) return "only type 0 and 1 files";
    if (division == 0) return "no time division";
    // Milliseconds per tick at a tempo, or fixed for SMPTE time.
    double smpteMs = 0;
    if (division & 0x8000) {
        int fps = -static_cast<int8_t>(division >> 8);
        double rate = fps == 29 ? 29.97 : fps;
        uint32_t perFrame = division & 0xFF;
        if (rate <= 0 || perFrame == 0) return "bad SMPTE time division";
        smpteMs = 1000.0 / (rate * perFrame);
    }

    std::vector<In> events;
    uint32_t seq = 0;
    uint32_t endTick = 0;
    int beats = 0;
    bool marked = false;
    uint32_t markTick = 0;
    for (uint32_t t = 0; t < tracks && r.left() >= 8; t++) {
        // Chunks other than tracks are skipped, as the format asks.
        const uint8_t* id = r.p;
        r.skip(4);
        uint32_t length = r.be(4);
        if (!r.ok || length > r.left()) return "truncated file";
        if (id[0] != 'M' || id[1] != 'T' || id[2] != 'r' || id[3] != 'k') {
            r.skip(length);
            t--;
            continue;
        }
        Reader tr{r.p, r.p + length};
        r.skip(length);
        uint32_t tick = 0;
        uint8_t running = 0;
        while (tr.ok && tr.left()) {
            uint32_t delta = tr.vlq();
            if (delta > 0x0FFFFFFF - tick) return "file too long";
            tick += delta;
            uint8_t s = tr.p < tr.end ? *tr.p : 0;
            if (s & 0x80) {
                tr.byte();
            } else {
                if (!running) return "data without a status";
                s = running;
            }
            if (s == 0xFF) {
                uint8_t type = tr.byte();
                uint32_t len = tr.vlq();
                const uint8_t* body = tr.p;
                tr.skip(len);
                if (!tr.ok) break;
                if (type == 0x2F) break;  // end of track
                if (type == 0x51 && len == 3) {
                    uint32_t tempo = static_cast<uint32_t>(body[0] << 16 | body[1] << 8 | body[2]);
                    if (tempo) events.push_back({tick, seq++, 0xFF, 0, 0, tempo});
                } else if (type == 0x58 && len >= 1 && !beats) {
                    beats = body[0] ? body[0] : 4;
                } else if (type == 0x06 && len == 3 && memcmp(body, kEndMarker, 3) == 0) {
                    marked = true;
                    markTick = tick;
                }
                continue;
            }
            if (s == 0xF0 || s == 0xF7) {
                tr.skip(tr.vlq());
                continue;
            }
            if (s >= 0xF0) return "unexpected system message";
            running = s;
            uint8_t a = tr.byte() & 0x7F;
            uint8_t b = 0;
            uint8_t kind = s & 0xF0;
            if (kind != 0xC0 && kind != 0xD0) b = tr.byte() & 0x7F;
            if (kind == 0x80 || kind == 0x90 || kind == 0xE0 || (kind == 0xB0 && (a == 120 || a == 123))) {
                events.push_back({tick, seq++, kind, a, b, 0});
            }
        }
        if (!tr.ok) return "truncated track";
        endTick = std::max(endTick, tick);
    }
    if (!r.ok) return "truncated file";

    std::stable_sort(events.begin(), events.end(), [](const In& x, const In& y) {
        return x.tick != y.tick ? x.tick < y.tick : x.seq < y.seq;
    });

    // Ticks to milliseconds through the tempo map.
    uint32_t tempo = 500000;
    uint32_t firstTempo = 0;
    uint32_t mapTick = 0;
    double mapMs = 0;
    auto ms = [&](uint32_t tick) {
        double v = smpteMs > 0 ? tick * smpteMs : mapMs + (tick - mapTick) * (tempo / 1000.0) / (division & 0x7FFF);
        return v;
    };
    auto toMs = [](double v) { return static_cast<uint32_t>(lround(v)); };

    Clip c;
    uint32_t open[128];  // index in c.notes + 1, 0 if none
    for (auto& o : open) o = 0;
    auto close = [&](uint8_t n, uint32_t at) {
        if (!open[n]) return;
        Note& note = c.notes[open[n] - 1];
        note.d = at > note.t ? at - note.t : 1;
        open[n] = 0;
    };
    for (const In& e : events) {
        double now = ms(e.tick);
        if (now > kMaxLength) return "longer than 4 hours";
        uint32_t at = toMs(now);
        if (e.status == 0xFF) {
            if (smpteMs == 0) {
                mapMs = now;
                mapTick = e.tick;
                tempo = e.tempo;
            }
            if (!firstTempo) firstTempo = e.tempo;
            continue;
        }
        switch (e.status) {
            case 0x90:
                if (e.b) {
                    close(e.a, at);
                    if (c.notes.size() >= kMaxNotes) return "more than 1024 notes";
                    Note note;
                    note.t = at;
                    note.n = e.a;
                    note.v = e.b;
                    c.notes.push_back(note);
                    open[e.a] = static_cast<uint32_t>(c.notes.size());
                    break;
                }
                close(e.a, at);
                break;
            case 0x80:
                close(e.a, at);
                break;
            case 0xB0:
                for (uint8_t n = 0; n < 128; n++) close(n, at);
                break;
            case 0xE0: {
                if (c.bends.size() >= kMaxBends) return "more than 1024 pitch bend points";
                BendPoint b;
                b.t = at;
                b.v = static_cast<int16_t>((e.b << 7 | e.a) - 8192);
                c.bends.push_back(b);
                break;
            }
            default:
                break;
        }
    }
    // The marker's tick through the tempo map: the events are past it all.
    auto msAt = [&](uint32_t tick) {
        uint32_t t = 500000, at = 0;
        double base = 0;
        if (smpteMs > 0) return tick * smpteMs;
        for (const In& e : events) {
            if (e.status != 0xFF || e.tick > tick) continue;
            base += (e.tick - at) * (t / 1000.0) / (division & 0x7FFF);
            at = e.tick;
            t = e.tempo;
        }
        return base + (tick - at) * (t / 1000.0) / (division & 0x7FFF);
    };
    double endMs = ms(endTick);
    if (endMs > kMaxLength) return "longer than 4 hours";
    uint32_t end = toMs(endMs);
    for (uint8_t n = 0; n < 128; n++) close(n, end);
    if (marked) {
        end = toMs(msAt(markTick));
    } else {
        for (const Note& n : c.notes) end = std::max(end, n.t + n.d);
        for (const BendPoint& b : c.bends) end = std::max(end, b.t);
    }
    c.length = end;
    if (firstTempo) {
        double bpm = 60000000.0 / firstTempo;
        c.bpm = static_cast<float>(bpm < kMinBpm ? kMinBpm : bpm > kMaxBpm ? kMaxBpm : bpm);
    }
    if (beats) c.beats = static_cast<uint8_t>(beats > 16 ? 16 : beats);
    c.sort();
    clip = std::move(c);
    return nullptr;
}

}  // namespace smf
}  // namespace photophonique
