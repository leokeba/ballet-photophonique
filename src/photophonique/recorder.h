#pragma once

// The recorder (docs/DESIGN.md section 5.6): what clients play through
// /midi/send is recorded into the current recording, which replays through
// Midi on the player's clock, looped or not, with overdub, and is edited by
// note and bend point ids. Recordings are Standard MIDI files (smf.h) in
// ClipFiles. Everything runs under the API lock: the tree's handlers, and
// poll() from the player's task (platform/esp/player_esp.cpp).

#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

#include <ArduinoJson.h>

#include "photophonique/clip.h"
#include "photophonique/midi.h"
#include "tesser/api.h"
#include "tesser/status.h"

namespace photophonique {

struct ClipEntry {
    std::string name;
    uint32_t size = 0;  // bytes
};

// The recordings' files, by name (without ".mid"): recordings/<name>.mid in
// TesserKIT's Files on the device (firmware/main/clip_files.h).
class ClipFiles {
public:
    virtual ~ClipFiles() = default;
    virtual std::vector<ClipEntry> list() = 0;  // by name
    virtual bool read(const std::string& name, std::string& out) = 0;  // at most kMaxFile bytes
    virtual bool write(const std::string& name, std::string_view data) = 0;
    virtual bool remove(const std::string& name) = 0;
    virtual bool rename(const std::string& from, const std::string& to) = 0;
};

// The player's time, and its wake-ups.
class PlayerClock {
public:
    virtual ~PlayerClock() = default;
    virtual uint64_t nowUs() = 0;  // monotonic
    // The schedule changed: poll() again soon, from the player's task.
    virtual void wake() = 0;
};

class Recorder : public MidiListener {
public:
    enum class State : uint8_t { Stopped, Armed, Recording, Playing };
    static constexpr uint64_t kIdle = UINT64_MAX;
    static constexpr uint32_t kSaveDelayMs = 2000;
    static constexpr uint32_t kThinMs = 10;    // bend points closer than this replace each other
    static constexpr uint32_t kNewLength = 8000;  // a new recording: 4 bars at 120 BPM
    static constexpr size_t kMaxName = 28;
    static constexpr int kMaxCue = 64;

    Recorder(tesser::Api& api, Midi& midi, ClipFiles& files, PlayerClock& clock);

    // Settings, bound by the tree and persisted.
    bool loop = false;
    bool overdub = false;

    // After the stored settings are loaded: lists the recordings and loads
    // the current one.
    void begin();

    // The transport.
    tesser::Check record();
    tesser::Check play();
    void stop();
    tesser::Check setPlaying(bool on);  // true plays unless playing; false stops
    tesser::Check cue(int n);           // n-th recording plays; 0 stops
    int lastCue() const { return cue_; }

    // The current recording, "" for none.
    const std::string& recording() const { return name_; }
    tesser::Check choose(const std::string& name);

    // Plays what is due and saves a recording edited kSaveDelayMs ago, from
    // the player's task under the API lock. Returns the microseconds until
    // it should run again, or kIdle until wake().
    uint64_t poll();

    // Edits of the current recording (docs/DESIGN.md section 6: `edit`,
    // `quantize`), refused while recording.
    tesser::Check edit(JsonVariantConst body);
    tesser::Check quantize(JsonVariantConst body);
    tesser::Check setLength(uint32_t ms);
    tesser::Check setBpm(float bpm);
    tesser::Check setBeats(uint8_t beats);

    // Recordings.
    static tesser::Check checkName(std::string_view name);
    tesser::Check create(const std::string& name);
    tesser::Check rename(const std::string& from, const std::string& to);
    tesser::Check remove(const std::string& name);
    // An uploaded file, checked before it replaces anything.
    tesser::Check store(const std::string& name, std::string_view data);
    // A file to download, the current recording saved first; "" if none.
    std::string file(const std::string& name);
    const std::vector<ClipEntry>& entries() const { return entries_; }

