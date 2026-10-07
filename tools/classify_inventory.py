#!/usr/bin/env python3
# tools/classify_inventory.py —— 把「字符串全记录」变成翻译决策的输入（离线，只用标准库）
#
# 它只分类、只建议，绝不自动写进词典；运行期仍然只做 canon + 精确相等（匹配器里没有兜底）。
#
# 输入（都可以用参数覆盖；默认按"仓库相对位置 + 候选搜索"，不硬编码任何绝对路径）：
#   · --inventory   strings_<run>.txt          字符串全记录（游戏 scripts\AC1_CJK\ 下，每 run 一份）
#   · --lg-dict     tools\input\LGCStringDict_01.txt  主词典（CP950 成对块，只读）
#   · --dict        data\dict.txt                  那一份词典（UTF-8；= 部署的 AC1_Dict.txt 的源，
#                                                  已覆盖/回填/写法差异按它判定；别名 --dict-file/--supplement）
#   · --notranslate data\dict_notranslate.txt      不译清单（UTF-8）
#
# ★ 缺输入不许静默降级（读成空表 = 整桶恒为空，而报告看着照样「完整」）：
#   · 参数点名给的路径读不到 ⇒ SystemExit（敲错了，别往下走）
#   · 仓库默认文件（data\dict.txt）若不在 —— 换了词典源 / 还没建 —— ⇒ 降级，
#     但控制台打 ⚠、报告头多一段「⚠ 输入缺失」
#
# 输出（默认 tools\out\）：
#   · AC1_CJK_Inventory_Report.md             人读报告：总览 + 分桶表 + 判据
#   · dict_additions.suggested.txt            可补条目（ENG<TAB>译文）——过目后合并进 data\dict.txt
#   · AC1_CJK_Inventory_NeedsTranslation.txt  需要人工翻译的串（每行一条）
#
# 关键：主词典索引逐字镜像 modules/dict/src/dict.cpp 的 load_main ——
#   一对 ENG/CHI 块 = 一个键（整块文本，canon 折叠换行）；运行期不做行内逐行配对，
#   所以本工具另建"行索引"，把"整块键不匹配、但等于块里某一行"的串单独列出来 ——
#   那是可补的条目（补一行数据），不是匹配器的新兜底。
#
# 分桶（判据都是离线数据分类，不是运行期兜底；运行期一个字都没改）：
#   已覆盖        状态 命中/替换 ⇒ 无事可做
#   回填核对      状态 未命中、词典里却已有 ⇒ 异常情形，值得看一眼
#   可补·行内匹配 等于主词典某块的一行（整块键不含它）⇒ 建议补一行
#   可补·写法差异 与词典键只差大小写 / 尾冒号 ⇒ 建议补一行（译文取自主词典原样）
#   不译·清单     data\dict_notranslate.txt 命中（_Yes/_No/N/lorem ipsum/OBSOLETE: *）
#   不译·图标     含 ≥0x80 码位且无 ASCII 字母（图标转义类；实例如 ¢ £ µ ƒ）
#   不译·数字符号 只有数字/符号/空白，没有 ASCII 字母
#   不译候选·碎片 是另一条更高频已记录串的严格前缀，且无词典变体（供过目，不自动判死）
#   需要人工翻译  其余

import argparse
import glob
import os
import re
import sys
import tempfile
import time
from dataclasses import dataclass

import lgdict
from lgdict import (
    DICT_KEY_MAX,
    canon,
    decode_chi,
    decode_eng,
    has_cjk,
    iter_paired_blocks,
    load_notranslate,
    load_supplement,
    trim_bytes,
)

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEF_DICT = os.path.join(REPO, "data", "dict.txt")
DEF_NTR = os.path.join(REPO, "data", "dict_notranslate.txt")
DEF_LG = os.path.join(REPO, "tools", "input", "LGCStringDict_01.txt")

RUN_HDR_RE = re.compile(r"^#\s*===\s*run\s+(.*?)\s*===\s*$")
ENTRY_RE = re.compile(
    r"^\[[×x](\d+)\]\s+(替换|命中|未命中·图标|未命中|未知)\s+caller=([0-9A-Fa-f]{8})\s?(.*)$"
)

STATUS_RANK = {"未知": 0, "未命中·图标": 1, "未命中": 2, "命中": 3, "替换": 4}

