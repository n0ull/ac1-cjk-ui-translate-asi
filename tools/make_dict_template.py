# tools/make_dict_template.py —— 从主词典导出「翻译模板」
#
# 目的：跑出模板（英文键 + 空译文列），供人工填译文。然后再跑 pack_glyphs.py 打包成图集 + 清单。
#   1) 跑本脚本拿到 data\dict.<lang>.txt（英文键 + **空译文列**）
#   2) 只填译文那一列（UTF-8）
#   3) 跑 pack_glyphs.py --lang <lang>，得到该语言的图集 + 清单
#
# 不给「已翻好的」模板：我们只有中文。**译文内容是人的活，工具不该假装能做。**
#
# 输入：LG 主词典 LGCStringDict_01.txt（CP950 成对块；默认 tools\input\ 那份）
# 输出：data\dict.<lang>.txt（UTF-8，每行 `英文<TAB>译文`）
# 参数：--lg-dict 主词典 ｜ --lang zh/ja/ko（默认 ja）｜ --out 输出文件 ｜ --prefill
#       用 LG 词典的 CHI 块预填译文列（只有中文该用）
# 退出码：0 成功；1 校验警告（成对块数不等 / 行结构不符 —— 产物已写，但要过目）；2 缺输入/输入不对
#
# 用法：
#   python tools/make_dict_template.py --lg-dict <LGCStringDict_01.txt> --lang ja
#
# ★ 产物**就是那份词典**（data\dict.<lang>.txt）：填完直接部署成
#   scripts\AC1_Dict.txt。运行期只读它 —— 查到就替换，**查不到留英文原文**，
#   绝不回退到 LG 词典的中文。与游戏的 LGCStringDict_01.txt **完全解耦**。

import argparse
import os
import sys

from lgdict import (
    E_CHI,
    E_ENG,
    S_CHI,
    S_ENG,
    expand_hex_escapes,
    iter_blocks,
    looks_like_lg_dict,
    trim_bytes,
    decode_chi,
)

RELEASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEF_LG = os.path.join(RELEASE_DIR, "tools", "input", "LGCStringDict_01.txt")

LANGS = {"zh": "简体中文", "ja": "日本語", "ko": "한국어"}

# 丢弃原因的计数器（每个被过滤的条目都写入文件头统计）
DROPS = {"控制字符": 0, "非ASCII": 0, "空": 0, "重复": 0, "CHI不可解": 0}


# ---------------------------------------------------------------- 键与预填值
def fold_key(eb):
    """ENG 块 → 折叠键：多行折单空格、剔 NUL（键里不能有换行，词典一行一条的格式会破）。

    ⚠ 行内连续空白**不**折叠：运行期 load_supplement 会对键做 canon()，
      这里保留主词典键的可读形式。
    ⚠ 按 `\\n` 严格切行（splitlines 会把 \\v/\\f/U+0085/U+2028 也折成行，与运行期不符）。
    """
    lines = (x.strip() for x in eb.decode("latin-1").replace("\x00", "").split("\n"))
    return " ".join(x for x in lines if x)


def key_text(folded):
    """键的判据：纯 ASCII + `\\xNNNN` 展开 + 无控制字符 → (键, None)，丢弃 → (None, 原因)。

    ★ 非 ASCII 的键**丢弃并计数**，不许 `encode("ascii","replace")` 悄悄换 '?' 混进键
      —— 键列口径 = 运行期 expand_ascii（ASCII + 转义），非 ASCII 字节是输入坏了。
    ★ 剔掉带**控制字符**的键并**计数**：它们是 UI 控制码 / 格式串，人翻不了。
    """
    if not folded.isascii():
        DROPS["非ASCII"] += 1
        return None, "含非 ASCII 字节"
    s = expand_hex_escapes(folded)
    if any(ord(c) < 0x20 or 0x7F <= ord(c) <= 0x9F for c in s):
        DROPS["控制字符"] += 1
        return None, "含控制字符"
    if not s.strip():
        DROPS["空"] += 1
        return None, "空键"
    return s, None


def chi_prefill_value(raw):
    """CHI 块 → 预填译文；解不出 → None（计数后丢弃）。

    模板保留 `\\xNNNN` 转义；解码和 Big5 边界规则仍统一由 lgdict.decode_chi 提供。
    """
    try:
        v = decode_chi(raw, preserve_escapes=True, strict=True)
    except UnicodeDecodeError:
        DROPS["CHI不可解"] += 1
        return None
    return v.replace("\x00", "").strip()


# ---------------------------------------------------------------- 词典抽取
def eng_keys(path):
    """切出 ENG 侧的键并去重（只出键，不配译文）。"""
    with open(path, "rb") as f:
        data = f.read()
    out, seen, dup = [], set(), 0
    for eb in iter_blocks(data, S_ENG, E_ENG):
        s, _ = key_text(fold_key(eb))
        if s is None or not s.strip():
            continue
        if s in seen:
            dup += 1
            continue
        seen.add(s)
        out.append(s)
    return out, dup


