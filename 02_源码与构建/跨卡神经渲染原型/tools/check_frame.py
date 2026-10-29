import struct, statistics

CAP = r'E:\AI\tmp\nr_capture'

def load_bmp(path, step=4):
    d = open(path, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w = struct.unpack_from('<i', d, 18)[0]
    h = struct.unpack_from('<i', d, 22)[0]
    H = abs(h)
    rows = range(0, H, step)
    cols = range(0, w, step)
    out = []
    for y in rows:
        s = off + y * w * 4
        row = []
        for x in cols:
            row.append((d[s + x * 4 + 2], d[s + x * 4 + 1], d[s + x * 4]))
        out.append(row)
    return w, H, out

w, h, A = load_bmp(CAP + r'\nr_before.bmp')
_, _, B = load_bmp(CAP + r'\nr_after_x1.bmp')

signed = [0.0, 0.0, 0.0]
absd = []
n = 0
for ya, yb in zip(A, B):
    for pa, pb in zip(ya, yb):
        for c in range(3):
            dv = pb[c] - pa[c]
            signed[c] += dv
            absd.append(abs(dv))
        n += 1
signed = [s / n for s in signed]
print('== 采样统计（每 4 像素取 1，共 %d 像素）==' % n)
print('  有符号平均偏移 (R,G,B) = (%.2f, %.2f, %.2f)  ← 接近 0 说明没有整体偏色/亮度漂移' % tuple(signed))
print('  绝对平均改动          = %.2f / 255' % (sum(absd) / len(absd)))
print('  改动中位数            = %d / 255' % statistics.median(absd))
print('  |差|>=4 的通道占比     = %.1f%%' % (100.0 * sum(1 for x in absd if x >= 4) / len(absd)))
print('  |差|>=16 的通道占比    = %.1f%%' % (100.0 * sum(1 for x in absd if x >= 16) / len(absd)))
print('  |差|>=64 的通道占比    = %.2f%%' % (100.0 * sum(1 for x in absd if x >= 64) / len(absd)))

# 相关性（亮度），用来判断是不是同一幅画面、有没有错位
la = [0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2] for row in A for p in row]
lb = [0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2] for row in B for p in row]
ma = sum(la) / len(la); mb = sum(lb) / len(lb)
cov = sum((x - ma) * (y - mb) for x, y in zip(la, lb))
va = sum((x - ma) ** 2 for x in la); vb = sum((y - mb) ** 2 for y in lb)
print('  亮度相关 r            = %.6f  （≈1 说明结构一致，没有错位）' % (cov / (va * vb) ** 0.5))
print('  亮度均值 前 %.2f → 后 %.2f (%+.2f)' % (ma, mb, mb - ma))

# 细节量：局部梯度能量是否上升（NR 的「细节提升」特征）
def grad(L, w, h):
    s = 0.0; c = 0
    for y in range(1, len(L) // w):
        base = y * w
        for x in range(1, w):
            s += abs(L[base + x] - L[base + x - 1]) + abs(L[base + x] - L[base - w + x])
            c += 2
    return s / c
sw = (w + 3) // 4
print('  梯度能量（越高越锐）  前 %.3f → 后 %.3f' % (grad(la, sw, h), grad(lb, sw, h)))
