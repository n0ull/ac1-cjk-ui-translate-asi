#!/usr/bin/env python3
# tools/rgba_to_dds.py —— ACGX（pack_glyphs.py 产出的 .rgba）→ DDS（默认 BC3/DXT5，Levels=1）
#
# 目的：把打包器出的 RGBA 图集转成 AnvilToolkit 纹理 Replace 的输入（不要勾 mip）。
#
# 输入：atlas_<set>.rgba（ACGX 头 + 整幅 RGBA，pack_glyphs.py 的产物）
# 输出：atlas_<set>.dds（--all 模式落 tools\out\glyphs\dds\；显式 a8r8g8b8 落 dds_a8r8g8b8\）
# 参数：
#   --format bc3|bc2|a8r8g8b8  目标格式（默认 bc3 = 出货格式）
#   --all                      转四套（默认 tools\out\glyphs\atlas_*.rgba）
#   --out-dir <目录>           --all 的输出目录（默认随 --format 变，见上）
#   <in.rgba> <out.dds>        单文件模式（给 out.dds 就落哪）
# 退出码：0 成功；1 有输入缺失/格式不符；2 用法或输入错误。
#
# 用法：
#   python tools\rgba_to_dds.py --all                       # 四套全转
#   python tools\rgba_to_dds.py tools\out\glyphs\atlas_textpc.rgba out.dds
#   python tools\rgba_to_dds.py --format=bc2 --all          # 显式换 BC2/DXT3
#   python tools\rgba_to_dds.py --format=a8r8g8b8 --all     # 仅实验用（见下）
#
# 出货格式 = **BC3/DXT5**（--all 的 BC2/BC3 都落 dds\；显式 a8r8g8b8 落 dds_a8r8g8b8\）。
#   实机结论：未压缩 A8R8G8B8 的 fmt=0 图集在 **Dx10 点击即崩**（Dx9 可跑，别被它骗）；
#   BC3 是压缩格式（format 码 4），Dx9/Dx10 全通。原版图集本身是 BC2/DXT3（tms 可对账）。
import argparse
import os
import struct
import sys

SETS = ("textpc", "bold", "techno", "title")  # 镜像 pack_glyphs.FONT_SETS 的套名
DEFAULT_FMT = "bc3"  # 出货格式；--format 可临时换
TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))


def convert(src, dst, fmt=DEFAULT_FMT):
    """ACGX .rgba → DDS。→ (w, h, 产物字节数)。输入不像 ACGX / 长度不符 ⇒ SystemExit。"""
    with open(src, "rb") as f:
        raw = f.read()
    if len(raw) < 16:
        print("!! ACGX 文件头不完整：%s" % src)
        raise SystemExit(2)
    magic, w, h, zero = struct.unpack_from("<4sIII", raw, 0)
    if magic != b"ACGX" or zero != 0:
        print("!! 不是 ACGX（pack_glyphs.py 的 .rgba）：%s" % src)
        raise SystemExit(2)
    px = raw[16:]
    if len(px) != w * h * 4:
        print("!! 长度不符：%s（%d != %d*%d*4）" % (src, len(px), w, h))
        raise SystemExit(2)
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    if fmt in ("bc2", "bc3"):
        # BC2/DXT3（与原版字体图集同格式，format 码同为 4，唯一差异=宽高）或
        # BC3/DXT5（备选）。走 Pillow 的 bcn 编码器；Levels=1（Pillow 默认不写 mip）。
        from PIL import Image

        img = Image.frombytes("RGBA", (w, h), px)
        img.save(dst, pixel_format="DXT3" if fmt == "bc2" else "DXT5")
        return w, h, os.path.getsize(dst)
    bgra = bytearray(len(px))
    bgra[0::4] = px[2::4]  # B
    bgra[1::4] = px[1::4]  # G
    bgra[2::4] = px[0::4]  # R
    bgra[3::4] = px[3::4]  # A
    hdr = b"DDS " + struct.pack(
        "<31I",
        124,
        0x1 | 0x2 | 0x4 | 0x8 | 0x1000,  # CAPS|HEIGHT|WIDTH|PITCH|PIXELFORMAT
        h,
        w,
        w * 4,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        32,
        0x40 | 0x1,
        0,
        32,  # pf: RGB|ALPHAPIXELS, 32bpp
        0x00FF0000,
        0x0000FF00,
        0x000000FF,
        0xFF000000,
        0x1000,  # DDSCAPS_TEXTURE
        0,
        0,
        0,
        0,
    )
    # ★ 同 pack_glyphs 的落盘纪律：用 `with`，写全/写完由 close 保证，出错会抛；
    #   裸 open(...).write(...) 在磁盘满时会静默留截断的 DDS。
    with open(dst, "wb") as f:
        f.write(hdr + bytes(bgra))
    return w, h, os.path.getsize(dst)


def convert_all(fmt, out_dir):
    """四套全转 → 退出码（0 全转成功；1 有缺的）。缺文件点名报出，不静默跳过。"""
    base = os.path.join(TOOLS_DIR, "out", "glyphs")
    missing = 0
    for s in SETS:
        src = os.path.join(base, "atlas_%s.rgba" % s)
        if not os.path.isfile(src):
            print("!! 缺 %s（先跑 pack_glyphs.py）" % src)
            missing += 1
            continue
        dst = os.path.join(out_dir, "atlas_%s.dds" % s)
        w, h, n = convert(src, dst, fmt)
        print("%-8s %4dx%-5d -> %s（%d B）" % (s, w, h, dst, n))
    return 1 if missing else 0


def main():
    ap = argparse.ArgumentParser(
        description="ACGX .rgba → DDS（默认 BC3/DXT5，Levels=1）",
        epilog="退出码：0 成功 / 1 有输入缺失或格式不符 / 2 用法错误。",
    )
    ap.add_argument(
        "--format",
        choices=("bc3", "bc2", "a8r8g8b8"),
        default=DEFAULT_FMT,
        help="目标格式（默认 bc3 = 出货格式；a8r8g8b8 仅实验，Dx10 下点击即崩）",
    )
    ap.add_argument("--all", action="store_true", help="转四套 tools\\out\\glyphs\\atlas_*.rgba")
    ap.add_argument(
        "--out-dir",
        help="--all 的输出目录（默认 tools\\out\\glyphs\\dds\\；a8r8g8b8 时 dds_a8r8g8b8\\）",
    )
    ap.add_argument("inputs", nargs="*", help="单文件模式：<in.rgba> <out.dds>")
    args = ap.parse_args()

    if args.all:
        if args.inputs:
            ap.error("--all 与单文件模式（in.rgba out.dds）二选一")
        out_dir = args.out_dir or os.path.join(
            TOOLS_DIR,
            "out",
            "glyphs",
            "dds" if args.format in ("bc2", "bc3") else "dds_a8r8g8b8",
        )
        return convert_all(args.format, out_dir)

    if len(args.inputs) != 2:
        ap.error("单文件模式要两个参数：<in.rgba> <out.dds>（或用 --all）")
    src, dst = args.inputs
    if not os.path.isfile(src):
        print("!! 找不到 %s" % src)
        return 2
    w, h, n = convert(src, dst, args.format)
    print("%dx%d -> %s（%d B）" % (w, h, dst, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
