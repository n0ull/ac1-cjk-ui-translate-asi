#!/usr/bin/env python3
# tools/pack_glyphs.py —— 离线图集打包器：「字体原始数据 + 原图集 + 可变字重字体」→ 两份产物
#
# 目的：给四套游戏字体各烘一张 CJK 图集 + 一份 ACGG 字形清单。只写 --out 指的目录
#   （默认 tools\out\glyphs\）；不碰游戏目录、不碰其它模块、不部署、不改源码树。
#
# 产物 A：AC1_CJK_Glyphs.bin（ACGG 字形集清单）—— 给运行期 modules/glyph 的
#   load_manifest 读。格式的唯一权威是 modules\glyph\include\ac1\glyph\glyph.h
#   （头 12B / 每套头 40B / 记录 0x20B）；本脚本逐字节按它写。
#   产物 B：atlas_<name>.rgba（新图集，经 tools\rgba_to_dds.py 转 DDS 后由
#   AnvilToolkit 打进 DataPC.forge）。头 ACGX + u32 W + u32 H + u32 0，
#   其后是整幅 RGBA，顶行在前。
#
# 输入（默认值都归 tools\input\，见下表）：
#   mft-dir\*.MagmaMftFile   四套原字体字形记录（度量/UV/指纹的来源）
#   dds-dir\*_Map.dds        四张原图集（1:1 贴进新图集左下角）
#   LGCStringDict_01.txt     LG 主词典（仅 --lang zh 读，繁中语料）
#   data\dict.txt            那一份词典（UTF-8，ENG<TAB>译文）——语料的另一半
#   NotoSansSC-VariableFont_wght.ttf  可变字重字体（fvar/wght），按套实例化字重
#   tms\*.TextureMapSpec     （可选）原纹理描述符，独立对账 ow/oh
#
# 参数（--help 有全表）：
#   --mft-dir/--dds-dir/--lg-dict/--dict/--ttf/--tms-dir/--out/--only/--lang
#   --self-check  内置小夹具自检（不需要真数据）
#   --verify      回读刚生成的清单并对账
#
# 退出码：0 成功；1 校验/对账失败；2 用法或输入错误。
#
# 事实来源：度量/布局/UV/指纹公式来自逆向；本脚本按那些结论落地，别在别处"再推导"一遍。
# 依赖：Python 3 + Pillow（读 DDS、烘字）。整个仓库里只有本离线工具链需要 Pillow，
#   modules\* 与 app\ 是 C++/Win32。--self-check 不读真数据（也不烘字）。
#
# 用法：
#   python tools\pack_glyphs.py                 # 真数据全跑（四套，默认值全在 input\）
#   python tools\pack_glyphs.py --self-check    # 内置小夹具自检
#   python tools\pack_glyphs.py --verify        # 回读刚生成的清单并对账
#   python tools\pack_glyphs.py --only textpc,bold

import argparse
import os
import struct
import sys
import tempfile
from dataclasses import dataclass

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # 明确报错，别让人以为是数据问题
    sys.stderr.write("!! 需要 Pillow（只有离线工具链需要）：pip install Pillow\n")
    raise

import lgdict  # 词典文本层：与 dict.cpp 对齐的解码/规整（唯一实现）

sys.stdout.reconfigure(encoding="utf-8")

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
RELEASE_DIR = os.path.dirname(TOOLS_DIR)  # 仓库根（git toplevel）
INPUT_DIR = os.path.join(TOOLS_DIR, "input")

# ====================================================================== 全局布局常量
# 四套统一 1024 宽 —— 依据：图集容量核算与字体图集布局笔记。
W, H = 1024, 2048
# CJK 栅格起始 y：引擎换关卡时可能把它自己那份 256×512 / 512×1024 DXT3 回写进我们纹理的顶部
# （512×1024 DXT3 = 524288 B ÷ 我们 1024 宽的 pitch 4096 = 顶部 128 行）⇒ 顶部 128 行不放格子，
# 留 32 行余量 ⇒ Y0=160。依据：字体图集布局笔记中的顶部保留区约定。
Y0 = 160

# 游戏把这两个码位当「按钮图标」用（实机取证：按钮提示串 = 按+钮+数字；
# 日志「命中码位 Top：U+6309 ×8012」「漏字 Top：U+94AE ×8012」）。
# 它们不在词典译文里，但**必须映射**：不映射 ⇒ 引擎回退到字形 0（育碧占位图标），
# 补丁后就会变成采样错图集的乱像素（上次那个竖线就是这么来的）。
# 已核：这两个码位**不在**四套原字体的自有码位里（并集 261 个，零冲突）。
GAME_ICON_CPS = (0x6309, 0x94AE)  # 按 / 钮


# ====================================================================== 四套字体参数表
# 每条字段出处：dds/mft/尺寸/字形数/期望指纹 ⇒ 笔记 §2 的度量表 + §4 的指纹公式；
#               cell/step/px ⇒ 笔记 §2 末段（"四套共用同一 TTF 与同一 px，仅 title 降一档"）。
# ⚠ 期望指纹是**从真机日志抄下来的常量**，脚本会把 MFT 直算的结果与它硬比对（差 >1e-6 即报错退出）。
#   这条断言同时验证了两件事：MFT 解析没错、指纹公式没错。
# 每套 em（= 2× 拉丁 adv 实测，来自实机 [度量] 日志）——
#   techno 2×15=30  textpc 2×15=30  bold 2×21=42  title 2×42=84
# 打包时按**容量**自动收缩到装得下的最大格子（见 fit_cell），目标不是"能用就行"。
#
# weight = 可变字重字体（--ttf 的 fvar/wght）里的命名实例，一一对上原字体名里的字重：
#   AnimusTitle **Thin** 74 / AnimusText **Bold**_PC / AnimusText PC（= Regular）/
#   AnimusTechno **Regular** 22_PC。字重只改墨形粗细，不改任何度量（em/格子/写规）。
@dataclass(frozen=True)
class FontSet:
    name: str
    weight: str  # VF 命名实例：Thin / Bold / Regular
    em: int
    dds: str
    ow: int
    oh: int
    ambiguous: int
    mft: str
    n_glyph: int
    exp_du: float
    exp_dv: float
    cell: int
    step: int
    px: int


FONT_SETS = [
    FontSet(
        name="title",
        weight="Thin",
        em=84,
        dds="737_-_AnimusTitle Thin 74 tga32_Map.dds",
        ow=512,
        oh=1024,
        ambiguous=0,
        mft="267_-_Animus Title.MagmaMftFile",
        n_glyph=261,
        exp_du=0.107421,
        exp_dv=0.057617,
        # ★ em=84 装不下（见容量测算），封顶 40；尺寸问题记为已知限制
        #   （1024×2048 只有 ≈558 格 << 2465，放不下 46px 的格）
        cell=40,
        step=42,
        px=38,
    ),
    # ★★ em 取该字体自己的 adv 上界（拉丁 '@' 的 adv），不取「2× 拉丁 adv 中位」。
    #   实机证据：em=30 让 CJK 渲染成英文的 2.02 倍（43px → 86px），
    #   而行距、分隔线 y 完全一致 ⇒ 布局没变，是字被放大了；
    #   放大量恰好 = (h比 23/18) × (adv比 30/19) = 2.017 ⇒ 引擎按记录里的 adv 定渲染缩放。
    #   「全角 = 半角两倍」的惯例在这里不成立：这个字体的 em 只有 19，按 2× 中位去烘
    #   等于把字画在了两倍字号上。
    FontSet(
        name="textpc",
        weight="Regular",
        em=23,
        dds="738_-_AnimusText PC tga32_Map.dds",
        # ★ textpc 的图集在 forge 路线下已确认可换（256×256）。
        #   若 UV 仍按 0..1 跨整张图集采样 ⇒ 满屏乱码。
        ow=256,
        oh=256,
        ambiguous=0,
        mft="268_-_AnimusText PC.MagmaMftFile",
        n_glyph=199,
        exp_du=0.070312,
        exp_dv=0.074218,
        cell=23,
        step=25,
        px=19,
    ),  # em = 1.2×拉丁'@'的adv(19) ⇒ CJK/拉丁 高≈0.99
    # ★ 同 textpc：em 取该字体自己的 adv 上界。bold 的 '@' adv=24（度量日志 套#2）；
    #   em 取大了一烘就会等比放大（格子 = em×step）。
    # ★★ 主菜单用的就是这套 —— DrawText 日志 font=0FB86DC0，套#2。
    FontSet(
        name="bold",
        weight="Bold",
        em=29,
        dds="736_-_AnimusText Bold_PC tga32_Map.dds",
        ow=256,
        oh=512,
        ambiguous=0,
        mft="266_-_AnimusText Bold_PC.MagmaMftFile",
        n_glyph=199,
        exp_du=0.089843,
        exp_dv=0.046875,
        cell=29,
        step=31,
        px=24,
    ),  # em = 1.2×拉丁'@'的adv(24)
    FontSet(
        name="techno",
        weight="Regular",
        em=23,
        dds="735_-_AnimusTechno Regular 22_PC tga32_Map.dds",
        ow=64,
        oh=1024,
        ambiguous=0,
        mft="265_-_AnimusTechno Regular 22_PC.MagmaMftFile",
        n_glyph=198,
        exp_du=0.281250,
        exp_dv=0.018554,
        cell=23,
        step=25,
        px=19,
    ),  # em = 1.2×拉丁'@'的adv(19)；原 30 偏大 1.32×
]
SET_BY_NAME = {s.name: s for s in FONT_SETS}

# MFT 二进制常量（依据：MFT 解析笔记与现有资源实测）
MFT_MARK = b"PixmapFont"
MFT_CNT_OFF = 0x0A  # 条数 = 「PixmapFont」标记 +0x0A 处的 u16
MFT_REC_OFF = 0x18  # 记录区 = 标记 +0x18
MFT_REC = 30  # 每条 30 字节
MFT_CP_OFF = 18  # 码位 = 记录 +18 处 u16
# 度量在记录里的字节偏移（30B = 16B UV + 14B 尾）。这四个位置是**实测钉死**的：
#   旁证 ①：每套的 '@'（cp=0x40）记录里 +18 恰好是 0x40；
#   旁证 ②：用它们算出的指纹与笔记 §2 的真机日志值逐位一致（见脚本里的硬断言）。
#   布局：[+16]=0 保留 | [+18]=cp | [+20]=w | [+22]=h | [+24]=xoff | [+26]=yoff | [+28]=adv
MFT_W_OFF, MFT_H_OFF = 20, 22

# ====================================================================== 字集判据
# 中文标点白名单：CJK 标点区 + 小型标点区 + 全角区（排除全角数字/字母/假名半角那几段）
#   + 常用中西共用标点。出处：f6_atlas.py 的 CN_PUNCT。
CN_PUNCT = set(
    "…—～·‥‘’“”〈〉《》「」『』【】〔〕〖〗〇、。〃・！！＂＃％＆＇（）＊＋，－．／：；＜＝＞？＠［＼］＾＿｀｛｜｝～　"
)

