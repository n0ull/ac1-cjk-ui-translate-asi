"""离线词典查询与不译规则；生效键值由生产 C++ oracle 逐项对账。"""

import os

# 只用于拒绝误传的 LG 输入，不解析其块或码页。
S_ENG = b"xfhsm_res_ENG_Start"
S_CHI = b"xfhsm_res_CHI_Start"
DICT_KEY_MAX = 4096
BS = chr(92)


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
    r"""4 个字符全是十六进制数才返回码元，否则 None。

    ★ 不能用 int(chars, 16)：它接受首尾空白、正负号与下划线（`\x 019` / `\x+123` / `\x1_23`
      会被它当转义吃掉）；运行期转义解码拒绝这些写法、按字面保留。
    """
    if len(chars) != 4 or any(c not in "0123456789abcdefABCDEF" for c in chars):
        return None
    return chr(int(chars, 16))


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


def trim_value(value):
    """运行期值规整：只去首尾空白，保留译文内部换行与空格。"""
    return value.strip(" \t\r\n")


def load_supplement(path):
    """UTF-8 TSV → 查询候选：键规范化，值先裁剪源行边缘空白、再解转义。

    解转义后的空白值仍可保留；是否生效由原生查询对账决定，与审计拒收规则分开。

    ⚠ 路径不可用**不许静默降级**（那会让依赖它的判据整桶恒为空，而报告看着仍然完整 ——
      读者看不出这一路判据根本没跑）。
      路径不可用一律 raise SystemExit。
    """
    m = {}
    if not path or not os.path.isfile(path):
        print("用户词典读不到：%s" % (path or "（路径为空）"))
        raise SystemExit(2)
    # 这里只枚举查询键；坏行是否导致整份拒绝，由生产 oracle 决定。
    # 不替换非法 UTF-8，避免向查询集合注入 U+FFFD。
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
        if not k or not v:
            continue
        # 查询候选不收超长键；运行时还会因容量失败拒绝整份输入，由 oracle 最终判定。
        if len(k) > DICT_KEY_MAX:
            continue
        m[k] = v
    return m


def load_notranslate(path):
    """→ [(模式, 原因)]；模式以 '*' 结尾 = 前缀匹配（如 `OBSOLETE: *`）。

    路径不可用一律 raise SystemExit。
    """
    rules = []
    if not path or not os.path.isfile(path):
        print("不译清单读不到：%s" % (path or "（路径为空）"))
        raise SystemExit(2)
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


def looks_like_lg_dict(path):
    """检查文件头窗口中的 LG 标记，避免把 LG 输入当作 UTF-8 TAB 词典。"""
    if not path or not os.path.isfile(path):
        return False
    with open(path, "rb") as f:
        head = f.read(1 << 20)
    return S_ENG in head or S_CHI in head
