"""A render target's dump as the report keeps it: devbench's 24-bit BMP turned into a PNG, with the standard library."""

from __future__ import annotations

import struct
import zlib

_PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def _chunk(kind: bytes, data: bytes) -> bytes:
    """One PNG chunk: its length, type, data and the CRC of type and data."""
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def bmp_to_png(bmp: bytes, left_half: bool = False) -> bytes:
    """
    An uncompressed 24-bit BMP (rows padded to 4 bytes, bottom-up unless its height is negative) as an RGB PNG. With
    left_half, only the left half of each row: the left eye of a VR frame, whose render targets hold both eyes side by
    side. ValueError for another kind of BMP.
    """
    if bmp[:2] != b"BM" or len(bmp) < 54:
        raise ValueError("the dump is not a BMP")
    offset, header, width, height = struct.unpack_from("<I I i i", bmp, 10)
    bits, compression = struct.unpack_from("<H I", bmp, 28)
    if header < 40 or bits != 24 or compression != 0 or width <= 0 or height == 0:
        raise ValueError(f"the dump is a {bits}-bit BMP of compression {compression}, not an uncompressed 24-bit one")
    stride, rows = (width * 3 + 3) & ~3, abs(height)
    if len(bmp) < offset + stride * rows:
        raise ValueError("the dump is shorter than its header says")
    kept = width // 2 if left_half else width
    order = range(rows - 1, -1, -1) if height > 0 else range(rows)
    bgr = b"".join(bmp[offset + row * stride : offset + row * stride + kept * 3] for row in order)
    rgb = bytearray(len(bgr))
    rgb[0::3], rgb[1::3], rgb[2::3] = bgr[2::3], bgr[1::3], bgr[0::3]
    line = kept * 3
    # each scanline starts with its filter type: 0, none
    raw = b"".join(b"\x00" + rgb[row * line : (row + 1) * line] for row in range(rows))
    header_data = struct.pack(">IIBBBBB", kept, rows, 8, 2, 0, 0, 0)
    return _PNG_SIGNATURE + _chunk(b"IHDR", header_data) + _chunk(b"IDAT", zlib.compress(raw, 6)) + _chunk(b"IEND", b"")
