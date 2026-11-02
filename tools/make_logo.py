#!/usr/bin/env python3
"""Builds assets/logo.png, the app icon, from the pixel art in assets/logo_art.png.

The art's flat background is replaced with a dithered stage of envelope bars, on the
same pixel grid, and the result goes in a macOS icon frame. Needs only the standard library.
"""
import math
import struct
import zlib
from pathlib import Path

ASSETS = Path(__file__).resolve().parent.parent / "assets"

ART_CELLS = 128
ART_CELL_SIZE = 8  # Pixels per cell in the source art.
ART_BACKGROUND = (102, 56, 191)

# Apple's icon grid: a 824 square body on a 1024 canvas, with room for its shadow.
CANVAS = 1024
BODY = 824
BODY_CENTRE_Y = 502
SUPERELLIPSE_POWER = 5.0
ICON_CELL_SIZE = 7

RAMP = [(0x24, 0x11, 0x5e), (0x32, 0x19, 0x7c), (0x44, 0x23, 0x9c), (0x57, 0x2e, 0xb6),
        (0x6c, 0x3b, 0xcb), (0x86, 0x50, 0xdd), (0xa3, 0x70, 0xef)]
BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]

FLOOR_Y = 100
BAR_WIDTH, BAR_GAP = 5, 2
BAR_LIT = (0xc9, 0xa6, 0xff)
STAR = (0xe6, 0xd8, 0xff)
STARS = [(14, 12), (27, 22), (40, 9), (88, 11), (104, 19), (115, 8), (97, 30), (19, 33), (120, 27)]


def read_png(path):
    """Reads an 8-bit RGB PNG into rows of colours."""
    data = path.read_bytes()
    position, compressed = 8, b""

    while position < len(data):
        length, kind = struct.unpack(">I4s", data[position:position + 8])
        chunk = data[position + 8:position + 8 + length]
        position += 12 + length

        if kind == b"IHDR":
            width, height, depth, colour_type, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
            assert depth == 8 and colour_type == 2 and interlace == 0, "expected 8-bit RGB"
        elif kind == b"IDAT":
            compressed += chunk

    raw = zlib.decompress(compressed)
    stride = width * 3
    previous = bytearray(stride)
    rows = []

    for y in range(height):
        start = y * (stride + 1)
        kind = raw[start]
        line = bytearray(raw[start + 1:start + 1 + stride])

        for i in range(stride):
            left = line[i - 3] if i >= 3 else 0
            up = previous[i]
            corner = previous[i - 3] if i >= 3 else 0

            if kind == 1:
                line[i] = (line[i] + left) & 255
            elif kind == 2:
                line[i] = (line[i] + up) & 255
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif kind == 4:
                a, b, c = abs(up - corner), abs(left - corner), abs(left + up - 2 * corner)
                line[i] = (line[i] + (left if a <= b and a <= c else up if b <= c else corner)) & 255

        rows.append([tuple(line[x * 3:x * 3 + 3]) for x in range(width)])
        previous = line

    return rows


def write_png(path, width, height, rgba_rows):
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    raw = b"".join(b"\0" + bytes(row) for row in rgba_rows)
    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 9))
                     + chunk(b"IEND", b""))


def dithered(level, x, y):
    """A colour from the ramp, breaking up the bands between its shades."""
    position = max(0.0, min(1.0, level)) * (len(RAMP) - 1)
    return RAMP[min(len(RAMP) - 1, int(position + (BAYER[y & 3][x & 3] + 0.5) / 16.0))]


def bar_height(x):
    """Bars stand tall at the sides and dip behind the squirrel, like a drawn volume envelope."""
    column = x // (BAR_WIDTH + BAR_GAP)
    distance = abs((column + 0.5) * (BAR_WIDTH + BAR_GAP) - ART_CELLS / 2) / (ART_CELLS / 2)
    return int(6 + 44 * distance ** 1.6 + 7 * math.sin(column * 2.4))


