// modules/glyph/src/font_track.cpp —— 字体生命周期跟踪：计数 + 登记表 + 待办队列 + 失败名单 + 指纹
//
// 字体的全部"它现在处于什么状态"都由本文件的三张表回答：
//   登记表：已补丁字体的幂等跟踪（font → 我们换上去的表 + 用的哪套）
//   待办队列：查表钩发现、DrawText 前置钩消费的缓冲
//   失败名单：补丁失败被拉黑的字体（不再排队/重试/刷日志）
// 一致性纪律：占槽一律走 core::ctr_claim（先占槽后写），扫描一律按游标读长度，
//   见各函数注释。
//
// 依赖：core（ctr/log）+ glyph_internal.h。不碰钩、不碰引擎 API。

#include "ac1/glyph/glyph.h"

#include "ac1/core/log.h"
#include "ac1/core/ctr.h"

#include "glyphsets.h"
#include "glyph_internal.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>

namespace ac1 {
namespace glyph {

// ======================================================================
// 计数（多个线程可能同时进来，各字段都是 volatile LONG，InterlockedIncrement）
// 收成 core::Counters（modules/core/include/ac1/core/ctr.h）。
// 槽位编号与顺序不许改 —— stats() 的逐字段拷贝、状态行、单测断言
// 全都按这个顺序读，所以下面这个 enum 的值只准增不准改（见 glyph_internal.h）。
// ======================================================================
static_assert(COUNTER_SLOTS <= core::COUNTER_SLOTS_MAX, "计数槽数超过了 core::Counters 的定容上限");
// 静态初始化（不是运行期 init）：n 直接写死，钩子装上之前也不会有零值窗口。
core::Counters g_ctr = { { 0 }, 0, COUNTER_SLOTS };

// 登记表/队列的长度不是独立变量：它们就是各自 ctr_claim 游标的 next
//   （自增在前、序号全局唯一）。n 静态写死，不留运行期零值窗口。
static core::Counters g_regC = { { 0 }, 0, GLYPH_MAX_FONTS };
static core::Counters g_pendC = { { 0 }, 0, GLYPH_PENDING_MAX };

void inc(int slot) { core::ctr_inc(&g_ctr, slot); }

// ---- 游标长度读取 ----
//   ctr_claim 只增不减（满了也照增），所以 next 可能 > 容量；读长度必须自己夹一下。
int reg_count()
{
    long n = (long)InterlockedCompareExchange((volatile LONG*)&g_regC.next, 0, 0);
    return (int)(n > (long)GLYPH_MAX_FONTS ? (long)GLYPH_MAX_FONTS : n);
}
int pend_count()
{
    long n = (long)InterlockedCompareExchange((volatile LONG*)&g_pendC.next, 0, 0);
    return (int)(n > (long)GLYPH_PENDING_MAX ? (long)GLYPH_PENDING_MAX : n);
}
// 队列压紧后主动把游标退回去（core::ctr_claim 的 next 因此可以变小的唯一合法场景：
//   槽位被释放了，不是被别人占着）。走 InterlockedExchange，与 ctr_claim 的
//   InterlockedIncrement 构成同一个原子序列。
void pend_set_count(int n)
{
    if (n < 0) n = 0;
    if (n > GLYPH_PENDING_MAX) n = GLYPH_PENDING_MAX;
    InterlockedExchange(&g_pendC.next, (LONG)n);
}

// 1e-6 刻度的定点整数比较。要求打包器把跨度存成 f32 相减的结果
// （见 glyph.h 里 fp_u_span / fp_v_span 那段），否则差在第 7 位小数上会判成"不是这一套"。
static bool span_eq(float a, float b) { return (int)(a * 1000000.0f) == (int)(b * 1000000.0f); }

// ======================================================================
// 幂等登记表：字体指针 + 我们换上去的表指针 + 用的哪套。
// 表指针是查表钩判断"引擎是不是把对象重置了"的唯一依据（见 detours.cpp discover_font）。
// ======================================================================
void* g_fonts[GLYPH_MAX_FONTS] = { 0 };
void* g_ourTable[GLYPH_MAX_FONTS] = { 0 };
int   g_setUsed[GLYPH_MAX_FONTS] = { 0 };

int registry_find(const void* font)
{
    int n = reg_count();
    for (int i = 0; i < n; i++)
        if (g_fonts[i] == font) return i;
    return -1;
}
// 先占槽、后写：core::ctr_claim 里的 InterlockedIncrement 是第一步，
//   拿到的序号全局唯一。原来的「读 n → 写 g_fonts[n] → 自增」顺序下，
//   font_loaded_post（引擎字体加载线程）与 hook_drawtext（渲染线程）可同时进来，
//   拿到同一个 n、同时写第 n 格（丢一条）并把计数器顶过 32，
//   之后 `for (i < 登记表长度)` 的 registry_find / fp_sample_begin 就越界读了。
//   返回 0 = 没占到（表已满），调用方按「登记表已满」处理（见 font_loaded_post）。
int registry_add(const void* font, const void* ourTable, int setIdx)
{
    int slot = core::ctr_claim(&g_regC);
    if (slot < 0) return 0;
    g_fonts[slot] = (void*)font;
    g_ourTable[slot] = (void*)ourTable;
    g_setUsed[slot] = setIdx;
    return 1;
}
void registry_set_table(const void* font, const void* ourTable, int setIdx)
{
    int i = registry_find(font);
    if (i < 0) return;
    g_ourTable[i] = (void*)ourTable;
    g_setUsed[i] = setIdx;
}

// 「某个字体对象 = 清单里第几套」的对外出口。返回套下标；未登记返回 -1。
int font_set_of(const void* font)
{
    // g_setUsed[] 存的是 0 基 的 setIdx（registry_add 直接存 setIdx，
    //   模块里所有「套#%d」日志也直接打 setIdx），这里不再减 1。
    int i = registry_find(font);
    if (i < 0) return -1;
    int u = g_setUsed[i];
    return (u >= 0 && u < (int)GLYPH_MAX_SETS) ? u : -1;
}

// ======================================================================
// 待办队列：查表钩往里放，DrawText 前置钩从里面取
// ======================================================================
struct Pending {
    const void* font;
    int         setIdx;
    int         tries;
};
static Pending g_pending[GLYPH_PENDING_MAX];

// 返回 1 = 刚排上（或被重新排队），调用方据此只打那一行日志；
// 返回 0 = 已经在队列里（静默）—— 查表钩每次排字都会调进来，不静默就是每字一行日志。
int queue_pending(const void* font, int setIdx)
{
    int n = pend_count();
    for (int i = 0; i < n; i++)
        if (g_pending[i].font == font) {
            g_pending[i].setIdx = setIdx;
            g_pending[i].tries = 0;
            return 0;
        }
    // 同样是先占槽再写：查表钩（渲染线程）与 font_loaded_post（引擎线程）
    //   都能排同一个字体，抢同一格会让 g_npending 越过 GLYPH_PENDING_MAX，
    //   而 apply_pending 是 `for (i < g_npending)` 逐格读 ⇒ 越界。
    int slot = core::ctr_claim(&g_pendC);
    if (slot < 0) return 0; // 队列满 ⇒ 静默丢弃（与原行为一致）
    g_pending[slot].font = font;
    g_pending[slot].setIdx = setIdx;
    g_pending[slot].tries = 0;
    return 1;
}

// 由 DrawText 前置钩调用：逐个应用队列里的字体（apply_patch 在 apply_patch.cpp）。
void apply_pending()
{
    int n = pend_count();
    int appliedHere = 0; // 观测用：本次真正换掉表的字体数（不参与任何判定）
    for (int i = 0; i < n; i++) {
        if (!g_pending[i].font) continue;
        // 拉黑过的直接摘掉：apply_patch 会回滚并拉黑，摘掉是它失败后的常态
        if (fail_listed(g_pending[i].font)) {
            core::log_line("[字形] DrawText 前：字体 %p 被闸门拒绝（已拉黑）⇒ 摘出队列，不再重试",
                           g_pending[i].font);
            g_pending[i].font = NULL;
            continue;
        }
        if (apply_patch(g_pending[i].font, g_pending[i].setIdx, "DrawText前")) {
            g_pending[i].font = NULL; // 摘掉
            inc(C_APPLIED_DRAW);
            appliedHere++;
            continue;
        }
        // 失败不立刻放弃：字体对象可能正处在"半就绪"的瞬间（很短暂）。
        // 但必须有上限，否则一个永久失败的字体会每帧刷一行日志。
        if (++g_pending[i].tries >= GLYPH_PENDING_TRIES) {
            core::log_line("[字形] !! 字体 %p 排队 %d 次都应用不了 ⇒ 放弃（不再重试）", g_pending[i].font,
                           GLYPH_PENDING_TRIES);
            g_pending[i].font = NULL;
        }
    }
    int kept = 0; // 压实：把还挂着的收到前面
    for (int i = 0; i < n; i++)
        if (g_pending[i].font) g_pending[kept++] = g_pending[i];
    pend_set_count(kept);
    if (appliedHere)
        core::log_line("[字形] DrawText 前换表：本次应用 %d 个字体（入队 %d ⇒ 仍挂 %d）", appliedHere, n,
                       kept);
}

// ======================================================================
// 失败名单
//   补丁失败后如果不拉黑：同一个字体会每帧被重新发现、重新排队、重新失败、
//   每帧刷一行日志。进榜的字体一律跳过（不排队、不重试、不再打日志），
//   只在入榜时打一行。有界（≤32 条）。满了以后收下新来的、顶掉最早入榜的
//   那条（round-robin 淘汰，见 fail_list_add 里的注释）。
// ======================================================================
const void* g_failList[GLYPH_MAX_FONTS] = { 0 };
// stats().failByCode 的数据源：入榜原因码按位存这里，汇总时按码分桶。
int           g_failCode[GLYPH_MAX_FONTS] = { 0 };
volatile LONG g_nfail = 0;
// 满了以后轮转淘汰的游标：InterlockedIncrement 保证两线程不会淘汰同一格。
// 第一次淘汰的是槽 0（最先入榜的那条），之后 0→1→…→31→0。
// g_nfail 本身仍封顶 GLYPH_MAX_FONTS —— 它是 stats().failListed 的口径，
// 也是 failByCode 汇总时遍历的上界。
static volatile LONG g_failNext = 0;

bool fail_listed(const void* font)
{
    int n = (int)g_nfail;
    for (int i = 0; i < n; i++)
        if (g_failList[i] == font) return true;
    return false;
}
void fail_list_add(const void* font, int code)
{
    if (fail_listed(font)) return;
    int n = (int)g_nfail;
    // 满了以后收下新来的、淘汰最早入榜的那条（round-robin 淘汰）。
    //   不能简单钉在最后一槽：同一字体反复失败时，每次都会把自己的上一条记录顶掉，
    //   被顶掉的字体下一帧又被重新发现、重新排队、重新失败、重新打一行日志 ——
    //   正是这张失败名单要掐掉的日志风暴循环。g_nfail 仍封顶 GLYPH_MAX_FONTS。
    int slot;
    if (n < GLYPH_MAX_FONTS) {
        slot = n;
        InterlockedIncrement(&g_nfail);
    }
    else {
        // 第一次淘汰槽 0（最先入榜的那条），之后 0→1→…→31→0。
        slot = (int)((InterlockedIncrement(&g_failNext) - 1) % (LONG)GLYPH_MAX_FONTS);
    }
    g_failList[slot] = font;
    g_failCode[slot] = code;
    const char* why = "未知";
    switch (code) {
    case GLYPH_FAIL_ALLOC: why = "新表分配失败"; break;
    case GLYPH_FAIL_ENTRY: why = "charmap 写槽没生效"; break;
    case GLYPH_FAIL_FAULT: why = "打补丁途中异常（已回滚）"; break;
    case GLYPH_FAIL_LIMIT: why = "字形数装不进 u16"; break;
    default: break;
    }
    core::log_line("[字形] !! 字体 %p 补丁失败（%s）⇒ 拉黑：此后不再排队、不再重试、不再刷日志", font, why);
}

// ======================================================================
// 字体指纹（只读）
//
// 全模块唯一一份取指纹的实现：匹配（match_set）、采样日志、单测、
//   将来的离线打包器都走它，免得两处口径漂移。
//   hasAt = 0（表里没有 '@'）是合法结果（返回 1）：那套字体没法指纹匹配，
//   采样日志会照实打出来，而不是伪装成"读不到"。
// ======================================================================
int font_fingerprint(const void* font, GlyphFingerprint* out)
{
    if (!out) return 0;
    out->count = 0;
    out->hasAt = 0;
    out->u_span = 0.0f;
    out->v_span = 0.0f;
    if (!font) return 0;

    // font 是引擎对象，形状只信到闸门那一层；这里被单测以外的人直接调也得能兜住
    __try {
        const unsigned char* f = (const unsigned char*)font;
        unsigned             count = *(const unsigned short*)(f + GLYPH_OFF_COUNT);
        if (count == 0) return 0;
        const unsigned char* table = *(const unsigned char* const*)(f + GLYPH_OFF_TABLE);
        if (!table) return 0;
        out->count = count;

        for (unsigned i = 0; i < count; i++) {
            const unsigned char* r = table + (size_t)i * (size_t)GLYPH_REC_SIZE;
            if (*(const unsigned short*)r != 0x40) continue; // '@'
            out->u_span = *(const float*)(r + GLYPH_REC_U1) - *(const float*)(r + GLYPH_REC_U0);
            out->v_span = *(const float*)(r + GLYPH_REC_V1) - *(const float*)(r + GLYPH_REC_V0);
            out->hasAt = 1;
            break;
        }
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        out->count = 0;
        out->hasAt = 0;
        out->u_span = 0.0f;
        out->v_span = 0.0f;
        return 0;
    }
}

// 用 '@' 的 UV 跨度选套。返回套下标，-1 = 没匹配上。
// 指纹取法只有 font_fingerprint() 一份实现（离线打包器要按同一口径核对），
//   这里不再另写一遍 '@' 查找。
int match_set(const void* font)
{
    // 一套都没加载时直接回 -1：查表钩是热路径，没有数据可比就别去扫那张表
    if ((int)g_nsets <= 0) return -1;
    GlyphFingerprint fp;
    if (!font_fingerprint(font, &fp) || !fp.hasAt) return -1; // 取不到 / 没有 '@' ⇒ 无从匹配
    for (int i = 0; i < (int)g_nsets; i++) {
        const GlyphSet& s = g_sets[i];
        if (s.fp_glyph_count && s.fp_glyph_count != fp.count) continue;
        if (!span_eq(fp.u_span, s.fp_u_span)) continue;
        if (!span_eq(fp.v_span, s.fp_v_span)) continue;
        return i;
    }
    return -1;
}

// 指纹样本计数（上限 GLYPH_FP_SAMPLES）。见 font_fingerprint 与 fp_sample_begin。
volatile LONG g_fpSamples = 0;

// 要不要为这个字体打一行指纹样本？
// 采样点刻意在所有闸门与匹配之前：那里正是"匹配不上"发生的地方，
//   放在闸门之后就只剩一句 no_match，看不到跨度，打包器无从得知该写哪几套的指纹。
// 但要在幂等表登记之前判一次：已登记（=已打补丁）的字体再来时不重复采样，
//   否则 8 个样本位会被同一个字体的重复调用吃光。
static bool fp_sample_begin(const void* font)
{
    int nf = reg_count();
    for (int i = 0; i < nf; i++)
        if (g_fonts[i] == font) return false;
    if (g_fpSamples >= (LONG)GLYPH_FP_SAMPLES) return false;
    return InterlockedIncrement(&g_fpSamples) <= (LONG)GLYPH_FP_SAMPLES;
}

// 打出那一行指纹样本（额度用完就什么都不做）。
// 加载钩与查表钩共用这一份，免得两条路各打一遍、把 8 个位子吃光。
void sample_fingerprint(const void* font)
{
    if (!fp_sample_begin(font)) return;
    __try {
        GlyphFingerprint fp;
        unsigned         vt = *(const unsigned*)font;
        if (font_fingerprint(font, &fp)) {
            if (fp.hasAt)
                core::log_line("[字形] 指纹样本 #%d 字体 %p vtable=%08X 字形=%u "
                               "'@'跨度 u=%.6f v=%.6f",
                               (int)g_fpSamples, font, vt, fp.count, (double)fp.u_span, (double)fp.v_span);
            else
                core::log_line("[字形] 指纹样本 #%d 字体 %p vtable=%08X 字形=%u "
                               "'@' 缺失（该字体无法指纹匹配）",
                               (int)g_fpSamples, font, vt, fp.count);
        }
        else {
            core::log_line("[字形] 指纹样本 #%d 字体 %p vtable=%08X 取不到指纹（对象不可信）",
                           (int)g_fpSamples, font, vt);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        core::log_line("[字形] 指纹样本 #%d 字体 %p：读指纹时异常 ⇒ 这一套采不到", (int)g_fpSamples, font);
    }
}

} // namespace glyph
} // namespace ac1
