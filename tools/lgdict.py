# tools/lgdict.py —— 词典文本层：与 modules/dict/src/dict.cpp 对齐的解码/规整
#
# 为什么单独一份：pack_glyphs / classify_inventory / make_dict_template 都要按运行期
#   同一套判据读 LG 词典；判据分家 = 静默漂移（三个工具曾各写一份 _chi_decode，写法
#   还不一致）。这里是唯一实现，工具侧只许 import，不许再抄。
#   「与 dict.cpp 同构」的实证注释集中在本文件；改判据先改这里、跑 self_check，
#   再跑各工具的 --self-check。
#
# 接口（刻意小）：
#   canon / hex4 / unescape_value / expand_hex_escapes   —— 键与值的规整/转义
#   decode_chi / decode_eng / trim_bytes                —— LG 词典两列的字节级解码
#   iter_blocks / iter_paired_blocks                    —— 成对块扫描（镜像 load_main）
#   load_supplement / load_notranslate                  —— 那一份词典 / 不译清单
#   self_check                                          —— 夹具自检（不需要真数据）
#
# 编码纪律：%-format（与 C++ 侧 _snprintf 同形，见 pyproject.toml 的 UP031 例外）。

import os
import re

# LG 词典的成对块标记（资源名；_02 变体装的是 DBCS 码位对，不是自然语言）
S_ENG, E_ENG = b"xfhsm_res_ENG_Start", b"xfhsm_res_ENG_End"
S_CHI, E_CHI = b"xfhsm_res_CHI_Start", b"xfhsm_res_CHI_End"

DICT_KEY_MAX = 4096  # 见 modules/dict/include/ac1/dict/dict.h

BS = chr(92)  # 反斜杠（写成 chr，避免本文件注释里的转义层把 \\x 解释两次）
ESC = (BS + "x").encode()  # b"\x"
HEX_ESC_RE = re.compile(r"\\x([0-9A-Fa-f]{4})")


# ---------------------------------------------------------------- 键与值的规整/转义
def canon(s):
    """按运行期规则折叠空白（space/tab/CR/LF）并去首尾。"""
    out = []
    pend = False
    for ch in s:
        if ch in " \t\r\n":
            if out:
                pend = True
            continue
        if pend:
            out.append(" ")
            pend = False
        out.append(ch)
    return "".join(out)


def hex4(chars):
    r"""4 个字符全是十六进制数才返回码元，否则 None（严格对齐 dict.cpp 的 hex4）。

    ★ 不能用 int(chars, 16)：它接受首尾空白、正负号与下划线（`\x 019` / `\x+123` / `\x1_23`
      会被它当转义吃掉），而运行期的 hex4 一律拒绝、按字面保留。
    """
    if len(chars) != 4 or any(c not in "0123456789abcdefABCDEF" for c in chars):
        return None
    return chr(int(chars, 16))


def expand_hex_escapes(s):
    r"""只展开 `\xNNNN`（严格 4 位十六进制），其余原样 —— 语料/模板键等"只认码元转义"的场合用。"""
    return HEX_ESC_RE.sub(lambda m: chr(int(m.group(1), 16)), s)


def unescape_value(s):
    r"""\xNNNN = 一个 UTF-16 码元；`\n` / `\t` / `\\` 与 dict.cpp 的 unescape_value 同义。

    ★ `\\` 必须最先判（否则 `\n` 会被读成「反斜杠 + n」）—— 与运行期同一顺序纪律。
    """
    out = []
    i = 0
    while i < len(s):
        if s[i] == BS and i + 1 < len(s):
            c = s[i + 1]
            if c == BS:
                out.append(BS)
                i += 2
                continue
            if c == "n":
                out.append("\n")
                i += 2
                continue
            if c == "t":
                out.append("\t")
                i += 2
                continue
            if c == "x" and i + 5 < len(s):
                v = hex4(s[i + 2 : i + 6])
                if v is None:
                    out.append(s[i])
                    i += 1
                    continue
                out.append(v)
                i += 6
                continue
        out.append(s[i])
        i += 1
    return "".join(out)


