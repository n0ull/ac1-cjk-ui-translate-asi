// modules/dict/src/dict.cpp
//
// 词典层实现。模块边界与依赖约定见 ac1/dict/dict.h 顶部：本模块只依赖 core 的
// 纯 Win32 原语（file.h 整文件读取、str.h 安全累加器），日志走出口注入、
// 不 include core 的 log.h。
//
// 内部结构：两个解析器（LG 成对块 / UTF-8 TSV）只负责把字节解码成键值对；
//   入库全部走统一的 ingest_one 管道（canon → 校验 → 兜底 → 入池 → 插表 → 回滚）。
//   跳过/覆盖的统计口径两条路径不同，由各自 loader 壳把管道结果码映射到 DictStats。
//
//   下面所有文件级辅助函数都带 static（内部链接）⇒ 不会和别的模块的符号撞名。

#include "ac1/dict/dict.h"
#include "ac1/dict/fallback.h"
#include "ac1/core/file.h"
#include "ac1/core/str.h"

#include <windows.h>
#include <sal.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

namespace ac1 {
namespace dict {

// ---- 日志出口（不依赖 core 的关键）----
static LogFn g_log = NULL;
static LogFn g_warnLog = NULL;

void set_log(LogFn fn) { g_log = fn; }
void set_warn_log(LogFn fn) { g_warnLog = fn; }

static DictStats g_st = { 0 }; // 载入统计（expand_big5 的落'?'计数等也在里面）

// ======================================================================
// 格式串安全网
//
// 背景：MSVC 19.44 实测——SAL 注解抓不到"实参不足"：
//   · 注解加在函数指针 typedef 上 ⇒ 调用点根本不校验
//   · 注解加在具名函数上 + /analyze ⇒ 能报 C6271(实参过多)、C6273(类型不符)，
//     但报不出"实参不足"：只要那个函数自己用 va_start/_vsnprintf 消费了 fmt，
//     分析器就认为格式串"已校验过"，不再回头看调用点。唯一能触发 C6064 的形状是
//     "注解函数完全不用 fmt"，而那样的函数没法真的打日志。
//   ⇒ 只能对"实参不足"另设一道网。
//
// 现在的两道网（互补，各管一半）：
//   ① static_assert 数转换符 vs 数实参   → 抓实参不足与实参过多
//   ② dlog_impl 的 _Printf_format_string_ + /analyze → 抓类型不符（C6273）与实参过多
//
// ①的实现要点：
//   · count_conv 只接受字面量，在常量表达式里数转换符（%% 不计，%* 与 %.* 各吃一个实参）
//   · 实参个数用经典的 DLOG_NARG 预处理技巧数 —— 这个技巧在 MSVC 的传统预处理器下
//     恒返回 1，必须配 /Zc:preprocessor（dict\build.ps1 里带了它）。
//   · DLOG 的实参不允许有副作用（会被 static_assert 那次求值 + 实际那次各求一次）。
//     本文件所有调用点传的都是结构体字段/常量，满足。
// ======================================================================
constexpr bool is_len_mod(char c)
{
    return c == 'h' || c == 'l' || c == 'L' || c == 'q' || c == 'j' || c == 'z' || c == 't' || c == 'I' ||
           c == 'w' || c == 'b';
}
constexpr bool is_flag(char c)
{ return c == '-' || c == '+' || c == ' ' || c == '#' || c == '0' || c == '\''; }
constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

// 数一个格式串字面量里的转换符个数
constexpr int count_conv(const char* s, int n)
{
    int c = 0, i = 0;
    while (i < n && s[i]) {
        if (s[i] != '%') {
            ++i;
            continue;
        }
        ++i;
        if (i < n && s[i] == '%') {
            ++i;
            continue;
        } // %% = 字面百分号
        while (i < n && is_flag(s[i])) ++i; // - + 空格 # 0 '
        if (i < n && s[i] == '*') {
            ++i;
            ++c;
        } // %* 吃一个实参
        else
            while (i < n && is_digit(s[i])) ++i; // 宽度
        if (i < n && s[i] == '.') {              // 精度
            ++i;
            if (i < n && s[i] == '*') {
                ++i;
                ++c;
            }
            else
                while (i < n && is_digit(s[i])) ++i;
        }
        while (i < n && is_len_mod(s[i])) ++i; // 长度修饰 h l L q j z t ...
        if (i < n && s[i]) {
            ++c;
            ++i;
        } // 转换字符本身
        else
            break; // 串尾（格式串写坏了，人工兜住）
    }
    return c;
}
constexpr int count_conv(const char* s)
{
    int n = 0;
    while (s[n]) ++n;
    return count_conv(s, n);
}

// ②真正的日志函数：注解让 /analyze 校验调用点的实参类型（配合 dict\build.ps1 的 /analyze）
static void dlog_impl(_Printf_format_string_ const char* fmt, ...)
{
    char    buf[2048];
    va_list ap;
    buf[0] = 0; // CRT 一个字节都没写就返回负数时（非法格式串），消息体必须是空串而不是栈垃圾
    va_start(ap, fmt);
    int n = _vsnprintf(buf, (int)sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    // MSVC 的 _vsnprintf 截断时把 count 字节写满、不补 NUL、返回负数（见 core/str.h 的
    //   工具链陷阱）⇒ 两条路都自己补 NUL；只清 buf[0] 会把整条消息丢掉、只剩前缀。
    if (n < 0 || (size_t)n >= sizeof(buf) - 1) buf[sizeof(buf) - 2] = 0;
    if (g_log) g_log("%s", buf);
}

// ①取出 __VA_ARGS__ 的第 1 个（格式串字面量），供 static_assert 用
#define DLOG_FMT1(_1, ...) _1
// ①数 __VA_ARGS__ 的实参个数（支持到 16 个）。必须配 /Zc:preprocessor。
#define DLOG_NARG(...) DLOG_NARG_(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1)
#define DLOG_NARG_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N

// ①编译期对账：转换符个数必须 == 实参个数 - 1（减掉格式串本身）
#define DLOG_CHK(fmt, n)                                                                                     \
    static_assert(count_conv(fmt) == (n) - 1,                                                                \
                  "DLOG: 格式串的转换符个数与实参数量不符（少给/多给都会在这里编译失败）")

#define DLOG(...)                                                                                            \
    do {                                                                                                     \
        DLOG_CHK(DLOG_FMT1(__VA_ARGS__, 0), DLOG_NARG(__VA_ARGS__));                                         \
        if (g_log) dlog_impl(__VA_ARGS__);                                                                   \
    } while (0)

// WARN 通道（降级运行：词典未就绪）。WARN 出口未接时回退到 INFO 出口，
// 保证「只接 set_log」的单测/第三方照样能看到这行。
static void dwarn_impl(_Printf_format_string_ const char* fmt, ...)
{
    char    buf[2048];
    va_list ap;
    buf[0] = 0; // CRT 一个字节都没写就返回负数时（非法格式串），消息体必须是空串而不是栈垃圾
    va_start(ap, fmt);
    int n = _vsnprintf(buf, (int)sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    // MSVC 的 _vsnprintf 截断时把 count 字节写满、不补 NUL、返回负数（见 core/str.h 的
    //   工具链陷阱）⇒ 两条路都自己补 NUL；只清 buf[0] 会把整条消息丢掉、只剩前缀。
    if (n < 0 || (size_t)n >= sizeof(buf) - 1) buf[sizeof(buf) - 2] = 0;
    if (g_warnLog)
        g_warnLog("%s", buf);
    else if (g_log)
        g_log("%s", buf);
}
#define DWARN(...)                                                                                           \
    do {                                                                                                     \
        DLOG_CHK(DLOG_FMT1(__VA_ARGS__, 0), DLOG_NARG(__VA_ARGS__));                                         \
        if (g_warnLog || g_log) dwarn_impl(__VA_ARGS__);                                                     \
    } while (0)

// ================= 小工具 =================
static bool is_ws(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r'; }

// 匹配策略：只做 canon（空白折叠）+ 精确相等。
//   大小写 / 写法差异一律由词典数据补全（主词典 + 补充词典），匹配期不加任何模糊兜底。

static unsigned hash_key(const wchar_t* s, int n)
{
    unsigned h = 2166136261u; // FNV-1a
    for (int i = 0; i < n; i++) {
        h ^= (unsigned)s[i];
        h *= 16777619u;
    }
    return h;
}

int canon(const wchar_t* in, int n, wchar_t* out, int cap)
{
    if (!in || !out || cap <= 0) return -1;
    if (n < 0) { // n < 0 ⇒ 按 NUL 结尾自行算长度
        n = 0;
        while (in[n]) n++;
    }
    int o = 0, pend = 0;
    for (int i = 0; i < n; i++) {
        wchar_t c = in[i];
        if (c == 0) break; // 遇 0 视为串尾
        if (is_ws(c)) {
            if (o) pend = 1;
            continue;
        }
        if (pend) {
            if (o >= cap) return -1;
            out[o++] = L' ';
            pend = 0;
        }
        if (o >= cap) return -1;
        out[o++] = c;
    }
    return o;
}

static int hex4(const unsigned char* p)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int c = p[i], d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else
            return -1;
        v = (v << 4) | d;
    }
    return v;
}

// ENG 侧：`\xNNNN` = 一个 UTF-16 码元；其余字节按 ASCII 取（直接当码元）
static int expand_ascii(const unsigned char* b, int n, wchar_t* out, int cap)
{
    int o = 0, i = 0;
    while (i < n && o < cap) {
        if (b[i] == '\\' && i + 6 <= n && b[i + 1] == 'x') {
            int v = hex4(b + i + 2);
            if (v >= 0) {
                out[o++] = (wchar_t)v;
                i += 6;
                continue;
            }
        }
        out[o++] = (wchar_t)b[i];
        i++;
    }
    return o;
}

// CHI 侧：转义同上；其余按固定代码页 950（Big5）解码（绝不用 CP_ACP）。
// 解不出 ⇒ 落 '?' 并计数（宁可显眼的问号，也不要静悄悄的乱码）。
// *truncated（可空）：因输出缓冲满而提前收工时置 1（调用方用来记账）。
static int expand_big5(const unsigned char* b, int n, wchar_t* out, int cap, int* truncated)
{
    int o = 0, i = 0, dropped = 0;
    while (i < n && o < cap) {
        if (b[i] == '\\' && i + 6 <= n && b[i + 1] == 'x') {
            int v = hex4(b + i + 2);
            if (v >= 0) {
                out[o++] = (wchar_t)v;
                i += 6;
                continue;
            }
        }
        if (b[i] < 0x80) {
            out[o++] = (wchar_t)b[i];
            i++;
            continue;
        }
        // Big5 低位字节合法范围 0x40..0x7E 含 0x5C（`\`）⇒ 尾字节为 0x5C 的合法汉字
        //   （許/功/蓋…）必须按双字节吃掉。曾为「后随转义」特判单字节，会把它们拆成
        //   「替换符 + 字面反斜杠」；只有真的截在尾部（缺尾字节）才按单字节。
        const int need = (i + 2 > n) ? 1 : 2;
        wchar_t   wb[4];
        // MB_ERR_INVALID_CHARS：坏序列必须走 got<=0 的 '?' 分支并计数。不设它时
        //   Vista+ 会用 U+FFFD 悄悄替换（返回 >0），decode_qmark 恒 0 = 假阴性。
        int got = MultiByteToWideChar(950, MB_ERR_INVALID_CHARS, (const char*)(b + i), need, wb, 4);
        if (got <= 0) {
            out[o++] = 0x003F;
            i += need;
            g_st.decode_qmark++;
            continue;
        }
        for (int k = 0; k < got; k++) {
            if (o >= cap) { // 一对多码元被 cap 截在中间：必须记账，别静默丢
                dropped = 1;
                break;
            }
            out[o++] = wb[k];
        }
        i += need;
    }
    // 还有输入没消费（缓冲满）也算截断 —— 与上面「半对被截」两种情形合起来才完整。
    if (truncated) *truncated = (dropped || i < n) ? 1 : 0;
    return o;
}

// 补充词典的译文侧：UTF-8 解出的 wchar 里再解一遍转义。
//
// 支持四个转义，与游戏自己用的约定一致（实测 LG 词典 2,992 个块里 529 个
//   含反斜杠，全是这一套）：
//     `\xNNNN`  一个 UTF-16 码元（游戏拿它放按键图标 `\x00A4`=¤、字面 `‰`=U+2030、
//                 以及控制码；`%1%`/`%2%` 是它自己的格式占位符，另算）
//     `\n`      真实换行符 —— 游戏存多行文本用的就是真 LF，所以词典里写 `\n`
//                 必须在写进游戏 wstring 之前变成真换行，否则两行标签折成一行。
//     `\t`      制表符
//     `\\`      字面反斜杠
// 四个分支的顺序无关（各转义两字符前缀互不相同，不存在误吞）。
static int unescape_value(const wchar_t* s, int n, wchar_t* out, int cap)
{
    int o = 0, i = 0;
    while (i < n && o < cap) {
        if (s[i] == L'\\' && i + 1 < n) {
            const wchar_t c = s[i + 1];
            if (c == L'\\') {
                out[o++] = L'\\';
                i += 2;
                continue;
            }
            if (c == L'n') {
                out[o++] = L'\n';
                i += 2;
                continue;
            }
            if (c == L't') {
                out[o++] = L'\t';
                i += 2;
                continue;
            }
            if (c == L'x' && i + 6 <= n) {
                int v = 0, ok = 1;
                for (int k = 0; k < 4; k++) {
                    wchar_t h = s[i + 2 + k];
                    int     d;
                    if (h >= L'0' && h <= L'9')
                        d = h - L'0';
                    else if (h >= L'A' && h <= L'F')
                        d = h - L'A' + 10;
                    else if (h >= L'a' && h <= L'f')
                        d = h - L'a' + 10;
                    else {
                        ok = 0;
                        break;
                    }
                    v = (v << 4) | d;
                }
                if (ok) {
                    out[o++] = (wchar_t)v;
                    i += 6;
                    continue;
                }
            }
        }
        out[o++] = s[i++]; // 落单的 `\` 或不认识的转义：原样保留
    }
    return o;
}

static int key_has_cjk(const wchar_t* s, int n)
{
    for (int i = 0; i < n; i++)
        if (s[i] >= 0x3000) return 1;
    return 0;
}

// ================= 存储引擎（BSS，无堆分配 ⇒ 加载失败也只是一个空表）=================
#pragma pack(push, 1)
struct DictKey {
    unsigned       hash;
    unsigned short len;
    unsigned       off;  // 在键池里的起始下标（码元）
    unsigned       voff; // 在值池里的起始下标（码元）
    unsigned short vlen;
};
#pragma pack(pop)

// 全部加载期状态聚成一份：要换数据源（如烘焙表）或实例化时只动这里。
struct Store {
    DictKey        keys[DICT_MAX_KEYS];
    unsigned short slots[DICT_SLOTS]; // 0 = 空槽；否则 = 键下标 + 1
    wchar_t        keypool[DICT_KEYPOOL];
    wchar_t        valpool[DICT_VALPOOL];
    int            nkeys;
    int            keypoolN;
    int            valpoolN;
    int            ready;
};
static Store g_store;

static char g_mainPath[MAX_PATH] = "";
static char g_suppPath[MAX_PATH] = "";
static char g_status[512] = "";
// 本轮实际命中的用户词典文件名（给日志/单测看；空 = 没命中）
static char g_userDictName[64] = { 0 };

// ---- 入库管道的结果码（store_* 与 ingest_one 共用）----
enum IngestResult {
    INGEST_NEW = 2,           // 新建键
    INGEST_OVER = 1,          // 覆盖旧译文
    INGEST_SAME = 0,          // 同键同值（真·无变化）
    INGEST_SKIP_NOKEY = -1,   // 空键
    INGEST_SKIP_KEYLONG = -2, // 键 canon 后超长
    INGEST_SKIP_KEYCJK = -3,  // 键含 CJK
    INGEST_SKIP_NOVAL = -4,   // 无译文 / 译文全空白
    INGEST_SKIP_VALFULL = -5, // 值池满
    INGEST_SKIP_KEYFULL = -6, // 键表/键池满（含探测耗尽）
    INGEST_SKIP_DUP = -7,     // 同键已存在（不覆盖路径）
    INGEST_SKIP_FALLBACK = -8 // 兜底展开后装不下（整条丢弃；fallback_full 已计）
};

// ---- 存储引擎原语（都在加载期单线程调用；就绪后只读）----

// 追加式值池：调用方 arena_append 拿 voff 并写入，插表失败用 arena_unwind(voff) 回滚。
// （valpoolN 严格单调 ⇒ 回到本次 voff 即还原，这正是「重复键不白烧池子」的回收。）
static bool arena_append(const wchar_t* val, int len, int* voff)
{
    if (g_store.valpoolN + len > DICT_VALPOOL) return false;
    *voff = g_store.valpoolN;
    memcpy(g_store.valpool + *voff, val, (size_t)len * sizeof(wchar_t));
    g_store.valpoolN += len;
    return true;
}
static void arena_unwind(int voff) { g_store.valpoolN = voff; }

// 精确匹配的核：返回键下标，未命中 -1。
static int store_find(const wchar_t* key, int n)
{
    // n 的上界是契约的一部分：DictKey::len 是 u16，且表里只可能存 canon 后
    //   ≤DICT_KEY_MAX 的键 —— 更长的键必然未命中；直接拒掉才能保证比较循环
    //   不会越读键池（n >= 65536 时 (unsigned short)n 的窄化比较会让它扫出去）。
    if (!key || n <= 0 || n > DICT_KEY_MAX) return -1;
    unsigned h = hash_key(key, n);
    unsigned slot = h & (DICT_SLOTS - 1);
    for (int probe = 0; probe < DICT_SLOTS; probe++) {
        unsigned short ix = g_store.slots[slot];
        if (ix == 0) return -1;
        DictKey* k = &g_store.keys[ix - 1];
        if (k->hash == h && k->len == (unsigned short)n) {
            int same = 1;
            for (int i = 0; i < n; i++)
                if (g_store.keypool[k->off + i] != key[i]) {
                    same = 0;
                    break;
                }
            if (same) return (int)(ix - 1);
        }
        slot = (slot + 1) & (DICT_SLOTS - 1);
    }
    return -1;
}

// 不覆盖路径：建成 = INGEST_NEW；同键已存在 = INGEST_SKIP_DUP（先到先得）。
static int store_insert(const wchar_t* s, int n, int voff, int vlen)
{
    if (n <= 0 || n > DICT_KEY_MAX) return INGEST_SKIP_KEYLONG;
    if (g_store.nkeys >= DICT_MAX_KEYS || g_store.keypoolN + n > DICT_KEYPOOL) return INGEST_SKIP_KEYFULL;
    if (store_find(s, n) >= 0) return INGEST_SKIP_DUP;
    unsigned h = hash_key(s, n);
    unsigned slot = h & (DICT_SLOTS - 1);
    for (int probe = 0; probe < DICT_SLOTS; probe++) {
        if (g_store.slots[slot] == 0) {
            DictKey* k = &g_store.keys[g_store.nkeys];
            memcpy(g_store.keypool + g_store.keypoolN, s, (size_t)n * sizeof(wchar_t));
            k->hash = h;
            k->len = (unsigned short)n;
            k->off = (unsigned)g_store.keypoolN;
            k->voff = (unsigned)voff;
            k->vlen = (unsigned short)vlen;
            g_store.keypoolN += n;
            g_store.nkeys++;
            g_store.slots[slot] = (unsigned short)g_store.nkeys; // 最后发布
            return INGEST_NEW;
        }
        slot = (slot + 1) & (DICT_SLOTS - 1);
    }
    return INGEST_SKIP_KEYFULL; // 探测耗尽（槽全满）
}

// 覆盖路径：新建 / 覆盖 / 同键同值（真·无变化）。同值判定必须比译文内容：
// voff 是每次新分配的池下标，严格单调，恒不等于任何旧条目的 voff，拿它判同会让
// 「同键同值」永远命中不了。
static int store_insert_or_override(const wchar_t* s, int n, int voff, int vlen)
{
    int ex = store_find(s, n);
    if (ex >= 0) {
        const DictKey* k = &g_store.keys[ex];
        if ((int)k->vlen == vlen &&
            memcmp(g_store.valpool + k->voff, g_store.valpool + voff, (size_t)vlen * sizeof(wchar_t)) == 0)
            return INGEST_SAME;
        g_store.keys[ex].voff = (unsigned)voff;
        g_store.keys[ex].vlen = (unsigned short)vlen;
        return INGEST_OVER;
    }
    return store_insert(s, n, voff, vlen); // DUP 在此路径不会发生（刚 find 过）
}

// 兜底规整必须在每条词典加载路径上都跑——只在一处调用会让「只读用户词典」的
// 部署形态漏掉 U+2026 → "..." 这类规整。
// 每条路径一组基线：fallback 侧是累计计数，取差值才知道「这一条命中了几次」。
// （fallback_hits 归 fallback.cpp 所有、没有 reset，所以基线不必随 reset 清零。）
static int fbMainBefore[FALLBACK_MAX_RULES] = { 0 };
static int fbSupBefore[FALLBACK_MAX_RULES] = { 0 };

static int apply_fallback(wchar_t* buf, int len, int cap, int* fullCounter, int hits[],
                          int before[FALLBACK_MAX_RULES])
{
    const int n = fallback_apply(buf, len, cap);
    if (n < 0) {
        (*fullCounter)++;
        return -1;
    }
    for (int r = 0; r < FALLBACK_MAX_RULES; r++) {
        hits[r] += fallback_hits(r) - before[r];
        before[r] = fallback_hits(r);
    }
    return n;
}

// ================= 入库管道（两条载入路径共用）=================
// canon → 键校验 → 译文裁剪 → 兜底 → 入池 → 插表 → 失败回滚。
// rawKey = 解析器解码后的键（未 canon）；val = 解析器解码后的译文（未裁剪，
//   可写——兜底就地展开）；valCap = val 缓冲的总容量（码元），展开上限 = valCap - 裁剪偏移。
// allowOverride：true = 补充/用户词典（同键覆盖）；false = LG 主词典（同键丢弃）。
// ckey：调用方提供的 canon 输出缓冲（DICT_KEY_MAX + 2 码元）。
static int ingest_one(const wchar_t* rawKey, int rawKeyN, wchar_t* val, int valN, int valCap,
                      bool allowOverride, wchar_t* ckey, int fbBefore[FALLBACK_MAX_RULES],
                      int fbHits[FALLBACK_MAX_RULES], int* fbFull)
{
    const int cn = canon(rawKey, rawKeyN, ckey, DICT_KEY_MAX);
    if (cn < 0) return INGEST_SKIP_KEYLONG;
    if (cn == 0) return INGEST_SKIP_NOKEY;
    if (key_has_cjk(ckey, cn)) return INGEST_SKIP_KEYCJK;

    int vs = 0, ve = valN;
    while (vs < ve && is_ws(val[vs])) vs++;
    while (ve > vs && is_ws(val[ve - 1])) ve--;
    const int vlen = ve - vs;
    if (vlen <= 0) return INGEST_SKIP_NOVAL;

    // 兜底规整（可能变长 ⇒ 入池一律用展开后的 vlen2，按原长算会写溢出/漏字）。
    const int vlen2 = apply_fallback(val + vs, vlen, valCap - vs, fbFull, fbHits, fbBefore);
    if (vlen2 < 0) return INGEST_SKIP_FALLBACK;

    int voff = 0;
    if (!arena_append(val + vs, vlen2, &voff)) return INGEST_SKIP_VALFULL;

    const int r =
        allowOverride ? store_insert_or_override(ckey, cn, voff, vlen2) : store_insert(ckey, cn, voff, vlen2);
    // 只有真正落表的（NEW 追加新格 / OVER 换指针指向新段）才保留译文；
    //   SAME（同键同值）与 DUP/KEYLONG/KEYFULL 都是「这段译文没人引用」⇒ 退回池子。
    if (r == INGEST_NEW || r == INGEST_OVER) return r;
    arena_unwind(voff);
    return r;
}

// ================= 公共查表（就绪后只读，无锁）=================

DictVal lookup(const wchar_t* key, int n)
{
    DictVal   r = { NULL, 0 };
    const int ix = store_find(key, n);
    if (ix < 0) return r;
    const DictKey* k = &g_store.keys[ix];
    r.p = g_store.valpool + k->voff;
    r.len = (int)k->vlen;
    return r;
}

static void stats_sync()
{
    g_st.keys = g_store.nkeys;
    g_st.keypool_used = g_store.keypoolN;
    g_st.valpool_used = g_store.valpoolN;
}

const DictStats& stats() { return g_st; }

// ================= 文件读取 =================
// 本模块不依赖 core 的日志（那会形成不必要的耦合，日志仍走 set_log 注入），
//   但用 core 的纯 Win32 原语是允许的：依赖方向仍是 dict → core 单向。
static int read_file_all(const char* path, unsigned char** out, int* outN)
{ return core::file_read_all(path, out, outN, 0) ? 1 : 0; }

// 在 [from, to) 里找 pat；找不到 -1。to 是排他上界 —— 成对块搜索靠它把范围钉在本块内。
static int find_bytes(const unsigned char* b, int from, int to, const char* pat)
{
    int m = (int)strlen(pat);
    if (m <= 0 || from < 0 || to < from || from + m > to) return -1;
    for (int i = from; i + m <= to; i++)
        if (b[i] == (unsigned char)pat[0] && memcmp(b + i, pat, (size_t)m) == 0) return i;
    return -1;
}

static bool byte_is_ws(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// ================= LG 主词典（CP950 成对块）=================
// 本壳只做格式级的事：成对块定位、两侧解码。入库语义（校验/兜底/池/回滚）全在 ingest_one。
int load_main(const char* path)
{
    if (!path || !path[0]) return 0;
    static const char* S_ENG = "xfhsm_res_ENG_Start";
    static const char* E_ENG = "xfhsm_res_ENG_End";
    static const char* S_CHI = "xfhsm_res_CHI_Start";
    static const char* E_CHI = "xfhsm_res_CHI_End";

    unsigned char* buf = NULL;
    int            n = 0;
    if (!read_file_all(path, &buf, &n)) {
        DLOG("[词典] !! 主词典不可用（不存在 / 空文件 / 超过 %d MB / 读失败）：%s",
             core::FILE_READ_MAX_DEFAULT / (1024 * 1024), path);
        return 0;
    }
    DLOG("[词典] · 主词典 %s（%d 字节）", path, n);
    const int n0 = g_store.nkeys; // 本次调用新建的键数 = 调用后的差（返回值口径）

    static wchar_t kraw[8192];
    static wchar_t craw[8192];
    static wchar_t ckey[DICT_KEY_MAX + 2];

    int pos = 0;
    while (pos < n) {
        int a = find_bytes(buf, pos, n, S_ENG);
        if (a < 0) break;
        int as = a + (int)strlen(S_ENG);
        int eEnd = find_bytes(buf, as, n, E_ENG);
        if (eEnd < 0) break;
        // CHI 侧只许在本块范围内找：下一块的 ENG_Start（没有则 EOF）是硬上界。
        //   不设界时，某块缺 CHI 会偷到下一块的译文并跳掉下一块的 ENG（错配 + 丢块，
        //   且 main_noval 仍为 0、日志上看不出来）。缺侧 ⇒ 本块按「无译文」计。
        const int nxt = find_bytes(buf, eEnd, n, S_ENG);
        const int lim = (nxt < 0) ? n : nxt;
        int       cS = find_bytes(buf, eEnd, lim, S_CHI);
        int       cE = (cS < 0) ? -1 : find_bytes(buf, cS + (int)strlen(S_CHI), lim, E_CHI);
        pos = (cE < 0) ? (eEnd + 1) : (cE + (int)strlen(E_CHI));
        g_st.main_pairs++;

        // ENG 侧（掐掉两侧 CR/LF/空白）→ 管道里 canon
        int es = as, ee = eEnd;
        while (es < ee && byte_is_ws(buf[es])) es++;
        while (ee > es && byte_is_ws(buf[ee - 1])) ee--;
        const int kn = expand_ascii(buf + es, ee - es, kraw, 8192);

        // CHI 侧（CP950）；cE<0 ⇒ 该块没有译文，管道按 SKIP_NOVAL 丢弃
        int vn = 0;
        if (cE >= 0) {
            int cs = cS + (int)strlen(S_CHI), ce = cE;
            while (cs < ce && byte_is_ws(buf[cs])) cs++;
            while (ce > cs && byte_is_ws(buf[ce - 1])) ce--;
            int trunc = 0;
            vn = expand_big5(buf + cs, ce - cs, craw, 8192, &trunc);
            if (trunc) g_st.main_valtrunc++; // 值被静默截半入库：计数，别让它无声无息
        }

        const int r = ingest_one(kraw, kn, craw, vn, 8192, false, ckey, fbMainBefore, g_st.main_fallback,
                                 &g_st.main_fallback_full);
        switch (r) {
        case INGEST_NEW: break;
        case INGEST_SKIP_NOKEY:
        case INGEST_SKIP_NOVAL: g_st.main_noval++; break;
        case INGEST_SKIP_KEYLONG: g_st.main_keylong++; break;
        case INGEST_SKIP_KEYCJK: g_st.main_cjk++; break;
        case INGEST_SKIP_VALFULL: g_st.main_valfull++; break;
        case INGEST_SKIP_KEYFULL: g_st.main_keyfull++; break;
        case INGEST_SKIP_DUP: g_st.main_dup++; break; // 重复键被丢弃（先到先得）；译文已回滚
        default: break; // INGEST_SKIP_FALLBACK：只计 fallback_full（管道内已计）
        }
    }
    core::file_free(buf); // 分配走 core::file_read_all ⇒ 释放也走 core（同一约定）

    stats_sync();
    g_store.ready = (g_store.nkeys > 0);
    DLOG("[词典] · 主词典解析完成：成对块 %d ⇒ 建键 %d 条（键池 %d/%d 码元，译文池 %d/%d 码元）",
         g_st.main_pairs, g_store.nkeys, g_store.keypoolN, DICT_KEYPOOL, g_store.valpoolN, DICT_VALPOOL);
    DLOG("[词典] · 主词典跳过：无译文/空串 %d、键含 CJK %d、键过长(>%d) %d、译文池满 %d、重复键 %d、"
         "键表/键池满 %d；译文超 8192 被截断 %d 条（截断后不一定入库）；CP950 解不出落 '?' %d 次",
         g_st.main_noval, g_st.main_cjk, DICT_KEY_MAX, g_st.main_keylong, g_st.main_valfull, g_st.main_dup,
         g_st.main_keyfull, g_st.main_valtrunc, g_st.decode_qmark);
    DLOG("[词典] · 译文兜底规整：%s（丢弃 %d 条）"
         "（**不接管**原字体自有的码位 —— 那些位置是游戏自己的资源；改动只在译文侧）",
         fallback_status(), g_st.main_fallback_full);
    return g_store.nkeys - n0; // 本次新建的键数（同 load_supplement 的「本次」口径）
}

// ================= 补充/用户词典（UTF-8 TSV，同键覆盖）=================
// 行级格式的事在这里（注释判据/坏行/TAB 拆分/UTF-8 解码）；入库同样在 ingest_one。
int load_supplement(const char* path)
{
    if (!path || !path[0]) return 0;
    unsigned char* buf = NULL;
    int            n = 0;
    if (!read_file_all(path, &buf, &n)) {
        DLOG("[词典] · 补充词典不可用（未找到或读不了，可选）：%s", path);
        return 0;
    }
    DLOG("[词典] · 补充词典 %s（%d 字节，UTF-8）", path, n);

    // 解码上限 8192 码元，与主词典路径（load_main 的 kraw/craw）对齐：4096 会让
    //   「确实过长」与「不是合法 UTF-8」共用返回 0，日志说错原因。键最终仍由
    //   canon(DICT_KEY_MAX=4096) 把关，值上限 = 8192。
    static wchar_t kraw[8192], kraw2[8192], ckey[DICT_KEY_MAX + 2];
    static wchar_t wval[8192], wval2[8192];
    enum {
        DECODE_CAP = 8192
    };

    int pos = 0;
    // UTF-8 BOM：不跳的话首行判据失效（注释判据看 buf[a]，首条数据行会被 U+FEFF
    //   带头 ⇒ 被 key_has_cjk 拒、整行丢掉，且日志只报「键非法」）。
    if (n >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
        pos = 3;
        DLOG("[词典] · 补充词典带 UTF-8 BOM：已跳过（仍建议存成无 BOM）");
    }
    while (pos < n) {
        int e = pos;
        while (e < n && buf[e] != '\n') e++;
        int a = pos, b = e;
        pos = e + 1;
        while (b > a && (buf[b - 1] == '\r' || buf[b - 1] == ' ' || buf[b - 1] == '\t')) b--;
        while (a < b && (buf[a] == ' ' || buf[a] == '\t')) a++;
        if (a >= b) {
            g_st.sup_empty++;
            continue;
        }
        // 先找 TAB，再判注释 —— 顺序反了会吃掉真实数据。
        //   游戏的 UI 串里有一批以 `#` 开头的（按键提示一类，实测 106 条）：
        //       # Press ½ to select your Throwing Knives
        //       # Ability Lost: Defense Break …
        //   旧判据「`buf[a]=='#'` 就是注释」把它们当注释丢掉 —— 静默丢 7% 的词典。
        //   判据：数据行必有 TAB，注释行永远没有（注释是散文，不含分隔符）。
        int tab = -1;
        for (int i = a; i < b; i++)
            if (buf[i] == '\t') {
                tab = i;
                break;
            }
        if (buf[a] == '#' && tab < 0) {
            g_st.sup_comment++;
            continue;
        }
        g_st.sup_lines++;
        if (tab < 0) {
            g_st.sup_bad++;
            continue;
        }
        int ke = tab, vs = tab + 1;
        while (ke > a && (buf[ke - 1] == ' ' || buf[ke - 1] == '\t')) ke--;
        while (vs < b && (buf[vs] == ' ' || buf[vs] == '\t')) vs++;
        if (ke <= a || vs >= b) {
            g_st.sup_bad++;
            continue;
        }

        // 键列与译文列走完全相同的两步：先按 UTF-8 解码，再解一遍转义。
        //   键列不能只走 expand_ascii —— 它只认 \xNNNN、其余字节原样当一个码元，
        //   而游戏发过来的多字节字符是一个码元 ⇒ canon + 精确相等必然判不等 ⇒ 未命中。
        // 借 unescape_value 顺带保证游戏自己的 \xNNNN 约定仍然有效
        //   （键、译文同一份约定，一个入口，不搞两套 —— 这是不在此处写新解码器的理由）。
        int kGot = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)(buf + a), ke - a, kraw,
                                       DECODE_CAP);
        // 必须紧跟调用读错误码（中间不调任何会覆盖 last error 的 Win32/日志）：
        //   缓冲不足与非法编码都返回 0，只有它能把两者分开。
        const DWORD kErr = (kGot <= 0) ? GetLastError() : 0;
        if (kGot <= 0) {
            if (kErr == ERROR_INSUFFICIENT_BUFFER) {
                g_st.sup_keylong++;
                if (g_st.sup_keylong <= 3)
                    DLOG("[词典] !! 补充词典第 %d 条：键过长（>%d 码元）⇒ 跳过该行（表里装不下）",
                         g_st.sup_lines, DECODE_CAP);
            }
            else {
                g_st.sup_keybad++;
                if (g_st.sup_keybad <= 3)
                    DLOG("[词典] !! 补充词典第 %d 条：键不是合法 UTF-8（跳过该行；本文件必须存成 UTF-8）",
                         g_st.sup_lines);
            }
            continue;
        }
        const int kn = unescape_value(kraw, kGot, kraw2, DECODE_CAP);

        int vGot = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)(buf + vs), b - vs, wval,
                                       DECODE_CAP);
        const DWORD vErr = (vGot <= 0) ? GetLastError() : 0;
        if (vGot <= 0) {
            if (vErr == ERROR_INSUFFICIENT_BUFFER) {
                g_st.sup_vallong++;
                if (g_st.sup_vallong <= 3)
                    DLOG("[词典] !! 补充词典第 %d 条：译文过长（>%d 码元）⇒ 跳过该行（值池装不下）",
                         g_st.sup_lines, DECODE_CAP);
            }
            else {
                g_st.sup_valbad++;
                if (g_st.sup_valbad <= 3)
                    DLOG("[词典] !! 补充词典第 %d 条：译文不是合法 UTF-8（跳过该行；本文件必须存成 UTF-8）",
                         g_st.sup_lines);
            }
            continue;
        }
        const int vn = unescape_value(wval, vGot, wval2, DECODE_CAP);

