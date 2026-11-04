"""PNG decoder built on zlib.

Reads every colour type and bit depth of non-interlaced PNG and returns 8-bit grey and alpha.
"""

import struct
import zlib

from .errors import DecalError

_SIGNATURE = b"\x89PNG\r\n\x1a\n"

# colour type -> (samples per pixel, allowed bit depths)
_COLOUR_TYPES = {
    0: (1, (1, 2, 4, 8, 16)),
    2: (3, (8, 16)),
    3: (1, (1, 2, 4, 8)),
    4: (2, (8, 16)),
    6: (4, (8, 16)),
}

_KNOWN_CRITICAL = (b"IHDR", b"PLTE", b"IDAT", b"IEND")


def _fail(path: str, what: str) -> DecalError:
    return DecalError(f"The PNG {path} cannot be read. {what}")


def _chunks(path: str, data: bytes):
    """Yield (type, body) for every chunk, checking length and CRC."""
    pos = len(_SIGNATURE)
    while pos < len(data):
        if pos + 8 > len(data):
            raise _fail(path, "The file ends inside a chunk header, so it is truncated.")
        length, ctype = struct.unpack(">I4s", data[pos:pos + 8])
        end = pos + 8 + length
        if end + 4 > len(data):
            raise _fail(path, f"The {ctype.decode('latin-1')} chunk runs past the end of the file, "
                              "so it is truncated.")
        body = data[pos + 8:end]
        (crc,) = struct.unpack(">I", data[end:end + 4])
        if zlib.crc32(ctype + body) & 0xFFFFFFFF != crc:
            raise _fail(path, f"The {ctype.decode('latin-1')} chunk fails its CRC check, "
                              "so the file is damaged.")
        yield ctype, body
        pos = end + 4
        if ctype == b"IEND":
            return
    raise _fail(path, "The file has no IEND chunk, so it is truncated.")


def _unfilter(path: str, raw: bytes, height: int, stride: int, bpp: int) -> list:
    """Undo the per-row filters and return the rows as bytearrays."""
    rows = []
    prev = bytearray(stride)
    pos = 0
    for _ in range(height):
        ftype = raw[pos]
        cur = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        if ftype == 0:
            pass
        elif ftype == 1:
            for i in range(bpp, stride):
                cur[i] = (cur[i] + cur[i - bpp]) & 255
        elif ftype == 2:
            cur = bytearray([(a + b) & 255 for a, b in zip(cur, prev)])
        elif ftype == 3:
            for i in range(min(bpp, stride)):
                cur[i] = (cur[i] + (prev[i] >> 1)) & 255
            for i in range(bpp, stride):
                cur[i] = (cur[i] + ((cur[i - bpp] + prev[i]) >> 1)) & 255
        elif ftype == 4:
            for i in range(min(bpp, stride)):
                cur[i] = (cur[i] + prev[i]) & 255
            for i in range(bpp, stride):
                a = cur[i - bpp]
                b = prev[i]
                c = prev[i - bpp]
                p = a + b - c
                pa = abs(p - a)
                pb = abs(p - b)
                pc = abs(p - c)
                if pa <= pb and pa <= pc:
                    pred = a
                elif pb <= pc:
                    pred = b
                else:
                    pred = c
                cur[i] = (cur[i] + pred) & 255
        else:
            raise _fail(path, f"A row uses the unknown filter type {ftype}.")
        rows.append(cur)
        prev = cur
    return rows


def _unpack_row(row: bytearray, depth: int, width: int) -> bytearray:
    """Expand a sub-byte row to one sample per byte (raw sample values)."""
    per_byte = 8 // depth
    mask = (1 << depth) - 1
    out = bytearray(len(row) * per_byte)
    for k in range(per_byte):
        shift = 8 - depth * (k + 1)
        out[k::per_byte] = bytearray([(b >> shift) & mask for b in row])
    return out[:width]


