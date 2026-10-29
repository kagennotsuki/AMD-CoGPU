import struct, zlib, os

CAP = r'E:\AI\tmp\mgpu'
BEFORE = os.path.join(CAP, 'mgpu_in.bmp')
AFTER = os.path.join(CAP, 'mgpu_out_s1.00.bmp')


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
                for _ in range(x0, x1):
                    r += px[base]; g += px[base + 1]; b += px[base + 2]
                    base += 3; n += 1
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
w, h, before = load_bmp(BEFORE)
w2, h2, after = load_bmp(AFTER)
print('  尺寸', w, h, '/', w2, h2)

# 差分图（×8，128 中性灰）
diff = bytearray(w * h * 3)
for i in range(w * h):
    for c in range(3):
        dv = after[i * 3 + c] - before[i * 3 + c]
        v = 128 + dv * 8
        diff[i * 3 + c] = 0 if v < 0 else (255 if v > 255 else v)

# 找差分最强的 800x450 区域
best = (0, 0, -1)
for by in range(0, h - 450, 32):
    for bx in range(0, w - 800, 32):
        s = 0
        for yy in range(0, 450, 45):
            base = ((by + yy) * w + bx) * 3
            for xx in range(0, 800, 50):
                o = base + xx * 3
                s += abs(diff[o] - 128) + abs(diff[o + 1] - 128) + abs(diff[o + 2] - 128)
        if s > best[2]:
            best = (bx, by, s)
cx, cy = best[0], best[1]
print('  差分最强区域:', cx, cy)

CW, CH = 800, 450
tiles = [crop(w, h, before, cx, cy, CW, CH),
         crop(w, h, after, cx, cy, CW, CH),
         crop(w, h, diff, cx, cy, CW, CH)]
MW, MH = CW * 3, CH
mont = bytearray(MW * MH * 3)
for i, t in enumerate(tiles):
    tx = i * CW
    for y in range(CH):
        d = (y * MW + tx) * 3
        mont[d:d + CW * 3] = t[y * CW * 3:(y + 1) * CW * 3]
write_png(os.path.join(CAP, 'adapter_cmp.png'), MW, MH, mont)

for name, px in [('adapter_before_1600.png', before),
                 ('adapter_after_1600.png', after),
                 ('adapter_diff8_1600.png', diff)]:
    write_png(os.path.join(CAP, name), 1600, 900, downscale(w, h, px, 1600, 900))