        const int r = ingest_one(kraw2, kn, wval2, vn, DECODE_CAP, true, ckey, fbSupBefore, g_st.sup_fallback,
                                 &g_st.sup_fallback_full);
        switch (r) {
        case INGEST_NEW: g_st.sup_new++; break;
        case INGEST_OVER: g_st.sup_over++; break;
        case INGEST_SAME: g_st.sup_same++; break; // 同键同值，译文已回滚
        case INGEST_SKIP_NOKEY:
        case INGEST_SKIP_KEYCJK: g_st.sup_keybad++; break;
        case INGEST_SKIP_KEYLONG:
            g_st.sup_keylong++; // 键 canon 后 >4096：与「键非法」分开计（可判读）
            break;
        case INGEST_SKIP_NOVAL: g_st.sup_valbad++; break;
        case INGEST_SKIP_VALFULL:
            g_st.sup_poolfull++;
            if (g_st.sup_poolfull <= 3)
                DLOG("[词典] !! 补充词典第 %d 条：译文池满（%d/%d 码元）⇒ 丢弃该条"
                     "（后续更短的仍会入池）",
                     g_st.sup_lines, g_store.valpoolN, DICT_VALPOOL);
            break;
        case INGEST_SKIP_KEYFULL: g_st.sup_keyfull++; break;
        default: break; // FALLBACK（只计 fallback_full）；DUP 在 override 路径不发生
        }
    }
    core::file_free(buf); // 分配走 core::file_read_all ⇒ 释放也走 core（同一约定）

