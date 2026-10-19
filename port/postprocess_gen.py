#!/usr/bin/env python3
"""Reapply the local JSRF fixes to freshly generated recompiler output.

The public recompiler has two gaps that matter for this title:

  * Wrapped manual overrides are emitted as ``sub_X_gen`` and every generated
    call/dispatch entry targets the raw body, so the hand-written wrapper is
    never called.  On the path to gameplay this leaves the NV2A PGRAPH
    interrupt unacknowledged (the DPC spins forever) and the DirectSound DSP
    doorbell unregistered (the title spins in DirectSound init).

  * MSVC switch tables whose entries point inside the same function are
    emitted as ``RECOMP_ITAIL``; the targets that are not function starts are
    never generated, so the switch does nothing and the caller's stack is
    corrupted.

This script fixes the cases we hit.  It is idempotent and refuses to guess: if
an expected pattern is missing it fails instead of producing a build that looks
fine and hangs at runtime.

Run it after ``tools.recomp`` and before ``cmake --build``::

    python3 -m tools.disasm ...
    python3 -m tools.func_id ...
    python3 -m tools.abi_analysis ...
    python3 -m tools.recomp ... --gen-dir port/recomp/gen ...
    python3 port/postprocess_gen.py
    cmake --build build -j6
"""

import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, REPO)

# Functions whose manual wrapper must be the entry point.  Keep this list
# explicit: wiring all 90 wrappers at once changes behaviour far beyond the
# two paths this port has validated.
WRAPPERS = (0x00194210, 0x001A1769)   # NV2A PGRAPH ISR, DirectSound doorbell
SWITCH_ADDR = 0x0002F600              # jump table at guest 0x0002F690