BUCKETS = [
    "已覆盖",
    "回填核对",
    "可补·行内匹配",
    "可补·写法差异",
    "不译·清单",
    "不译·图标",
    "不译·数字符号",
    "不译候选·碎片",
    "需要人工翻译",
]

BUCKET_NOTE = {
    "已覆盖": "词典命中/已替换 ⇒ 无事可做",
    "回填核对": "判了未命中、词典里却已有 ⇒ 异常，值得看一眼",
    "可补·行内匹配": "等于主词典块里的一行（逐行键对未实现）⇒ 建议补一行",
    "可补·写法差异": "与词典键只差大小写/尾冒号 ⇒ 建议补一行",
    "不译·清单": "`data/dict_notranslate.txt` 命中（用户裁定 ①+③）",
    "不译·图标": "含 ≥0x80 码位且无 ASCII 字母（图标转义类）",
    "不译·数字符号": "只有数字/符号/空白",
    "不译候选·碎片": "是另一条更高频串的严格前缀，且无词典变体（供过目）",
    "需要人工翻译": "见 `AC1_CJK_Inventory_NeedsTranslation.txt`",
}


# ---------------------------------------------------------------- 基础工具
# canon / hex4 / 转义 / CHI 与 ENG 解码 / trim_bytes 全在 lgdict（与 dict.cpp 同构的唯一实现）


def split_lines(raw):
    return raw.replace(b"\r\n", b"\n").replace(b"\r", b"\n").split(b"\n")


def say(msg):
    sys.stdout.write(msg + "\n")


# ---------------------------------------------------------------- 词典读取


def load_main_dict(path):
    """→ (block_index, line_index, 统计)。

    block_index：canon(整块 ENG 文本) → (译文, 块号) —— **运行期真索引**
    line_index： canon(块内某一行) → (该行译文, 块号, 行号, 整块键) —— 仅供"可补条目"建议（运行期未实现）

    成对块扫描/两列解码都在 lgdict（含 load_main 的窗口规则：CHI 只许在本块窗口内找，
    缺侧 ⇒ 本块记无译文；不跳块、不偷下一块）。
    """
    with open(path, "rb") as f:
        data = f.read()
    st = {
        "pairs": 0,
        "skip_empty": 0,
        "skip_cjk": 0,
        "skip_long": 0,
        "dup": 0,
        "line_pairs": 0,
        "line_blocks": 0,
    }
    block_index, line_index = {}, {}
    for bi, (eng_full, chi_full) in enumerate(iter_paired_blocks(data)):
        st["pairs"] += 1

        eng_raw = trim_bytes(eng_full)
        key = canon(decode_eng(eng_raw))
        if not key:
            st["skip_empty"] += 1
            continue
        if len(key) > DICT_KEY_MAX:
            st["skip_long"] += 1
            continue
        if has_cjk(key):
            st["skip_cjk"] += 1
            continue

        chi_raw = trim_bytes(chi_full) if chi_full is not None else b""
        val = lgdict.trim_value(lgdict.decode_chi(chi_raw))
        if not val:
            st["skip_empty"] += 1
            continue

        if key in block_index:
            st["dup"] += 1
        else:
            block_index[key] = (val, bi)

        el, cl = split_lines(eng_raw), split_lines(chi_raw)
        if len(el) == len(cl) and len(el) > 1:
            st["line_blocks"] += 1
            for li, (ek, cv) in enumerate(zip(el, cl)):
                k2 = canon(decode_eng(trim_bytes(ek)))
                v2 = lgdict.trim_value(lgdict.decode_chi(trim_bytes(cv)))
                if not k2 or not v2 or has_cjk(k2) or len(k2) > DICT_KEY_MAX:
                    continue
                if k2 in line_index:
                    continue
                line_index[k2] = (v2, bi, li, key)
                st["line_pairs"] += 1
    return block_index, line_index, st


# load_supplement / load_notranslate 在 lgdict（与 dict.cpp 同构的唯一实现）


def notranslate_hit(text, rules):
    for pat, why in rules:
        if pat.endswith("*"):
            if text.startswith(pat[:-1]):
                return pat, why
        elif text == pat or canon(text) == canon(pat):
            return pat, why
    return None