    stats_sync();
    DLOG("[词典] · 补充词典解析完成：有效行 %d ⇒ 新建键 %d / 覆盖主词典 %d / 与主词典同值 %d；"
         "跳过：注释 %d、空行 %d、无TAB或空侧 %d、键非法 %d、键过长 %d、译文解码失败 %d、译文过长 %d、"
         "值池满 %d、键表/键池满 %d",
         g_st.sup_lines, g_st.sup_new, g_st.sup_over, g_st.sup_same, g_st.sup_comment, g_st.sup_empty,
         g_st.sup_bad, g_st.sup_keybad, g_st.sup_keylong, g_st.sup_valbad, g_st.sup_vallong,
         g_st.sup_poolfull, g_st.sup_keyfull);
    DLOG("[词典] · 用户词典译文兜底规整：%s（丢弃 %d 条）"
         "（**不接管**原字体自有的码位 —— 那些位置是游戏自己的资源）",
         fallback_status(), g_st.sup_fallback_full);
    return g_st.sup_new + g_st.sup_over;
}

// ================= 定位 + 启动 =================
// 用户词典的候选文件名（按顺序试）。这是「那一份词典」 ——
//   语言无关、大小不定、格式 UTF-8 的 `ENG<TAB>译文`。
//   有了它就只读它：查得到就替换，查不到就英文原文。
//   绝不拿 LG 主词典（繁体中文）兜底 —— 那会让没翻的条目露出中文。
static const char* kUserDictNames[] = {
    "AC1_Dict.txt", // 只有这一个名字是「那一份词典」
};
// 「额外补充词典」是另一个角色：它叠加在 LG 主词典之上，不是取代，
//   所以它不在 kUserDictNames 里。
static const char* kExtraSupplementName = "AC1_CN_Dict_Supplement.txt";

