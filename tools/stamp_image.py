#!/usr/bin/env python3
"""Stamps, checks and converts cubevox firmware images. Standard library only.

  python3 -I tools/stamp_image.py stamp IN.bin [--out OUT.bin]
  python3 -I tools/stamp_image.py check IN.bin|IN.uf2
  python3 -I tools/stamp_image.py uf2 IN.bin OUT.uf2 --family cubevox|weact [--base 0x08020000]
  python3 -I tools/stamp_image.py selftest

The application region, the image header layout and magic, and the UF2 family ids come from
firmware/cubevox/src/board/boot_block.h (--header names another file), so this tool holds no
second copy of them. It is the only UF2 writer in the project.
"""

import argparse
import collections
import contextlib
import io
import os
import re
import struct
import sys
import tempfile
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DEFAULT_HEADER = os.path.join(ROOT, "firmware", "cubevox", "src", "board", "boot_block.h")

# The UF2 container (github.com/microsoft/uf2): 512-byte blocks, a 32-byte header, 476 data bytes
# of which cubevox fills 256, then the end magic.
UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID = 0x00002000
UF2_BLOCK_BYTES = 512
UF2_HEADER_BYTES = 32
UF2_DATA_BYTES = 476
UF2_PAYLOAD_BYTES = 256
UF2_END_OFFSET = 508

NEEDED = (
    "CV_APP_START", "CV_APP_END", "CV_IMAGE_HEADER_OFFSET", "CV_IMAGE_HEADER_SIZE",
    "CV_IMAGE_OFFSET_MAGIC", "CV_IMAGE_OFFSET_HEADER_VERSION", "CV_IMAGE_OFFSET_LENGTH",
    "CV_IMAGE_OFFSET_CRC", "CV_IMAGE_OFFSET_VERSION", "CV_IMAGE_VERSION_BYTES",
    "CV_IMAGE_MAGIC", "CV_IMAGE_HEADER_VERSION", "CV_UF2_FAMILY_CUBEVOX", "CV_UF2_FAMILY_WEACT",
)
DEFINE = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+(CV_[A-Z0-9_]+)[ \t]+(0[xX][0-9A-Fa-f]+|[0-9]+)[uUlL]*[ \t]*$",
    re.M)

Info = collections.namedtuple("Info", "version length crc")


class ToolError(Exception):
    """A refusal the user must see. main() prints it as `[ERROR] <what> <where>` and exits 1."""


class Parser(argparse.ArgumentParser):
    """Reports usage errors as ToolError so they print and exit like every other refusal."""

    def error(self, message):
        raise ToolError(f"{self.prog}: {message}")


# ---- files and the header contract -------------------------------------------------------------

