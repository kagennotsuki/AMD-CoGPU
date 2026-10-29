# 抓帧导出：.raw（BGRA8，原尺寸）→ PNG（无损，保留原图）
# 同时额外生成一份 1280 宽的 JPG 供快速查看
# 用法：python raw2img.py <dump目录> [查看用宽度，默认1280]
#   raw 文件默认【保留】，加 --delraw 才会删除
import sys, os, glob
from PIL import Image

def read_meta(txt, w, h, pitch):
    if os.path.exists(txt):
        for line in open(txt, encoding="utf-8"):
            for kv in line.strip().split():
                if kv.startswith("w="): w = int(kv[2:])
                elif kv.startswith("h="): h = int(kv[2:])
                elif kv.startswith("pitch="): pitch = int(kv[6:])
    return w, h, pitch

def convert(raw_path, view_w=1280):
    txt = raw_path[:-4] + ".txt"
    w, h, pitch = read_meta(txt, 3840, 2160, None)
    if pitch is None:
        pitch = w * 4
    need = pitch * h
    data = open(raw_path, "rb").read()
    if len(data) < need:
        raise RuntimeError("文件比预期小: %d < %d" % (len(data), need))
    img = Image.frombytes("RGBA", (w, h), data[:need], "raw", "BGRA", pitch).convert("RGB")
    # 1) 原图无损 PNG
    png = os.path.splitext(raw_path)[0] + ".png"
    img.save(png, compress_level=6)
    # 2) 快速查看用小图
    small = os.path.splitext(raw_path)[0] + "_view.jpg"
    if w > view_w:
        img.resize((view_w, int(h * view_w / w)), Image.LANCZOS).save(small, quality=92)
    else:
        img.save(small, quality=92)
    return png, small

def main():
    args = [a for a in sys.argv[1:]]
    delraw = "--delraw" in args
    args = [a for a in args if not a.startswith("--")]
    d = args[0] if args else r"E:\SteamLibrary\steamapps\common\RESIDENT EVIL 2  BIOHAZARD RE2\dump"
    view_w = int(args[1]) if len(args) > 1 else 1280
    files = sorted(glob.glob(os.path.join(d, "*.raw")))
    if not files:
        print("没有 .raw 文件：", d); return
    for f in files:
        try:
            png, small = convert(f, view_w)
            print("PNG :", png)
            print("视图:", small)
            if delraw:
                os.remove(f)
        except Exception as e:
            print("失败", f, e)

if __name__ == "__main__":
    main()
