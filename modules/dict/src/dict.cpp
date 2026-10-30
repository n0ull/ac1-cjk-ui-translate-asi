// 包内 UTF-8 TSV → 唯一规范键 → 稳定映射；就绪后只读。日志经 set_log 注入。

#include "ac1/dict/dict.h"
#include "ac1/core/file.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>

namespace ac1 {
namespace dict {

// ---- 日志出口（不依赖 core 的关键）----
static LogFn g_log = NULL;
static LogFn g_error = NULL;

void set_log(LogFn info, LogFn error) { g_log = info; g_error = error; }

static DictStats g_st = { 0 };
static int g_ellipsis_hits = 0; // 日志按进程累计；DictStats 只记录本次初始化。

// Expand at the call site so MSVC checks the literal format against its arguments.
// The sink receives up to 2046 bytes as data through "%s"; it never guesses severity.
#define DLOG_TO(sink, ...)                                                                   \
    do {                                                                                     \
        if (sink) {                                                                          \
            char message[2048];                                                              \
            _snprintf_s(message, sizeof(message) - 1, _TRUNCATE, __VA_ARGS__);                 \
            sink("%s", message);                                                             \
        }                                                                                    \
    } while (0)
#define DLOG(...) DLOG_TO(g_log, __VA_ARGS__)
#define DLOG_ERROR(...) DLOG_TO(g_error, __VA_ARGS__)

// ================= 小工具 =================
static bool is_ws(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r'; }

// 匹配策略：只做 canon（空白折叠）+ 精确相等。
//   大小写 / 写法差异一律由词典数据补全，匹配期不加任何模糊兜底。

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

// ================= 存储引擎（BSS，无堆分配 ⇒ 加载失败也只是一个空表）=================
#pragma pack(push, 1)
struct DictKey {
    unsigned       hash;
    unsigned short len;
    unsigned       off;  // 在键池里的起始下标（码元）
    unsigned       voff; // 在值池里的起始下标（码元）
    unsigned short vlen;
    unsigned       source_line;
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
static char g_status[512] = "";

// ---- 入库管道的结果码（store_* 与 ingest_one 共用）----
enum IngestResult {
    INGEST_NEW = 1,           // 新建键
    INGEST_SKIP_NOKEY = -1,   // 空键
    INGEST_SKIP_KEYLONG = -2, // 键 canon 后超长
    INGEST_DUPLICATE = -3,    // 规范键重复，包含同值重复
    INGEST_SKIP_NOVAL = -4,   // 无译文 / 译文全空白
    INGEST_SKIP_VALFULL = -5, // 值池满
    INGEST_SKIP_KEYFULL = -6, // 键表/键池满（含探测耗尽）
    INGEST_SKIP_FALLBACK = -8 // 兜底展开后装不下（整条丢弃；fallback_full 已计）
};

// ---- 存储引擎原语（都在加载期单线程调用；就绪后只读）----

static bool arena_append(const wchar_t* val, int len, int* voff)
{
    if (g_store.valpoolN + len > DICT_VALPOOL) return false;
    *voff = g_store.valpoolN;
    memcpy(g_store.valpool + *voff, val, (size_t)len * sizeof(wchar_t));
    g_store.valpoolN += len;
    return true;
}

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

// 仅插入新键；调用方已确认该键不存在。
static int store_insert(const wchar_t* s, int n, const wchar_t* value, int vlen, int line)
{
    if (n <= 0 || n > DICT_KEY_MAX) return INGEST_SKIP_KEYLONG;
    if (g_store.nkeys >= DICT_MAX_KEYS || g_store.keypoolN + n > DICT_KEYPOOL) return INGEST_SKIP_KEYFULL;
    int voff = 0;
    if (!arena_append(value, vlen, &voff)) return INGEST_SKIP_VALFULL;
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
            k->source_line = (unsigned)line;
            g_store.keypoolN += n;
            g_store.nkeys++;
            g_store.slots[slot] = (unsigned short)g_store.nkeys; // 最后发布
            return INGEST_NEW;
        }
        slot = (slot + 1) & (DICT_SLOTS - 1);
    }
    return INGEST_SKIP_KEYFULL; // 探测耗尽（槽全满）
}

// U+2026 在原 title 字体中是装饰图标，译文改用三个 ASCII 点，不接管原码位。
// 仅用于解码后的值缓冲：0 <= len <= cap。先验容量再反向展开，失败不改内容。
static int expand_ellipsis(wchar_t* buf, int len, int cap)
{
    int hits = 0;
    for (int i = 0; i < len; ++i)
        if (buf[i] == 0x2026) ++hits;
    if (!hits) return len;
    // 用剩余容量比较，避免先计算展开长度时溢出。
    if (hits > (cap - len) / 2) {
        g_st.sup_fallback_full++;
        return -1;
    }
    const int expanded = len + hits * 2;
    int out = expanded;
    for (int i = len - 1; i >= 0; --i) {
        const wchar_t c = buf[i];
        if (c == 0x2026) {
            buf[--out] = L'.';
            buf[--out] = L'.';
            buf[--out] = L'.';
        }
        else buf[--out] = c;
    }
    g_st.sup_ellipsis += hits;
    g_ellipsis_hits += hits;
    return expanded;
}

// ================= 入库管道 =================
// canon → 唯一键校验 → 译文裁剪 → 兜底 → 入池与插表。
// rawKey = 解析器解码后的键（未 canon）；val = 解析器解码后的译文（未裁剪，
//   可写——兜底就地展开）；valCap = val 缓冲的总容量（码元），展开上限 = valCap - 裁剪偏移。
// ckey：调用方提供的 canon 输出缓冲（DICT_KEY_MAX + 2 码元）。
static int ingest_one(const wchar_t* rawKey, int rawKeyN, wchar_t* val, int valN, int valCap,
                      wchar_t* ckey, int line)
{
    const int cn = canon(rawKey, rawKeyN, ckey, DICT_KEY_MAX);
    if (cn < 0) return INGEST_SKIP_KEYLONG;
    if (cn == 0) return INGEST_SKIP_NOKEY;
    if (store_find(ckey, cn) >= 0) return INGEST_DUPLICATE;

    int vs = 0, ve = valN;
    while (vs < ve && is_ws(val[vs])) vs++;
    while (ve > vs && is_ws(val[ve - 1])) ve--;
    const int vlen = ve - vs;
    if (vlen <= 0) return INGEST_SKIP_NOVAL;

    // 兜底规整（可能变长 ⇒ 入池一律用展开后的 vlen2，按原长算会写溢出/漏字）。
    const int vlen2 = expand_ellipsis(val + vs, vlen, valCap - vs);
    if (vlen2 < 0) return INGEST_SKIP_FALLBACK;

    return store_insert(ckey, cn, val + vs, vlen2, line);
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

static void note_failure(const char* reason, int line)
{
    if (++g_st.rejected_lines == 1) {
        g_st.first_rejected_line = line;
        g_st.failure_line = line;
        g_st.reason = reason;
    }
}

static bool stable_mappings()
{
    wchar_t key[DICT_KEY_MAX];
    for (int i = 0; i < g_store.nkeys; ++i) {
        const DictKey& entry = g_store.keys[i];
        const wchar_t* value = g_store.valpool + entry.voff;
        const int length = canon(value, entry.vlen, key, DICT_KEY_MAX);
        const DictVal next = lookup(key, length);
        if (!next.p) continue;
        const bool unchanged = next.len == entry.vlen &&
            memcmp(next.p, value, (size_t)entry.vlen * sizeof(wchar_t)) == 0;
        if (unchanged) continue;
        note_failure("non_idempotent_value", (int)entry.source_line);
        return false;
    }
    return true;
}

// ================= 包内词典（UTF-8 TSV，规范键唯一）=================
// 行级格式的事在这里（注释判据/坏行/TAB 拆分/UTF-8 解码）；入库同样在 ingest_one。
static bool runtime_units_valid(const wchar_t* text, int length)
{
    for (int i = 0; i < length; ++i) {
        const unsigned cp = (unsigned short)text[i];
        const bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
        const bool control = cp < 32 && cp != '\n' && cp != '\r' && cp != '\t';
        if (surrogate || control || cp == 127) return false;
    }
    return true;
}

static int load_utf8(const unsigned char* buf, int n)
{
    if (!buf || n <= 0 || n > core::FILE_READ_MAX_DEFAULT) return 0;
    // 解码上限 8192 码元：4096 会让
    //   「确实过长」与「不是合法 UTF-8」共用返回 0，日志说错原因。键最终仍由
    //   canon(DICT_KEY_MAX=4096) 把关，值上限 = 8192。
    static wchar_t kraw[8192], kraw2[8192], ckey[DICT_KEY_MAX + 2];
    static wchar_t wval[8192], wval2[8192];
    enum {
        DECODE_CAP = 8192
    };

    int pos = 0, line = 0;
    // BOM 仅在文件头处理，不让它进入首个键或破坏注释判定。
    if (n >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
        pos = 3;
        DLOG("[词典] · 补充词典带 UTF-8 BOM：已跳过（仍建议存成无 BOM）");
    }
    while (pos < n) {
        ++line;
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
            note_failure("malformed_row", line);
            continue;
        }
        int ke = tab, vs = tab + 1;
        while (ke > a && (buf[ke - 1] == ' ' || buf[ke - 1] == '\t')) ke--;
        while (vs < b && (buf[vs] == ' ' || buf[vs] == '\t')) vs++;
        if (ke <= a || vs >= b) {
            g_st.sup_bad++;
            note_failure("malformed_row", line);
            continue;
        }

        // 键列与译文列走完全相同的两步：先按 UTF-8 解码，再解一遍转义。
        //   键列不能逐字节当码元，否则 UTF-8 多字节字符的匹配会失效：
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
                note_failure("key_too_long", line);
            }
            else {
                g_st.sup_keybad++;
                note_failure("invalid_utf8_key", line);
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
                note_failure("value_too_long", line);
            }
            else {
                g_st.sup_valbad++;
                note_failure("invalid_utf8_value", line);
            }
            continue;
        }
        const int vn = unescape_value(wval, vGot, wval2, DECODE_CAP);