def read_file(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError as e:
        raise ToolError(f"cannot read {path}: {e.strerror}")


def write_file(path, data):
    tmp = path + ".tmp"
    try:
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
    except OSError as e:
        raise ToolError(f"cannot write {path}: {e.strerror}")


def read_defines(path):
    """Every `#define CV_NAME <integer literal>` line of the header, comments removed."""
    text = read_file(path).decode("utf-8", errors="replace")
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    values = {}
    for name, literal in DEFINE.findall(text):
        value = int(literal, 16 if literal[:2].lower() == "0x" else 10)
        if values.setdefault(name, value) != value:
            raise ToolError(f"{name} is defined twice with different values in {path}")
    return values


class Contract:
    """The constants this tool reads from boot_block.h."""

    def __init__(self, path):
        v = read_defines(path)
        missing = [name for name in NEEDED if name not in v]
        if missing:
            raise ToolError(f"{', '.join(missing)} not defined in {path}")
        self.app_start = v["CV_APP_START"]
        self.app_end = v["CV_APP_END"]
        self.header_offset = v["CV_IMAGE_HEADER_OFFSET"]
        self.header_size = v["CV_IMAGE_HEADER_SIZE"]
        self.off_magic = v["CV_IMAGE_OFFSET_MAGIC"]
        self.off_header_version = v["CV_IMAGE_OFFSET_HEADER_VERSION"]
        self.off_length = v["CV_IMAGE_OFFSET_LENGTH"]
        self.off_crc = v["CV_IMAGE_OFFSET_CRC"]
        self.off_version = v["CV_IMAGE_OFFSET_VERSION"]
        self.version_bytes = v["CV_IMAGE_VERSION_BYTES"]
        self.magic = v["CV_IMAGE_MAGIC"]
        self.header_version = v["CV_IMAGE_HEADER_VERSION"]
        self.families = {"cubevox": v["CV_UF2_FAMILY_CUBEVOX"], "weact": v["CV_UF2_FAMILY_WEACT"]}
        self.max_bytes = self.app_end - self.app_start


# ---- image ---------------------------------------------------------------------------------------

def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def parse_version(c, data, where):
    start = c.header_offset + c.off_version
    text, _, rest = data[start:start + c.version_bytes].partition(b"\0")
    if not text or any(rest):
        raise ToolError(f"version string is empty or not NUL padded in {where}")
    if not all(0x20 <= b < 0x7F for b in text):
        raise ToolError(f"version string has non-printable bytes in {where}")
    return text.decode("ascii")


def read_header(c, data, where):
    """Checks the header's place, magic and version. Returns (length, crc, version string)."""
    base = c.header_offset
    if len(data) < base + c.header_size:
        raise ToolError(f"{len(data)} bytes is too short for the image header at 0x{base:X} in {where}")
    magic = u32(data, base + c.off_magic)
    if magic != c.magic:
        raise ToolError(f"image magic 0x{magic:08X} at 0x{base:X}, want 0x{c.magic:08X} in {where}")
    version = u32(data, base + c.off_header_version)
    if version != c.header_version:
        raise ToolError(f"image header version {version}, want {c.header_version} in {where}")
    return u32(data, base + c.off_length), u32(data, base + c.off_crc), parse_version(c, data, where)


def image_crc(c, data):
    """CRC-32 (zlib) over the whole image with the header's CRC field as zero."""
    at = c.header_offset + c.off_crc
    return zlib.crc32(data[:at] + bytes(4) + data[at + 4:]) & 0xFFFFFFFF


def require_size(c, data, where):
    if len(data) > c.max_bytes:
        raise ToolError(f"image is {len(data)} bytes, the application region holds {c.max_bytes} in {where}")


def check_image(c, data, where):
    """Verifies a stamped image. Returns Info or raises ToolError."""
    length, crc, version = read_header(c, data, where)
    if length == 0 and crc == 0:
        raise ToolError(f"image is not stamped, run stamp first on {where}")
    require_size(c, data, where)
    if length != len(data):
        raise ToolError(f"header length {length}, image is {len(data)} bytes in {where}")
    computed = image_crc(c, data)
    if crc != computed:
        raise ToolError(f"crc mismatch, header 0x{crc:08X}, computed 0x{computed:08X} in {where}")
    return Info(version, length, crc)


def stamp_bytes(c, data, where):
    """Fills length and CRC. Stamping a stamped image gives the same bytes. Returns (bytes, Info)."""
    read_header(c, data, where)
    require_size(c, data, where)
    buf = bytearray(data)
    struct.pack_into("<I", buf, c.header_offset + c.off_length, len(buf))
    struct.pack_into("<I", buf, c.header_offset + c.off_crc, image_crc(c, bytes(buf)))
    return bytes(buf), check_image(c, bytes(buf), where)


# ---- UF2 -----------------------------------------------------------------------------------------

def make_uf2(image, family, base):
    count = (len(image) + UF2_PAYLOAD_BYTES - 1) // UF2_PAYLOAD_BYTES
    out = bytearray()
    for i in range(count):
        chunk = image[i * UF2_PAYLOAD_BYTES:(i + 1) * UF2_PAYLOAD_BYTES]
        out += struct.pack("<8I", UF2_MAGIC0, UF2_MAGIC1, UF2_FLAG_FAMILY_ID,
                           base + i * UF2_PAYLOAD_BYTES, UF2_PAYLOAD_BYTES, i, count, family)
        out += chunk.ljust(UF2_DATA_BYTES, b"\0")
        out += struct.pack("<I", UF2_MAGIC_END)
    return bytes(out)


def read_uf2_block(c, blob, index, count, where):
    """Checks one block's magics, flags, size, numbering and family. Returns (address, family, payload)."""
    block = blob[index * UF2_BLOCK_BYTES:(index + 1) * UF2_BLOCK_BYTES]
    magic0, magic1, flags, addr, size, number, total, fam = struct.unpack_from("<8I", block, 0)
    if (magic0, magic1, u32(block, UF2_END_OFFSET)) != (UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END):
        raise ToolError(f"block {index} has a bad magic in {where}")
    if flags != UF2_FLAG_FAMILY_ID or size != UF2_PAYLOAD_BYTES:
        raise ToolError(f"block {index} has flags 0x{flags:X} and payload {size}, "
                        f"want 0x{UF2_FLAG_FAMILY_ID:X} and {UF2_PAYLOAD_BYTES} in {where}")
    if number != index or total != count:
        raise ToolError(f"block {index} says it is {number} of {total}, the file has {count} in {where}")
    if fam not in c.families.values():
        raise ToolError(f"block {index} has family 0x{fam:08X}, not a cubevox family in {where}")
    return addr, fam, block[UF2_HEADER_BYTES:UF2_HEADER_BYTES + UF2_PAYLOAD_BYTES]


def parse_uf2(c, blob, where):
    """Checks every block and reassembles the payload. Returns (base, family id, payload)."""
    if not blob or len(blob) % UF2_BLOCK_BYTES:
        raise ToolError(f"{len(blob)} bytes is not a whole number of {UF2_BLOCK_BYTES}-byte blocks in {where}")
    count = len(blob) // UF2_BLOCK_BYTES
    base, family, first = read_uf2_block(c, blob, 0, count, where)
    payload = bytearray(first)
    for i in range(1, count):
        addr, fam, chunk = read_uf2_block(c, blob, i, count, where)
        if fam != family or addr != base + i * UF2_PAYLOAD_BYTES:
            raise ToolError(f"block {i} breaks the family or the address run in {where}")
        payload += chunk
    return base, family, bytes(payload)


def check_uf2(c, blob, where):
    """Verifies a UF2 by reassembling its image. Returns Info."""
    base, _, payload = parse_uf2(c, blob, where)
    if base != c.app_start:
        raise ToolError(f"uf2 starts at 0x{base:08X}, the application starts at 0x{c.app_start:08X} in {where}")
    length, _, _ = read_header(c, payload, where)
    if length == 0:
        raise ToolError(f"image is not stamped, run stamp first on {where}")
    if length > len(payload) or len(payload) - length >= UF2_PAYLOAD_BYTES:
        raise ToolError(f"header length {length} does not fit {len(payload)} bytes of blocks in {where}")
    if any(payload[length:]):
        raise ToolError(f"nonzero padding after the image in {where}")
    return check_image(c, payload[:length], where)


def is_uf2(path, data):
    return path.lower().endswith(".uf2") or data[:4] == struct.pack("<I", UF2_MAGIC0)


# ---- commands ------------------------------------------------------------------------------------

def parse_address(text):
    try:
        value = int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(f"{text!r} is not a number")
    if not 0 <= value < 1 << 32:
        raise argparse.ArgumentTypeError(f"{text!r} is not a 32-bit address")
    return value


def cmd_stamp(c, args):
    stamped, info = stamp_bytes(c, read_file(args.input), args.input)
    write_file(args.out or args.input, stamped)
    print(f"stamped {info.version} {info.length} bytes crc {info.crc:08x}")
    return 0


def cmd_check(c, args):
    data = read_file(args.input)
    info = check_uf2(c, data, args.input) if is_uf2(args.input, data) else check_image(c, data, args.input)
    print(f"ok {info.version} {info.length} bytes crc {info.crc:08x}")
    return 0


def cmd_uf2(c, args):
    data = read_file(args.input)
    check_image(c, data, args.input)
    base = c.app_start if args.base is None else args.base
    if base % UF2_PAYLOAD_BYTES or base + len(data) > 1 << 32:
        raise ToolError(f"base 0x{base:08X} is not a {UF2_PAYLOAD_BYTES}-byte aligned address that fits {len(data)} bytes")
    blob = make_uf2(data, c.families[args.family], base)
    write_file(args.output, blob)
    print(f"wrote {len(blob) // UF2_BLOCK_BYTES} blocks, family {args.family}, base 0x{base:08X}")
    return 0


# ---- selftest ------------------------------------------------------------------------------------

def fake_image(c, size, version=b"v2.00.10"):
    """An unstamped image: an odd-period body, the header at its offset, length and CRC zero."""
    buf = bytearray((bytes(range(251)) * (size // 251 + 1))[:size])
    h = c.header_offset
    buf[h:h + c.header_size] = bytes(c.header_size)
    struct.pack_into("<I", buf, h + c.off_magic, c.magic)
    struct.pack_into("<I", buf, h + c.off_header_version, c.header_version)
    buf[h + c.off_version:h + c.off_version + len(version)] = version
    return bytes(buf)


def crc32_bitwise(data):
    """Bit-by-bit CRC-32, an independent check of zlib's."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1))
    return crc ^ 0xFFFFFFFF


def flipped(data, index):
    out = bytearray(data)
    out[index] ^= 0x01
    return bytes(out)


def selftest_image(c, failures):
    def expect(ok, what):
        if not ok:
            failures.append(what)

    def refused(what, fn, *fn_args):
        try:
            fn(*fn_args)
        except ToolError:
            return
        failures.append(f"{what} was accepted")

    raw = fake_image(c, 3072)
    refused("an unstamped image", check_image, c, raw, "raw")
    image, info = stamp_bytes(c, raw, "raw")
    expect((info.version, info.length) == ("v2.00.10", 3072), "stamp reports the version and length")
    expect(info.crc == crc32_bitwise(image[:c.header_offset + c.off_crc] + bytes(4) +
                                     image[c.header_offset + c.off_crc + 4:]),
           "the CRC equals an independent bitwise CRC-32")
    expect(stamp_bytes(c, image, "image")[0] == image, "re-stamping gives the same bytes")
    expect(check_image(c, image, "image") == info, "check agrees with stamp")
    refused("a flipped body byte", check_image, c, flipped(image, 100), "image")
    refused("a flipped magic byte", check_image, c, flipped(image, c.header_offset), "image")
    refused("a header only", stamp_bytes, c, raw[:c.header_offset + 8], "short")
    refused("an image without a header", stamp_bytes, c, bytes(3072), "zeros")
    refused("an oversize image", stamp_bytes, c, fake_image(c, c.max_bytes + 4), "big")
    refused("a version string with a control byte", stamp_bytes, c, fake_image(c, 3072, b"v2\x01"), "ctl")
    return image, info


def selftest_uf2(c, failures, image, info):
    def refused(what, blob):
        try:
            check_uf2(c, blob, "uf2")
        except ToolError:
            return
        failures.append(f"{what} was accepted")

    cubevox = c.families["cubevox"]
    blob = make_uf2(image, cubevox, c.app_start)
    if len(blob) != 12 * UF2_BLOCK_BYTES or check_uf2(c, blob, "uf2") != info:
        failures.append("the uf2 of a 3072-byte image does not round trip through 12 blocks")
    odd, odd_info = stamp_bytes(c, fake_image(c, 3000), "odd")
    if check_uf2(c, make_uf2(odd, cubevox, c.app_start), "odd.uf2") != odd_info:
        failures.append("a uf2 whose last block is padded does not round trip")
    refused("a flipped payload byte", flipped(blob, UF2_HEADER_BYTES + 100))
    refused("a foreign family", make_uf2(image, 0x6DB66082, c.app_start))
    refused("a base outside the application", make_uf2(image, cubevox, c.app_end))
    mixed = bytearray(blob)
    struct.pack_into("<I", mixed, UF2_BLOCK_BYTES + 28, c.families["weact"])
    refused("two families in one file", bytes(mixed))
    refused("a missing block", blob[:5 * UF2_BLOCK_BYTES] + blob[6 * UF2_BLOCK_BYTES:])
    refused("a truncated file", blob[:-UF2_BLOCK_BYTES])
    refused("a partial block", blob[:-1])
    refused("a bad end magic", flipped(blob, UF2_END_OFFSET))
    flagless = bytearray(blob)
    struct.pack_into("<I", flagless, 8, 0)
    refused("a block without the family flag", bytes(flagless))


def selftest_cli(c, args, failures):
    def cli(*argv):
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            code = main(["--header", args.header, *argv])
        return code, err.getvalue()

    def expect(ok, what):
        if not ok:
            failures.append(what)

    with tempfile.TemporaryDirectory() as tmp:
        raw, stamped, uf2 = (os.path.join(tmp, n) for n in ("raw.bin", "stamped.bin", "out.uf2"))
        write_file(raw, fake_image(c, 3072))
        expect(cli("stamp", raw, "--out", stamped)[0] == 0, "cli stamp --out succeeds")
        expect(cli("check", stamped)[0] == 0, "cli check accepts the stamped file")
        expect(cli("uf2", stamped, uf2, "--family", "weact")[0] == 0, "cli uf2 succeeds")
        expect(cli("check", uf2)[0] == 0, "cli check accepts the uf2")
        expect(cli("stamp", raw)[0] == 0 and cli("check", raw)[0] == 0, "cli stamps in place")
        write_file(raw, fake_image(c, 3072))
        for what, argv in (
                ("an unstamped bin", ("check", raw)),
                ("an unstamped bin to uf2", ("uf2", raw, os.path.join(tmp, "x.uf2"), "--family", "cubevox")),
                ("an unknown family", ("uf2", stamped, os.path.join(tmp, "y.uf2"), "--family", "stm32h7")),
                ("a missing file", ("check", os.path.join(tmp, "missing.bin"))),
                ("an unknown command", ("flash",))):
            code, err = cli(*argv)
            expect(code == 1 and err.startswith("[ERROR]"), f"cli refuses {what} with [ERROR] and exit 1")
        expect(not os.path.exists(os.path.join(tmp, "x.uf2")), "no uf2 is written for an unstamped image")


def cmd_selftest(c, args):
    failures = []
    image, info = selftest_image(c, failures)
    selftest_uf2(c, failures, image, info)
    selftest_cli(c, args, failures)
    for what in failures:
        print(f"[ERROR] selftest: {what}", file=sys.stderr)
    if failures:
        return 1
    print("PASS")
    return 0


# ---- entry ---------------------------------------------------------------------------------------

def build_parser():
    common = Parser(add_help=False)
    common.add_argument("--header", default=argparse.SUPPRESS, help=argparse.SUPPRESS)

    top = Parser(prog="stamp_image.py", description="Stamp, check and convert cubevox firmware images.")
    top.add_argument("--header", default=DEFAULT_HEADER, help="boot_block.h to read (default: the repository's)")
    sub = top.add_subparsers(dest="command", required=True)

    p = sub.add_parser("stamp", parents=[common], help="write the length and CRC into the image header")
    p.add_argument("input")
    p.add_argument("--out", help="output file (default: stamp in place)")
    p.set_defaults(run=cmd_stamp)

    p = sub.add_parser("check", parents=[common], help="verify a stamped .bin or a .uf2")
    p.add_argument("input")
    p.set_defaults(run=cmd_check)

    p = sub.add_parser("uf2", parents=[common], help="convert a stamped .bin to .uf2")
    p.add_argument("input")
    p.add_argument("output")
    p.add_argument("--family", required=True, choices=("cubevox", "weact"))
    p.add_argument("--base", type=parse_address, help="address of byte 0 (default: the application start)")
    p.set_defaults(run=cmd_uf2)

    p = sub.add_parser("selftest", parents=[common], help="run every command on a fake image")
    p.set_defaults(run=cmd_selftest)
    return top


def main(argv=None):
    try:
        args = build_parser().parse_args(argv)
        return args.run(Contract(args.header), args)
    except ToolError as e:
        print(f"[ERROR] {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
