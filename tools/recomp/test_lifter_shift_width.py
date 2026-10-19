"""
Self-check for shift/rotate width and count semantics.

x86 masks a shift or rotate count to five bits and rotates a byte or word
operand inside its own width. C does neither, and a constant count at or above
the width is undefined -- so "shl eax, 0x80" is a no-op on x86 and something
else on another target. Keep the emitted code saying what the hardware does
before the port leaves x86.

Run: python3 tools/recomp/test_lifter_shift_width.py
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.lifter import Lifter  # noqa: E402


class _Op:
    def __init__(self, text, type="reg", reg=None, size=4, mem_size=None, imm=0):
        self.text = text
        self.type = type
        self.reg = reg
        self.size = size
        self.mem_size = mem_size
        self.imm = imm


class _Insn:
    is_cond_jump = False

    def __init__(self, mnemonic, operands):
        self.mnemonic = mnemonic
        self.operands = operands


def _lift(mnemonic, ops, needs_cf=True):
    lifter = Lifter()
    lifter.needs_cf = needs_cf
    return "\n".join(lifter.lift_instruction(_Insn(mnemonic, ops)))


EAX = _Op("eax", reg="eax")
AL = _Op("al", reg="al")
ECX = _Op("ecx", reg="ecx")
CL = _Op("cl", reg="cl")
ONE = _Op("1", type="imm", imm=1)
BIG = _Op("0x80", type="imm", imm=0x80)


def test_shift_masks_constant_count():
    out = _lift("shl", [EAX, BIG])
    assert "& 31" in out, out
    assert "<< ((0x80) & 31)" in out, out


def test_shift_cf_uses_masked_count():
    out = _lift("shr", [ECX, ONE])
    assert "& 31" in out, out
    assert "_cf =" in out, out


def test_rol_is_width_aware():
    out = _lift("rol", [AL, CL])
    assert "ROL8(" in out, out
    assert "ROL32(" not in out, out


def test_ror_is_width_aware():
    out = _lift("ror", [AL, ONE])
    assert "ROR8(" in out, out


def test_rol32_still_used_for_dwords():
    out = _lift("rol", [EAX, CL])
    assert "ROL32(" in out, out


def test_sar_sign_extends_narrow_operand():
    out = _lift("sar", [AL, CL])
    assert "(int32_t)(int8_t)" in out, out


def test_shld_guards_zero_count():
    out = _lift("shld", [EAX, ECX, CL])
    assert "if (_n)" in out, out
    assert "& 31" in out, out
    assert "(32 - _n) & 31" in out, out


def test_shrd_guards_zero_count():
    out = _lift("shrd", [EAX, ECX, CL])
    assert "if (_n)" in out, out
    assert "(32 - _n) & 31" in out, out


def test_rcl_masks_count():
    out = _lift("rcl", [EAX, CL])
    assert "& 31u" in out, out


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_"):
            continue
        try:
            fn()
            print(f"  ok   {name}")
        except AssertionError as exc:
            failures += 1
            print(f"  FAIL {name}\n{exc}")
    print("shift width: " + ("OK" if not failures else f"{failures} FAILED"))
    sys.exit(1 if failures else 0)
