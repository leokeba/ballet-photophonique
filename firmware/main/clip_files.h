#pragma once

// The recorder's files (docs/DESIGN.md section 5.6): recordings/<name>.mid
// in TesserKIT's Files (LittleFS), through its helpers, which write whole
// files atomically.

#include <string>
#include <string_view>
#include <vector>

#include "TesserKIT.h"
#include "photophonique/recorder.h"

class ClipFilesOnFiles : public photophonique::ClipFiles {
public:
    explicit ClipFilesOnFiles(tesserkit::Files& files) : files_(files) {}

    std::vector<photophonique::ClipEntry> list() override {
        std::vector<photophonique::ClipEntry> out;
        if (!files_.mounted()) return out;
        for (const tesserkit::FileEntry& e : files_.list(kDir)) {
            size_t n = e.name.size();
            if (e.isDir || n <= 4 || e.name[0] == '.' || e.name.compare(n - 4, 4, ".mid") != 0) continue;
            out.push_back({e.name.substr(0, n - 4), e.size});
        }
        return out;
    }
    bool read(const std::string& name, std::string& out) override {
        return files_.read(path(name), out, photophonique::kMaxFile).isOk();
    }
    bool write(const std::string& name, std::string_view data) override {
        return files_.write(path(name), data).isOk();
    }
    bool remove(const std::string& name) override { return files_.remove(path(name)).isOk(); }
    bool rename(const std::string& from, const std::string& to) override {
        return files_.rename(path(from), path(to)).isOk();
    }

private:
    static constexpr const char* kDir = "recordings";
    static std::string path(const std::string& name) { return std::string(kDir) + "/" + name + ".mid"; }

    tesserkit::Files& files_;
};
