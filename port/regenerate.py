#!/usr/bin/env python3
"""Regenerate ``port/recomp/gen`` from your own ``default.xbe``.

This is the JSRF-specific wrapper around the title-agnostic pipeline:

    tools.disasm  -> tools.func_id -> tools.abi_analysis -> tools.recomp
                  -> port/postprocess_gen.py

The seed sets live in ``port/seeds``. ``icall_seeds.json`` is the filtered
ICALL feedback from a run that reached gameplay; ``manual_seeds.json`` and
``ctor_seeds.json`` are the hand-picked starts for the manual overrides and
constructors. The generated tree is not in git.

Usage::

    python3 port/regenerate.py --game-dir ~/jsrf-game
    cmake -S port -B build -DCMAKE_BUILD_TYPE=Release -DNV2A_GL_CONTEXT=sdl
    cmake --build build -j6
"""

import argparse
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(cmd):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=REPO, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game-dir", default=os.environ.get("JSRF_GAME_DIR",
                                                        os.path.expanduser("~/jsrf-game")),
                    help="directory holding default.xbe (default: %(default)s)")
    ap.add_argument("--xbe", default=None, help="override the XBE path")
    ap.add_argument("--gen-dir", default=os.path.join(REPO, "port", "recomp", "gen"),
                    help="generated-code directory (default: %(default)s)")
    ap.add_argument("--seeds-dir", default=os.path.join(REPO, "port", "seeds"),
                    help="seed-set directory (default: %(default)s)")
    ap.add_argument("--split", type=int, default=1000,
                    help="functions per generated .c file (default: %(default)s)")
    ap.add_argument("--python", default=sys.executable,
                    help="Python interpreter for the tools (default: this one)")
    args = ap.parse_args()

    xbe = args.xbe or os.path.join(args.game_dir, "default.xbe")
    if not os.path.isfile(xbe):
        raise SystemExit("regenerate: no XBE at %s (use --game-dir or --xbe)" % xbe)

    seed_args = []
    for name in ("icall_seeds", "manual_seeds", "ctor_seeds"):
        path = os.path.join(args.seeds_dir, name + ".json")
        if not os.path.isfile(path):
            raise SystemExit("regenerate: missing seed file %s" % path)
        seed_args += ["--seed-functions", path]

    run([args.python, "-m", "tools.disasm", xbe, "--force", "-v"] + seed_args)
    run([args.python, "-m", "tools.func_id", xbe, "-v"])
    run([args.python, "-m", "tools.abi_analysis", xbe, "-v"])

    if os.path.isdir(args.gen_dir):
        shutil.rmtree(args.gen_dir)
    os.makedirs(args.gen_dir, exist_ok=True)

    manual = os.path.join(REPO, "port", "recomp_manual.c")
    run([args.python, "-m", "tools.recomp", xbe, "--all",
         "--split", str(args.split), "--gen-dir", args.gen_dir,
         "--exclude-manual", manual,
         "--game-name", "Jet Set Radio Future"])
    run([args.python, os.path.join(REPO, "port", "postprocess_gen.py"),
         "--gen-dir", args.gen_dir, "--manual", manual])

    print("\nGenerated code is ready in %s" % args.gen_dir)
    print("Build it with:\n"
          "  cmake -S port -B build -DCMAKE_BUILD_TYPE=Release -DNV2A_GL_CONTEXT=sdl\n"
          "  cmake --build build -j6")


if __name__ == "__main__":
    main()
