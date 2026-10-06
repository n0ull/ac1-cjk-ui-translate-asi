// modules/glyph/src/cpindex.cpp —— 码位索引 + 命中/漏字统计
//
// 依赖：core（只用日志）。不依赖 dict / text / app。
//
// 存在的理由见 glyphsets.h：要定策略就得先知道"哪个码位被查、被谁查"，
// 而这条信息只有这里有（文本层那份清单因为"含 CJK 闸门"看不到这些码位）。
//
// ===== 两张表 =====
//   命中表：清单里所有码位 → 去重升序 → 二分。配套 1 位/码位的拒斥位图做 O(1) 预筛。
//   漏字表：游戏想要、我们没映射的码位（只看 C >= 0x3000）。码位空间 0x3000..0xFFFF
//           只有 0xD000 项 ⇒ 直接索引（不做线性表：热路径上扫上万项不可接受）。
//
// ===== 饱和 =====
// 计数饱和在 0xFFFF（u16 的上限）：继续加也不会变，而"变不变"不影响任何决策
// —— 排序只按大小，"被查过"只按 !=0 判断。饱和后不再自增，省掉一次写。
//
// ===== 两行 Top 的确切格式（抓取脚本对这份）=====
//   [字形] 命中码位 Top（我们映射的，被游戏查过）：U+4E2D×12@00892C8B ｜ U+3001×3@00899A10
//   [字形] 漏字 Top（游戏想要、我们没映射）：U+9F8D×5@00892C8B ｜ …
//   · 条目 = `U+%04X` + `×%u`（次数，饱和值 65535）+ `@%08X`（第一次被查的调用者
//     返回地址，即引擎里那个 call 点的下一条指令）
//   · 条目之间用全角 `｜` + 一个空格分隔；最多 TOP_N 条，按次数降序
//   · 一个都没有时打 `（空）`（让人能分清"钩子活着但还没被查过"和"报表没跑"）
//   · 只在"命中种类数或漏字种类数变过"时打，空转时一行都不打

#include "ac1/core/log.h"
#include "ac1/core/str.h"

#include "glyphsets.h"

#include <string.h>
#include <stdio.h>