def eng_chi_pairs(path):
    """切出 (英文键, 中文译文, 原文块行数) 成对块。

    ★ LG 的资源是**平行块**：`xfhsm_res_ENG_Start…End` 与 `xfhsm_res_CHI_Start…End`
      一一对应、按出现顺序配对（项目笔记 §「成对块」）。`_02` 里也是同样结构，
      但它装的是 DBCS 码位对（条目形如 `¡@`），不是自然语言 —— 本函数照样能切，
      只是切出来没有可用的译文。
      ⚠ 这里的配对是**按顺序 zip**（模板导出策略），与 lgdict.iter_paired_blocks 的
        load_main 窗口规则不同 —— 别互相替换，各自服务各自的消费方。
    """
    with open(path, "rb") as f:
        data = f.read()
    engs = list(iter_blocks(data, S_ENG, E_ENG))
    chis = list(iter_blocks(data, S_CHI, E_CHI))
    pairs = []
    for eb, cb in zip(engs, chis):
        s, _ = key_text(fold_key(eb))
        if s is None or not s.strip():
            continue
        v = chi_prefill_value(cb)
        if v is None:
            continue
        # ★★★ 译文的内部换行**必须转义成 \\n，绝不能折叠成空格** ★★★
        #   实测 **469 / 1496 条（31%）** 的 CHI 块含内部换行，而它们恰恰是
        #   「按键提示」串 —— 本质上就是多行列表：
        #       #\n失去能力：中斷防禦\n\n失去能力：短刀專長（傷害）\n\n…
        #   折叠掉会把 5 行提示变成 1 行超长行，实机会炸版。
        #   ⚠ 键那侧相反：内部换行**折叠成空格是对的**，因为 canon() 也会折叠，
        #     两边一致才能匹配；键里绝不能留真换行（会破「一行一条」）。
        # ★ 只 rstrip 每行，**不丢空行** —— 游戏是按原文的行结构排版的，
        #   原文 `#\nA\n\nB\n\nC` 里条目之间**有空行**做视觉分隔，
        #   过滤掉会让提示挤成一坨。键那侧相反（canon 折叠，不需要结构）。
        # ★ 与 fold_key 同口径：按 `\\n` 严格切行，行数才与 inner_lines 的计数对得上。
        v = "\\n".join(x.rstrip() for x in v.split("\n"))
        pairs.append((s, v, inner_lines(eb)))
    return pairs, len(engs), len(chis)


def inner_lines(b):
    """块**去掉首尾空白**之后的行数（游戏按这个结构排版；只按 `\\n` 计行）。"""
    return trim_bytes(b).count(b"\n") + 1


# ---------------------------------------------------------------- 模板组装
def collect_rows(path, prefill):
    """抽取 + 去重 → (键序, 译文 map, 行数 map, 重复数, 警告列表)。"""
    warnings = []
    if prefill:
        pairs, n_eng, n_chi = eng_chi_pairs(path)
        if n_eng != n_chi:
            warnings.append(
                "成对块计数不等：ENG=%d / CHI=%d —— 配对按出现顺序 zip，"
                "缺块会让后续译文整体错位（请先修输入文件）" % (n_eng, n_chi)
            )
        seen, rows, dup = set(), [], 0
        for k, v, nl in pairs:
            if k in seen:
                dup += 1
                continue
            seen.add(k)
            rows.append((k, v, nl))
        keys = [k for k, _v, _n in rows]
        vals = {k: v for k, v, _n in rows}
        linecounts = {k: n for k, _v, n in rows}
    else:
        keys, dup = eng_keys(path)
        vals, linecounts = {}, {}
    return keys, vals, linecounts, dup, warnings


