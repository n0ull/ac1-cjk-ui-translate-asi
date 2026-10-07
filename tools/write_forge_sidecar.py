#!/usr/bin/env python3
# tools/write_forge_sidecar.py —— repack 之后跑：落「启动期文件核验」的 sidecar
#
# 目的：把当前 DataPC.forge 的尺寸/CRC32 记成 sidecar，插件启动时据此决定字形补丁开不开。
#
# 输入：<游戏>\DataPC.forge（--game 指游戏根目录）
# 输出：<游戏>\scripts\AC1_CJK_Forge.txt（--check 时只读不写）
# 参数：--game 游戏根目录（必给）｜ --check 只核验不写 ｜ --verify-crc flags 写 1
#       （插件启动时也核 CRC32，多读 ~200MB，启动慢 1-2s；默认只核尺寸）
# 退出码：0 成功/一致；1 --check 对不上；2 找不到输入。
#
# 用法：
#   python tools\write_forge_sidecar.py --game "<游戏目录>"            # 写 sidecar
#   python tools\write_forge_sidecar.py --game "<游戏目录>" --check    # 只核验，不写
#
# 产物格式（UTF-8，# 开头为注释；数据行四个字段）：
#     DataPC.forge <尺寸字节> <crc32-hex8> <flags: 1=运行期也核 CRC32>
#
# 背景：forge 路线下字形补丁（插表+charmap 页+UV 缩放）以「我们的图集在 forge 里」
#   为前提。sidecar 是安装步骤留下的证据；插件启动时 stat 一次比对，
#   不符 ⇒ 整套补丁不装（Steam verify 还原 / 拷到干净机器 ⇒ 原版游戏零影响）。
#   尺寸 alone 可区分三种已知状态（实测）：
#     原版 203,259,904 / 补丁后 203,489,280 / ATK 重打包原版内容 203,030,528。
import argparse
import os
import sys
import time
import zlib

SIDECAR = "AC1_CJK_Forge.txt"
FORGE = "DataPC.forge"


def crc32_of(path, chunk=1 << 20):
    crc = 0
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            crc = zlib.crc32(b, crc)
    return crc & 0xFFFFFFFF


def read_sidecar(path):
    """返回 (size, crc, flags)；文件缺失/无数据行 ⇒ None。"""
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 4 and parts[0] == FORGE:
                try:
                    return int(parts[1]), int(parts[2], 16), int(parts[3])
                except ValueError:
                    return None
    return None


def main():
    ap = argparse.ArgumentParser(description="写/验 forge 补丁 sidecar（启动期文件核验）")
    ap.add_argument("--game", required=True, help="游戏根目录（DataPC.forge 所在）")
    ap.add_argument(
        "--check", action="store_true", help="只核验 sidecar 与当前 forge 是否一致，不写"
    )
    ap.add_argument(
        "--verify-crc",
        action="store_true",
        help="flags 写 1：插件启动时也核 CRC32（多读 ~200MB，启动慢 1-2s；默认只核尺寸）",
    )
    args = ap.parse_args()

    forge = os.path.join(args.game, FORGE)
    sidecar = os.path.join(args.game, "scripts", SIDECAR)
    if not os.path.isfile(forge):
        print("!! 找不到 %s" % forge)
        raise SystemExit(2)

    size = os.path.getsize(forge)

    if args.check:
        rec = read_sidecar(sidecar)
        if rec is None:
            print("!! sidecar 缺失或无数据行：%s（⇒ 插件侧闸门 = 关）" % sidecar)
            raise SystemExit(2)
        esize, ecrc, eflags = rec
        ok_size = size == esize
        print(
            "forge 尺寸 %d ｜ sidecar 记录 %d ⇒ %s"
            % (size, esize, "尺寸一致 ✓" if ok_size else "★尺寸不符")
        )
        if not ok_size:
            sys.exit(1)
        if eflags & 1:
            crc = crc32_of(forge)
            print(
                "forge CRC32 %08X ｜ sidecar 记录 %08X ⇒ %s"
                % (crc, ecrc, "CRC 一致 ✓" if crc == ecrc else "★CRC 不符")
            )
            if crc != ecrc:
                sys.exit(1)
        else:
            print("flags=0：运行期只核尺寸（sidecar 里的 CRC 仅供诊断）")
        return

    crc = crc32_of(forge)
    flags = 1 if args.verify_crc else 0
    os.makedirs(os.path.dirname(sidecar), exist_ok=True)
    with open(sidecar, "w", encoding="utf-8", newline="\n") as f:
        f.write(
            "# AC1 forge 补丁核验 sidecar（write_forge_sidecar.py 于 %s）\n"
            % time.strftime("%Y-%m-%d %H:%M:%S")
        )
        f.write("# 格式：<文件名> <尺寸> <crc32-hex> <flags: 1=运行期也核 CRC32>\n")
        f.write("# 此文件由 repack/安装步骤产出；插件启动时据此决定字形补丁开不开。\n")
        f.write("%s %d %08x %d\n" % (FORGE, size, crc, flags))
    print("已写 %s" % sidecar)
    print("  %s 尺寸=%d CRC32=%08X flags=%d" % (FORGE, size, crc, flags))


if __name__ == "__main__":
    main()
