# -*- coding: utf-8 -*-
"""Generate the R Weather HVIF icon (a sun behind a cloud) as rdef hex.

Usage: python3 tools/make_icon.py  -> prints the vector_icon resource body.
"""

import math


def coord(value):
    # Two-byte HVIF coordinate: fixed point with 1/102 steps, offset by 128.
    encoded = int(round((value + 128.0) * 102.0))
    return bytes([0x80 | (encoded >> 8), encoded & 0xFF])


def polygon(points):
    # 0x0A = closed | no curves
    data = bytearray([0x0A, len(points)])
    for x, y in points:
        data += coord(x) + coord(y)
    return data


def circle(cx, cy, r, steps=28):
    return [(cx + r * math.cos(2 * math.pi * i / steps),
             cy + r * math.sin(2 * math.pi * i / steps)) for i in range(steps)]


def rect(left, top, right, bottom):
    return [(left, top), (right, top), (right, bottom), (left, bottom)]


def color(hex_rgb):
    return bytes([0x01]) + bytes.fromhex(hex_rgb) + b"\xFF"


def main():
    styles = [color("D98C00"), color("FFC61E"), color("6E7480"), color("FAFAFC")]
    paths = []
    shapes = []

    def add(style, points):
        paths.append(polygon(points))
        shapes.append(bytes([0x0A, style, 1, len(paths) - 1, 0]))

    add(0, circle(40, 22, 14))
    add(1, circle(40, 22, 12))

    cloud = [((20, 42), 10), ((33, 34), 13), ((47, 43), 9)]
    for grow, style in ((2.0, 2), (0.0, 3)):
        for (cx, cy), r in cloud:
            add(style, circle(cx, cy, r + grow))
        add(style, rect(12 - grow, 42 - grow, 50 + grow, 52 + grow))

    data = bytearray(b"ncif")
    data.append(len(styles))
    for s in styles:
        data += s
    data.append(len(paths))
    for p in paths:
        data += p
    data.append(len(shapes))
    for s in shapes:
        data += s

    text = data.hex().upper()
    print("resource vector_icon {")
    for i in range(0, len(text), 64):
        print('\t$"%s"' % text[i:i + 64])
    print("};")


if __name__ == "__main__":
    main()
