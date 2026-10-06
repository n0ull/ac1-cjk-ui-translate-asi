// modules/dict/src/fallback.cpp —— 词典兜底规整的实现
//
// 口径写在 include/ac1/dict/fallback.h 顶上，这里只讲怎么改才不出事。
//
// ===== 为什么是「两趟」=====
// 第一趟只算新长度、不写任何字节；第二趟才真正搬。
// 只做一趟的话，调用方拿不到长度就得先猜容量 —— 而兜底规则会变长
// （`…` 1 → `...` 3），猜小了写溢出、猜大了白占。
//
// 两趟分离还顺带保证了 -1 时 buf 一个字节都没动：
//   第一趟只读，所以「装不下」这条早退路径天然是干净的。
//   （早退判定全部落在第一趟里：len<0 / len>cap / 展开后>cap。）
//
// ===== 为什么第二趟可以从后往前就地展开 =====
// 就地往后扩展会覆盖还没读的数据；从前往后就地扩展不可能（要凭空多出 2 格）。
// **前提：extra ≥ 0**（每条规则 toLen ≥ 1；空替换会让 j 掉到 i 之下并越界，
//   已在 fallback_apply 的规则命中处直接拒绝）。在此前提下从后往前是安全的，证明：
//   设 len=原长、extra=总增量、emitted=已写出的增量、某一步开始时（`i--` 之后）i_new。
//   不变量：j = i + extra - emitted（j = 下一个要写的下标，i = 下一个要读的下标）。
//     不变量成立 ⇒ 写位置 j-1 ≥ i。
//   于是一步之内：先读 buf[i]（拿到原值），再写 j-1 / j-2 / j-3。
//   写的位置全部 ≥ i，而 buf[i] 已在写之前读走 ⇒ 永远不会覆盖未读的数据。
//   唯一「写回同一格」的情形（写位置正好 = i）也是安全的，因为先读后写。
// 一句话：从后往前 + 先读后写，就地展开不需要任何暂存缓冲。

#include "ac1/dict/fallback.h"

#include <stdio.h>  // _snprintf（fallback_status 用）
#include <string.h> // wcslen（规则长度现场取）

namespace ac1 {
namespace dict {

namespace {

struct Rule {
    wchar_t        from; // 命中的码位
    const wchar_t* to;   // 换成的写法
    const char*    name;
    const char*    why; // 为什么必须换 —— 写给下一个读代码的人
};

// 加一条兜底 = 加一行这里 + 一条单测。别的地方都不该动。
//   to 的长度现场用 wcslen 取（不手写长度字段：写大了会越界读 to 并把 NUL 写进译文，
//   写小了会少写字符 —— 两种漂移都不可接受）。
const Rule kRules[] = {
    { 0x2026, L"...", "省略号 U+2026",
      "四套原字体里只有 title 自带 U+2026，而它指向的是**育碧的装饰图标**"
      "（实测：解四套 MagmaMftFile，只有 title 的 charmap 含该码位）。"
      "不接管该码位，改成英文原文本来就在用的 `...`。" },
};

const int kRuleCount = (int)(sizeof(kRules) / sizeof(kRules[0]));

// DictStats 的逐条计数数组按 FALLBACK_MAX_RULES 定长读（dict.cpp 的 apply_fallback），
//   规则数超过它 ⇒ 多出来的规则静默丢统计（规则本身仍生效，更难发现）。要加规则就同步抬上限。
static_assert(kRuleCount <= FALLBACK_MAX_RULES,
              "规则数 > FALLBACK_MAX_RULES：DictStats 装不下逐条计数，请抬 fallback.h 的上限");

int g_hits[kRuleCount] = { 0 };

} // namespace

int fallback_rule_count() { return kRuleCount; }

const char* fallback_rule_name(int i) { return (i >= 0 && i < kRuleCount) ? kRules[i].name : "?"; }

int fallback_hits(int i) { return (i >= 0 && i < kRuleCount) ? g_hits[i] : 0; }

int fallback_apply(wchar_t* buf, int len, int cap)
{
    if (!buf || cap < 0) return -1;
    if (len < 0) { // 按 NUL 自行算长度
        len = 0;
        // 容量检查必须先于解引用：前 cap 个码元都非 NUL 时，停在 len==cap 直接拒，
        //   绝不去读 buf[cap]（那是缓冲外的一个 wchar_t）。
        while (len < cap && buf[len] != 0) len++;
        if (len >= cap) return -1; // 没有 NUL 且装不下 ⇒ 直接拒
    }
    if (len > cap) return -1;

    // ---- 第一趟：只算长度、顺带数命中次数，一个字节都不写 ----
    // 短路判据是「有没有规则命中」，不是「净增量是否为 0」：等长替换（to 与 from 等长）的
    //   净增量恒为 0，按增量短路会让它永不生效、也不计数（同一串里还要看别的规则脸色）。
    int extra = 0, nHits = 0;
    for (int i = 0; i < len; i++) {
        for (int r = 0; r < kRuleCount; r++) {
            if (buf[i] == kRules[r].from) {
                const int toLen = (int)wcslen(kRules[r].to);
                // 规则表只许「替换成非空写法」：空串会让 extra 变负，而第二趟从后往前就地
                //   展开的证明前提是 extra ≥ 0（写水位 j 不会掉到读指针之下）——删字符
                //   需要前向 + 暂存，本实现不支持，必须在规则表处挡住。
                if (toLen < 1) return -1;
                if (toLen > cap) return -1; // 单条替换就超容量 ⇒ 装不下（也让 extra 不可能回绕）
                extra += toLen - 1;
                nHits++;
                break;
            }
        }
    }
    if (nHits == 0) return len; // 没有任何规则命中 ⇒ 不用动

    const int newlen = len + extra;
    // extra ≥ 0 由上面「toLen < 1 ⇒ 拒」保证；这两道闸是防御性兜底（长度账不平就不动 buf）。
    if (newlen < 0) return -1;
    if (newlen > cap) return -1; // 装不下 ⇒ 早退，buf 仍原样

    // ---- 第二趟：从后往前就地展开（先读后写，见文件头的证明）----
    int j = newlen;
    for (int i = len - 1; i >= 0; i--) {
        const wchar_t c = buf[i];
        int           hit = -1;
        for (int r = 0; r < kRuleCount; r++) {
            if (c == kRules[r].from) {
                hit = r;
                break;
            }
        }
        if (hit < 0) { buf[--j] = c; }
        else {
            const Rule& R = kRules[hit];
            const int   toLen = (int)wcslen(R.to);
            for (int t = toLen - 1; t >= 0; t--) buf[--j] = R.to[t];
            g_hits[hit]++;
        }
    }
    return newlen;
}

const char* fallback_status()
{
    static char s[256];
    int         o = _snprintf(s, sizeof(s), "兜底规整 %d 条规则：", kRuleCount);
    if (o < 0) o = 0;
    for (int r = 0; r < kRuleCount && o + 24 < (int)sizeof(s); r++) {
        // 不把返回值直接当偏移：截断时 MSVC 返回负数（见 core/str.h 的工具链陷阱），
        //   负增量会让写位置回退、反复重写同一段。
        const int w =
            _snprintf(s + o, sizeof(s) - o, "%s%s 命中 %d", r ? "｜" : "", kRules[r].name, g_hits[r]);
        if (w < 0) break;
        o += w;
    }
    s[o] = 0;
    return s;
}

} // namespace dict
} // namespace ac1