// 候选顺序（不硬编码任何绝对路径）：
//   ① ASI 同目录            <asi>\<name>
//   ② scripts\ 同级         <asi>\..\scripts\<name>
//   ③ LG_Data\（工作区）    <asi>\..\LG_Data\<name>
//   ④ 宿主根目录            <asi>\..\<name>
static bool try_candidates(const char* asi_dir, const char* name, char* out)
{
    char stem[MAX_PATH];
    lstrcpynA(stem, asi_dir, MAX_PATH);
    size_t n = lstrlenA(stem);
    while (n && (stem[n - 1] == '\\' || stem[n - 1] == '/')) stem[--n] = 0;
    if (!n) return false; // asi_dir 为空 ⇒ 无从定位（单测直接调 dict_load_*）

    const char* forms[4] = { "%s\\%s", "%s\\..\\scripts\\%s", "%s\\..\\LG_Data\\%s", "%s\\..\\%s" };
    for (int i = 0; i < 4; i++) {
        char cand[MAX_PATH];
        // 拼路径必须带容量自检：被截断的路径会安静地指向一个不存在的文件，
        //   日志里与「文件不存在」完全无法区分。core::Str 满则 clamp、不越界。
        core::Str cs;
        core::str_init(&cs, cand, (int)sizeof(cand));
        core::str_addf(&cs, forms[i], stem, name);
        if (core::file_exists(cand)) {
            lstrcpynA(out, cand, MAX_PATH);
            return true;
        }
        DLOG("[词典] · 候选不存在：%s", cand);
    }
    return false;
}

