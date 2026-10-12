# A virtual MIDI keyboard: a CoreMIDI source named "Photophonique Test" that
# browsers list as an input. Reads commands on stdin, one per line:
#   on NOTE VEL | off NOTE | bend VALUE14 | cc NUM VAL
# and prints "ready" once the port exists, "sent" after each command.
import sys

import rtmidi

out = rtmidi.MidiOut()
out.open_virtual_port("Photophonique Test")
print("ready", flush=True)
for line in sys.stdin:
    p = line.split()
    if not p:
        continue
    if p[0] == "on":
        out.send_message([0x90, int(p[1]), int(p[2])])
    elif p[0] == "off":
        out.send_message([0x80, int(p[1]), 0])
    elif p[0] == "bend":
        v = int(p[1])
        out.send_message([0xE0, v & 0x7F, v >> 7])
    elif p[0] == "cc":
        out.send_message([0xB0, int(p[1]), int(p[2])])
    print("sent", flush=True)