def read(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


TABLES_STUB = """/* Provisional: empty tables so recomp_manual.c links.
 * The real ones are not produced by the public tools. */
#include <stdint.h>
const struct { uint32_t va; void (*fn)(void); } g_recomp_seeded[1] = { { 0, 0 } };
const unsigned g_recomp_seeded_count = 0;
const struct { uint32_t va; void (*fn)(void); } g_recomp_holes[1] = { { 0, 0 } };
const unsigned g_recomp_holes_count = 0;
"""


def ensure_tables_stub(gen_dir):
    """The public recompiler does not emit the seeded/holes tables.

    recomp_manual.c references them, so the build needs a definition.  A
    private build that produced real tables wins: never shadow them.
    """
    real = os.path.join(gen_dir, "recomp_tables.c")
    stub = os.path.join(gen_dir, "recomp_tables_stub.c")
    if os.path.exists(stub) or os.path.exists(real):
        print("  tables stub: already present")
        return 0
    write(stub, TABLES_STUB)
    print("  tables stub: created")
    return 1


def patch_wrappers(gen_dir, manual_path):
    from tools.recomp.manual_scan import scan
    _skip, wrap, _referenced = scan(manual_path)

    changed = 0
    for addr in WRAPPERS:
        if addr not in wrap:
            raise SystemExit(
                "postprocess: 0x%08X is not a wrapped function in %s"
                % (addr, manual_path))
        body = "sub_%08X_gen" % addr
        wrapper = "sub_%08X" % addr
        abi_old = "RECOMP_ABI_CALL(0x%08Xu, %s)" % (addr, body)
        abi_new = "RECOMP_ABI_CALL(0x%08Xu, %s)" % (addr, wrapper)
        direct = re.compile(r"(?<!void )\b%s\(\)" % re.escape(body))

        for name in sorted(os.listdir(gen_dir)):
            if not name.endswith(".c"):
                continue
            path = os.path.join(gen_dir, name)
            src = read(path)
            out = src.replace(abi_old, abi_new)
            out = direct.sub(wrapper + "()", out)
            if out != src:
                write(path, out)
                changed += 1
                print("  wrapper 0x%08X: %s" % (addr, name))

        disp = os.path.join(gen_dir, "recomp_dispatch.c")
        src = read(disp)
        old = "{ 0x%08Xu, (recomp_func_t)%s }" % (addr, body)
        new = "{ 0x%08Xu, (recomp_func_t)%s }" % (addr, wrapper)
        if old in src:
            write(disp, src.replace(old, new))
            changed += 1
            print("  dispatch 0x%08X" % addr)
        elif new not in src:
            raise SystemExit("postprocess: dispatch entry for 0x%08X not found" % addr)

        hdr = os.path.join(gen_dir, "recomp_funcs.h")
        src = read(hdr)
        decl = "void %s(void);" % wrapper
        if decl not in src:
            need = "void %s(void);" % body
            if need not in src:
                raise SystemExit("postprocess: prototype for %s not found" % body)
            src = src.replace(need, need + "\n" + decl + "   /* manual wrapper */", 1)
            write(hdr, src)
            changed += 1
            print("  prototype 0x%08X" % addr)

    return changed


def find_function_file(gen_dir, symbol):
    needle = "void %s(void)" % symbol
    for name in sorted(os.listdir(gen_dir)):
        if not name.endswith(".c"):
            continue
        path = os.path.join(gen_dir, name)
        if needle in read(path):
            return path
    raise SystemExit("postprocess: %s not found in %s" % (symbol, gen_dir))


def patch_switch(gen_dir):
    path = find_function_file(gen_dir, "sub_%08X" % SWITCH_ADDR)
    src = read(path)
    if "case 1: goto loc_0002F642;" in src:
        print("  switch 0x%08X: already patched (manual form)" % SWITCH_ADDR)
        return 0
    if ("switch: 6 entries, 6 targets" in src
            or "if (_jt == 0x0002F642u) goto loc_0002F642;" in src):
        print("  switch 0x%08X: already emitted by the lifter" % SWITCH_ADDR)
        return 0

    old1 = """loc_0002F613: ;
    g_seh_ebp = ebp; RECOMP_ITAIL(MEM32(ecx * 4 + 0x2F690)); return; /* indirect tail jmp */

    edx = MEM32(esp + 0xC);"""
    new1 = """loc_0002F613: ;
    switch (ecx) {
    case 0: goto loc_0002F61A;
    case 1: goto loc_0002F642;
    case 2: goto loc_0002F650;
    case 3: goto loc_0002F65E;
    case 4: goto loc_0002F66C;
    case 5: goto loc_0002F67A;
    default: g_seh_ebp = ebp; sub_0002F688(); return;
    }

loc_0002F61A: ;
    edx = MEM32(esp + 0xC);"""
    if src.count(old1) != 1:
        raise SystemExit("postprocess: switch prologue pattern count = %d"
                         % src.count(old1))
    src = src.replace(old1, new1, 1)

    old2 = """loc_0002F62B: ;
    eax = MEM32(eax + edx * 4 + 4);
    esp += 16; return; /* ret 12 */

}"""
    new2 = """loc_0002F62B: ;
    eax = MEM32(eax + edx * 4 + 4);
    esp += 16; return; /* ret 12 */

loc_0002F642: ;
    ecx = MEM32(esp + 0xC);
    eax = MEM32(eax + ecx * 4 + 0x84);
    esp += 16; return; /* ret 12 */

loc_0002F650: ;
    edx = MEM32(esp + 0xC);
    eax = MEM32(eax + edx * 4 + 0xD0);
    esp += 16; return; /* ret 12 */

loc_0002F65E: ;
    ecx = MEM32(esp + 0xC);
    eax = MEM32(eax + ecx * 4 + 0xD8);
    esp += 16; return; /* ret 12 */

loc_0002F66C: ;
    edx = MEM32(esp + 0xC);
    eax = MEM32(eax + edx * 4 + 0xDC);
    esp += 16; return; /* ret 12 */

loc_0002F67A: ;
    ecx = MEM32(esp + 0xC);
    eax = MEM32(eax + ecx * 4 + 0x100);
    esp += 16; return; /* ret 12 */

}"""
    if src.count(old2) != 1:
        raise SystemExit("postprocess: switch epilogue pattern count = %d"
                         % src.count(old2))
    src = src.replace(old2, new2, 1)
    write(path, src)
    print("  switch 0x%08X: %s" % (SWITCH_ADDR, os.path.basename(path)))
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--gen-dir", default=os.path.join(REPO, "port", "recomp", "gen"))
    ap.add_argument("--manual", default=os.path.join(REPO, "port", "recomp_manual.c"))
    args = ap.parse_args()

    if not os.path.isdir(args.gen_dir):
        raise SystemExit("postprocess: no such directory: %s" % args.gen_dir)

    print("postprocess: %s" % args.gen_dir)
    changed = ensure_tables_stub(args.gen_dir)
    changed += patch_wrappers(args.gen_dir, args.manual)
    changed += patch_switch(args.gen_dir)
    print("postprocess: done (%d change(s))" % changed)


if __name__ == "__main__":
    main()