        if (!runtime_units_valid(kraw2, kn)) {
            note_failure("unsupported_key_unit", line);
            return -1;
        }
        if (!runtime_units_valid(wval2, vn)) {
            note_failure("unsupported_value_unit", line);
            return -1;
        }

        const int r = ingest_one(kraw2, kn, wval2, vn, DECODE_CAP, ckey, line);
        switch (r) {
        case INGEST_NEW: g_st.sup_new++; break;
        case INGEST_DUPLICATE:
            g_st.sup_duplicate++;
            note_failure("duplicate_key", line);
            break;
        case INGEST_SKIP_NOKEY:
            g_st.sup_keybad++;
            note_failure("empty_key", line);
            break;
        case INGEST_SKIP_KEYLONG:
            g_st.sup_keylong++; // 键 canon 后 >4096：与「键非法」分开计（可判读）
            note_failure("key_too_long", line);
            break;
        case INGEST_SKIP_NOVAL:
            g_st.sup_valbad++;
            note_failure("empty_value", line);
            break;
        case INGEST_SKIP_VALFULL:
            g_st.sup_poolfull++;
            note_failure("value_pool_full", line);
            break;
        case INGEST_SKIP_KEYFULL:
            g_st.sup_keyfull++;
            note_failure("key_capacity_full", line);
            break;
        case INGEST_SKIP_FALLBACK: note_failure("fallback_full", line); break;
        }
    }
    stats_sync();
    DLOG("[词典] 数据行=%d 新键=%d 重复键=%d 拒绝=%d 注释=%d 空行=%d",
         g_st.sup_lines, g_st.sup_new, g_st.sup_duplicate, g_st.rejected_lines,
         g_st.sup_comment, g_st.sup_empty);
    DLOG("[词典] · 用户词典译文兜底规整：省略号 U+2026 累计命中 %d（丢弃 %d 条）"
         "（**不接管**原字体自有的码位 —— 那些位置是游戏自己的资源）",
         g_ellipsis_hits, g_st.sup_fallback_full);
    return g_st.sup_new;
}

