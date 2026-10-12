#pragma once

// Standard MIDI files (docs/DESIGN.md section 5.6): a recording written as a
// type 0 file, and type 0 or 1 files read into a recording.

#include <string>
#include <string_view>

#include "photophonique/clip.h"

namespace photophonique::smf {

// One track at 480 ticks per quarter note: the tempo, the metre, the notes
// and bends on channel 1, the end of the track at the length.
std::string write(const Clip& clip);

// Reads `data` into `clip` (ids left at 0), within the limits of clip.h.
// Returns null, or why the file was refused.
const char* read(std::string_view data, Clip& clip);

}  // namespace photophonique::smf
