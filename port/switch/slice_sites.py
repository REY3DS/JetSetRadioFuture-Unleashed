#!/usr/bin/env python3
"""Put a scheduler check at every loop back-edge in the generated code.

The lifter emits RECOMP_TRACE_ENTER only at function entries, and this build
does not emit it at all, so a loop that never calls anything -- a spin waiting
for a flag, a poll for a driver bit -- runs from entry to exit without once
looking at the scheduler. On the console a timer tick would take the CPU away
from it. Here the guest lock stays with it, and every other guest thread stops
for as long as the loop lasts, which is what "held the guest lock for 22604 ms"
in the log is.

A back-edge is where the console's tick is due, so that is where the check
goes. It costs one increment and a masked test until it is due.

Run from the repository root after (re)generating port/recomp/gen:

    python3 port/switch/slice_sites.py

Idempotent: a label that already carries the check is left alone.
"""

import os
import re
import sys

GEN_DIR = os.path.join("port", "recomp", "gen")
MARK = "RECOMP_SLICE_SITE();"
LABEL = re.compile(r"^loc_([0-9A-Fa-f]+): ;")
GOTO = re.compile(r"goto loc_([0-9A-Fa-f]+);")


def process(path):
    with open(path, encoding="utf-8") as f:
        lines = f.readlines()

    label_at = {}
    back_edge = set()
    for i, line in enumerate(lines):
        m = LABEL.match(line)
        if m:
            label_at[m.group(1)] = i
            continue
        for t in GOTO.findall(line):
            at = label_at.get(t)
            if at is not None and at < i:
                back_edge.add(t)

    if not back_edge:
        return 0

    out = []
    added = 0
    for i, line in enumerate(lines):
        out.append(line)
        m = LABEL.match(line)
        if not m or m.group(1) not in back_edge:
            continue
        if i + 1 < len(lines) and MARK in lines[i + 1]:
            continue
        out.append("    %s\n" % MARK)
        added += 1

    if added:
        with open(path, "w", encoding="utf-8") as f:
            f.writelines(out)
    return added


def main():
    total = 0
    for name in sorted(os.listdir(GEN_DIR)):
        if not re.match(r"recomp_[0-9]+\.c$", name):
            continue
        n = process(os.path.join(GEN_DIR, name))
        if n:
            print("%s: %d loop heads" % (name, n))
        total += n
    print("total %d" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
