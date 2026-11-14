#!/usr/bin/env python3
# Compares native vs web renders of each scene, and culled vs unculled on each backend.
import sys, os, base64, struct, zlib
W, H = 200, 150
out = sys.argv[1]
web = {}
for line in open(os.path.join(out, 'web', 'dump.txt')):
    parts = line.strip().split(' ', 1)
    if len(parts) == 2: web[parts[0]] = base64.b64decode(parts[1])
def png(path, rows, w, h):
    raw = b''.join(b'\x00' + r for r in rows)
    def ch(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))
def rgb(px):  # composite over white
    o = bytearray(W * H * 3)
    for i in range(W * H):
        a = px[i*4+3]
        for k in range(3): o[i*3+k] = (px[i*4+k] * a + 255 * (255 - a) + 127) // 255
    return o
def diff(a, b):
    mx = 0; tot = 0; big = 0
    for i in range(0, len(a)):
        d = abs(a[i] - b[i])
        if d > mx: mx = d
        tot += d
        if d > 16: big += 1
    return mx, tot / len(a), 100.0 * big / len(a)
names = [n[:-5] for n in sorted(os.listdir(os.path.join(out, 'native'))) if n.endswith('.rgba') and '.nocull' not in n]
print(f"{'scene':16s} {'native~chrome max/mean/%>16':>30s}   {'cull==nocull native':>20s} {'web':>6s}")
for n in names:
    nat = open(os.path.join(out, 'native', n + '.rgba'), 'rb').read()
    natn = open(os.path.join(out, 'native', n + '.nocull.rgba'), 'rb').read()
    w = web.get(n); wn = web.get(n + '.nocull')
    nc = 'same' if nat == natn else 'DIFF %d/%.3f/%.2f' % diff(nat, natn)
    if w is None:
        print(f"{n:16s} {'(no web)':>30s}   {nc:>20s}"); continue
    wc = 'same' if w == wn else 'DIFF %d/%.3f/%.2f' % diff(w, wn)
    a, b = rgb(nat), rgb(w)
    mx, mean, big = diff(a, b)
    print(f"{n:16s} {mx:10d} {mean:8.3f} {big:8.2f}%   {nc:>20s} {wc:>6s}")
    rows = [bytes(a[y*W*3:(y+1)*W*3]) + b'\xff\x00\x00' * 2 + bytes(b[y*W*3:(y+1)*W*3]) for y in range(H)]
    png(os.path.join(out, 'png', n + '.png'), rows, W * 2 + 2, H)