def read(path: str):
    """Decode a PNG into (width, height, grey, alpha).

    grey and alpha are row-major bytearrays of 0..255. alpha is None when the file carries
    no transparency. Colour becomes grey by luma. 16-bit samples keep their high byte.
    """
    try:
        with open(path, "rb") as handle:
            data = handle.read()
    except OSError as exc:
        raise DecalError(f"The file {path} cannot be opened. {exc.strerror or exc}.") from exc
    if data[:8] != _SIGNATURE:
        found = data[:8].hex(" ") if data else "an empty file"
        raise DecalError(f"The file {path} is not a PNG. Its first bytes are {found}.")

    header = None
    palette = b""
    trns = b""
    idat = []
    for ctype, body in _chunks(path, data):
        if ctype == b"IHDR":
            header = body
        elif ctype == b"PLTE":
            palette = body
        elif ctype == b"tRNS":
            trns = body
        elif ctype == b"IDAT":
            idat.append(body)
        elif ctype[0:1].isupper() and ctype not in _KNOWN_CRITICAL:
            raise _fail(path, f"It has the critical chunk {ctype.decode('latin-1')}, which the "
                              "decoder does not handle. Re-export the image from Preview.")
    if header is None or len(header) != 13:
        raise _fail(path, "The IHDR chunk is missing or the wrong size.")

    width, height, depth, ctype_id, _comp, _filt, interlace = struct.unpack(">IIBBBBB", header)
    if interlace != 0:
        raise _fail(path, "It is interlaced (Adam7), which the decoder does not handle. "
                          "Re-export the image from Preview without interlacing.")
    if ctype_id not in _COLOUR_TYPES or depth not in _COLOUR_TYPES[ctype_id][1]:
        raise _fail(path, f"Colour type {ctype_id} at {depth} bits is not a valid PNG mode.")
    if width == 0 or height == 0:
        raise _fail(path, "The image has no pixels.")
    if ctype_id == 3 and not palette:
        raise _fail(path, "A palette image has no PLTE chunk.")

    samples = _COLOUR_TYPES[ctype_id][0]
    bits_per_pixel = samples * depth
    stride = (width * bits_per_pixel + 7) // 8
    bpp = max(1, bits_per_pixel // 8)
    try:
        raw = zlib.decompress(b"".join(idat))
    except zlib.error as exc:
        raise _fail(path, f"The image data fails to decompress ({exc}).") from exc
    if len(raw) < height * (stride + 1):
        raise _fail(path, "The image data is shorter than the header says, so it is truncated.")

    rows = _unfilter(path, raw, height, stride, bpp)
    return _to_grey(path, width, height, depth, ctype_id, rows, palette, trns)


def _to_grey(path, width, height, depth, ctype_id, rows, palette, trns):
    """Turn unfiltered rows into 8-bit grey and optional alpha."""
    grey = bytearray()
    alpha = bytearray()
    has_alpha = ctype_id in (4, 6)
    scale = 255 // ((1 << depth) - 1) if depth < 8 else 1

    if ctype_id == 3:
        entries = len(palette) // 3
        lut = bytearray(256)
        for i in range(entries):
            r, g, b = palette[3 * i:3 * i + 3]
            lut[i] = (299 * r + 587 * g + 114 * b + 500) // 1000
        alpha_lut = bytearray(b"\xff" * 256)
        alpha_lut[:len(trns)] = trns[:256]
        has_alpha = bool(trns)
    colour_key = None
    if ctype_id in (0, 2) and trns:
        colour_key = struct.unpack(">%dH" % (len(trns) // 2), trns)
        has_alpha = True

    for row in rows:
        if ctype_id in (0, 3):
            if depth == 16:
                raw_samples = row[0::2]
                full = [(row[2 * i] << 8) | row[2 * i + 1] for i in range(width)] if colour_key \
                    else None
            elif depth == 8:
                raw_samples = row[:width]
                full = raw_samples
            else:
                raw_samples = _unpack_row(row, depth, width)
                full = raw_samples
            if ctype_id == 3:
                grey += raw_samples.translate(bytes(lut))
                alpha += raw_samples.translate(bytes(alpha_lut))
            else:
                if depth < 8:
                    grey += bytearray([v * scale for v in raw_samples])
                else:
                    grey += raw_samples
                if colour_key:
                    alpha += bytearray([0 if v == colour_key[0] else 255 for v in full])
        elif ctype_id == 4:
            step = 2 if depth == 8 else 4
            grey += row[0::step]
            alpha += row[step // 2::step]
        else:
            step = (3 if ctype_id == 2 else 4) * (1 if depth == 8 else 2)
            lane = 1 if depth == 8 else 2
            red = row[0::step]
            green = row[lane::step]
            blue = row[2 * lane::step]
            grey += bytearray([(299 * r + 587 * g + 114 * b + 500) // 1000
                               for r, g, b in zip(red, green, blue)])
            if ctype_id == 6:
                alpha += row[3 * lane::step]
            elif colour_key:
                if depth == 8:
                    keyed = [(r, g, b) == colour_key for r, g, b in zip(red, green, blue)]
                else:
                    keyed = [
                        (struct.unpack(">3H", bytes(row[i * 6:i * 6 + 6])) == colour_key)
                        for i in range(width)
                    ]
                alpha += bytearray([0 if k else 255 for k in keyed])
    return width, height, grey, (alpha if has_alpha else None)