# ---------------------------------------------------------------- 字节级解码
def trim_bytes(raw):
    a, b = 0, len(raw)
    while a < b and raw[a : a + 1] in (b" ", b"\t", b"\r", b"\n"):
        a += 1
    while b > a and raw[b - 1 : b] in (b" ", b"\t", b"\r", b"\n"):
        b -= 1
    return raw[a:b]


def trim_value(value):
    """运行期值规整：只去首尾空白，保留译文内部换行与空格。"""
    return value.strip(" \t\r\n")


def decode_chi(raw, preserve_escapes=False, strict=False):
    r"""CHI 侧：\xNNNN 转义 + 其余按 CP950 解码（绝不用 CP_ACP）。

    逐字节推进，与 dict.cpp 的 expand_big5 对齐：转义只在「字符起点」判；高位字节必须
    先按双字节吃掉（Big5 低位字节 0x40..0x7E 含 0x5C ⇒ 尾字节 0x5C 的合法字，如
    功/蓋，要在 `\x` 判据之前被配对消费，否则会被拆成「替换符 + 字面反斜杠」）。
    解不出落 '?'（与运行期 MB_ERR_INVALID_CHARS 的 '?' 兜底一致）。

      ⚠ Python 的 cp950 与 Win32 的 CP950 并非全等；此外，非法双字节按运行期规则整对
      消费并落 '?'。当前 LG 数据（1496 块）逐字符一致；换数据源后必须重跑一致性比对。
    """
    out, i, n = [], 0, len(raw)
    while i < n:
        if raw[i : i + 2] == ESC and i + 6 <= n:
            v = hex4(raw[i + 2 : i + 6].decode("latin-1"))
            if v is not None:
                out.append(raw[i : i + 6].decode("latin-1") if preserve_escapes else v)
                i += 6
                continue
        if raw[i] < 0x80:
            out.append(chr(raw[i]))
            i += 1
            continue
        if raw[i] == 0x80 and i + 1 == n:
            out.append("\u0080")
            i += 1
            continue
        need = 2 if i + 2 <= n else 1
        try:
            out.append(raw[i : i + need].decode("cp950"))
        except UnicodeDecodeError:
            if strict:
                raise
            out.append("?")
        i += need
    return "".join(out)


def decode_eng(raw):
    r"""**主词典** ENG 列：\xNNNN 转义 + 其余字节按 ASCII 取 —— 逐字镜像
    modules/dict/src/dict.cpp 的 expand_ascii（**只** load_main 那一路）。

    ⚠ 别把它当「那一份词典的键解码器」：补充词典（data/dict.txt → AC1_Dict.txt）的键
      走的是 unescape_value（文件整体按 UTF-8 读，键与译文共用一条路）。本函数只属于
      LG 主词典的 ENG 列 —— 在这里修"键的解码问题"会修错地方。
    """
    out, i = [], 0
    while i < len(raw):
        if raw[i : i + 2] == ESC and i + 6 <= len(raw):
            v = hex4(raw[i + 2 : i + 6].decode("latin-1"))
            if v is not None:
                out.append(v)
                i += 6
                continue
        out.append(chr(raw[i]))  # 非法/不完整转义 ⇒ 按普通字节原样保留（= 运行期 expand_ascii）
        i += 1
    return "".join(out)


# ---------------------------------------------------------------- 成对块扫描
def iter_blocks(data, start, end):
    """按 start…end 标记切出所有块的原文（bytes 切片，未去空白）。"""
    pos = 0
    while True:
        a = data.find(start, pos)
        if a < 0:
            return
        a += len(start)
        e = data.find(end, a)
        if e < 0:
            return
        yield data[a:e]
        pos = e + len(end)


def iter_paired_blocks(data):
    """逐块产出 (eng_raw, chi_raw)；chi_raw=None 表示本块缺 CHI 侧。

    窗口规则与 dict.cpp 的 load_main 同构：CHI 只许在本块窗口内找，下一块的
    ENG_Start（没有则 EOF）是硬上界；缺侧 ⇒ 本块记无译文，**不跳块、不偷下一块**。
    """
    pos = 0
    while True:
        a = data.find(S_ENG, pos)
        if a < 0:
            return
        as_ = a + len(S_ENG)
        e = data.find(E_ENG, as_)
        if e < 0:
            return
        nxt = data.find(S_ENG, e)
        lim = len(data) if nxt < 0 else nxt
        c_s = data.find(S_CHI, e, lim)
        c_e = -1 if c_s < 0 else data.find(E_CHI, c_s + len(S_CHI), lim)
        pos = (e + 1) if c_e < 0 else (c_e + len(E_CHI))
        chi_raw = None if c_e < 0 else data[c_s + len(S_CHI) : c_e]
        yield data[as_:e], chi_raw