def make_variant_index(index):
    """键的"写法差异"索引：小写形态 + 去掉尾冒号的小写形态 → 键（有歧义的不进）。"""
    out = {}
    for k in index:
        for shape in (k.lower(), k.lower().rstrip(":")):
            if shape in out and out[shape] != k:
                out[shape] = None  # 歧义 ⇒ 作废
            else:
                out.setdefault(shape, k)
    return {s: k for s, k in out.items() if k}


def find_variants(text, vidx):
    """按"写法差异"找候选：→ [(词典键, 差异种类)]；差异种类 ∈ 大小写 / 尾冒号。"""
    hits, seen = [], set()
    for shape in (text.lower(), text.lower().rstrip(":")):
        k = vidx.get(shape)
        if not k or k in seen:
            continue
        seen.add(k)
        kind = "大小写" if text.lower() == k.lower() else "尾冒号"
        hits.append((k, kind))
    return hits


# ---------------------------------------------------------------- 全记录读取


def parse_inventory(path):
    """→ (runs, entries)。entries: 原文 → {count, status, caller}（跨 run 合并）。"""
    runs, entries = [], {}
    for line in open(path, encoding="utf-8", errors="replace").read().splitlines():
        if line.startswith("#"):
            m = RUN_HDR_RE.match(line)
            if m:
                runs.append(m.group(1))
            continue
        if not line.strip():
            continue
        m = ENTRY_RE.match(line)
        if not m:
            continue
        cnt, status, caller, text = m.group(1), m.group(2), m.group(3).upper(), m.group(4)
        e = entries.get(text)
        if e is None:
            entries[text] = {"count": int(cnt), "status": status, "caller": caller}
        else:
            e["count"] = max(e["count"], int(cnt))
            if STATUS_RANK[status] > STATUS_RANK[e["status"]]:
                e["status"] = status
    return runs, entries


# ---------------------------------------------------------------- 分类