# ====================================================================== 语言档
# ★ 引擎侧与语言**无关**（反汇编已定案）：
#   charmap 按 `C>>8` 分页、页由 `0x896600` **按需分配**、容量 256×256 = 整个 BMP。
#   所以「加一门语言」= 改这张表 + 换语料 + 换字体 VF，**引擎侧一行都不用动**。
#
# ★ 硬墙在 **U+FFFF**（超 BMP 打不出来）：emoji `U+1F600+`、CJK 扩展 B+ `U+20000+`。
#   日文假名（0x3040-0x30FF）与谚文音节（0xAC00-0xD7A3）**都在 BMP 内** ⇒ 不受影响。
#
# ★ `zh` 的取值**逐字照抄**原实现（不是「重新整理过的等价物」）：默认构建必须
#   与已实机验证的那一份**字节一致**。加语言时别动 zh 的任何一项。
LANGS = {
    "zh": dict(
        label="简体中文",
        # 行为探针：引擎会查 charmap 页 0x4E（0x4E2D 的高字节），所以「中」必须
        # 在集合里、还得排最前，否则探针字排到 256 之外就找不到。
        test_chars="中下测试汉字",
        ideo=[(0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0xF900, 0xFAFF)],  # 扩展A + 基本 + 兼容
        kana=[],
        hangul=[],
        punct_extra="",
        # 可变字重字体（fvar/wght）候选；找到第一个存在的就用。字重映射见 FONT_SETS.weight。
        ttf_candidates=[os.path.join(INPUT_DIR, "NotoSansSC-VariableFont_wght.ttf")],
        ttf_note="Noto Sans SC（可变字重 VF：Thin/Regular/Bold 按套实例化）",
    ),
    "ja": dict(
        label="日本語",
        test_chars="あ中下日語",
        ideo=[(0x3400, 0x4DBF), (0x4E00, 0x9FFF), (0xF900, 0xFAFF)],  # 汉字与中文同表
        kana=[
            (0x3040, 0x30FF),  # 平假名 + 片假名 + ゝゞヽヾー
            (0x31F0, 0x31FF),  #  音标扩展
            (0xFF66, 0xFF9F),
        ],  # 半角片假名
        hangul=[],
        punct_extra="々〆ヶ",  # 迭代记号 / 省略记号 / 小写ヶ
        ttf_candidates=[os.path.join(INPUT_DIR, "NotoSansJP-VariableFont_wght.ttf")],
        ttf_note="Noto Sans JP 可变字重 VF（自备；放到 input/ 或用 --ttf 指定）",
    ),
    "ko": dict(
        label="한국어",
        test_chars="한글대국",
        ideo=[],  # 韩文一般不用汉字
        kana=[],
        hangul=[
            (0xAC00, 0xD7A3),  # 谚文音节（预组合）
            (0x1100, 0x11FF),  # 谚文字母
            (0x3130, 0x318F),  # 兼容字母
            (0xA960, 0xA97F),
            (0xD7B0, 0xD7FF),
        ],  # 扩展-A/B
        punct_extra="",
        ttf_candidates=[os.path.join(INPUT_DIR, "NotoSansKR-VariableFont_wght.ttf")],
        ttf_note="Noto Sans KR 可变字重 VF（自备；放到 input/ 或用 --ttf 指定）",
    ),
}


def lang_spec(code):
    if code not in LANGS:
        raise SystemExit("未知语言 %r（可选：%s）" % (code, "/".join(LANGS)))
    return LANGS[code]


# ====================================================================== ACGG / ACGX 字节常量
# 全部出自 modules\glyph\include\ac1\glyph\glyph.h
ACGG_MAGIC = b"ACGG"  # = 字节 41 43 47 47 = glyph.h 的 GLYPH_SET_MAGIC 0x47474341（小端 u32）
ACGG_VERSION = 1  # == GLYPH_SET_VERSION
SET_HDR = "<IIffIIIIII"  # 40B: count | fp_glyph_count | fp_u_span | fp_v_span |
#       orig_w | orig_h | flags(保留=0) | our_w | our_h | reserved
#   ★ orig_w/orig_h 是该套原图集的尺寸。字形模块打补丁时用它把
#   原字形记录的 UV 乘以 (orig_w/our_w, orig_h/our_h) 缩到我们图集的原字形区，
#   否则原字形（英文/数字/符号）的 UV 仍是 0..1，会横跨整张图集采样到
#   CJK 格子 ⇒ 满屏乱码。
REC_FMT = "<3H3h4f2H"  # 32B(0x20): cp,w,h(都是 **u16**，汉字可到 U+9FFF=40959 > i16 上限) | xoff,yoff,adv(i16) | u0,v0,u1,v1 | page,pad
ACGX_MAGIC = b"ACGX"
ACGX_HDR = "<4sIII"  # 16B: magic | W | H | 0
FP_TOL = 1e-6  # 指纹硬断言容差（= 真机日志 %.6f 的刻度）


# 一条字形记录（glyph.h 的 0x20B 记录；pad 由 write_manifest 写 0，不入字段）。
@dataclass(frozen=True)
class GlyphRec:
    cp: int
    w: int
    h: int
    xoff: int
    yoff: int
    adv: int
    u0: float
    v0: float
    u1: float
    v1: float
    page: int


# 一套的 40B 套头。count/reserved 不作为契约字段：落盘时 count=len(recs)、reserved=0。
@dataclass(frozen=True)
class SetHeader:
    fp_glyph_count: int
    du: float
    dv: float
    ow: int
    oh: int
    flags: int
    nw: int
    nh: int


# 一套的完整打包单元：套头 + 记录。
@dataclass(frozen=True)
class PackSet:
    header: SetHeader
    recs: tuple


# ====================================================================== 小工具
def say(msg=""):
    sys.stdout.write(msg + "\n")


def f32(v):
    """把 Python 双精度舍入成 IEEE-754 单精度再取回来。

    ⚠ glyph.h 明确要求：fp_u_span / fp_v_span 必须是**算出来的 f32 结果**，不能写十进制字面量。
      本项目的指纹来源是 (w+1)/W，而 W 全是 2 的幂（64/256/512/1024）⇒ 该商在二进制里
      **可精确表示** ⇒ 这个 f32 转换是无损的，写出去的值与运行期读到的 f32 逐位相同。
      这正是 glyph.h 那条 ⚠ 想要的效果（而不是"碰巧同一个数"）。
    """
    return struct.unpack("<f", struct.pack("<f", v))[0]


# ---------------------------------------------------------------- MFT 解析
def mft_parse(path):
    """解析一个 .MagmaMftFile（PixmapFont 容器）→ dict。

    正确解析（笔记 §5 / _f81_mft_count.py）：条数取「PixmapFont」标记 +0x0A 的 u16，
    记录区 = 标记 +0x18、每条 30B、码位在 +18。
    ⚠ **不要**使用固定记录上限或 UV 越界早停：它们会把尾部有效记录误判为结束。
      （越过记录区把零填充和后面的索引池当记录），读出 5 个假码位 U+0000/0F00/1D00/2C00/3B00。
    """
    with open(path, "rb") as f:
        m = f.read()
    found = m.find(MFT_MARK)
    if found < 0:
        raise SystemExit("MFT 里找不到 PixmapFont 标记：%s" % path)
    cnt = struct.unpack_from("<H", m, found + MFT_CNT_OFF)[0]
    base = found + MFT_REC_OFF
    if base + cnt * MFT_REC > len(m):
        raise SystemExit("MFT 记录区越界（头里的条数不可信）：%s cnt=%d" % (path, cnt))
    recs = []
    for j in range(cnt):
        o = base + j * MFT_REC
        cp = struct.unpack_from("<H", m, o + MFT_CP_OFF)[0]
        w = struct.unpack_from("<H", m, o + MFT_W_OFF)[0]
        h = struct.unpack_from("<H", m, o + MFT_H_OFF)[0]
        recs.append(dict(cp=cp, w=w, h=h))
    return dict(path=path, n_recs=cnt, recs=recs, cps=[r["cp"] for r in recs])


def mft_at(mft):
    """取 '@'（0x40）那条记录 —— 指纹的来源。找不到抛错（任何拉丁字体都该有 '@'）。"""
    for r in mft["recs"]:
        if r["cp"] == 0x40:
            return r
    raise SystemExit("MFT 里没有 '@'（U+0040）记录，指纹无从算起：%s" % mft["path"])


def fingerprint(mft, ow, oh):
    """指纹：du = (met('@').w + 1) / W_原图集，dv = (met('@').h + 1) / H_原图集。

    出处：笔记 §4「★ 指纹可离线算」—— 四套全部精确命中 18/256、19/256、23/256、24/512、
    55/512、59/1024、18/64、19/1024。也就是说**格子纹素尺寸 = (w+1)×(h+1)**。
    返回值已是 f32（见 f32() 的说明）。
    """
    a = mft_at(mft)
    return f32((a["w"] + 1) / float(ow)), f32((a["h"] + 1) / float(oh))


# ---------------------------------------------------------------- 词典语料
def corpus_from_main_dict(path):
    r"""主词典的**全部 CHI 段**的译文里出现过的码位（CP950 + \xNNNN 展开）。

    ⚠ 整个文件不能一次按 Big5 解码，否则会把 "\\xNNNN" 里的
      反斜杠和 x 当成字符收进字集。这里按块走 + 展开转义，与 modules\dict 的读法一致。
    """
    with open(path, "rb") as f:
        data = f.read()
    cps = set()
    blocks = 0
    for raw in lgdict.iter_blocks(data, lgdict.S_CHI, lgdict.E_CHI):
        cps |= set(ord(c) for c in lgdict.decode_chi(lgdict.trim_bytes(raw)))
        blocks += 1
    return cps, blocks


def corpus_from_supplement(path):
    """补充词典译文里的码位（UTF-8，`ENG<TAB>译文`，值里可写 \\xNNNN）。"""
    cps, n = set(), 0
    if not path or not os.path.isfile(path):
        say("找不到那一份词典：%s（用 --dict/--supplement 指定）" % (path or "（路径为空）"))
        raise SystemExit(2)
    with open(path, encoding="utf-8-sig", errors="replace") as f:
        lines = f.read().splitlines()
    for line in lines:
        s = line.strip(" \t")
        # 判据与运行期一致（dict.cpp 的 load_supplement）：**含 TAB 的行就是数据** ——
        #   游戏 UI 里有一批 `#` 开头的真实串（按键提示，实测 106 条），它们必须进语料。
        if not s or "\t" not in s:
            continue
        val = s.split("\t", 1)[1]
        cps |= set(ord(c) for c in lgdict.expand_hex_escapes(val))
        n += 1
    return cps, n


# ---------------------------------------------------------------- 码位筛选
def is_cjk(cp, language=None):
    """表意文字（汉字）。`language` 为 None 时按 **zh** 处理 —— 保持旧调用点行为不变。"""
    if language is None:
        language = LANGS["zh"]
    return any(a <= cp <= b for a, b in language["ideo"])


def is_kana(cp, language=None):
    if language is None:
        language = LANGS["zh"]
    return any(a <= cp <= b for a, b in language["kana"])


def is_hangul(cp, language=None):
    if language is None:
        language = LANGS["zh"]
    return any(a <= cp <= b for a, b in language["hangul"])


def is_cn_punct(cp, language=None):
    if language is None:
        language = LANGS["zh"]
    if cp in CN_PUNCT or cp in set(language["punct_extra"]):
        return True
    if 0x3000 <= cp <= 0x303F:  # CJK 标点
        return True
    if 0xFE30 <= cp <= 0xFE4F:  # 小型形式
        return True
    if 0xFF00 <= cp <= 0xFFEF:  # 全角：排掉数字/字母/半角假名那几段
        return not (
            0xFF10 <= cp <= 0xFF19
            or 0xFF21 <= cp <= 0xFF3A
            or 0xFF41 <= cp <= 0xFF5A
            or 0xFF66 <= cp <= 0xFF9F
        )
    return False


# ★ 一门语言里「值得烘字形」的码位 = 表意文字 ∪ 该语言的假名/谚文 ∪ 标点。
def wanted(cp, language=None):
    return (
        is_cjk(cp, language)
        or is_kana(cp, language)
        or is_hangul(cp, language)
        or is_cn_punct(cp, language)
    )