# ---------------------------------------------------------------- 两份词典文件
def load_supplement(path, optional=False):
    """→ map: canon 键 → canon 值（与 dict.cpp 的 load_supplement 同构：UTF-8、首个 TAB、
    值去首尾空白）。

    ⚠ 路径不可用**不许静默降级**（那会让依赖它的判据整桶恒为空，而报告看着仍然完整 ——
      读者看不出这一路判据根本没跑）。
      optional=True 只留给「仓库里本来就还没有这个文件」：调用方必须显式警告，
      并把缺失写进产物（报告头等）；其余一律 raise SystemExit。
    """
    m = {}
    if not path or not os.path.isfile(path):
        if not optional:
            print("用户词典读不到：%s" % (path or "（路径为空）"))
            raise SystemExit(2)
        return m
    # 逐行严格 UTF-8 解码：运行期用 MB_ERR_INVALID_CHARS，非法行整行跳过 —— 离线这侧
    #   若用 errors="replace"，那行会以 U+FFFD 进表，把桶判定带偏（运行期根本查不到它）。
    #   行切分只按 `\n`（运行期只认 '\n'，`\r` 属行尾空白）；BOM 只在文件头跳一次。
    if os.path.getsize(path) > 16 * 1024 * 1024:  # 镜像 core::file_read_all（超限整份拒读）
        raise SystemExit("用户词典超过 16 MB：运行期会整份拒读（上限见 core/file.h）")
    with open(path, "rb") as f:
        raw = f.read()
    first = True
    for bline in raw.split(b"\n"):
        if first:
            first = False
            if bline.startswith(b"\xef\xbb\xbf"):
                bline = bline[3:]  # UTF-8 BOM
        try:
            line = bline.decode("utf-8")
        except UnicodeDecodeError:
            continue
        s = line.rstrip("\r").strip(" \t")
        # 判据与运行期一致：含 TAB 的行就是数据（`#` 开头的真实 UI 串要参与判定）
        if not s or "\t" not in s:
            continue
        k, v = s.split("\t", 1)
        k = canon(unescape_value(k.strip(" \t")))
        v = unescape_value(v.strip(" \t\r\n"))
        if len(v) > 8192:
            continue
        if not k or not v or has_cjk(k):
            continue
        # 镜像运行期上限（dict.cpp：键 canon 后 ≤DICT_KEY_MAX；值解码 ≤8192 码元）——
        #   超长的行运行期整行丢弃，离线若收进来会报出「用户词典已有该键」的假回填。
        if len(k) > DICT_KEY_MAX:
            continue
        m[k] = v
    return m


def load_notranslate(path, optional=False):
    """→ [(模式, 原因)]；模式以 '*' 结尾 = 前缀匹配（如 `OBSOLETE: *`）。

    ⚠ 同 load_supplement：路径不可用一律 raise SystemExit；optional=True 仅供
      「仓库默认文件还没建」的降级，且必须由调用方显式警告 + 写进产物。
    """
    rules = []
    if not path or not os.path.isfile(path):
        if not optional:
            print("不译清单读不到：%s" % (path or "（路径为空）"))
            raise SystemExit(2)
        return rules
    with open(path, encoding="utf-8-sig", errors="replace") as f:
        lines = f.read().splitlines()
    for line in lines:
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        pat, why = (s.split("\t", 1) + [""])[:2] if "\t" in s else (s, "")
        pat, why = pat.strip(), why.strip()
        if pat:
            rules.append((pat, why))
    return rules


def has_cjk(s):
    """键里出现 CJK 码位 ⇒ 不是英文键（运行期同样拒收）。"""
    return any(ord(c) >= 0x3000 for c in s)