def classify(entries, block_index, line_index, supp, rules):
    buckets = {b: [] for b in BUCKETS}
    vidx_block = make_variant_index(block_index)
    vidx_line = make_variant_index(line_index)
    vidx_supp = make_variant_index(supp)
    texts = sorted(entries.keys(), key=lambda t: (-entries[t]["count"], t))

    for t in texts:
        e = entries[t]
        key = canon(t)
        item = {
            "text": t,
            "count": e["count"],
            "caller": e["caller"],
            "status": e["status"],
            "note": "",
            "proposal": None,
        }

        parents = [
            t2
            for t2 in texts
            if t2 != t
            and len(t2) > len(t)
            and t2.startswith(t)
            and entries[t2]["count"] > e["count"]
        ]
        frag = ""
        if parents:
            p = max(parents, key=lambda x: entries[x]["count"])
            frag = "（也是 %s（×%d）的前缀）" % (repr(p), entries[p]["count"])

        if e["status"] in ("命中", "替换"):
            item["note"] = "词典命中" + ("，已写回译文" if e["status"] == "替换" else "（只观测）")
            buckets["已覆盖"].append(item)
            continue

        if key in supp:
            item["note"] = "用户词典已有该键（异常：为何仍判未命中？）" + frag
            buckets["回填核对"].append(item)
            continue
        if key in block_index:
            item["note"] = "主词典已有该键（异常：为何仍判未命中？）" + frag
            buckets["回填核对"].append(item)
            continue

        hit = notranslate_hit(t, rules)
        if hit:
            item["note"] = "不译清单：%s%s" % (hit[0], (" —— " + hit[1]) if hit[1] else "")
            buckets["不译·清单"].append(item)
            continue

        # 图标转义 = 极少码元的纯符号（¢ £ ƒ ² ³ µ …）。**含 ASCII 字母的说明它是文本**，
        # 不是图标 —— 本轮实测：西语残留串 `Habla con el jefe de los asesinos en Jerusalén.`
        # 若只按 "≥0x80" 判，会被误当图标（它有 é/ó 两个重音字母）。
        has_extended = any(ord(c) >= 0x80 for c in t)
        has_ascii_letter = any(ch.isascii() and ch.isalpha() for ch in t)
        if has_extended and not has_ascii_letter:
            item["note"] = "含 ≥0x80 码位、无 ASCII 字母（图标转义类），本就不该翻"
            buckets["不译·图标"].append(item)
            continue

        if not any(ch.isalpha() for ch in t):
            item["note"] = "只有数字/符号/空白"
            buckets["不译·数字符号"].append(item)
            continue

        # ① 行内匹配（整块键不含它，但等于块里某一行）
        if key in line_index:
            val, bi, li, bkey = line_index[key]
            item["note"] = "主词典块 %d 的第 %d 行（整块键『%s』不匹配 ⇒ 逐行键对未实现）%s" % (
                bi,
                li,
                bkey[:40],
                frag,
            )
            item["proposal"] = (t, val)
            buckets["可补·行内匹配"].append(item)
            continue

        # ② 写法差异（大小写 / 尾冒号）
        hits = find_variants(key, vidx_block)
        supp_hits = find_variants(key, vidx_supp)
        lhits = find_variants(key, vidx_line)
        if len(hits) == 1:
            k, kind = hits[0]
            val, bi = block_index[k]
            item["note"] = "主词典键『%s』块 %d（写法差异：%s）%s" % (k, bi, kind, frag)
            item["proposal"] = (t, val)
            buckets["可补·写法差异"].append(item)
            continue
        if len(supp_hits) == 1:
            k, kind = supp_hits[0]
            item["note"] = "用户词典键『%s』（写法差异：%s）%s" % (k, kind, frag)
            item["proposal"] = (t, supp[k])
            buckets["可补·写法差异"].append(item)
            continue
        if len(lhits) == 1:
            k, kind = lhits[0]
            val, bi, li, bkey = line_index[k]
            item["note"] = "主词典块 %d 第 %d 行『%s』（写法差异：%s）%s" % (bi, li, k, kind, frag)
            item["proposal"] = (t, val)
            buckets["可补·写法差异"].append(item)
            continue

        has_dictionary_variant = bool(hits or lhits or supp_hits)
        if parents and not has_dictionary_variant:
            item["note"] = "是 %s 的前缀（无对应词典键）" % repr(
                max(parents, key=lambda x: entries[x]["count"])
            )
            buckets["不译候选·碎片"].append(item)
            continue

        if len(hits) > 1 or len(lhits) > 1 or len(supp_hits) > 1:
            item["note"] = "多个候选（%s）⇒ 需人工择一" % "、".join(
                [k for k, _ in (hits + lhits + supp_hits)][:4]
            )
        buckets["需要人工翻译"].append(item)
    return buckets


# ---------------------------------------------------------------- 输出
@dataclass(frozen=True)
class ReportData:
    path: str
    source: str
    main_path: str
    supplement_path: str
    rules_path: str
    rules: list
    main_stats: dict
    block_count: int
    line_count: int
    runs: list
    entries: dict
    buckets: dict
    supplement_count: int
    missing: tuple = ()


