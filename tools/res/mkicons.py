#!/usr/bin/env python3
"""Draws the AUDIONT program icons as 16-colour pixel art (the palette of NT
3.x / VGA): audmix.ico, audtest.ico, setup.ico and setup2.ico (uninstall).

  python3 mkicons.py [preview.png]
"""
import os, struct, sys
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
VGA = [(0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0), (0, 0, 128), (128, 0, 128), (0, 128, 128),
       (192, 192, 192), (128, 128, 128), (255, 0, 0), (0, 255, 0), (255, 255, 0), (0, 0, 255), (255, 0, 255),
       (0, 255, 255), (255, 255, 255)]
K, DR, DG, OL, NV, PU, TE, SI, GR, RE, LI, YE, BL, MA, CY, WH = range(16)
T = -1                                          # transparent


class Pic:
    def __init__(self, n):
        self.n = n
        self.p = [[T] * n for _ in range(n)]

    def px(self, x, y, c):
        if 0 <= x < self.n and 0 <= y < self.n:
            self.p[y][x] = c

    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.px(x, y, c)

    def frame(self, x0, y0, x1, y1, c):
        for x in range(x0, x1 + 1):
            self.px(x, y0, c); self.px(x, y1, c)
        for y in range(y0, y1 + 1):
            self.px(x0, y, c); self.px(x1, y, c)

    def box3d(self, x0, y0, x1, y1, face=SI, light=WH, dark=GR, edge=K):
        """a raised box: light top/left, dark bottom/right, black outline"""
        self.rect(x0, y0, x1, y1, face)
        self.frame(x0, y0, x1, y1, edge)
        for x in range(x0 + 1, x1):
            self.px(x, y0 + 1, light); self.px(x, y1 - 1, dark)
        for y in range(y0 + 1, y1):
            self.px(x0 + 1, y, light); self.px(x1 - 1, y, dark)

    def line(self, x0, y0, x1, y1, c):
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        e = dx + dy
        while True:
            self.px(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * e
            if e2 >= dy:
                e += dy; x0 += sx
            if e2 <= dx:
                e += dx; y0 += sy

    def disc(self, cx, cy, r, c, edge=None):
        for y in range(self.n):
            for x in range(self.n):
                d = (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2
                if d <= r * r:
                    self.px(x, y, edge if edge is not None and d > (r - 1) ** 2 else c)

    def fill(self, x, y, c):
        old = self.p[y][x]
        st = [(x, y)]
        while st:
            x, y = st.pop()
            if 0 <= x < self.n and 0 <= y < self.n and self.p[y][x] == old and old != c:
                self.p[y][x] = c
                st += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]

    def art(self, x0, y0, rows, keys):
        for j, row in enumerate(rows):
            for i, ch in enumerate(row):
                if ch in keys:
                    self.px(x0 + i, y0 + j, keys[ch])

    def outline(self, c=K):
        """a dark edge around every opaque area (the icon's silhouette)"""
        q = [r[:] for r in self.p]
        for y in range(self.n):
            for x in range(self.n):
                if self.p[y][x] == T:
                    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        xx, yy = x + dx, y + dy
                        if 0 <= xx < self.n and 0 <= yy < self.n and self.p[yy][xx] not in (T, c):
                            q[y][x] = c
                            break
        self.p = q



# ---------------------------------------------------------------- AUDIONT pictures

def speaker(p, x, y, h=24, face=SI):
    """a loudspeaker facing right with its top left at x, y and height h (even)"""
    m = h // 2
    bw, bh = max(2, h // 4), max(3, h * 3 // 8)            # magnet box
    by = y + m - bh // 2
    p.rect(x, by, x + bw - 1, by + bh - 1, GR); p.frame(x, by, x + bw - 1, by + bh - 1, K)
    cw = max(3, h * 3 // 8)                                  # cone width
    for i in range(cw):
        top = by - (i * (by - y)) // (cw - 1)
        bot = by + bh - 1 + (i * (y + h - 1 - (by + bh - 1))) // (cw - 1)
        p.line(x + bw + i, top, x + bw + i, bot, face)
        p.px(x + bw + i, top, K); p.px(x + bw + i, bot, K)
    p.line(x + bw + cw - 1, y, x + bw + cw - 1, y + h - 1, K)
    if h >= 16:
        p.line(x + bw, by + 1, x + bw + cw - 2, y + 1, WH)
    return x + bw + cw                                       # right edge


def arcs(p, cx, cy, radii, c=NV, x0=0):
    for r in radii:
        for j in range(-r, r + 1):
            for t in (0,):
                xx = cx + int(((r - t) ** 2 - j * j) ** 0.5 + 0.5) if (r - t) ** 2 >= j * j else None
                if xx is not None and xx > x0 and abs(j) < r * 0.8:
                    p.px(xx, cy + j, c)


def ico_mix():
    p = Pic(32)
    # a mixing desk: three channels with knobs at different levels, a speaker on top
    p.box3d(1, 12, 30, 31)
    for i, (h, c) in enumerate(((19, RE), (23, LI), (16, BL))):
        x = 6 + 9 * i
        p.rect(x, 15, x + 1, 28, K)
        p.rect(x - 3, h, x + 4, h + 4, c); p.frame(x - 3, h, x + 4, h + 4, K)
        p.line(x - 2, h + 1, x + 3, h + 1, WH)
    right = speaker(p, 3, 0, 12)
    arcs(p, right - 4, 6, (6, 9, 12), NV, right)
    return p


def sm_mix():
    p = Pic(16)
    p.box3d(0, 6, 15, 15)
    for i, (h, c) in enumerate(((9, RE), (11, LI), (8, BL))):
        x = 3 + 5 * i
        p.line(x, 8, x, 14, K)
        p.rect(x - 1, h, x + 1, h + 1, c)
    speaker(p, 2, 0, 6)
    return p


def ico_test():
    p = Pic(32)
    # a monitor showing a sine wave, a green check mark
    p.box3d(4, 0, 31, 21)
    p.rect(7, 3, 28, 17, K)
    import math
    prev = None
    for x in range(7, 29):
        y = 10 + int(round(5 * math.sin((x - 7) * 2 * math.pi / 11)))
        if prev is not None:
            p.line(x - 1, prev, x, y, LI)
        prev = y
    p.rect(14, 22, 21, 23, GR); p.box3d(10, 24, 25, 27)
    speaker(p, 0, 18, 12)
    p.art(20, 23, ["........##",
                   ".......###",
                   "##....###.",
                   "###..###..",
                   ".######...",
                   "..####....",
                   "...##....."], {'#': DG})
    return p


def sm_test():
    p = Pic(16)
    p.box3d(2, 0, 15, 11)
    p.rect(4, 2, 13, 9, K)
    import math
    prev = None
    for x in range(4, 14):
        y = 5 + int(round(2.5 * math.sin((x - 4) * 2 * math.pi / 6)))
        if prev is not None:
            p.line(x - 1, prev, x, y, LI)
        prev = y
    speaker(p, 0, 8, 8)
    return p


def box(p, x0, y0, x1, y1):
    p.rect(x0, y0, x1, y1, OL); p.frame(x0, y0, x1, y1, K)
    p.line(x0 + 1, y0 + 1, x1 - 1, y0 + 1, YE)


def ico_inst():
    p = Pic(32)
    # an open box, a speaker going in
    box(p, 4, 17, 27, 31)
    for i in range(5):
        p.line(4 - i, 17 - i, 9 - i, 17 - i, YE if i else OL)
        p.line(22 + i, 17 - i, 27 + i, 17 - i, YE if i else OL)
    p.line(0, 13, 5, 13, K); p.line(0, 13, 4, 17, K)
    p.line(26, 13, 31, 13, K); p.line(31, 13, 27, 17, K)
    speaker(p, 9, 0, 18)
    p.art(24, 1, ["..#..",
                  "..#..",
                  "#####",
                  ".###.",
                  "..#.."], {'#': DG})
    return p


def sm_inst():
    p = Pic(16)
    box(p, 1, 9, 14, 15)
    speaker(p, 4, 0, 10)
    return p


def ico_uninst():
    p = ico_inst()
    for i in range(-1, 2):
        p.line(18 + i, 19, 29 + i, 30, RE)
        p.line(29 + i, 19, 18 + i, 30, RE)
    return p


def sm_uninst():
    p = sm_inst()
    p.line(9, 9, 15, 15, RE); p.line(15, 9, 9, 15, RE)
    p.line(10, 9, 15, 14, RE); p.line(14, 9, 9, 14, RE)
    return p


ICONS = [('audmix', [(ico_mix, sm_mix)]), ('audtest', [(ico_test, sm_test)]),
         ('setup', [(ico_inst, sm_inst), (ico_uninst, sm_uninst)])]


def bmp4(p):
    """bottom-up 4-bit pixels and a 1-bit AND mask (transparent = 1)"""
    n = p.n
    xor = bytearray()
    for y in range(n - 1, -1, -1):
        row = [c if c != T else 0 for c in p.p[y]]
        b = bytearray((row[i] << 4) | row[i + 1] for i in range(0, n, 2))
        while len(b) % 4:
            b.append(0)
        xor += b
    andm = bytearray()
    for y in range(n - 1, -1, -1):
        bits = 0
        out = bytearray()
        for x in range(n):
            bits = (bits << 1) | (1 if p.p[y][x] == T else 0)
            if x % 8 == 7:
                out.append(bits); bits = 0
        while len(out) % 4:
            out.append(0)
        andm += out
    return bytes(xor), bytes(andm)


def ico_bytes(pics):
    """an .ico with the images (32x32 and 16x16, 16 colours each)"""
    imgs = []
    for p in pics:
        xor, andm = bmp4(p)
        hdr = struct.pack('<IiiHHIIiiII', 40, p.n, p.n * 2, 1, 4, 0, len(xor) + len(andm), 0, 0, 16, 0)
        pal = b''.join(struct.pack('BBBB', b, g, r, 0) for r, g, b in VGA)
        imgs.append(hdr + pal + xor + andm)
    out = struct.pack('<HHH', 0, 1, len(pics))
    off = 6 + 16 * len(pics)
    for p, d in zip(pics, imgs):
        out += struct.pack('<BBBBHHII', p.n, p.n, 16, 0, 1, 4, len(d), off)
        off += len(d)
    return out + b''.join(imgs)


def render(p, scale):
    im = Image.new('RGB', (p.n * scale, p.n * scale), (0, 128, 128))
    d = ImageDraw.Draw(im)
    for y in range(p.n):
        for x in range(p.n):
            if p.p[y][x] != T:
                d.rectangle([x * scale, y * scale, x * scale + scale - 1, y * scale + scale - 1], fill=VGA[p.p[y][x]])
    return im


def main():
    previews = []
    for name, makers in ICONS:
        for i, (mk, mks) in enumerate(makers):
            big, small = mk(), mks()
            previews.append((big, small))
            fn = name + ('' if i == 0 else str(i + 1)) + '.ico'
            open(os.path.join(HERE, fn), 'wb').write(ico_bytes([big, small]))
    if len(sys.argv) > 1:
        sc = 6
        im = Image.new('RGB', (len(previews) * 36 * sc, 36 * sc), (192, 192, 192))
        for i, (b, s) in enumerate(previews):
            im.paste(render(b, sc), (i * 36 * sc, 0))
            im.paste(render(s, 3), (i * 36 * sc + 33 * sc - 48, 34 * sc - 48))
            im.paste(render(b, 1), (i * 36 * sc + 2, 34 * sc - 34))
        im.save(sys.argv[1])


if __name__ == '__main__':
    main()