namespace ac1 {
namespace glyph {

unsigned short     g_allCp[GLYPH_MAX_RECS];
unsigned short     g_cpHits[GLYPH_MAX_RECS];
unsigned           g_cpCaller[GLYPH_MAX_RECS]; // 该码位第一次被查时的调用者返回地址
unsigned short     g_missHits[0xD000];
unsigned           g_missFirst[0xD000]; // 与 g_missHits 同下标，存第一次的调用者
unsigned long long g_cpPage[1024];
volatile long      g_nAllCp = 0;

static const int TOP_N = 8;

// 上一次打报表时的种类数（-1 = 从没打过 ⇒ 第一次一定打）
static int g_lastHitKinds = -1;
static int g_lastMissKinds = -1;

void cp_index_reset()
{
    g_nAllCp = 0;
    memset(g_cpPage, 0, sizeof(g_cpPage));
    memset(g_cpHits, 0, sizeof(g_cpHits));
    memset(g_missHits, 0, sizeof(g_missHits));
    memset(g_missFirst, 0, sizeof(g_missFirst));
}

void cp_index_add(unsigned short cp)
{
    if ((long)g_nAllCp >= (long)GLYPH_MAX_RECS) return;
    g_allCp[g_nAllCp++] = cp;
}

void cp_index_finish()
{
    // 插入排序：清单只有几千条，比 qsort 便宜得多，也不引入 CRT 的比较器类型
    long n = (long)g_nAllCp;
    for (long i = 1; i < n; i++) {
        unsigned short v = g_allCp[i];
        long           j = i - 1;
        while (j >= 0 && g_allCp[j] > v) {
            g_allCp[j + 1] = g_allCp[j];
            j--;
        }
        g_allCp[j + 1] = v;
    }
    long k = 0;
    for (long i = 0; i < n; i++)
        if (i == 0 || g_allCp[i] != g_allCp[i - 1]) g_allCp[k++] = g_allCp[i];
    g_nAllCp = k;

    memset(g_cpPage, 0, sizeof(g_cpPage));
    for (long i = 0; i < k; i++) g_cpPage[g_allCp[i] >> 6] |= (1ull << (g_allCp[i] & 63));
}

int cp_index_find(unsigned short C)
{
    long lo = 0, hi = (long)g_nAllCp - 1;
    while (lo <= hi) {
        long           mid = (lo + hi) >> 1;
        unsigned short v = g_allCp[mid];
        if (v == C) return (int)mid;
        if (v < C)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

// 漏字：只记 C >= 0x3000 且不在我们表里的
static void miss_observe(unsigned short C, unsigned caller)
{
    unsigned i = (unsigned)C - 0x3000u;
    if (g_missFirst[i] == 0) g_missFirst[i] = caller;
    if (g_missHits[i] != 0xFFFF) g_missHits[i]++;
}

void cp_index_observe(unsigned short C, unsigned caller)
{
    // 热路径第一道闸：`C < 0x3000`（拉丁字母、数字、控制符、图标区）直接走人。
    //   清单里全是汉字与全角标点，全在 ≥0x3000 ⇒ 这一刀能把绝大多数排字请求挡掉。
    if (C < 0x3000) return;
    // 第二道闸：没有清单 ⇒ 整段短路，一个字节都不写（绝大多数实机情形）
    if ((long)g_nAllCp <= 0) return;
    // 第三道闸：位图 O(1) 拒掉"我们这一页（64 个码位）什么都没有"的请求
    if (!(g_cpPage[C >> 6] & (1ull << (C & 63)))) {
        miss_observe(C, caller);
        return;
    }
    int i = cp_index_find(C);
    if (i < 0) {
        miss_observe(C, caller);
        return;
    }
    if (g_cpCaller[i] == 0) g_cpCaller[i] = caller; // 只记第一次
    if (g_cpHits[i] != 0xFFFF) g_cpHits[i]++;
}

int cp_index_hit_kinds()
{
    long k = 0;
    for (long i = 0; i < (long)g_nAllCp; i++)
        if (g_cpHits[i]) k++;
    return (int)k;
}

long cp_index_hit_total()
{
    long t = 0;
    for (long i = 0; i < (long)g_nAllCp; i++) t += g_cpHits[i];
    return t;
}

int cp_index_miss_kinds()
{
    int k = 0;
    for (unsigned i = 0; i < 0xD000u; i++)
        if (g_missHits[i]) k++;
    return k;
}

// ---- Top 报表 ----
// 8 次扫描取最大。已经选中的下标记在 emitted[] 里（≤8 个，线性扫即可）——
// 不要用"在结果数组里按源下标打标记"那种写法：源表有 0xD000 项，结果数组只有 8。
struct TopEntry {
    unsigned hits, caller, cp;
};
struct Emitted {
    int idx[TOP_N];
    int n;
};
static bool already(const Emitted& e, int i)
{
    for (int k = 0; k < e.n; k++)
        if (e.idx[k] == i) return true;
    return false;
}

static void emit_top(const char* label, const TopEntry* t, int n)
{
    // 用 core::Str 累加（str_addf 自己封顶、不越界；「拿 _snprintf 返回值当写偏移」
    //   是越界来源，见 core/str.h 的文件头）。
    char      line[1024];
    core::Str w;
    core::str_init(&w, line, (int)sizeof(line));
    core::str_addf(&w, "[字形] %s：", label);
    if (n == 0) {
        core::str_addf(&w, "（空）");
        core::log_line("%s", line);
        return;
    }
    for (int i = 0; i < n && !core::str_full(&w); i++)
        core::str_addf(&w, "%sU+%04X×%u@%08X", i ? " ｜ " : "", t[i].cp, t[i].hits, t[i].caller);
    core::log_line("%s", line);
}

void cp_index_report_tops()
{
    int hitK = cp_index_hit_kinds();
    int missK = cp_index_miss_kinds();
    if (hitK == g_lastHitKinds && missK == g_lastMissKinds) return; // 没变过 ⇒ 一行都不打
    g_lastHitKinds = hitK;
    g_lastMissKinds = missK;

    // 命中 Top
    TopEntry t[TOP_N];
    memset(t, 0, sizeof(t));
    Emitted e;
    e.n = 0;
    int  got = 0;
    long n = (long)g_nAllCp;
    for (int k = 0; k < TOP_N; k++) {
        int best = -1;
        for (long i = 0; i < n; i++) {
            if (g_cpHits[i] == 0 || already(e, (int)i)) continue;
            if (best < 0 || g_cpHits[i] > g_cpHits[best]) best = (int)i;
        }
        if (best < 0) break;
        e.idx[e.n++] = best;
        t[got].cp = g_allCp[best];
        t[got].hits = g_cpHits[best];
        t[got].caller = g_cpCaller[best];
        got++;
    }
    emit_top("命中码位 Top（我们映射的，被游戏查过）", t, got);

    // 漏字 Top
    memset(t, 0, sizeof(t));
    e.n = 0;
    got = 0;
    for (int k = 0; k < TOP_N; k++) {
        int best = -1;
        for (unsigned i = 0; i < 0xD000u; i++) {
            if (g_missHits[i] == 0 || already(e, (int)i)) continue;
            if (best < 0 || g_missHits[i] > g_missHits[best]) best = (int)i;
        }
        if (best < 0) break;
        e.idx[e.n++] = best;
        t[got].cp = 0x3000u + (unsigned)best;
        t[got].hits = g_missHits[best];
        t[got].caller = g_missFirst[best];
        got++;
    }
    emit_top("漏字 Top（游戏想要、我们没映射）", t, got);
}

} // namespace glyph
} // namespace ac1