def write_report(report):
    path = report.path
    L = []
    A = L.append
    A("# AC1 汉化 —— 字符串全记录分类报告\n")
    A("- 生成时间：%s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    A("- 全记录：`%s`" % report.source)
    A("- 主词典：`%s`" % report.main_path)
    A(
        "  - 成对块 %d ⇒ 整块键 %d（运行期真索引；跳过 空 %d / 含CJK %d / 过长 %d / 重复 %d）"
        % (
            report.main_stats["pairs"],
            report.block_count,
            report.main_stats["skip_empty"],
            report.main_stats["skip_cjk"],
            report.main_stats["skip_long"],
            report.main_stats["dup"],
        )
    )
    A(
        "  - 行索引（**仅供建议**，运行期未实现逐行键对）：多行块 %d ⇒ 行键 %d"
        % (report.main_stats["line_blocks"], report.line_count)
    )
    miss = {p for _, p in report.missing}
    A(
        "- 用户词典（AC1_Dict.txt 源）：`%s`（%d 键%s）"
        % (
            report.supplement_path,
            report.supplement_count,
            "　⚠ **文件不存在 ⇒ 这一路判据整桶为空**"
            if report.supplement_path in miss
            else "",
        )
    )
    A(
        "- 不译清单：`%s`（%d 条规则%s）"
        % (
            report.rules_path,
            len(report.rules),
            "　⚠ **文件不存在 ⇒ `不译·清单` 整桶为空**"
            if report.rules_path in miss
            else "",
        )
    )
    A("- run 头 %d 个：%s" % (len(report.runs), "；".join(report.runs) if report.runs else "（无）"))
    if report.missing:
        A("")
        A("## ⚠ 输入缺失 —— 本轮报告**不完整**\n")
        for label, p in report.missing:
            A("- **%s**：`%s` 读不到 ⇒ 依赖它的判据没跑，**对应桶恒为空**" % (label, p))
            A("  （空桶别读成「没有这类串」）；补上文件后重跑。")

    A("")
    A("## 总览\n")
    A("| 桶 | 条数 | 说明 |")
    A("|---|---|---|")
    for b in BUCKETS:
        A("| %s | %d | %s |" % (b, len(report.buckets[b]), BUCKET_NOTE[b]))
    A("")
    A("唯一串合计：**%d**" % len(report.entries))
    A("")

    for b in BUCKETS:
        items = sorted(report.buckets[b], key=lambda x: (-x["count"], x["text"]))
        if not items:
            continue
        A("## %s（%d）\n" % (b, len(items)))
        A("| × | 状态 | caller | 原文 | 判据 |")
        A("|---|---|---|---|---|")
        for it in items[:200]:
            esc = it["text"].replace("|", "\\|")
            note = it["note"].replace("|", "\\|")
            if it["proposal"]:
                note += " ⇒ 建议 `%s<TAB>%s`" % (it["text"], it["proposal"][1])
            A("| %d | %s | %s | `%s` | %s |" % (it["count"], it["status"], it["caller"], esc, note))
        if len(items) > 200:
            A("")
            A("（本桶只列前 200 条）")
        A("")

    A("## 下一步\n")
    A("1. 过目本报告 —— 特别是 `可补·*`（建议条目）与 `需要人工翻译`（缺口）。")
    A("2. 认可的建议行合并进 `data/dict.txt`（**加条目，不加兜底**）。")
    A("3. 部署：把 `data/dict.txt` 复制成 `<游戏>\\scripts\\AC1_Dict.txt`，")
    A("   重跑一轮，看 `未命中` 是否下降、`容量不足` 是否仍为 0。")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L) + "\n")


def write_suggested(path, buckets, src):
    L = [
        "# AC1 汉化 —— 分类器建议补进的词条（**请过目后再合并进 data/dict.txt**）",
        "# 来源：%s" % src,
        "# 生成：%s" % time.strftime("%Y-%m-%d %H:%M:%S"),
        "# 格式与那一份词典一致：ENG<TAB>译文；`#` 开头为注释。",
        "# 译文取自词典**原样**（主词典繁体 / 用户词典原样；繁→简转换未做）。",
        "",
    ]
    for b in ("可补·行内匹配", "可补·写法差异"):
        items = sorted(buckets[b], key=lambda x: (-x["count"], x["text"]))
        if not items:
            continue
        L.append("# ======== %s（%d 条） ========" % (b, len(items)))
        L.append("")
        for it in items:
            L.append(
                "# %s（×%d caller=%s）%s" % (it["text"], it["count"], it["caller"], it["note"])
            )
            L.append("%s\t%s" % it["proposal"])
            L.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L) + "\n")


def write_needs(path, buckets):
    L = [
        "# AC1 汉化 —— 需要人工翻译的串（每行一条；计数与 caller 见分类报告）",
        "# 来源：字符串全记录（未命中 · 非图标 · 不在不译清单 · 词典里没有对应写法）",
        "",
    ]
    for it in sorted(buckets["需要人工翻译"], key=lambda x: (-x["count"], x["text"])):
        L.append(it["text"])
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(L) + "\n")


# ---------------------------------------------------------------- 自检

SELFTEST_DICT = (
    b"xfhsm_res_ENG_Start\r\nSELECT\r\nxfhsm_res_ENG_End\r\n"
    b"xfhsm_res_CHI_Start\r\n{xuanze}\r\nxfhsm_res_CHI_End\r\n"
    b"xfhsm_res_ENG_Start\r\nBACK\r\nxfhsm_res_ENG_End\r\n"
    b"xfhsm_res_CHI_Start\r\n{fanhui}\r\nxfhsm_res_CHI_End\r\n"
    b"xfhsm_res_ENG_Start\r\nLINE ONE\r\nLINE TWO\r\nxfhsm_res_ENG_End\r\n"
    b"xfhsm_res_CHI_Start\r\n{xing1}\r\n{xing2}\r\nxfhsm_res_CHI_End\r\n"
)

