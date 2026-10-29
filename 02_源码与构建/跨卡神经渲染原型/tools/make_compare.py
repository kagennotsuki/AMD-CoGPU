import struct, zlib, os

CAP = r'E:\AI\tmp\nr_capture'

def load_bmp(path):
    d = open(path, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w = struct.unpack_from('<i', d, 18)[0]
    h = struct.unpack_from('<i', d, 22)[0]
    H = abs(h)
    px = bytearray(w * H * 3)
    i = 0
    for y in range(H):
        s = off + y * w * 4
        for x in range(w):
            px[i] = d[s + x * 4 + 2]
            px[i + 1] = d[s + x * 4 + 1]
            px[i + 2] = d[s + x * 4]
            i += 3
    return w, H, px

def write_png(path, w, h, rgb):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y * w * 3:(y + 1) * w * 3]
    def chunk(t, data):
        c = t + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(bytes(raw), 6))
           + chunk(b'IEND', b''))
    open(path, 'wb').write(png)
    print('  ->', os.path.basename(path), w, 'x', h, len(png) // 1024, 'KB')

def downscale(w, h, px, tw, th):
    out = bytearray(tw * th * 3)
    for y in range(th):
        y0 = y * h // th
        y1 = max(y0 + 1, (y + 1) * h // th)
        for x in range(tw):
            x0 = x * w // tw
            x1 = max(x0 + 1, (x + 1) * w // tw)
            r = g = b = n = 0
            for yy in range(y0, y1):
                base = (yy * w + x0) * 3
                for xx in range(x0, x1):
                    r += px[base]; g += px[base + 1]; b += px[base + 2]
                    base += 3
                    n += 1
            o = (y * tw + x) * 3
            out[o] = r // n; out[o + 1] = g // n; out[o + 2] = b // n
    return out

def crop(w, h, px, cx, cy, cw, ch):
    out = bytearray(cw * ch * 3)
    for y in range(ch):
        sy = min(h - 1, max(0, cy + y))
        s = (sy * w + max(0, cx)) * 3
        out[y * cw * 3:(y + 1) * cw * 3] = px[s:s + cw * 3]
    return out

print('读取…')
names = ['nr_before', 'nr_after_x1', 'nr_after_x4', 'nr_diff_x8']
imgs = {}
for n in names:
    p = os.path.join(CAP, n + '.bmp')
    if os.path.exists(p):
        imgs[n] = load_bmp(p)

w, h, before = imgs['nr_before']
print('  尺寸', w, h)

# 找出「差分最强」的区域，crop 到那里
_, _, diff = imgs['nr_diff_x8']
BW, BH = 16, 16
best = (0, 0, -1.0)
for by in range(0, h - 450, BH):
    for bx in range(0, w - 800, BW):
        s = 0
        for yy in range(0, 450, 45):
            base = ((by + yy) * w + bx) * 3
            for xx in range(0, 800, 50):
                o = base + xx * 3
                s += abs(diff[o] - 128) + abs(diff[o + 1] - 128) + abs(diff[o + 2] - 128)
        if s > best[2]:
            best = (bx, by, s)
cx, cy = best[0], best[1]
print('  差分最强区域: (%d,%d)' % (cx, cy))

CW, CH = 800, 450
tiles = []
for n in names:
    if n not in imgs:
        continue
    _, _, px = imgs[n]
    tiles.append(crop(w, h, px, cx, cy, CW, CH))

# 2x2 拼图（1:1 像素）
cols = 2
rows = (len(tiles) + 1) // 2
MW, MH = CW * cols, CH * rows
mont = bytearray(MW * MH * 3)
for i, t in enumerate(tiles):
    tx = (i % cols) * CW
    ty = (i // cols) * CH
    for y in range(CH):
        d = ((ty + y) * MW + tx) * 3
        mont[d:d + CW * 3] = t[y * CW * 3:(y + 1) * CW * 3]
write_png(os.path.join(CAP, 'cmp_crops.png'), MW, MH, mont)

# 全画面（缩到 1600x900）便于通看
for n in ['nr_before', 'nr_after_x1', 'nr_after_x4', 'nr_diff_x8']:
    if n in imgs:
        _, _, px = imgs[n]
        write_png(os.path.join(CAP, n + '_1600.png'), 1600, 900, downscale(w, h, px, 1600, 900))