    const Clip& clip() const { return clip_; }
    State state() const { return state_; }
    uint32_t position() const { return position_; }  // ms, as last announced
    tesser::Api& api() { return api_; }

    // MidiListener: what clients play.
    void heard(uint8_t status, uint8_t data1, uint8_t data2, uint32_t ageMs) override;

    // The tree's nodes, announced when they change.
    struct Nodes {
        tesser::Node* state = nullptr;
        tesser::Node* position = nullptr;
        tesser::Node* playing = nullptr;
        tesser::Node* recording = nullptr;
        tesser::Node* length = nullptr;
        tesser::Node* bpm = nullptr;
        tesser::Node* beats = nullptr;
        tesser::Node* notes = nullptr;
        tesser::Node* bends = nullptr;
        tesser::Node* entries = nullptr;
        tesser::EventNode* full = nullptr;
    };
    Nodes nodes;

private:
    friend void describe(tesser::Object& o, Recorder& recorder);

    struct Event {
        uint32_t t;
        uint8_t order;  // at one time: offs, bends, ons
        uint8_t status, a, b;
    };

    bool recording_() const { return state_ == State::Armed || state_ == State::Recording; }
    tesser::Check editable() const;
    int64_t msSince(uint64_t us) const;  // ms from startUs_ to `us`, floored
    uint32_t timeOf(uint64_t us) const;  // a recording's time for a moment
    uint32_t now();                      // the position now, while running
    void startPass(uint64_t nowUs);
    uint64_t playDue(uint64_t nowUs);
    void rebuild();
    void fire(const Event& e);
    void releaseHeld();
    void closeNote(uint8_t n, uint32_t t);
    void closeOpen(uint32_t t);
    void addNote(uint32_t t, uint8_t n, uint8_t v);
    void addBend(uint32_t t, int16_t v);
    void full();
    void finishTake(uint32_t at);
    void dropTake();
    tesser::Check load(const std::string& name);
    void adopt(Clip&& c);  // ids given
    void save();
    void flush();
    void markDirty();
    void refreshEntries();
    bool exists(const std::string& name) const;
    std::string takeName() const;
    Note* findNote(uint32_t id);
    BendPoint* findBend(uint32_t id);
    void changedClip();  // notes and bends changed
    void setPosition(uint32_t ms);
    void setState(State s);
    static void announce(tesser::Node* n);

    tesser::Api& api_;
    Midi& midi_;
    ClipFiles& files_;
    PlayerClock& clock_;
    bool begun_ = false;

    Clip clip_;
    std::string name_;
    std::string previous_;  // the current recording before a take
    bool take_ = false;     // recording a new take, not yet a file
    std::vector<ClipEntry> entries_;
    uint32_t nextId_ = 1;

    State state_ = State::Stopped;
    bool replaying_ = false;  // events played: playing, or overdubbing
    uint64_t startUs_ = 0;    // the current pass's start
    uint32_t position_ = 0;
    int cue_ = 0;

    std::vector<Event> events_;
    size_t next_ = 0;
    int64_t playedTo_ = -1;  // ms of this pass whose events were played
    bool eventsDirty_ = true;
    uint8_t held_[128] = {};  // notes replay holds, counted

    uint32_t open_[128] = {};  // recording: the id of each note held
    uint32_t lastBend_ = 0;    // recording: the last bend point of this pass
    bool fullSaid_ = false;

    bool dirty_ = false;
    uint64_t dirtyAtUs_ = 0;
};

// The /recorder tree (docs/DESIGN.md section 6): api.mount("recorder", recorder).
void describe(tesser::Object& o, Recorder& recorder);

#if defined(ESP_PLATFORM)
// The player (platform/esp/player_esp.cpp, docs/DESIGN.md section 4.2): the
// clock, which does nothing until the task starts, and the task, which
// polls the recorder under the API lock, woken by esp_timer and wake().
PlayerClock& defaultClock();
void startPlayer(tesser::Api& api, Recorder& recorder);
#endif

}  // namespace photophonique