SELFTEST_INV = (
    "# === run self-test ===\n"
    "[×9] 命中   caller=00000001 SELECT\n"
    "[×3] 未命中 caller=00000002 BACK\n"
    "[×5] 未命中 caller=00000003 LINE TWO\n"
    "[×7] 未命中 caller=00000004 Selective\n"
    "[×2] 未命中 caller=00000005 _Yes\n"
    "[×2] 未命中 caller=00000006 OBSOLETE: NO\n"
    "[×1] 未命中 caller=00000007 \u00b5\n"
    "[×40] 未命中 caller=00000008 LONGPARENT\n"
    "[×4] 未命中 caller=00000009 LONGP\n"
    "[×1] 未命中 caller=0000000A 12/34\n"
    "[×6] 未命中 caller=0000000B Something Else\n"
    "[×1] 未命中 caller=0000000C Caf\u00e9 break\n"
)


def self_test(tmpdir):
    bad = []
    # ⓪ 词典文本层判据的唯一实现在 lgdict，先过它的夹具自检
    if lgdict.self_check() != 0:
        bad.append("lgdict 自检失败（见上）")

    def w(name, text, mode="w"):
        p = os.path.join(tmpdir, name)
        with open(
            p,
            mode,
            encoding=None if "b" in mode else "utf-8",
            newline=None if "b" in mode else "\n",
        ) as f:
            f.write(text)
        return p

    d = w(
        "selftest_dict.txt",
        SELFTEST_DICT.replace(b"{xuanze}", "\u9078\u64c7".encode("cp950"))
        .replace(b"{fanhui}", "\u8fd4\u56de".encode("cp950"))
        .replace(b"{xing1}", "\u884c1".encode("cp950"))
        .replace(b"{xing2}", "\u884c2".encode("cp950")),
        "wb",
    )
    inv = w("selftest_inv.txt", SELFTEST_INV)
    sup = w("selftest_supp.txt", "SELECTIVE\t\u9078\u64c7\u6027\n")
    # ★ 键列解码（缺陷回归钉子）：键按 UTF-8 读，转义与译文共用 lgdict.unescape_value。
    #   若把键当逐字节 1:1，`ƒ`/`³` 这类字面 UTF-8 键会变成两个码元、与游戏送来的
    #   `\u0192` 对不上。
    sup_u8 = w(
        "selftest_supp_utf8.txt",
        "Move ƒ to walk on beams.\t在梁上行走\nPress \\x00A4 to interact\t交互\n",
    )
    ntr = w("selftest_ntr.txt", "_Yes\t内部键名\nOBSOLETE: *\t废弃项\n")

    block_index, line_index, st = load_main_dict(d)
    supp = load_supplement(sup)
    rules = load_notranslate(ntr)
    runs, entries = parse_inventory(inv)
    buckets = classify(entries, block_index, line_index, supp, rules)

    want = {
        "已覆盖": ["SELECT"],
        "回填核对": ["BACK"],
        "可补·行内匹配": ["LINE TWO"],
        "可补·写法差异": ["Selective"],
        "不译·清单": ["_Yes", "OBSOLETE: NO"],
        "不译·图标": ["\u00b5"],
        "不译·数字符号": ["12/34"],
        "不译候选·碎片": ["LONGP"],
        "需要人工翻译": ["LONGPARENT", "Something Else", "Café break"],
    }
    if len(block_index) != 3:
        bad.append("整块键应有 3 条（含多行块），实得 %d" % len(block_index))
    if len(line_index) != 2:
        bad.append("行索引应有 2 条，实得 %d" % len(line_index))
    for b, texts in want.items():
        got = sorted(x["text"] for x in buckets[b])
        if got != sorted(texts):
            bad.append("桶 %s 期望 %s，实得 %s" % (b, texts, got))
    prop = dict(x["proposal"] for b in ("可补·行内匹配", "可补·写法差异") for x in buckets[b])
    if prop.get("LINE TWO") != "\u884c2":
        bad.append("行内匹配译文不对：%r" % prop.get("LINE TWO"))
    if prop.get("Selective") != "\u9078\u64c7\u6027":
        bad.append("写法差异译文不对：%r" % prop.get("Selective"))
    su = load_supplement(sup_u8)
    for k, v in (("Move ƒ to walk on beams.", "在梁上行走"), ("Press ¤ to interact", "交互")):
        if su.get(k) != v:
            bad.append("用户词典键解码不对：%r ⇒ %r（期望 %r）" % (k, su.get(k), v))
    for x in bad:
        say("[自检] 失败：" + x)
    if not bad:
        say(
            "[自检] 通过：9 个桶判定 + 整块键/行索引计数 + 两处译文取值"
            " + 键列 UTF-8/转义解码 全部符合预期"
        )
    return 1 if bad else 0