def build_codepoints(corpus, owned, language=None):
    """码位集 = （语料 ∩ 本语言该有的字）− 四套原字体自有码位，最后把探针字置顶。

    「自有码位」必须扣掉：接管时我们要写 charmap 页槽，写进去就会把原字形**顶掉**
    （旧 F7 实机：育碧的占位图标变成 ①/‰/空白）。出处：笔记 §5「码位集」。
    `language` 为 None 时按 **zh** 处理 —— 默认构建的字节输出必须与改造前完全一致。
    返回 (有序码位列表, 统计)。
    """
    if language is None:
        language = LANGS["zh"]
    test_chars = language["test_chars"]
    han_punct = {cp for cp in corpus if wanted(cp, language)}
    dropped = {cp for cp in han_punct if cp in owned}
    kept = han_punct - owned
    # 游戏图标码位（按/钮）：不在语料里，但必须映射（见 GAME_ICON_CPS 注释）
    kept |= set(GAME_ICON_CPS)
    head = [ord(c) for c in test_chars if ord(c) not in kept]
    # 探针字可能被黑名单扣掉（理论上不会：它们是常用字，原字体里没有）——
    # 那就**强行放回**，因为它们是行为探针，不放探针就没法验。
    for cp in head:
        kept.add(cp)
    order = [ord(c) for c in test_chars] + sorted(kept - {ord(c) for c in test_chars})
    # 自检：图标码位必须在集合里（否则运行期会退回占位图标）
    #   ★ 显式报错而非 `assert`：`python -O` 会把 assert 整条剥掉，这条**出包前的不变量**
    #     会在优化模式下静默放行。
    if not all(cp in set(order) for cp in GAME_ICON_CPS):
        raise SystemExit("图标码位没进集合")
    # 自检：探针字必须**是本语言该有的字**（否则它进得了集合却不是「 wanted」，
    #   覆盖率红线会把它当成漏字，两处自检互相打脸）
    #   ★ 同上一条：显式报错，`python -O` 剥不掉。
    bad = [c for c in test_chars if not wanted(ord(c), language)]
    if bad:
        raise SystemExit("探针字 %s 不属于 %s 的字集范围" % ("".join(bad), language["label"]))
    stats = dict(
        n_corpus=len(corpus),
        n_han_punct=len(han_punct),
        n_dropped=len(dropped),
        n_kept=len(kept),
        n_total=len(order),
        dropped=sorted(dropped),
        test_forced=[c for c in head if c in dropped],
    )
    return order, stats


# ---------------------------------------------------------------- 布局
# 单套图集的**内存预算**（RGBA）。超出就缩小格子。
ATLAS_BUDGET = 32 * 1024 * 1024


# 一次布局拟合的结果（纯值，不修改 FONT_SETS）。
@dataclass(frozen=True)
class Layout:
    cell: int
    step: int
    px: int
    height: int


def fit_cell(font_set, n_need, budget=ATLAS_BUDGET):
    """在装得下 n_need 个字、且不超过内存预算的前提下，取**最大的**格子 → Layout。

    ★ 为什么必须自动收缩而不是手填常量：格子目标是 em（= 2×拉丁 adv 实测），
      但 1024 宽的图集装不下大格子（title 的 em=84 需要 1024×11,598 ≈ 47 MB）。
      中文若被砍得比它自己的拉丁大写字母还小 1.5 倍 ⇒ 混排时字形互相挤压。
      所以按 em 起步，装不下才一档一档退。
    ★ 纯函数：返回拟合值，不改传入的 FontSet —— 调用点不再需要 dict() 副本自保。
    """
    em = font_set.em or font_set.cell
    cell = int(em)
    while cell >= 16:
        step = cell + 2
        # ★ 烘多大的字 = 0.85 × cell，不是 cell-2。
        #   实机证据：cell-2 ⇒ 墨迹占 em 93%，CJK 侧边距只剩 1~2 单位，
        #   密笔画字（離/開/遊/戲）之间直接连上（9 字里 2 对粘连、中位间隔仅 4px/pitch 100px）。
        #   CJK 排版惯例是墨迹占 em 的 85~88%，两侧各留 ~6% ⇒ 0.85 是这个区间的中值。
        #   注意这不改变 cell/step，所以图集尺寸、格子数、清单结构全都不变。
        px = max(8, int(cell * 0.85))
        height = atlas_height(font_set.ow, font_set.oh, cell, step, n_need)
        fits = height * W * 4 <= budget
        has_room = len(cell_positions(font_set.ow, font_set.oh, cell, step, W, height)) >= n_need
        if fits and has_room:
            return Layout(cell=cell, step=step, px=px, height=height)
        cell -= 2
    raise SystemExit("%s：em=%d 装不下 %d 个字" % (font_set.name, em, n_need))


