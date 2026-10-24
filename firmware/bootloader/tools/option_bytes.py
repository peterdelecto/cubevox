#!/usr/bin/env python3
"""Reads and edits the STM32H743 option byte block the ST ROM DFU serves, for flash.sh.

  python3 -I option_bytes.py status BLOCK          prints "protected" or "writable" (bank 1 sector 0)
  python3 -I option_bytes.py unprotect IN OUT      writes IN with bank 1 sector 0 writable
  python3 -I option_bytes.py selftest

The ROM DFU's "@Option Bytes" alternate setting starts at 0x5200201C, FLASH_OPTSR_CUR, and reads as
the flash interface's option registers in address order (RM0433, FLASH register map). A running
bootloader write protects its own sector, and the ROM DFU then skips the erase and the write of that
sector without an error, so a shop reflash clears the bit first. Only that bit changes; every other
byte goes back as read. A block that does not look like that register window, or one whose
read protection is not level 0, is refused rather than written.
"""

import struct
import sys

# Word offsets inside the block, from FLASH_OPTSR_CUR at 0x5200201C (RM0433).
OPTSR_CUR = 0x00
OPTSR_PRG = 0x04
WPSN_CUR1 = 0x1C
WPSN_PRG1 = 0x20
BLOCK_MIN = WPSN_PRG1 + 4
# OPTSR RDP, bits 15:8. 0xAA is level 0, no read protection.
RDP_SHIFT = 8
RDP_LEVEL0 = 0xAA
# A clear WPSN bit write protects its sector.
SECTOR0 = 0x01


class ToolError(Exception):
    """A refusal flash.sh must show. main() prints it as [ERROR] and exits 1."""


def u32(block, offset):
    return struct.unpack_from("<I", block, offset)[0]


def checked(block):
    """The block, after the checks that it is the register window and at read protection level 0."""
    if len(block) < BLOCK_MIN:
        raise ToolError(f"option byte block is {len(block)} bytes, want at least {BLOCK_MIN}")
    for name, offset in (("OPTSR_CUR", OPTSR_CUR), ("OPTSR_PRG", OPTSR_PRG)):
        rdp = (u32(block, offset) >> RDP_SHIFT) & 0xFF
        if rdp != RDP_LEVEL0:
            raise ToolError(f"{name} reads RDP 0x{rdp:02X}, not level 0 (0xAA); refusing to write option bytes")
    if u32(block, WPSN_CUR1) & 0xFF != u32(block, WPSN_PRG1) & 0xFF:
        raise ToolError("WPSN_CUR1 and WPSN_PRG1 differ, so the block is not the option register window")
    return block


def protected(block):
    return u32(checked(block), WPSN_CUR1) & SECTOR0 == 0


def unprotected(block):
    out = bytearray(checked(block))
    for offset in (WPSN_CUR1, WPSN_PRG1):
        struct.pack_into("<I", out, offset, u32(out, offset) | SECTOR0)
    return bytes(out)


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as e:
        raise ToolError(f"cannot read {path}: {e.strerror}")


def write(path, data):
    try:
        with open(path, "wb") as f:
            f.write(data)
    except OSError as e:
        raise ToolError(f"cannot write {path}: {e.strerror}")


def sample(wpsn=0xFE, rdp=RDP_LEVEL0, size=128):
    """A block as the ROM serves it: BOR level 3, the given RDP, WPSN as given, the rest filler."""
    block = bytearray(range(size))
    optsr = 0x1000000C | (rdp << RDP_SHIFT)
    struct.pack_into("<II", block, OPTSR_CUR, optsr, optsr)
    struct.pack_into("<I", block, WPSN_CUR1, wpsn)
    struct.pack_into("<I", block, WPSN_PRG1, wpsn)
    return bytes(block)


def selftest():
    failures = []

    def refused(what, fn, *args):
        try:
            fn(*args)
        except ToolError:
            return
        failures.append(f"{what} was accepted")

    if not protected(sample(0xFE)) or protected(sample(0xFF)):
        failures.append("status misreads sector 0's bit")
    fixed = unprotected(sample(0xFE))
    if protected(fixed) or u32(fixed, WPSN_PRG1) != 0xFF:
        failures.append("unprotect leaves sector 0 protected")
    changed = [i for i in range(len(fixed)) if fixed[i] != sample(0xFE)[i]]
    if changed != [WPSN_CUR1, WPSN_PRG1]:
        failures.append(f"unprotect changed bytes {changed}, want only the two WPSN low bytes")
    if unprotected(sample(0xFF)) != sample(0xFF):
        failures.append("unprotect changes an unprotected block")
    if len(unprotected(sample(0xFE, size=88))) != 88:
        failures.append("an 88-byte block does not keep its size")
    refused("read protection level 1", protected, sample(rdp=0xBB))
    refused("a short block", unprotected, sample()[:BLOCK_MIN - 4])
    mismatch = bytearray(sample(0xFE))
    struct.pack_into("<I", mismatch, WPSN_PRG1, 0xFF)
    refused("a block whose CUR and PRG words differ", unprotected, bytes(mismatch))
    for what in failures:
        print(f"[ERROR] selftest: {what}", file=sys.stderr)
    if failures:
        return 1
    print("[selftest] PASS: option_bytes.py reads and clears only sector 0's write protection")
    return 0


def main(argv):
    try:
        if argv == ["selftest"]:
            return selftest()
        if len(argv) == 2 and argv[0] == "status":
            print("protected" if protected(read(argv[1])) else "writable")
            return 0
        if len(argv) == 3 and argv[0] == "unprotect":
            write(argv[2], unprotected(read(argv[1])))
            return 0
        raise ToolError("usage: option_bytes.py status BLOCK | unprotect IN OUT | selftest")
    except ToolError as e:
        print(f"[ERROR] {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