static void publish_status()
{
    const char* result = g_store.ready ? "clean" : "rejected";
    _snprintf_s(g_status, sizeof(g_status), _TRUNCATE,
                "result=%s reason=%s keys=%d key_pool=%d value_pool=%d rejected=%d "
                "first_rejected_line=%d failure_line=%d rules=%d",
                result, g_st.reason, g_st.keys, g_st.keypool_used, g_st.valpool_used,
                g_st.rejected_lines, g_st.first_rejected_line, g_st.failure_line, RULES_VERSION);
}

bool init_bytes(const unsigned char* data, int bytes)
{
    memset(&g_store, 0, sizeof(g_store));
    memset(&g_st, 0, sizeof(g_st));
    g_st.reason = "none";
    if (!data || bytes <= 0 || bytes > core::FILE_READ_MAX_DEFAULT) {
        g_st.reason = bytes == 0 ? "empty_input" : "invalid_input";
        if (bytes > core::FILE_READ_MAX_DEFAULT) g_st.reason = "input_too_large";
        publish_status();
        return false;
    }
    const int accepted = load_utf8(data, bytes);
    const bool loaded = accepted > 0 && !g_st.rejected_lines;
    const bool valid = loaded && stable_mappings();
    if (!valid) {
        if (!g_st.rejected_lines) g_st.reason = "no_effective_entries";
        DLOG_ERROR("[词典] 拒绝规则 v%d 输入：reason=%s line=%d", RULES_VERSION,
                   g_st.reason, g_st.failure_line);
        // Entries must disappear on failure; evidence of the attempt must survive.
        memset(&g_store, 0, sizeof(g_store));
        stats_sync();
        publish_status();
        return false;
    }
    g_store.ready = 1;
    publish_status();
    return true;
}

bool        ready() { return g_store.ready != 0; }
const char* status() { return g_status; }

} // namespace dict
} // namespace ac1