def looks_like_lg_dict(path):
    """在文件头窗口内找到 LG 块标记 ⇒ True。专门抓 `--dict` / `--lg-dict` 传反（两者的输入都是
    "词典文件"，传反时静默读错比报错危险得多）。"""
    if not path or not os.path.isfile(path):
        return False
    with open(path, "rb") as f:
        head = f.read(1 << 20)
    return S_ENG in head or S_CHI in head


# ---------------------------------------------------------------- 自检
def self_check():
    """纯内存夹具自检：canon / 转义 / CHI 与 ENG 解码 / 成对块窗口。返回 0 或 1。"""
    bad = []

    if canon("  a \t b\r\nc  ") != "a b c":
        bad.append("canon 折叠不对：%r" % canon("  a \t b\r\nc  "))
    if hex4("4E2D") != "\u4e2d" or hex4("4E2Z") is not None or hex4(" 123") is not None:
        bad.append("hex4 判据不对（严格 4 位十六进制）")
    if unescape_value(BS + BS + "n") != BS + "n":
        bad.append("unescape 的 \\\\ 分支不对（\\\\n 应是字面反斜杠+n）")
    if unescape_value("a" + BS + "x4E2D") != "a\u4e2d":
        bad.append("\\xNNNN 展开不对")
    if expand_hex_escapes("q" + BS + "x00A4r") != "q\u00a4r":
        bad.append("expand_hex_escapes 不对")

    # Big5 尾字节 0x5C 的合法字（功 = A5 5C）必须整对吃掉，不能被 \x 判据拆开
    if decode_chi(b"\xa5\x5c") != "\u529f":
        bad.append("decode_chi 没整对吃掉 0x5C 尾字节：%r" % decode_chi(b"\xa5\x5c"))
    if decode_chi(ESC + b"4e2d") != "\u4e2d":
        bad.append("decode_chi 的 \\xNNNN 展开不对")
    if decode_chi(ESC + b"4e2d", preserve_escapes=True) != "\\x4e2d":
        bad.append("decode_chi 保留转义不对")
    if decode_chi(b"\xff\xff") != "?":
        bad.append("decode_chi 解不出应回 '?'")
    if decode_chi(b"\x80\xa5\x5c") != "?\\":
        bad.append("decode_chi 的 0x80 后配对不对")
    if decode_eng(b"\\x12G4") != "\\x12G4" or decode_eng(b"\\") != "\\":
        bad.append("decode_eng 非法/不完整转义未原样保留")
    if decode_eng(b"A" + ESC + b"0042") != "AB":
        bad.append("decode_eng 的 \\xNNNN 展开不对")

    data = S_ENG + b"\nHELLO\n" + E_ENG + b"\n" + S_CHI + b"\n" + ESC + b"4e2d\n" + E_CHI
    pairs = list(iter_paired_blocks(data))
    if pairs != [(b"\nHELLO\n", b"\n" + ESC + b"4e2d\n")]:
        bad.append("iter_paired_blocks 配对不对：%r" % (pairs,))
    # 缺 CHI 侧 ⇒ None；下一块窗口内的 CHI 不能回填前一块。
    data2 = S_ENG + b"\nA\n" + E_ENG + S_CHI + b"\nX\n" + E_CHI + S_ENG + b"\nB\n" + E_ENG
    pairs2 = list(iter_paired_blocks(data2))
    if len(pairs2) != 2 or pairs2[0][1] != b"\nX\n" or pairs2[1][1] is not None:
        bad.append("iter_paired_blocks 缺侧处理不对：%r" % (pairs2,))
    data3 = (
        S_ENG + b"\nA\n" + E_ENG + S_ENG + b"\nB\n" + E_ENG
        + S_CHI + b"\nX\n" + E_CHI
    )
    pairs3 = list(iter_paired_blocks(data3))
    if pairs3 != [(b"\nA\n", None), (b"\nB\n", b"\nX\n")]:
        bad.append("iter_paired_blocks 窗口边界不对：%r" % (pairs3,))

    if bad:
        for b in bad:
            print("[lgdict 自检] 失败：" + b)
        print("[lgdict 自检] **%d 项失败**" % len(bad))
        return 1
    print("[lgdict 自检] 通过：canon / hex4 / 转义 / Big5 0x5C 尾字节 / 成对块窗口")
    return 0