static bool try_any_candidates(const char* asi_dir, const char* const* names, int nNames, char* out,
                               char* whichOut, int whichCap)
{
    for (int i = 0; i < nNames; i++) {
        if (try_candidates(asi_dir, names[i], out)) {
            if (whichOut && whichCap > 0) {
                _snprintf(whichOut, whichCap - 1, "%s", names[i]);
                whichOut[whichCap - 1] = 0;
            }
            return true;
        }
    }
    return false;
}

bool init(const char* asi_dir)
{
    if (!asi_dir || !asi_dir[0]) {
        DLOG("[词典] !! 不知道 ASI 所在目录，跳过词典初始化");
        return false;
    }

    // CP950 自检：Big5 → Unicode 的唯一通道（固定代码页）
    {
        wchar_t wb[4];
        int     got = MultiByteToWideChar(950, 0, "\xA4\xA4", 2, wb, 4);
        DLOG("[词典] · CP950(Big5) 自检：%s（A4A4 应 ⇒ U+4E2D 中）",
             (got == 1 && wb[0] == 0x4E2D) ? "可用 ✓" : "**不可用** ⇒ 译文会退化成 '?'");
    }

    // 只有一份词典说了算
    //   用户给了自己的词典（AC1_Dict.txt）就只读它：查得到就替换，查不到就英文原文。
    //   绝不把 LG 主词典（繁体中文）当兜底 —— 那会让「没翻的条目」露出中文
    //   而不是英文，对日语/韩语用户是错的。
    if (try_any_candidates(asi_dir, kUserDictNames, (int)(sizeof(kUserDictNames) / sizeof(kUserDictNames[0])),
                           g_suppPath, g_userDictName, (int)sizeof(g_userDictName))) {
        int nSup = load_supplement(g_suppPath);
        g_mainPath[0] = 0; // 明确标记「本轮没读 LG 主词典」
        DLOG("[词典] · 用户词典：%s（%s，%d 条）", g_userDictName, g_suppPath, nSup);
        DLOG("[词典] · **不读 LG 主词典**（它是繁体中文）：这份词典没列出的条目"
             "一律保留**英文原文**，不会回退成中文");
    }
    else {
        // 旧路径（兼容既有部署）：LG 主词典（中文）+ 旧补充词典覆盖
        g_suppPath[0] = 0;
        if (try_candidates(asi_dir, "LGCStringDict_01.txt", g_mainPath)) {
            DLOG("[词典] · LG 主词典：%s", g_mainPath);
            load_main(g_mainPath);
        }
        else {
            g_mainPath[0] = 0;
            DLOG("[词典] · LG 主词典未找到 —— **这不一定是错误**："
                 "用自有词典（AC1_Dict.txt）时刚才就已命中；两份都没有"
                 "⇒ 文本翻译未就绪（钩子仍装，只观测不替换）");
        }
        if (try_candidates(asi_dir, kExtraSupplementName, g_suppPath))
            load_supplement(g_suppPath); // 叠加在 LG 主词典之上
        else
            g_suppPath[0] = 0;
    }

    stats_sync();
    g_store.ready = (g_store.nkeys > 0);
    core::Str s;
    core::str_init(&s, g_status, (int)sizeof(g_status));
    core::str_addf(&s, "键 %d/%d 键池 %d/%d 译文池 %d/%d（%s）", g_store.nkeys, DICT_MAX_KEYS,
                   g_store.keypoolN, DICT_KEYPOOL, g_store.valpoolN, DICT_VALPOOL, "canon+精确相等");
    // 丢条必须出现在运行期反复可见的那行摘要里（app 每 10 秒的状态行与 strings 报表头都用它）：
    //   只在载入期日志里报一次，等于事后没人看得见。四类来源：值池满、键表/键池满、
    //   兜底展开满、以及主词典值被截半入库（错译入库，最该被看见）。
    if (g_st.sup_poolfull || g_st.main_valfull || g_st.sup_keyfull || g_st.main_keyfull ||
        g_st.sup_fallback_full || g_st.main_fallback_full || g_st.main_valtrunc)
        core::str_addf(&s, "｜丢弃 值池满=%d/%d 键表满=%d/%d 兜底满=%d/%d 截断=%d", g_st.sup_poolfull,
                       g_st.main_valfull, g_st.sup_keyfull, g_st.main_keyfull, g_st.sup_fallback_full,
                       g_st.main_fallback_full, g_st.main_valtrunc);

    if (g_store.ready)
        DLOG("[词典] 就绪 ✓：%s", g_status);
    else
        DWARN("[词典] 未就绪 ✗：%s", g_status);
    return g_store.ready != 0;
}

void reset()
{
    memset(&g_store, 0, sizeof(g_store));
    // 必须连槽表一起清（slots 在 Store 里，memset 已覆盖）——insert 靠
    //   `slots[slot] == 0` 找空槽；只清 keys 而留着 slots 的话，陈旧的非零槽会
    //   永久占用容量：探测跳过它们、再也找不到空槽，而 nkeys 明明还远低于
    //   DICT_MAX_KEYS ⇒ insert 报假的「探测耗尽」，负查找也不再在第一个空槽停下。
    //   这个函数是单测「两种部署形态」用例的接缝，一次进程里要 reset() 五次以上 ——
    //   正是最容易被这个缺陷污染的用法。
    memset(&g_st, 0, sizeof(g_st));
    g_mainPath[0] = 0;
    g_suppPath[0] = 0;
    g_userDictName[0] = 0;
    g_status[0] = 0;
}

bool        ready() { return g_store.ready != 0; }
const char* main_path() { return g_mainPath; }
const char* supp_path() { return g_suppPath; }
const char* status() { return g_status; }

} // namespace dict
} // namespace ac1
