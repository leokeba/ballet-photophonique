// Host test runner: ./photophonique_tests [filter]
#include "timing_cases.h"
#include "output_cases.h"
#include "tree_cases.h"
#include "midi_cases.h"

int main(int argc, char** argv) { return check::run(argc > 1 ? argv[1] : nullptr) ? 1 : 0; }