# ---------------------------------------------------------------- 入口


def first_existing(cands):
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return None


def resolve_input(label, path, given, flag, missing):
    """把一个输入路径归一化成 (路径, 是否缺失)。

    ★ 缺文件**不许静默降级**（缺失依赖会让整桶恒为空，
      而报告看着照样完整 —— 读者看不出这一路判据根本没跑，把空桶读成「没有这类串」）。
      · 参数**点名**给的路径读不到 = 敲错了 ⇒ raise SystemExit（同 pack_glyphs.py 的约定）
      · **仓库默认**文件不在（如 data\\dict.txt 换了词典源）⇒ 允许降级，
        但先 say() 一句、并 append 进 missing，最后由 write_report 写进报告头。
    """
    if os.path.isfile(path):
        return path, False
    if given:
        raise SystemExit(
            "找不到%s：%s（%s 指定的路径读不到；分类依赖它，缺了整桶会是空的）"
            % (label, path, flag)
        )
    say(
        "⚠ 找不到%s：%s —— 仓库里还没这个文件，本轮依赖它的判据**没跑**"
        "（对应桶恒为空，不是「没有这类串」）；已写进报告的「⚠ 输入缺失」段。" % (label, path)
    )
    missing.append((label, path))
    return path, True


def main():
    ap = argparse.ArgumentParser(description="字符串全记录 → 翻译决策输入（只分类、只建议）")
    ap.add_argument("--inventory", help="strings_<run>.txt（AC1_CJK\\ 目录下；缺省自动找最新一份）")
    ap.add_argument(
        "--game",
        help="游戏根目录（用它找 scripts\\AC1_CJK\\strings_*.txt 与 LG_Data\\LGCStringDict_01.txt）",
    )
    ap.add_argument(
        "--lg-dict",
        dest="dict_path",
        help="LG 主词典 LGCStringDict_01.txt（默认 tools\\input\\LGCStringDict_01.txt；"
        "给了 --game 时也会在游戏目录搜）",
    )
    ap.add_argument(
        "--dict",
        "--dict-file",
        "--supplement",
        dest="supplement",
        default=DEF_DICT,
        help="★ 那一份词典（默认 data\\dict.txt = 部署的 AC1_Dict.txt 的源；"
        "已覆盖/回填/写法差异按它判定；缺失会显式警告并写进报告头）",
    )
    ap.add_argument(
        "--notranslate", default=DEF_NTR, help="不译清单（默认 data\\dict_notranslate.txt）"
    )
    ap.add_argument("--out", default=os.path.join(REPO, "tools", "out"))
    ap.add_argument(
        "--self-check",
        "--self-test",
        dest="self_check",
        action="store_true",
        help="用合成词典/全记录跑一遍分类并核对桶计数",
    )
    args = ap.parse_args()

    if args.self_check:
        with tempfile.TemporaryDirectory() as td:
            return self_test(td)

    g = args.game

    def latest_strings(*dirs):
        # strings_<run>.txt 每 run 一份：取 mtime 最新的那份（排序即时间序）
        best = None
        for d in dirs:
            if not d:
                continue
            for p in glob.glob(os.path.join(d, "AC1_CJK", "strings_*.txt")):
                if best is None or os.path.getmtime(p) > os.path.getmtime(best):
                    best = p
        return best

    inv = args.inventory or latest_strings(
        os.path.join(REPO, "..", "scripts"),
        os.path.join(g, "scripts") if g else None,
    )
    dct = args.dict_path or first_existing(
        [
            DEF_LG,
            os.path.join(REPO, "..", "LG_Data", "LGCStringDict_01.txt"),
            os.path.join(REPO, "..", "scripts", "LGCStringDict_01.txt"),
            os.path.join(g, "LG_Data", "LGCStringDict_01.txt") if g else None,
            os.path.join(g, "scripts", "LGCStringDict_01.txt") if g else None,
        ]
    )
    if not inv or not os.path.isfile(inv):
        say(
            "找不到字符串全记录（scripts\\AC1_CJK\\strings_*.txt）。用 --inventory 指定，或 --game 指游戏根目录。"
        )
        return 2
    if not dct or not os.path.isfile(dct):
        say("找不到主词典（LGCStringDict_01.txt）。用 --lg-dict 指定，或 --game 指游戏根目录。")
        return 2
    # 两份词典传反是静默灾难 ⇒ 启动嗅探一次（--dict 现在是那一份词典、--lg-dict 是主词典）
    if os.path.isfile(args.supplement) and lgdict.looks_like_lg_dict(args.supplement):
        say(
            "--dict 指到 LG 主词典了：%s\n  那一份词典是 UTF-8 的 ENG<TAB>译文；主词典请用 --lg-dict。"
            % args.supplement
        )
        return 2
    if not lgdict.looks_like_lg_dict(dct):
        say(
            "--lg-dict 读到的不像 LG 主词典（缺 xfhsm_res_ 成对块标记）：%s\n"
            "  那一份词典请用 --dict。" % dct
        )
        return 2

    #    ★ 默认输入也必须先过 resolve_input：缺失文件时应明确记录降级状态 ⇒
    #      `回填核对` 与用户词典来源的 `可补·写法差异` 恒为空，而报告看着照样「完整」。
    missing = []
    supp_path, supp_absent = resolve_input(
        "用户词典", args.supplement, args.supplement != DEF_DICT, "--dict", missing
    )
    ntr_path, ntr_absent = resolve_input(
        "不译清单", args.notranslate, args.notranslate != DEF_NTR, "--notranslate", missing
    )
    block_index, line_index, main_st = load_main_dict(dct)
    supp_map = load_supplement(supp_path, optional=supp_absent)
    rules = load_notranslate(ntr_path, optional=ntr_absent)
    runs, entries = parse_inventory(inv)
    buckets = classify(entries, block_index, line_index, supp_map, rules)

    os.makedirs(args.out, exist_ok=True)
    rp = os.path.join(args.out, "AC1_CJK_Inventory_Report.md")
    sp = os.path.join(args.out, "dict_additions.suggested.txt")
    np_ = os.path.join(args.out, "AC1_CJK_Inventory_NeedsTranslation.txt")
    write_report(
        ReportData(
            path=rp,
            source=inv,
            main_path=dct,
            supplement_path=supp_path,
            rules_path=ntr_path,
            rules=rules,
            main_stats=main_st,
            block_count=len(block_index),
            line_count=len(line_index),
            runs=runs,
            entries=entries,
            buckets=buckets,
            supplement_count=len(supp_map),
            missing=tuple(missing),
        )
    )
    write_suggested(sp, buckets, inv)
    write_needs(np_, buckets)

    n_prop = len(buckets["可补·行内匹配"]) + len(buckets["可补·写法差异"])
    say("全记录：%s（run 头 %d）" % (inv, len(runs)))
    say(
        "主词典：成对块 %d ⇒ 整块键 %d（行索引 %d，仅供建议）"
        % (main_st["pairs"], len(block_index), len(line_index))
    )
    say(
        "用户词典：%d 键%s；不译清单：%d 条规则%s；唯一串：%d"
        % (
            len(supp_map),
            "（⚠ 文件缺失）" if supp_absent else "",
            len(rules),
            "（⚠ 文件缺失）" if ntr_absent else "",
            len(entries),
        )
    )
    for b in BUCKETS:
        say("  %-14s %d" % (b, len(buckets[b])))
    say("报告：%s" % rp)
    say("建议词条：%s（%d 条，**未合并**）" % (sp, n_prop))
    say("待译清单：%s（%d 条）" % (np_, len(buckets["需要人工翻译"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