def stage(x, y):
    """The background colour of one cell."""
    if y >= FLOOR_Y:
        return dithered(0.56 - (y - FLOOR_Y) * 0.012, x, y)

    glow = 1.0 - math.hypot(x - 64, (y - 50) * 1.05) / 72.0
    colour = dithered(0.04 + glow * 1.1, x, y)

    if x % (BAR_WIDTH + BAR_GAP) < BAR_WIDTH:
        top = FLOOR_Y - bar_height(x)

        if y == top:
            return BAR_LIT

        if y > top:
            shade = (FLOOR_Y - y) / max(1, FLOOR_Y - top)
            return dithered(0.30 + 0.34 * shade + glow * 0.2, x, y)

    if (x, y) in STARS:
        return STAR

    return colour


def build_art():
    source = read_png(ASSETS / "logo_art.png")
    half = ART_CELL_SIZE // 2
    rows = []

    for y in range(ART_CELLS):
        row = []

        for x in range(ART_CELLS):
            colour = source[y * ART_CELL_SIZE + half][x * ART_CELL_SIZE + half]
            row.append(stage(x, y) if colour == ART_BACKGROUND else colour)

        rows.append(row)

    return rows


def edge_distance(x, y):
    """Roughly how far a point is outside the icon's body, in pixels. Negative is inside."""
    half = BODY / 2
    dx, dy = abs(x - CANVAS / 2) / half, abs(y - BODY_CENTRE_Y) / half
    return ((dx ** SUPERELLIPSE_POWER + dy ** SUPERELLIPSE_POWER) ** (1.0 / SUPERELLIPSE_POWER) - 1.0) * half


def smooth(edge0, edge1, value):
    t = max(0.0, min(1.0, (value - edge0) / (edge1 - edge0)))
    return t * t * (3.0 - 2.0 * t)


def over(base, colour, alpha):
    return tuple(base[i] + (colour[i] - base[i]) * alpha for i in range(3))


def build_icon(art):
    offset_x = (CANVAS - ART_CELLS * ICON_CELL_SIZE) // 2
    offset_y = BODY_CENTRE_Y - ART_CELLS * ICON_CELL_SIZE // 2
    rows = []

    for y in range(CANVAS):
        row = bytearray(CANVAS * 4)

        for x in range(CANVAS):
            distance = edge_distance(x + 0.5, y + 0.5)
            coverage = max(0.0, min(1.0, 0.5 - distance))
            shadow = 0.3 * (1.0 - smooth(-10.0, 34.0, edge_distance(x + 0.5, y + 0.5 - 14.0)))

            if coverage <= 0.0:
                row[x * 4 + 3] = int(shadow * 255)
                continue

            cell_x = max(0, min(ART_CELLS - 1, (x - offset_x) // ICON_CELL_SIZE))
            cell_y = max(0, min(ART_CELLS - 1, (y - offset_y) // ICON_CELL_SIZE))
            colour = art[cell_y][cell_x]

            # A lit bevel: bright along the top of the rim, dark along the bottom.
            down = (y - (BODY_CENTRE_Y - BODY / 2)) / BODY
            rim = smooth(-8.0, -1.0, distance)
            colour = over(colour, (255, 255, 255), rim * 0.55 * (1.0 - down) ** 2)
            colour = over(colour, (14, 5, 40), rim * 0.6 * down ** 2)

            # Soft light falling from above, like glass over the art.
            colour = over(colour, (255, 255, 255), 0.07 * (1.0 - smooth(0.0, 0.5, down)))

            alpha = coverage + shadow * (1.0 - coverage)
            row[x * 4:x * 4 + 4] = bytes((int(colour[0] + 0.5), int(colour[1] + 0.5), int(colour[2] + 0.5), int(alpha * 255 + 0.5)))

        rows.append(row)

    return rows


def main():
    write_png(ASSETS / "logo.png", CANVAS, CANVAS, build_icon(build_art()))


if __name__ == "__main__":
    main()