def atlas_height(ow, oh, cell, step, n_need, y0=Y0, w=W):
    """用**真实的 cell_positions 布局**反解出装得下 n_need 个格子的最小图集高度。

    ★ 不能用"行数 = 字数 / 列数"估：`cell_positions` 是**上下两区**布局
      （原图集占左 ow 列 × 底 oh 行，右侧才是下区），下区宽度 = w − ow − 2，
      比上区窄得多。估出来的高度会偏小 ⇒ 静默截断记录。
      这里直接调用布局函数数格子，32 对齐向上取。
    """
    h = oh + cell + 64
    # ★ 下区起点是 `h − oh + 2`。若 h 太小，这个起点会掉到 y0 以下
    #   （techno 原图集 64×1024 又窄又高最容易踩到：格子缩小后 h 只剩 1120，
    #   下区从 y=98 开始 ⇒ 111 个格子落进顶部保留区，自检判失败、拒绝出包）。
    #   所以高度必须同时满足「装得下」**且**「下区不侵入保留区」。
    h = max(h, oh + y0 + cell)
    while h < 32768:
        if len(cell_positions(ow, oh, cell, step, w, h, y0)) >= n_need:
            return ((h + 31) // 32) * 32
        h += 32
    raise SystemExit("原图集 %dx%d：%d 个 %dpx 格即使 32768 高也装不下" % (ow, oh, n_need, cell))


def cell_positions(ow, oh, cell, step, w=W, h=H, y0=Y0):
    """格子左上角序列 —— 与布局公式一致（显式传尺寸，便于自检夹具）。

    上区：y 从 Y0 起、一直到 H-oh（不越过原图集区），x = 2, 2+step, ...
    下区：x ≥ ow+2（对齐到 x ≡ 2 (mod step)），y 从 H-oh+2 起。
    两条合起来保证**任何 CJK 格都不压到左 ow 列 × 底 oh 行的原图集区**，也不碰顶部 <Y0 行。
    """
    pos = []
    top_h = h - oh
    y = y0
    while y + cell <= top_h:
        x = 2
        while x + cell <= w:
            pos.append((x, y))
            x += step
        y += step
    x0 = ow + 2 + ((2 - (ow + 2)) % step)
    y = top_h + 2
    while y + cell <= h:
        x = x0
        while x + cell <= w:
            pos.append((x, y))
            x += step
        y += step
    return pos


# ---------------------------------------------------------------- 烘字
_bake_cache = {}
_font_cache = {}


def weight_font(font_path, px, weight):
    """取该字重的 ImageFont（每 (字体, 字号, 字重) 一份，创建时设定一次变体）。

    ⚠ 变体状态挂在 FreeType face 上：四套共用一个 VF 文件，**绝不能**共享同一个
      ImageFont 对象再各自 set_variation —— 会互相覆盖。缓存键必须含 weight。
    ★ 非 VF（无 fvar/wght）在这里就抛错：本工具**只支持可变字重字体**（约定见文件头）。
    """
    key = (font_path, px, weight)
    hit = _font_cache.get(key)
    if hit is not None:
        return hit
    font = ImageFont.truetype(font_path, px)
    try:
        font.set_variation_by_name(weight)
    except OSError as ex:
        # 输入错误 = 退出 2：说清原因（别让人以为是数据坏了），照 README 的退出码约定
        say(
            "字体不是可变字重（fvar）或没有命名实例 %r：%s\n"
            "  本工具只支持可变字重 TTF（fvar/wght，如 NotoSansSC-VariableFont_wght.ttf）。"
            % (weight, font_path)
        )
        raise SystemExit(2) from ex
    _font_cache[key] = font
    return font


def bake_cell(ch, font_path, px, cell, weight):
    """把一个码位烘成 cell×cell 的 L 掩码（**已垂直翻转**），并量出 yoff。

    流程：px 渲染到 2px×2px 画布 → getbbox 裁 → 超 cell-2 则等比缩 →
    居中贴进 cell×cell → FLIP_TOP_BOTTOM（垂直翻转是引擎 UV 约定要求的）。
    ★ 超限缩放必须等比：逐轴独立 clamp（min(w,…), min(h,…)）会拉扁字形。
    返回 (掩码 Image, yoff, 是否空格子, 墨迹包围盒 ibb)：
      yoff = **墨迹顶到格子底的距离**（= 墨迹末行下标 + 1），不是墨迹高（见写规注释）。
      ibb  = 墨迹包围盒 (x0, y0, x1, y1)，右下开区间。
    """
    key = (ch, px, cell, weight)
    hit = _bake_cache.get(key)
    if hit is not None:
        return hit
    font = weight_font(font_path, px, weight)
    tmp = Image.new("L", (px * 2, px * 2), 0)
    ImageDraw.Draw(tmp).text((2, 0), ch, font=font, fill=255)
    bb = tmp.getbbox()
    if bb is None:  # TTF 里没这个字形（U+3000 就是）⇒ 空格子
        # yoff 同样按「墨迹顶到格子底」给（这里墨迹 = 整个空格子 ⇒ 值 = cell），
        #   不写 0：写 0 会让记录出现 h==cell 而 yoff==0 的自相矛盾形状。
        res = (Image.new("L", (cell, cell), 0), cell, True, (0, 0, cell, cell))
        _bake_cache[key] = res
        return res
    g = tmp.crop(bb)
    lim = cell - 2
    if g.width > lim or g.height > lim:  # 等比缩到 lim 内（绝不拉扁）
        s = lim / float(max(g.width, g.height))
        g = g.resize((max(1, int(round(g.width * s))), max(1, int(round(g.height * s)))))
    cel = Image.new("L", (cell, cell), 0)
    cel.paste(g, ((cell - g.width) // 2, (cell - g.height) // 2))
    cel = cel.transpose(Image.FLIP_TOP_BOTTOM)  # 引擎绘制时再取 1-v 翻回来（笔记 §3）
    # yoff = 墨迹末行 + 1，从**我们自己的**位图量。
    #   ⚠ 门限取 `>0`（不是 `>8`）：墨迹边界上 Pillow 的抗锯齿会留一两格极低 alpha，
    #     换成 `>8/255` 会让 yoff 差 0~1px。差 1px 在 24px 格子上是肉眼可见的字距抖动，
    #     而 `>0` 与"位图上非黑即墨"的直觉一致。笔记 §6 把它列为待定项，这里**选定 >0 并注明**。
    #   翻转之后第 0 行就是字的底行 ⇒ 「末行下标 + 1」= 自底向上的墨迹行数。
    pxls = cel.load()
    # 翻转后第 0 行 = 字的**底行**；求墨迹包围盒（x0,y0,x1,y1，右开）
    x0, y0, x1, y1 = cell, cell, -1, -1
    for r in range(cell):
        for c in range(cell):
            if pxls[c, r] > 0:
                if r < y0:
                    y0 = r
                if r > y1:
                    y1 = r
                if c < x0:
                    x0 = c
                if c > x1:
                    x1 = c
    if y1 < 0:  # 兜底：整格
        y0, y1, x0, x1 = 0, cell - 1, 0, cell - 1
    bb = (x0, y0, x1 + 1, y1 + 1)  # 墨迹高 = y1+1（自底向上）
    res = (cel, bb[3], False, bb)
    _bake_cache[key] = res
    return res


# ---------------------------------------------------------------- 字体基线度量
_metric_cache = {}


def ttf_metrics(font_path):
    """读字体的基线度量 → (units_per_em, 升部比, 降部比)，比值已除过 upem、降部为正。

    ★ 唯一用途：决定「格子底到基线」的距离。我们的格子 = 该字体的 **em 框**，
      而 `OS/2.sTypoAscender + sTypoDescender` 恰好 = 1 em（排版口径）⇒ 用它。
      `hhea`/`usWin` 是给**行距**用的宽口径（Noto Sans SC 是 1.16/0.288 = 1.448 em），
      拿它当 em 框会把整行压低。缺 OS/2 才退回 hhea。
    ★ 可变字重字体各实例共用同一张 OS/2（实测 NotoSansSC VF：三字重 typo=880/-120、
      降部 0.12 em）⇒ 降部比与字重无关，一套一个值。

    ★ 吃 TTF / OTF / TTC 三种 sfnt 容器（表目录布局相同）：
      · TTF 与 OTF 的目录就在 +12（OTF 只是字形换成 CFF，头是 `OTTO`）；
      · **TTC（`ttcf`）的目录不在 +12**：+8 是字体数、+12 起是各字体的目录偏移表，
        必须先跳到第 0 个字体的目录（与 Pillow `truetype()` 默认取 index 0 一致）。
        不处理这一支会让 `--lang ja` 的候选（YuGothR.ttc / msgothic.ttc）直接崩。
    """
    hit = _metric_cache.get(font_path)
    if hit is not None:
        return hit
    with open(font_path, "rb") as f:
        d = f.read()
    magic = d[:4]
    if magic == b"ttcf":  # TrueType Collection：先取第 0 个字体的**表目录（偏移表）**位置
        if len(d) < 16:
            raise SystemExit("TTC 头太短：%s" % font_path)
        if not struct.unpack_from(">I", d, 8)[0]:
            raise SystemExit("TTC 里没有字体：%s" % font_path)
        dir_off = struct.unpack_from(">I", d, 12)[0]
    elif magic in (b"\x00\x01\x00\x00", b"OTTO", b"true", b"typ1"):  # 单字体 sfnt
        # ★ 单字体的偏移表就**在文件头**（dir_off = 0）：+4 = numTables、+12 = 表记录。
        #   写成 12 会把 numTables 读到第 0 条表记录的 tag 上（TTF/OTF 全崩）。
        dir_off = 0
    else:
        raise SystemExit("不是 sfnt 容器（TTF/OTF/TTC）：%s（头 %r）" % (font_path, magic))
    if dir_off + 12 > len(d):
        raise SystemExit(
            "表目录越界：%s（目录偏移 %d，文件 %d 字节）" % (font_path, dir_off, len(d))
        )
    n = struct.unpack_from(">H", d, dir_off + 4)[0]
    tabs = {}
    for i in range(n):
        o = dir_off + 12 + i * 16
        if o + 16 > len(d):
            raise SystemExit("表记录越界：%s（第 %d/%d 条）" % (font_path, i, n))
        tabs[d[o : o + 4].decode("latin-1")] = struct.unpack_from(">I", d, o + 8)[0]
    if "head" not in tabs:
        raise SystemExit("字体里没有 head 表：%s" % font_path)
    upem = struct.unpack_from(">H", d, tabs["head"] + 18)[0]
    if not upem:
        raise SystemExit("字体的 unitsPerEm = 0：%s" % font_path)
    if "OS/2" in tabs:  # sTypoAscender / sTypoDescender
        asc, desc = struct.unpack_from(">hh", d, tabs["OS/2"] + 68)
    elif "hhea" in tabs:
        asc, desc = struct.unpack_from(">hh", d, tabs["hhea"] + 4)
    else:
        raise SystemExit("字体里既无 OS/2 也无 hhea：%s" % font_path)
    res = (upem, asc / float(upem), -desc / float(upem))
    _metric_cache[font_path] = res
    return res


def descent_px(adv, descent_em):
    """格子底到基线的距离（px）。adv = 格子边长 = em 框边长。"""
    return int(adv * descent_em + 0.5)


# ---------------------------------------------------------------- 记录写规
def rec_rule_bad(recs, descent_em):
    """按写规**逐条**校验度量之间的关系，返回 [(下标, 期望 yoff)] 的违规清单。

    写规（见 build_atlas 里的注释）：w/h = 墨迹宽/高、xoff = 墨迹左缘、adv = 格子、
    page = 0、yoff = **墨迹顶到基线的距离** = adv − (adv − h)//2 − 降部px
    （墨迹在 em 框里居中贴 ⇒ 顶到格子底 = (cell+h)/2，再减去「格子底到基线」的降部）。

    ★ 判据必须**全量扫每一条**，不能只看首条：首条恒是探针字『中』（满格字），
      它的 yoff 恰好接近 h ⇒ 只看首条会放行「中 正确、一/二/三 沉底」——
      那正是 yoff 被写成墨迹高时的形态。
    ★ 生成期（build_atlas 的 chk）与回读对账（--verify）**共用本函数**，
      免得两边判据漂移、生成期拦得住而 --verify 对既有清单查不全。
    ★ 不要求 `yoff >= h`：墨迹底可以落在基线之下（降部字形，如拉丁 'g'），
      那时 yoff < h 是**对的**；真要抓的错是「值不等于写规算出来的那个」。
    """
    bad = []
    for i, r in enumerate(recs):
        want = r.adv - (r.adv - r.h) // 2 - descent_px(r.adv, descent_em)
        fits_box = 0 < r.w <= r.adv and 0 < r.h <= r.adv
        in_advance = 0 <= r.xoff and r.xoff + r.w <= r.adv
        if not (fits_box and in_advance and r.yoff == want and r.page == 0):
            bad.append((i, want))
    return bad


# ---------------------------------------------------------------- 图集
def build_atlas(font_set, layout, codepoints, dds_path, font_path, weight, w, y0=Y0):
    """烘一整套：原图集 1:1 贴左下角 + CJK 格从 Y0 起排。

    → (画布, 记录列表, 自检结论)。画布高度 = layout.height（fit_cell 刚拟合出来的）。
    """
    with Image.open(dds_path) as src_img:
        src = src_img.convert("RGBA")
    if src.size != (font_set.ow, font_set.oh):
        raise SystemExit(
            "%s 的原图集尺寸是 %dx%d，表里写的是 %dx%d"
            % (font_set.name, src.size[0], src.size[1], font_set.ow, font_set.oh)
        )
    # 原图集：RGB/alpha 原样保留 —— bold/textpc 含彩色手柄按钮图标（实测 2378/1251 个
    # 彩色像素），它们靠图集固有色呈色（引擎不给图标着色）；「只取 alpha、RGB 置白」
    # 会把图标抹成白色。普通字形本来就是白字，保留 RGB 不改变它们的呈色。
    orig = src.copy()

    h = layout.height
    cell, step, px = layout.cell, layout.step, layout.px
    pos = cell_positions(font_set.ow, font_set.oh, cell, step, w, h, y0)
    use = list(codepoints)
    truncated = use[len(pos) :] if len(use) > len(pos) else []
    use = use[: len(pos)]

    canvas = Image.new("RGBA", (w, h), (255, 255, 255, 0))
    canvas.paste(orig, (0, h - font_set.oh))

    recs, blanks = [], []
    # 格子底到基线的距离 = 该字体的降部（em 比）× 格子边长。全图集同一个值。
    desc_px = descent_px(cell, ttf_metrics(font_path)[2])
    for i, cp in enumerate(use):
        x, y = pos[i]
        cel, yoff, blank, ibb = bake_cell(chr(cp), font_path, px, cell, weight)
        if blank:
            blanks.append(cp)
        canvas.paste((255, 255, 255, 255), (x, y), cel)
        # UV 约定：清单里的 v 是已翻转的 v（v' = 1 − y/H），
        # 图集像素必须预 FLIP_TOP_BOTTOM，两者自洽（引擎绘制时再取 1−v 翻正）。
        # ★ 四边形只覆盖墨迹（ibb），不是整格：
        #   行距/块高完全不由字形度量决定（块高 = (行数−1)×(style->+0x34 − K) +
        #   fontSize×行数，fontSize = font+0x24，三个行距函数都不访问 font+0x38），
        #   所以 h 取整格不会「把整行顶上去」。收成墨迹盒的真实收益有两条：
        #     ① 墨迹占 em 从 93% 降到 88~91%，相邻字不再相接（横向重叠）
        #     ② 不再把格子里基线以下的空白画进四边形，避免采样到相邻 CJK 格子的边缘
        ix0, iy0, ix1, iy1 = ibb
        iw, ih = ix1 - ix0, iy1 - iy0
        u0, u1 = f32((x + ix0) / float(w)), f32((x + ix1) / float(w))
        v1, v0 = f32(1 - (y + iy0) / float(h)), f32(1 - (y + iy1) / float(h))
        # 写规（四边形只覆盖墨迹，见上）：w/h = 墨迹宽/高、xoff = 墨迹左缘相对前进笔位
        #   （方块居中 ⇒ 左半字身）、adv = 格子（= CJK em 框）。
        # ★ yoff = **墨迹顶到基线的距离** = bb[3] − 降部px，其中 bb[3] = 墨迹顶到格子底。
        #   引擎约定是「四边形顶 = 基线 + yoff、底 = 基线 + yoff − h」——实测 textpc MFT：
        #   'A' h=18 yoff=18（无降部，墨迹底贴基线）、'g' h=18 yoff=14（降部 4px 在基线之下）。
        #   格子 = 字体的 em 框，而**基线不在格子底**：降部 = OS/2 sTypoDescender/upem
        #   （Noto Sans SC = 0.12 em，见 ttf_metrics）。漏减这一项 ⇒ 整行被抬高一个降部
        #   （textpc 3px / title 8px）；写成 h 则是另一个错：稀疏字（一 二 三 丶）会沉到基线。
        #   两条一起验的办法：全格字的墨迹区间应几乎正好压在原字体 'A' 的 [0, +cap] 上。
        recs.append(
            GlyphRec(
                cp=cp,
                w=iw,
                h=ih,
                xoff=ix0,
                yoff=yoff - desc_px,
                adv=cell,
                u0=u0,
                v0=v0,
                u1=u1,
                v1=v1,
                page=0,
            )
        )

    # ---- 自检（全部只读，不改画布）----
    checks = {}
    # ① 贴回去的原图集区域与 DDS 解码结果**逐像素**一致
    checks["orig_identical"] = (
        canvas.crop((0, h - font_set.oh, font_set.ow, h)).tobytes() == orig.tobytes()
    )
    # ② 越界
    checks["out_of_bounds"] = sum(
        1 for (x, y) in pos[: len(use)] if x < 0 or y < 0 or x + cell > w or y + cell > h
    )
    # ③ 顶部保留区（y < Y0）
    checks["in_top_reserve"] = sum(1 for (x, y) in pos[: len(use)] if y < y0)
    # ④ 重叠：用一张占用掩码逐格盖一遍，撞车即计一次（比两两比对快得多）
    mask = bytearray(w * h)
    overlap = 0
    for x, y in pos[: len(use)]:
        for r in range(y, y + cell):
            base = r * w
            for c in range(x, x + cell):
                if mask[base + c]:
                    overlap += 1
                    break
                mask[base + c] = 1
    checks["overlap"] = overlap
    # ⑤ 压到原图集区（左 ow 列 × 底 oh 行）
    checks["hits_orig"] = sum(
        1 for (x, y) in pos[: len(use)] if x < font_set.ow and y + cell > h - font_set.oh
    )
    checks["n_used"] = len(use)
    checks["cap"] = len(pos)
    checks["truncated"] = truncated
    checks["blanks"] = blanks
    # ⑥ 度量写规不变量（**全量**，见 rec_rule_bad）：yoff 必须是「墨迹顶到基线的距离」，
    #   写成 h 会把墨迹底钉在基线上、稀疏字沉到基线。这条在打包期就拦，不让错清单离开 --out。
    checks["rule_bad"] = len(rec_rule_bad(recs, ttf_metrics(font_path)[2]))
    checks["ok"] = (
        checks["orig_identical"]
        and checks["out_of_bounds"] == 0
        and checks["in_top_reserve"] == 0
        and checks["overlap"] == 0
        and checks["hits_orig"] == 0
        and checks["rule_bad"] == 0
        and not checks["truncated"]
    )
    return canvas, recs, checks


# ---------------------------------------------------------------- tms 对账（可选输入）
# TextureMapSpec 是 forge 里的纹理描述符（scimitar::TextureMapSpec，167B）。字体渲染
#   路径不读它（实测：运行期尺寸/格式来自字体纹理页描述符，不读 DDS 头/MapSpec），
#   但它是我们手上的**独立于参数表**的第二来源 ⇒ 只做对账，缺了不阻塞出包。
# 实测偏移：+0x3A u16 = 原图集宽、+0x3E u16 = 原图集高（四份文件逐一命中 64×1024 /
#   256×512 / 512×1024 / 256×256）。缺失不阻塞出包；存在但不匹配则拒绝出包。
TMS_W_OFF, TMS_H_OFF = 0x3A, 0x3E


def check_tms(tms_dir, font_sets):
    """→ (逐套对账行, 是否有缺失, 是否有不匹配)。"""
    lines, missing, mismatch = [], False, False
    if not tms_dir or not os.path.isdir(tms_dir):
        return ["⚠ tms 目录不在：%s —— 跳过 TextureMapSpec 对账（可选输入）" % tms_dir], True, False
    specs = {}  # (w, h) → 文件名
    for fn in sorted(os.listdir(tms_dir)):
        if not fn.endswith(".TextureMapSpec"):
            continue
        with open(os.path.join(tms_dir, fn), "rb") as f:
            d = f.read()
        if len(d) <= TMS_H_OFF + 1:
            lines.append("!! %s 太短（%d B），尺寸无法核对" % (fn, len(d)))
            mismatch = True
            continue
        # 宽在 +0x3A、高在 +0x3E（中间隔一个字段）；四份实测逐一命中原图集尺寸
        w = struct.unpack_from("<H", d, TMS_W_OFF)[0]
        h = struct.unpack_from("<H", d, TMS_H_OFF)[0]
        specs[(w, h)] = fn
    for font_set in font_sets:
        fn = specs.get((font_set.ow, font_set.oh))
        if fn is None:
            lines.append(
                "⚠ %s：tms 里找不到 %dx%d 的描述符 —— 跳过该套对账"
                % (font_set.name, font_set.ow, font_set.oh)
            )
            missing = True
            continue
        lines.append(
            "tms 对账 %-7s %dx%d == 参数表 ✓（%s）" % (font_set.name, font_set.ow, font_set.oh, fn)
        )
    return lines, missing, mismatch


# ---------------------------------------------------------------- 落盘
def write_rgba(path, img, w, h):
    # ★ 用 `with`：CPython 的即时回收不是关闭，别依赖它关文件。
    #   这里落的是几 MB 的图集，磁盘写满时只有 close 才会抛错 —— 否则会**静默留下截断的
    #   blob**，而清单里 our_w/our_h 还写着完整尺寸，事后对不上账。
    with open(path, "wb") as f:
        f.write(struct.pack(ACGX_HDR, ACGX_MAGIC, w, h, 0) + img.tobytes())


def write_manifest(path, packs):
    r"""写 AC1_CJK_Glyphs.bin —— 逐字节按 modules\glyph\include\ac1\glyph\glyph.h。

    ⚠ fp_u_span / fp_v_span 传的是 f32（fingerprint() 已舍入），不是十进制字面量。
    ⚠ 记录末尾 page 显式写 0（PC 渲染不读它），pad 同样写 0。
    ⚠ count 写 len(recs)、套头 reserved 写 0（写规归本函数，不归调用方手填）。
    """
    n_total = sum(len(p.recs) for p in packs)
    out = [struct.pack("<4sHHI", ACGG_MAGIC, ACGG_VERSION, len(packs), n_total)]
    for p in packs:
        hdr = p.header
        out.append(
            struct.pack(
                SET_HDR,
                len(p.recs),
                hdr.fp_glyph_count,
                hdr.du,
                hdr.dv,
                hdr.ow,
                hdr.oh,
                hdr.flags,
                hdr.nw,
                hdr.nh,
                0,
            )
        )
        for r in p.recs:
            out.append(
                struct.pack(
                    REC_FMT,
                    r.cp,
                    r.w,
                    r.h,
                    r.xoff,
                    r.yoff,
                    r.adv,
                    r.u0,
                    r.v0,
                    r.u1,
                    r.v1,
                    r.page,
                    0,
                )
            )
    blob = b"".join(out)
    # ★ 同 write_rgba：不用裸 open(...).write(...)，写全/写完由 close 保证，出错会抛。
    with open(path, "wb") as f:
        f.write(blob)
    return len(blob)


@dataclass(frozen=True)
class ParsedSet:
    """parse_manifest 回读的一套：套头（含 reserved）+ 记录。"""

    count: int
    header: SetHeader
    reserved: int
    recs: tuple


def parse_manifest(path):
    """按 glyph.h / manifest.cpp 的规则回读清单 —— `--verify` 与自检都用它。"""
    with open(path, "rb") as f:
        d = f.read()
    errs = []
    if len(d) < 12:
        raise SystemExit("清单太短：%d B" % len(d))
    magic, ver, nsets, nrecs = struct.unpack_from("<4sHHI", d, 0)
    if magic != ACGG_MAGIC:
        errs.append("magic 不是 ACGG（读到 %r）" % magic)
    if ver != ACGG_VERSION:
        errs.append("version 不是 1（读到 %d）" % ver)
    if nsets == 0 or nsets > 8:  # GLYPH_MAX_SETS
        errs.append("n_sets 越界：%d" % nsets)
    if nrecs > 65536:  # GLYPH_MAX_RECS
        errs.append("n_recs_total 越界：%d" % nrecs)
    off, sets, nr = 12, [], 0
    for i in range(nsets if not errs else 0):
        if off + struct.calcsize(SET_HDR) > len(d):
            errs.append("套 #%d 的头被截断" % i)
            break
        count, fpc, du, dv, ow, oh, flags, nw, nh, resv = struct.unpack_from(SET_HDR, d, off)
        off += struct.calcsize(SET_HDR)
        if count == 0 or count > 65536:
            errs.append("套 #%d 的 count 越界：%d" % (i, count))
            break
        if nr + count > nrecs:
            errs.append("套 #%d 的记录数超过 n_recs_total" % i)
            break
        if off + count * 32 > len(d):
            errs.append("套 #%d 的记录被截断" % i)
            break
        recs = []
        for j in range(count):
            cp, cw, chh, xo, yo, adv, u0, v0, u1, v1, page, pad = struct.unpack_from(
                REC_FMT, d, off + j * 32
            )
            recs.append(
                GlyphRec(
                    cp=cp,
                    w=cw,
                    h=chh,
                    xoff=xo,
                    yoff=yo,
                    adv=adv,
                    u0=u0,
                    v0=v0,
                    u1=u1,
                    v1=v1,
                    page=page,
                )
            )
            if pad != 0 or page != 0:
                errs.append("套 #%d 记录 #%d：page/pad 非 0（page=%d pad=%d）" % (i, j, page, pad))
        off += count * 32
        nr += count
        sets.append(
            ParsedSet(
                count=count,
                header=SetHeader(
                    fp_glyph_count=fpc, du=du, dv=dv, ow=ow, oh=oh, flags=flags, nw=nw, nh=nh
                ),
                reserved=resv,
                recs=tuple(recs),
            )
        )
    if not errs:
        if off != len(d):
            errs.append("文件长度与声明对不上（读到 %d 字节，声明走到 %d）" % (len(d), off))
        if nr != nrecs:
            errs.append("实际记录 %d ≠ 声明 %d" % (nr, nrecs))
    return dict(size=len(d), nsets=nsets, nrecs=nrecs, sets=sets, errs=errs)


# ---------------------------------------------------------------- 自检
def _fixture_mft(cps, at_rec):
    """造一份最小的 PixmapFont 夹具（条数在 +0x0A、记录区在 +0x18、每条 30B、码位在 +18）。

    30B 的排布照 MFT 实测布局：4×f32 UV(16) | 保留(2) | cp(2) | w(2) | h(2) | xoff(2) | yoff(2) | adv(2)。
    """
    # 头部必须**正好** MFT_REC_OFF 字节：记录区紧跟在「PixmapFont」+0x18 处
    head = bytearray(MFT_MARK + b"\x00" * (MFT_REC_OFF - len(MFT_MARK)))
    struct.pack_into("<H", head, MFT_CNT_OFF, len(cps))
    body = b""
    for cp in cps:
        w, h = at_rec if cp == 0x40 else (5, 6)
        body += struct.pack("<4f", 0.1, 0.2, 0.3, 0.4)
        body += struct.pack("<7H", 0, cp, w, h, 0, 1, 8)
    return bytes(head) + body


def self_check(tmpdir):
    """内置小夹具自检：lgdict 解码层 / MFT 解析 / 布局 / 指纹公式 / UV 换算 + ACGG 写读往返。
    **不需要真数据，也不烘字。**"""
    bad = []

    # ⓪ 词典文本层（与 dict.cpp 对齐的解码/规整）——唯一实现，判据在 lgdict 里
    if lgdict.self_check() != 0:
        bad.append("lgdict 自检失败（见上）")
    say("[自检] 词典文本层（lgdict）—— %s" % ("OK" if not bad else "见下"))

    # ① MFT 解析：条数必须取自 +0x0A 的 u16（而不是"读到越界为止"）
    path = os.path.join(tmpdir, "fx.MagmaMftFile")
    cps = [0x20, 0x40, 0x41, 0x00C0, 0x3000, 0x4E2D]
    with open(path, "wb") as f:
        f.write(_fixture_mft(cps, (17, 18)))
    m = mft_parse(path)
    if m["n_recs"] != 6:
        bad.append("MFT 条数应为 6，实得 %d" % m["n_recs"])
    if m["cps"] != cps:
        bad.append("MFT 码位序列不对：%r" % (m["cps"],))
    if set(cps) != set(m["cps"]):
        bad.append("MFT 码位去重后对不上")
    # 记录区之后放垃圾字节：解析不该把它当记录（条数取自头，不读到文件尾）
    with open(path, "wb") as f:
        f.write(_fixture_mft(cps, (17, 18)) + b"\xaa" * 400)
    m2 = mft_parse(path)
    if m2["cps"] != cps:
        bad.append("MFT 越界尾巴被当记录了：%r" % (m2["cps"],))
    if mft_at(m2)["cp"] != 0x40 or mft_at(m2)["w"] != 17 or mft_at(m2)["h"] != 18:
        bad.append("MFT '@' 记录的 w/h 取错：%r" % (mft_at(m2),))
    say(
        "[自检] MFT 解析：条数取自 +0x0A 的 u16（6 条）、码位在 +18、尾巴不被当记录、'@' 的 w/h=17/18 —— %s"
        % ("OK" if not bad else "见下")
    )

    # ② 指纹公式：du=(w+1)/W、dv=(h+1)/H，且是 f32（不是十进制字面量）
    n0 = len(bad)
    du, dv = fingerprint(m2, 256, 256)
    if abs(du - 18.0 / 256) > 1e-9 or abs(dv - 19.0 / 256) > 1e-9:
        bad.append("指纹公式不对：%r/%r" % (du, dv))
    if struct.pack("<f", du) != struct.pack("<f", 0.0703125):
        bad.append("指纹没按 f32 落盘")
    if f32(0.04) == 0.14 - 0.10:  # 反例锚点：说明 f32 转换确实在起作用
        bad.append("f32() 夹具自相矛盾（0.04f 应不等于 0.14f-0.10f）")
    say(
        "[自检] 指纹公式：18/256、19/256 且按 f32 落盘（0.04f ≠ 0.14f-0.10f，转换有效） —— %s"
        % ("OK" if len(bad) == n0 else "见下")
    )

    # ③ 布局：**用 fit_cell 现场拟合出来的真实图集**检查，而不是拿一张
    #    早已不用的固定 1024×2048 去比容量常数。
    #    （旧断言的容量常数是「笔记 §3」时代的，与 fit_cell 引入后的按套定尺寸
    #     完全对不上；又因为 ⑤ 的夹具缺字段先崩了，这些失败一直被掩盖。）
    n0 = len(bad)
    caps = {}
    for font_set in FONT_SETS:
        n_need = 1458 + font_set.n_glyph  # 我们要烘的 CJK 数 + 原字形数
        layout = fit_cell(font_set, n_need)
        pos = cell_positions(font_set.ow, font_set.oh, layout.cell, layout.step, W, layout.height)
        caps[font_set.name] = len(pos)
        if len(pos) < n_need:
            bad.append(
                "%s 拟合出的图集 %dx%d 只放得下 %d 个格子 < 需要的 %d"
                % (font_set.name, W, layout.height, len(pos), n_need)
            )
        occ = set()
        oob = top = hit0 = 0
        for x, y in pos:
            # ★ 用**拟合出来的 cell / height**，不是 FontSet.cell / 模块默认 H：
            #   fit_cell 不改 FontSet，图集高度也不再是固定的 H。
            #   校验使用拟合后的图集高度，避免把布局参数与实际画布混用。
            if x < 0 or y < 0 or x + layout.cell > W or y + layout.cell > layout.height:
                oob += 1
            if y < Y0:
                top += 1
            if x < font_set.ow and y + layout.cell > layout.height - font_set.oh:
                hit0 += 1
            for r in range(y, y + layout.cell):
                for c in range(x, x + layout.cell):
                    if (c, r) in occ:
                        bad.append("%s 格子重叠 @%d,%d" % (font_set.name, x, y))
                        break
                    occ.add((c, r))
        if oob or top or hit0:
            bad.append(
                "%s 越界 %d / 顶部保留区 %d / 压原图集区 %d" % (font_set.name, oob, top, hit0)
            )
    say(
        "[自检] 布局：按 fit_cell 拟合的真实图集，四套容量 %s、无重叠、不越界、"
        "不压原图集区、不碰 y<%d —— %s"
        % (
            "/".join(str(caps[s.name]) for s in FONT_SETS),
            Y0,
            "OK" if len(bad) == n0 else "见下",
        )
    )

    # ④ UV 换算：v' = 1 − y/H（笔记 §3）；上下边界必须与矩形的 v0/v1 自洽
    n0 = len(bad)
    x, y, cell = 2, Y0, 24
    u0, u1 = f32(x / float(W)), f32((x + cell) / float(W))
    v1, v0 = f32(1 - y / float(H)), f32(1 - (y + cell) / float(H))
    if not (v0 < v1):
        bad.append("v 方向反了：v0=%r v1=%r" % (v0, v1))
    # 引擎读出的行 = (1-v)*H ⇒ v1 映回矩形顶、v0 映回矩形底
    if abs((1 - v1) * H - y) > 1e-3 or abs((1 - v0) * H - (y + cell)) > 1e-3:
        bad.append("1-v 映回的像素行对不上：%r %r" % (v1, v0))
    if abs((u1 - u0) * W - cell) > 1e-3:
        bad.append("u 跨度映回的像素宽对不上：%r" % (u1 - u0))
    say(
        "[自检] UV 换算：v'=1−y/H、1−v 映回 [y, y+cell]、u 跨度 = cell 像素 —— %s"
        % ("OK" if len(bad) == n0 else "见下")
    )

    # ⑤ ACGG 写读往返：逐字节按 glyph.h 的布局写，再按同一套规则读回来
    n0 = len(bad)
    p = os.path.join(tmpdir, "fx.bin")
    rec = (
        GlyphRec(
            cp=0x4E2D,
            w=24,
            h=24,
            xoff=0,
            yoff=12,
            adv=24,
            u0=f32(2 / 1024.0),
            v0=f32(1 - 184 / 2048.0),
            u1=f32(26 / 1024.0),
            v1=f32(1 - 160 / 2048.0),
            page=0,
        ),
    )
    n = write_manifest(
        p,
        [
            PackSet(
                header=SetHeader(
                    fp_glyph_count=199,
                    du=f32(18 / 256.0),
                    dv=f32(19 / 256.0),
                    ow=256,
                    oh=256,
                    flags=0,
                    nw=1024,
                    nh=2048,
                ),
                recs=rec,
            )
        ],
    )
    # 套头是 40 B（glyph.h：count/fp/fp_u/fp_v/orig_w/orig_h/flags/our_w/our_h/rsv）。
    expect_len = 12 + 40 + 1 * 32
    if n != expect_len:
        bad.append("ACGG 长度应为 %d，实得 %d" % (expect_len, n))
    with open(p, "rb") as f:
        raw = f.read()
    if raw[:4] != b"ACGG" or struct.unpack_from("<H", raw, 4)[0] != 1:
        bad.append("ACGG 头不对")
    if struct.unpack_from("<H", raw, 6)[0] != 1 or struct.unpack_from("<I", raw, 8)[0] != 1:
        bad.append("ACGG 的 n_sets / n_recs_total 不对")
    if struct.calcsize(REC_FMT) != 0x20 or struct.calcsize(SET_HDR) != 40:
        bad.append("记录/套头字节数不是 0x20 / 40")
    back = parse_manifest(p)
    if back["errs"]:
        bad.append("回读被拒：%s" % back["errs"])
    if back and not back["errs"]:
        s0 = back["sets"][0]
        r0 = s0.recs[0]
        if r0.cp != 0x4E2D or r0.yoff != 12 or r0.page != 0:
            bad.append("记录往返字段不对：%r" % (r0,))
        if s0.header.du != f32(18 / 256.0) or s0.reserved != 0:
            bad.append("套头往返字段不对：%r" % (s0,))
    say(
        "[自检] ACGG 写读往返：头 12B / 套头 40B / 记录 0x20B 逐字节往返一致 —— %s"
        % ("OK" if len(bad) == n0 else "见下")
    )

    # ⑥ 写规校验器自检：必须抓得住「yoff 被写成墨迹高」的**局部**形态
    #   （首条『中』对、稀疏字『一』沉底）—— 只看首条的判据会整体放行。
    #   夹具喂固定降部比 0.12（不依赖真字体，纯测校验器逻辑）。
    n0 = len(bad)
    DR = 0.12

    def _rec(cp, h, yoff, adv):
        return GlyphRec(
            cp=cp, w=adv - 2, h=h, xoff=1, yoff=yoff, adv=adv, u0=0, v0=0, u1=0, v1=0, page=0
        )

    # adv=24 ⇒ 降部px = round(24*0.12) = 3
    #   满格字 h=24 ⇒ yoff = 24 − 0 − 3 = 21（注意 yoff < h 是**对的**，见下）
    #   稀疏字 h=5  ⇒ yoff = 24 − 9 − 3 = 12
    #   降部字 h=20 ⇒ yoff = 24 − 2 − 3 = 19（yoff < h，与拉丁 'g' 同形）
    clean = [_rec(0x4E2D, 24, 21, 24), _rec(0x4E00, 5, 12, 24), _rec(0x67, 20, 19, 24)]
    sunk = [_rec(0x4E2D, 24, 21, 24), _rec(0x4E00, 5, 5, 24)]  # 「一」被写成 yoff=h
    if rec_rule_bad(clean, DR):
        bad.append("写规校验器误报（合规记录被判违规）：%r" % (rec_rule_bad(clean, DR),))
    got = rec_rule_bad(sunk, DR)
    if got != [(1, 12)]:
        bad.append("写规校验器没抓到「一」沉底（应报 [(1, 12)]，实得 %r）" % (got,))
    say(
        "[自检] 写规校验器：全量扫每一条 —— 合规 3 条不误报（含降部字形 yoff=19<h=20）、"
        "『中』对而『一』沉底仍能抓到（%r） —— %s" % (got, "OK" if len(bad) == n0 else "见下")
    )

    for x in bad:
        say("[自检] 失败：" + x)
    say("[自检] %s" % ("全部通过" if not bad else "**%d 项失败**" % len(bad)))
    return 1 if bad else 0


# ---------------------------------------------------------------- 输入归一
# 所有默认值与"参数 → 实际路径"的翻译都收在这里，主流程拿到的全是解析后的值。
@dataclass(frozen=True)
class Inputs:
    mft_dir: str
    dds_dir: str
    lg_dict: str  # 主词典（LG）；非 zh 不读
    lg_dict_given: bool  # 是否用户显式给的（决定"忽略 --lg-dict"要不要提醒）
    supplement: str  # 那一份词典（data\dict.txt）
    ttf: str
    tms_dir: str
    out: str
    lang: str
    language: dict  # LANGS[lang]，调用点不必再查表
    sets: tuple  # 要出的 FontSet（= --only 过滤后）


def first_existing(cands):
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return None


def resolve_inputs(args):
    """argparse 的原始值 → Inputs（路径翻译 + 缺失裁决）。用法/输入错 ⇒ SystemExit(2)。"""
    only = {s.strip() for s in args.only.split(",") if s.strip()}
    if only:
        unknown = only - set(SET_BY_NAME)
        if unknown:
            say("未知的套名：%s（可选 %s）" % (",".join(sorted(unknown)), ",".join(SET_BY_NAME)))
            raise SystemExit(2)
    if not only:
        only = set(SET_BY_NAME)
    sets = tuple(s for s in FONT_SETS if s.name in only)

    language = lang_spec(args.lang)
    lg_dict_given = args.lg_dict is not None
    lg_dict_path = args.lg_dict or first_existing(
        [
            os.path.join(INPUT_DIR, "LGCStringDict_01.txt"),
            os.path.join(RELEASE_DIR, "..", "LG_Data", "LGCStringDict_01.txt"),
            os.path.join(RELEASE_DIR, "..", "scripts", "LGCStringDict_01.txt"),
        ]
    )
    supplement = args.supplement or os.path.join(RELEASE_DIR, "data", "dict.txt")
    ttf = args.ttf or first_existing(language["ttf_candidates"])
    tms_dir = args.tms_dir
    mft_dir, dds_dir, out = args.mft_dir, args.dds_dir, args.out

    # 缺输入一律显式报错（退出 2），点名给的和默认搜的同样裁决 —— 不静默降级
    need_lg = args.lang == "zh"
    want = [
        ("主词典（--lg-dict）" if need_lg else "主词典（非中文语言不需要）", lg_dict_path, need_lg),
        ("%s字体（%s）" % (language["label"], language["ttf_note"]), ttf, True),
    ]
    for label, path, needed in want:
        if not needed:
            continue
        if not path or not os.path.isfile(path):
            say("找不到%s。用对应的参数指定（词典 --lg-dict / 字体 --ttf）。" % label)
            raise SystemExit(2)
    for label, path, flag in (
        ("MFT 目录（--mft-dir）", mft_dir, "--mft-dir"),
        ("DDS 目录（--dds-dir）", dds_dir, "--dds-dir"),
    ):
        if not path or not os.path.isdir(path):
            say("找不到%s：%s（%s 指定的目录）" % (label, path, flag))
            raise SystemExit(2)
    # 「仅支持可变字重」在启动就验：每个用到的字重各实例化一次，别烘到一半才炸
    for weight in sorted({s.weight for s in sets}):
        weight_font(ttf, 10, weight)
    # 两份词典长得都像"词典文件"，传反是静默灾难 ⇒ 启动时嗅探一次（成对块标记判据）
    if os.path.isfile(supplement) and lgdict.looks_like_lg_dict(supplement):
        say(
            "--dict 指到 LG 主词典了：%s\n  那一份词典是 UTF-8 的 ENG<TAB>译文；主词典请用 --lg-dict。"
            % supplement
        )
        raise SystemExit(2)
    if need_lg and lg_dict_path and not lgdict.looks_like_lg_dict(lg_dict_path):
        say(
            "--lg-dict 读到的不像 LG 主词典（缺 xfhsm_res_ 成对块标记）：%s\n"
            "  那一份词典请用 --dict。" % lg_dict_path
        )
        raise SystemExit(2)

    return Inputs(
        mft_dir=mft_dir,
        dds_dir=dds_dir,
        lg_dict=lg_dict_path,
        lg_dict_given=lg_dict_given,
        supplement=supplement,
        ttf=ttf,
        tms_dir=tms_dir,
        out=out,
        lang=args.lang,
        language=language,
        sets=sets,
    )


# ---------------------------------------------------------------- 主流程
def do_pack(inputs):
    # ① 四套 MFT：解析 → 自有码位并集（黑名单）+ 指纹
    mfts, owned, fingerprints = {}, set(), {}
    for font_set in inputs.sets:
        path = os.path.join(inputs.mft_dir, font_set.mft)
        m = mft_parse(path)
        if m["n_recs"] != font_set.n_glyph:
            raise SystemExit(
                "%s：MFT 条数 %d ≠ 参数表 %d（参数表或 MFT 变了？先改表）"
                % (font_set.name, m["n_recs"], font_set.n_glyph)
            )
        du, dv = fingerprint(m, font_set.ow, font_set.oh)
        if abs(du - font_set.exp_du) > FP_TOL or abs(dv - font_set.exp_dv) > FP_TOL:
            raise SystemExit(
                "%s：指纹 (%.9f, %.9f) ≠ 期望 (%.6f, %.6f)，差 >1e-6 ⇒ 拒绝出包"
                % (font_set.name, du, dv, font_set.exp_du, font_set.exp_dv)
            )
        mfts[font_set.name] = m
        fingerprints[font_set.name] = (du, dv)
        owned |= set(m["cps"])
    say("原字体自有码位（四套 MFT 并集）= %d 个；每套 '@' 直算指纹：" % len(owned))
    for font_set in FONT_SETS:
        if font_set.name in mfts:
            say(
                "  %-7s 条数 %3d（原字体字形数）  指纹 (%.6f, %.6f)  == 期望 ✓"
                % (
                    font_set.name,
                    mfts[font_set.name]["n_recs"],
                    fingerprints[font_set.name][0],
                    fingerprints[font_set.name][1],
                )
            )

    # ② 语料 → 码位集
    # ★ **非中文语言不读主词典**：它的 CHI 段是**繁体中文**语料。ja/ko 是**重新翻译**
    #   而不是覆盖中文，混进来会把几千个汉字塞进日语字集（图集白涨、且大部分用不上）。
    #   要中文以外的语言，语料应当只来自 `--dict`（即翻译模板填好的那份）。
    language = inputs.language
    if inputs.lang == "zh":
        c_main, n_block = corpus_from_main_dict(inputs.lg_dict)
    else:
        c_main, n_block = set(), 0
        if inputs.lg_dict_given:
            say(
                "⚠ --lang %s：按设计**忽略** --lg-dict（主词典 CHI 段是中文语料）；"
                "语料只取 --dict" % inputs.lang
            )
        else:
            say("--lang %s：语料**只取 --dict**（主词典 CHI 段是繁体中文，不读）" % inputs.lang)
    c_supp, n_supp = corpus_from_supplement(inputs.supplement)
    corpus = c_main | c_supp
    codepoints, stats = build_codepoints(corpus, owned, language)
    say(
        "语料：主词典 CHI 段 %d 块 / %d 个码位%s；补充词典 %d 条 / %d 个码位；并集 %d 个"
        % (
            n_block,
            len(c_main),
            ("（已跳过）" if inputs.lang != "zh" else ""),
            n_supp,
            len(c_supp),
            len(corpus),
        )
    )
    say(
        "  %s 字集 %d 个 → 被原字体扣掉 %d 个 → 进集合 %d 个（含置顶探针字 %d 个）"
        % (
            language["label"],
            stats["n_han_punct"],
            stats["n_dropped"],
            stats["n_total"],
            len(language["test_chars"]),
        )
    )
    if stats["dropped"]:
        say("  扣掉的：%s" % " ".join("U+%04X" % c for c in stats["dropped"]))
    if stats["test_forced"]:
        say(
            "  ⚠ 测试字被黑名单命中后**强行放回**：%s"
            % " ".join("U+%04X" % c for c in stats["test_forced"])
        )
    # ★ 覆盖率红线：译文里出现过、又是汉字/中文标点、却没进集合的 ⇒ 必须是 0，
    #   否则界面上就是"译文在、字形缺"的方块。非 0 直接报错退出。
    missed = sorted({cp for cp in corpus if wanted(cp, language)} - set(codepoints))
    say(
        "  译文里出现但没进集合的码位 = %d 个%s"
        % (len(missed), ("：%s" % " ".join("U+%04X" % c for c in missed)) if missed else " ✓")
    )
    if missed:
        raise SystemExit("覆盖率不完整（%d 个码位缺字形）⇒ 拒绝出包" % len(missed))
    for c in language["test_chars"]:  # 行为探针必须在集合里
        if ord(c) not in codepoints:
            raise SystemExit("测试字『%s』(U+%04X) 不在集合里 ⇒ 探针会失效" % (c, ord(c)))

    # tms 对账（可选输入）：独立于参数表的第二来源；存在但不匹配时拒绝出包
    tms_lines, tms_missing, tms_mismatch = check_tms(inputs.tms_dir, inputs.sets)
    for line in tms_lines:
        say("  " + line)
    if tms_mismatch:
        raise SystemExit("TextureMapSpec 尺寸无法核对或与参数表不符 ⇒ 拒绝出包")

    # ③ 逐套烘字 + 出图集
    os.makedirs(inputs.out, exist_ok=True)
    packs, rows = [], []
    for font_set in inputs.sets:
        dds = os.path.join(inputs.dds_dir, font_set.dds)
        n_need = len(codepoints)
        # 图集高度按**本套实际需要的格子数**算（不是一律 2048）
        layout = fit_cell(font_set, n_need)
        canvas, recs, checks = build_atlas(
            font_set, layout, codepoints, dds, inputs.ttf, font_set.weight, W
        )
        say(
            "[%s] 格子 %dpx / 步进 %dpx / 渲染 %dpx（字重 %s） ｜ 需要 %d 字 ⇒ 图集 %dx%d"
            % (
                font_set.name,
                layout.cell,
                layout.step,
                layout.px,
                font_set.weight,
                n_need,
                W,
                layout.height,
            )
        )
        ap = os.path.join(inputs.out, "atlas_%s.rgba" % font_set.name)
        write_rgba(ap, canvas, W, layout.height)
        ba = os.path.getsize(ap)
        packs.append(
            PackSet(
                header=SetHeader(
                    fp_glyph_count=font_set.n_glyph,
                    du=fingerprints[font_set.name][0],
                    dv=fingerprints[font_set.name][1],
                    ow=font_set.ow,
                    oh=font_set.oh,
                    flags=0,
                    nw=W,
                    nh=layout.height,
                ),
                recs=tuple(recs),
            )
        )  # ★ flags bit0=图集有歧义（后端不替换）
        rows.append((font_set, layout, ap, ba, checks))
        say("")
        say(
            "[%s] 原图集 %dx%d → 新图集 %dx%d  格 %dpx / 步进 %dpx / 渲染 %dpx  起始 y=%d"
            % (
                font_set.name,
                font_set.ow,
                font_set.oh,
                W,
                layout.height,
                layout.cell,
                layout.step,
                layout.px,
                Y0,
            )
        )
        say(
            "   容量 %d 格  装入 %d 条记录  截断 %d 条  空字形 %d 个（U+3000 无字形 ⇒ 空格子 + 我们自己的 adv=格宽）"
            % (checks["cap"], checks["n_used"], len(checks["truncated"]), len(checks["blanks"]))
        )
        say(
            "   指纹 (%.6f, %.6f)"
            % (fingerprints[font_set.name][0], fingerprints[font_set.name][1])
        )
        say(
            "   自检：原图集区逐像素一致=%s；CJK 格重叠=%d；越界=%d；落顶部保留区(<y%d)=%d；压原图集区=%d；度量写规违规=%d/%d ⇒ %s"
            % (
                checks["orig_identical"],
                checks["overlap"],
                checks["out_of_bounds"],
                Y0,
                checks["in_top_reserve"],
                checks["hits_orig"],
                checks["rule_bad"],
                len(recs),
                "通过" if checks["ok"] else "**失败**",
            )
        )
        say("   文件：%s（%d 字节）" % (os.path.basename(ap), ba))
        if not checks["ok"]:
            raise SystemExit("%s 的图集自检没过 ⇒ 拒绝出包" % font_set.name)

    # ④ 清单（记录顺序 = 参数表顺序，便于人工核对）
    mp = os.path.join(inputs.out, "AC1_CJK_Glyphs.bin")
    bm = write_manifest(mp, packs)
    say("")
    # ★ 套头是 **40 B**（SET_HDR="<IIffIIIIII"，10 个 4 字节字段；与 glyph.h 的布局说明、
    #   manifest.cpp 的 static_assert(sizeof(SetHdr)==40) 同口径）。标签若写别的数就是在骗人。
    say(
        "[清单] %s：%d 套 / 记录 %d 条 / %d 字节（头 12B + 套头 40B×%d + 记录 0x20B×%d）"
        % (
            os.path.basename(mp),
            len(packs),
            sum(len(p.recs) for p in packs),
            bm,
            len(packs),
            sum(len(p.recs) for p in packs),
        )
    )

    # ⑤ 报告
    report_lines = [
        "# pack_glyphs.py 打包报告",
        "",
        "生成时间：%s" % __import__("time").strftime("%Y-%m-%d %H:%M:%S"),
        # ★ 顶部只报四套真正共享的宽与栅格起始 y：高度由 fit_cell **各自拟合**（按 em 定格子、
        #   按用量定高度），互不相同 ⇒ 一句"图集：WxH"对其中几套就是假的。
        #   逐套的真实尺寸落在下面「逐套」表的「图集」列。
        "图集：宽 %d（高度按套单独拟合，见下表），CJK 栅格起始 y=%d（顶部 %d 行留给引擎回写，理由见笔记 §3）"
        % (W, Y0, Y0),
        "语言：%s（--lang %s）" % (language["label"], inputs.lang),
        "字体：%s（%s；只有离线工具链需要 Pillow）" % (inputs.ttf, language["ttf_note"]),
        "字重映射：%s" % "、".join("%s=%s" % (s.name, s.weight) for s in inputs.sets),
        "tms 对账：%s"
        % (
            "跳过（%s）" % inputs.tms_dir
            if tms_missing
            else "%d/%d 套尺寸一致" % (len(inputs.sets), len(inputs.sets))
        ),
        "",
        "## 码位集",
        "",
        "| 项 | 数 |",
        "|---|---|",
        "| 语料并集（主词典 CHI ∪ 补充词典） | %d |" % stats["n_corpus"],
        "| 其中汉字 + 中文标点 | %d |" % stats["n_han_punct"],
        "| 被四套原字体自有码位扣掉 | %d |" % stats["n_dropped"],
        "| 进入集合（= 清单记录数/套） | %d |" % stats["n_total"],
        "| 译文里出现但没进集合（必须 0） | %d |" % len(missed),
        "",
        "## 逐套",
        "",
        "| 套 | 字重 | 原字形数 | 记录数 | 容量 | 指纹 du,dv | 图集 | 图集文件 | 字节 | 自检 |",
        "|---|---|---|---|---|---|---|---|---|---|",
    ]
    for font_set, layout, ap, ba, checks in rows:
        report_lines.append(
            "| %s | %s | %d | %d | %d | %.6f, %.6f | %dx%d | %s | %d | %s |"
            % (
                font_set.name,
                font_set.weight,
                font_set.n_glyph,
                checks["n_used"],
                checks["cap"],
                fingerprints[font_set.name][0],
                fingerprints[font_set.name][1],
                W,
                layout.height,
                os.path.basename(ap),
                ba,
                "原图集区逐像素一致/无重叠/无越界" if checks["ok"] else "**失败**",
            )
        )
    report_lines += [
        "",
        "清单：`AC1_CJK_Glyphs.bin` %d 字节（%d 套 / %d 条记录）"
        % (bm, len(packs), sum(len(p.recs) for p in packs)),
        "",
        "回读对账：`python tools\\pack_glyphs.py --verify`",
        "",
    ]
    # ★ 报告同理：裸 open(...).write(...) 把"写完"当成了默认，close 的报错被丢掉。
    rp = os.path.join(inputs.out, "pack_report.txt")
    with open(rp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(report_lines))
    say("[报告] %s" % rp)
    return 0


# ---------------------------------------------------------------- 回读对账
def do_verify(inputs):
    mp = os.path.join(inputs.out, "AC1_CJK_Glyphs.bin")
    if not os.path.isfile(mp):
        say("找不到 %s —— 先跑一次 `python tools\\pack_glyphs.py`" % mp)
        return 2
    back = parse_manifest(mp)
    say(
        "回读 %s：%d 字节 / 声明 %d 套 / 记录 %d 条"
        % (mp, back["size"], back["nsets"], back["nrecs"])
    )
    if back["errs"]:
        for e in back["errs"]:
            say("  !! %s" % e)
        return 1
    say(
        "  格式校验（按 glyph.h 的规则）：头/套头/记录偏移全部对齐，reserved=%s"
        % ("0" if all(s.reserved == 0 for s in back["sets"]) else "非 0")
    )
    bad = []
    used = set()
    meta_by_name = {}
    for i, parsed in enumerate(back["sets"]):
        du, dv = parsed.header.du, parsed.header.dv
        # 套名**不能靠下标猜**（`--only` 出的清单只有一套，套头里又没有名字）⇒ 按指纹认领：
        # 存下来的 f32 指纹是每套唯一的（du 各不相同），谁对得上就是谁。
        font_set, name = None, "套#%d" % i
        for cand in FONT_SETS:
            if cand.name in used:
                continue
            if abs(du - cand.exp_du) <= FP_TOL and abs(dv - cand.exp_dv) <= FP_TOL:
                font_set, name = cand, cand.name
                meta_by_name[name] = parsed
                break
        if font_set is None:
            bad.append("套 #%d 的指纹 (%.7f, %.7f) 认不出是哪一套" % (i, du, dv))
            continue
        used.add(font_set.name)
        # 复算指纹：从该套 MFT 的 '@' 记录重算
        calc = None
        try:
            calc = fingerprint(
                mft_parse(os.path.join(inputs.mft_dir, font_set.mft)), font_set.ow, font_set.oh
            )
        except SystemExit as ex:
            say("  [%s] MFT 读不到（%s），指纹只与期望常量比对" % (name, ex))
        if calc is not None and (du != calc[0] or dv != calc[1]):  # 逐位相等（f32），不是"差不多"
            bad.append(
                "%s 指纹复算不一致：存 (%r,%r) 算 (%r,%r)" % (name, du, dv, calc[0], calc[1])
            )
        if parsed.header.fp_glyph_count != font_set.n_glyph:
            bad.append(
                "%s fp_glyph_count=%d 期望 %d"
                % (name, parsed.header.fp_glyph_count, font_set.n_glyph)
            )
        # 独立于 MFT 的复算：du×W 必须正好是整数 w+1（见笔记 §4「格子纹素 = (w+1)×(h+1)」）
        pu, pv = du * font_set.ow, dv * font_set.oh
        if abs(pu - round(pu)) > 1e-3 or abs(pv - round(pv)) > 1e-3:
            bad.append("%s 指纹×原图集尺寸不是整数（%.4f, %.4f）" % (name, pu, pv))
        # 写规：四边形只覆盖墨迹
        #   w/h = 墨迹宽/高、xoff = 墨迹左缘(居中留字身)、adv = 格子(=CJK em)
        #   yoff = 墨迹顶到基线的距离
        # ★ 校验**度量之间的关系**而不是具体数值 —— 格子是 fit_cell 现场拟合的
        #   （见表里的 em），拿表默认 cell 去比对会误报。
        # ★★ 判据要**全量扫每一条**（rec_rule_bad，与生成期共用）：只看首条抓不到
        #   「中 正确、一/二/三 沉底」—— 首条恒是探针字『中』（满格字，yoff 恰好接近 h），
        #   而 yoff 被写成墨迹高时，满格字几乎看不出来、稀疏字才露馅。
        #   --verify 是**既有清单**（含已部署那份）唯一的对账口，覆盖不全 = 查不出来。
        bad_recs = rec_rule_bad(parsed.recs, ttf_metrics(inputs.ttf)[2])
        if bad_recs:
            detail = "; ".join(
                "#%d U+%04X w=%d h=%d xo=%d yoff=%d adv=%d 期望 yoff=%d"
                % (
                    idx,
                    parsed.recs[idx].cp,
                    parsed.recs[idx].w,
                    parsed.recs[idx].h,
                    parsed.recs[idx].xoff,
                    parsed.recs[idx].yoff,
                    parsed.recs[idx].adv,
                    want,
                )
                for idx, want in bad_recs[:5]
            )
            bad.append(
                "%s 度量写规违规 %d/%d 条：%s%s"
                % (name, len(bad_recs), len(parsed.recs), detail, " …" if len(bad_recs) > 5 else "")
            )
        r0 = parsed.recs[0]
        expected_probe = ord(lang_spec(inputs.lang)["test_chars"][0])
        if r0.cp != expected_probe:
            bad.append(
                "%s 首条不是语言探针 U+%04X 而是 U+%04X"
                % (name, expected_probe, r0.cp)
            )
        # ★ 容量必须按**本套实际的布局参数**数：cell/step 由 fit_cell 按本套记录数现场拟合、
        #   高度取清单里的 our_w/our_h。拿表默认 cell/step + 模块默认 h=2048 去数，报出来的
        #   容量必须按清单记录数和本套布局重新计算。
        parsed_set = meta_by_name.get(font_set.name)
        cap = "?"
        if parsed_set:
            try:
                layout = fit_cell(font_set, len(parsed_set.recs))
                cap = len(
                    cell_positions(
                        font_set.ow,
                        font_set.oh,
                        layout.cell,
                        layout.step,
                        parsed_set.header.nw,
                        parsed_set.header.nh,
                    )
                )
                if layout.height != parsed_set.header.nh:
                    bad.append(
                        "%s 图集高度与布局对不上：清单 our_h=%d，按本套 %d 条记录重拟合得 %d"
                        % (name, parsed_set.header.nh, len(parsed_set.recs), layout.height)
                    )
            except SystemExit:
                pass
        say(
            "  [%s] 记录 %d 条（容量 %s）指纹 (%.7f, %.7f) = (%.0f/%.0f, %.0f/%.0f) 复算一致=%s"
            % (
                name,
                parsed.count,
                cap,
                du,
                dv,
                round(du * font_set.ow),
                font_set.ow,
                round(dv * font_set.oh),
                font_set.oh,
                "是" if calc is not None and du == calc[0] and dv == calc[1] else "（无 MFT 可比）",
            )
        )
    # 图集文件对账（只对**清单里出现过的套**要求图集存在；--only 出的清单天然只有那几套）
    # ★ 期望尺寸取自**清单**（每套独立 our_w/our_h），不是固定的 1024x2048
    for font_set in FONT_SETS:
        if font_set.name not in used:
            continue
        ap = os.path.join(inputs.out, "atlas_%s.rgba" % font_set.name)
        if not os.path.isfile(ap):
            bad.append("缺图集 %s" % os.path.basename(ap))
            continue
        with open(ap, "rb") as f:
            hd = f.read(16)
        mg, aw, ah, zero = struct.unpack(ACGX_HDR, hd)
        n = os.path.getsize(ap)
        # 图集尺寸是**每套不同**的（按 em 定格子、按用量定高度）⇒ 期望值取自清单
        parsed_set = meta_by_name.get(font_set.name)
        exp = (parsed_set.header.nw, parsed_set.header.nh) if parsed_set else None
        if not exp or (aw, ah) != exp or zero != 0 or n != 16 + aw * ah * 4:
            bad.append(
                "%s 头/长度不对：%r %dx%d zero=%d 共 %d 字节"
                % (os.path.basename(ap), mg, aw, ah, zero, n)
            )
        else:
            say("  [图集] %-14s %dx%d RGBA，共 %d 字节" % (os.path.basename(ap), aw, ah, n))
    for b in bad:
        say("  !! %s" % b)
    say("对账结论：%s" % ("全部一致" if not bad else "**%d 项对不上**" % len(bad)))
    return 1 if bad else 0


# ---------------------------------------------------------------- 入口
def parse_cli():
    ap = argparse.ArgumentParser(
        description="离线图集打包器：字体原始数据 + 原图集 + 可变字重字体 → ACGG 清单 + 新图集",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="输入/输出/默认值全表见 tools\\README.md；退出码：0 成功 / 1 校验失败 / 2 用法或输入错误。",
    )
    ap.add_argument(
        "--mft-dir",
        default=os.path.join(INPUT_DIR, "mft-dir"),
        help="四个 .MagmaMftFile 所在目录（默认 tools\\input\\mft-dir）",
    )
    ap.add_argument(
        "--dds-dir",
        default=os.path.join(INPUT_DIR, "dds-dir"),
        help="四张 *_Map.dds 所在目录（默认 tools\\input\\dds-dir）",
    )
    ap.add_argument(
        "--dict",
        "--dict-file",
        "--supplement",
        dest="supplement",
        help="★ **那一份词典**（UTF-8，ENG<TAB>译文）。语料与译文的唯一来源，"
        "与游戏的 LG 词典**完全解耦**。默认 data\\dict.txt",
    )
    ap.add_argument(
        "--lg-dict",
        dest="lg_dict",
        help="游戏的 LGCStringDict_01.txt（CP950）。**可选**，"
        "只有 --lang zh 才会读：它白送 1496 条繁体中文译文；"
        "ja/ko 读它只会把几千个汉字塞进字集。"
        "默认 tools\\input\\LGCStringDict_01.txt",
    )
    ap.add_argument(
        "--lang",
        default="zh",
        choices=tuple(LANGS),
        help="目标语言：%s。**只影响打包（字集判据 / 探针字 / 默认字体），"
        "不影响引擎**（引擎侧与语言无关，反汇编已定案）。"
        "硬墙：超 BMP（emoji、CJK 扩展 B+）打不出来。"
        % " / ".join("%s=%s" % (k, v["label"]) for k, v in LANGS.items()),
    )
    ap.add_argument(
        "--ttf",
        help="**可变字重**字体文件（fvar/wght，如 NotoSansSC-VariableFont_wght.ttf）；"
        "title/bold/textpc/techno 分别实例化 Thin/Bold/Regular/Regular。"
        "不给则用该语言的默认候选（见 LANGS[].ttf_candidates；zh = tools\\input\\ 下那份 VF）",
    )
    ap.add_argument(
        "--tms-dir",
        default=os.path.join(INPUT_DIR, "tms"),
        help="TextureMapSpec 目录（可选）：独立对账四套原图集尺寸（默认 tools\\input\\tms；缺了不阻塞）",
    )
    ap.add_argument(
        "--out",
        default=os.path.join(TOOLS_DIR, "out", "glyphs"),
        help="产物目录（默认 tools\\out\\glyphs）。本工具只写这里",
    )
    ap.add_argument("--only", default="", help="只出这几套，逗号分隔（如 textpc,bold）")
    ap.add_argument("--self-check", action="store_true", help="内置小夹具自检，不需要真数据")
    ap.add_argument("--verify", action="store_true", help="回读刚生成的清单并对账")
    return ap.parse_args()


def main():
    args = parse_cli()

    if args.self_check:
        with tempfile.TemporaryDirectory() as td:
            return self_check(td)

    inputs = resolve_inputs(args)

    say("打包器：离线图集 → AC1_CJK_Glyphs.bin + %d 套 atlas_<name>.rgba" % len(inputs.sets))
    say("  MFT：%s" % inputs.mft_dir)
    say("  DDS：%s" % inputs.dds_dir)
    say("  词典：%s + %s" % (inputs.lg_dict, inputs.supplement))
    say(
        "  字体：%s（字重：%s）"
        % (inputs.ttf, "、".join("%s=%s" % (s.name, s.weight) for s in inputs.sets))
    )
    say("  输出：%s" % inputs.out)
    say("")
    return do_verify(inputs) if args.verify else do_pack(inputs)


if __name__ == "__main__":
    sys.exit(main())