def write_template(out, lang, prefill, keys, vals, linecounts, dup):
    """落模板文件（含文件头说明）→ 行结构不符的条数。"""
    label = LANGS[lang]
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("# AC1 汉化项目 —— %s 翻译模板\n" % label)
        f.write("#\n")
        f.write("# 格式：**UTF-8**，每行 `英文<TAB>译文`。注释 = 以 `#` 开头**且不含 TAB**的行\n")
        f.write("#   （游戏的按键提示串真的以 `#` 开头，所以不能只看第一个字符）。空行忽略。\n")
        f.write("# 译文里可写转义：`\\n` 换行、`\\t` 制表、`\\\\` 字面反斜杠、`\\xNNNN` 指定码元\n")
        f.write(
            "#   —— 这一套与**游戏自己**的约定一致（实测 LG 词典 2,992 块里 529 块含反斜杠）。\n"
        )
        f.write("# 用法：\n")
        f.write("#   1) 只填**译文那一列**，别动英文列（它是匹配键，改了就命中不了）\n")
        f.write("#   2) 跑打包器：\n")
        f.write(
            "#      python tools/pack_glyphs.py --lang %s --dict <本文件> --out <输出目录>\n" % lang
        )
        f.write("#   3) 把**本文件**复制成游戏 scripts\\AC1_Dict.txt（词典）\n")
        f.write("#   4) 打包产物 atlas_*.rgba + AC1_CJK_Glyphs.bin 也复制到 scripts\\\n")
        f.write("#\n")
        f.write(
            "# ★ 译文里可以写 `\\xNNNN` 表示引擎用不了的码元（U+2026 之类，见 tools/README.md 的兜底规整段）。\n"
        )
        f.write("# ★ 译文里**不要**留空行以外的空白；空译文 = 这条不翻，保留英文。\n")
        if prefill:
            f.write(
                "# ★ 本文件已用 LG 词典的 CHI 块**预填** ⇒ **自包含**，运行期不需要\n"
                "#   LGCStringDict_01.txt。改动译文列即可，无需再灌。\n"
            )
        f.write(
            "# ★ 共 %d 条。丢弃统计：重复 %d、控制字符 %d、非ASCII %d、空 %d、CHI不可解 %d\n"
            % (len(keys), dup, DROPS["控制字符"], DROPS["非ASCII"], DROPS["空"], DROPS["CHI不可解"])
        )
        f.write("#\n")
        # ★ 写盘前逐条校验行结构一致（译文换行数 == 原文块行数-1）。
        #   游戏按原文的结构排版：少一个换行 ⇒ 提示挤成一坨；多一个 ⇒ 排版被撑开。
        #   实测 LG 词典 469 条（占 31%）是多行译文。
        mismatch = []
        for k in keys:
            v = vals.get(k, "")
            if linecounts:
                want = linecounts.get(k, 0)
                got = v.count("\\n")
                if want and got + 1 != want:  # ★ 比的是**行数**：译文换行数+1
                    mismatch.append((k, want, got))
            f.write("%s\t%s\n" % (k, v))
        if mismatch:
            f.write("# ★★ 行结构不符 %d 条（译文换行数 != 原文）：\n" % len(mismatch))
            for k, want, got in mismatch[:5]:
                f.write("#    原文 %d 行 / 译文 %d 行：%s\n" % (want, got, k[:60]))
    return len(mismatch)


def find_lg_dict(given):
    """显式给的照用；未指定时只使用仓库约定的默认输入。"""
    if given:
        return given
    return DEF_LG if os.path.isfile(DEF_LG) else None


def parse_args():
    ap = argparse.ArgumentParser(description="导出翻译模板（英文键 + 空译文列）")
    ap.add_argument(
        "--lg-dict",
        "--dict",
        dest="dict_path",
        help="LG 主词典 LGCStringDict_01.txt（默认 tools\\input\\LGCStringDict_01.txt）",
    )
    ap.add_argument("--lang", default="ja", choices=sorted(LANGS))
    ap.add_argument("--out", default=None, help="默认 data/dict.<lang>.txt")
    ap.add_argument(
        "--prefill",
        action="store_true",
        help="★ 用 LG 词典的 **CHI 块预填译文列**。这样产出的词典"
        "**自包含**（不再依赖 LGCStringDict_01.txt）。"
        "只有中文该用：CHI 块本来就是这个游戏的中文译文。",
    )
    return ap.parse_args()


def main():
    args = parse_args()
    dpath = find_lg_dict(args.dict_path)
    if not dpath or not os.path.isfile(dpath):
        print("找不到主词典，用 --lg-dict 指定。", file=sys.stderr)
        return 2
    # 本工具只吃 LG 主词典；把 UTF-8 TSV 传进来必须立即报错。
    if not looks_like_lg_dict(dpath):
        print(
            "--lg-dict 读到的不像 LG 主词典（缺 xfhsm_res_ 成对块标记）：%s\n"
            "  那一份词典（data/dict.txt）不需要过本工具。" % dpath,
            file=sys.stderr,
        )
        return 2
    print("主词典：%s" % dpath)

    keys, vals, linecounts, dup, warnings = collect_rows(dpath, args.prefill)
    out = args.out or os.path.join(RELEASE_DIR, "data", "dict.%s.txt" % args.lang)
    n_mismatch = write_template(out, args.lang, args.prefill, keys, vals, linecounts, dup)

    for w in warnings:
        print("⚠ %s" % w, file=sys.stderr)
    pre = "，已预填译文 %d 条" % len(vals) if args.prefill else ""
    print("词典已写出：%s（%d 条%s）" % (out, len(keys), pre))
    print(
        "  丢弃统计：重复 %d、控制字符 %d、非ASCII %d、空 %d、CHI不可解 %d  ← 都在文件头里，别当没看见"
        % (dup, DROPS["控制字符"], DROPS["非ASCII"], DROPS["空"], DROPS["CHI不可解"])
    )
    print("  下一步：填译文列 → 部署成 scripts\\AC1_Dict.txt（打包时用 --dict %s）" % out)

    # 校验警告 ⇒ 退出 1：产物照写（仍有用），但不能让「成对块错位/行结构不符」以 0 收场
    if warnings or n_mismatch:
        if n_mismatch:
            print("⚠ 行结构不符 %d 条（文件尾有明细）" % n_mismatch, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
