// modules/text/src/path_funnel.cpp —— 单点漏斗（整个骨架里唯一"做实事"的模块）
//
// 依赖：core（hook/mem/log）+ dict。本文件不 include app 或 glyph 的任何东西。
//
// ===== 事实依据（已定案，勿再研究）=====
// 目标：Dx9 sub_8A1920（VA 0x8A1920 / RVA 0x4A1920）；Dx10 sub_BC3290（RVA 0x7C3290）。
//   · 入口 7 字节 = 6A FF 68 xx xx 55 01（push -1; push offset SEH）
//     —— 两条完整指令，≥5 字节 ⇒ MinHook 落钩的下限满足。
//   · 签名 __cdecl，两个参数 (void* dst, const wchar_t* src)，返回 dst；
//     函数尾 83 C4 58 C3（add esp,58h; ret）⇒ 调用者清栈。
//   · 语义：把 src 宽串拷进临时 wstring dst。Dx9 侧 24 个直接调用点覆盖全 UI 文本：
//     4 个 LoadVisitor 槽（0x8C3F90 / 0x8C8850 / 0x8C43A0 / 0x8C5690）、
//     流式填充 0x892E40、两个 widget setter（0x892C10→widget+0x14、0x895370→widget+0x10）。
//     Dx10 侧同样 24 个调用点，visitor 与流式填充共用。
//   · 所有调用者随后都会 assign(最终目标, dst, 0, -1)（Dx9: 0x8759B0 / Dx10: 0xB94A70），
//     而 assign 按真实长度搬运 ⇒ 我们在 post 阶段换掉 dst 的内容就等于换了最终文本 ⇒ 零填充。
//   · Dx9 侧唯一非 UI 调用者：sub_F8A0E0（诊断消息格式化，8 处）—— 词典不命中即无副作用。
//
// ===== post 而非 pre（决策依据）=====
// pre（改 src）会漏掉那些 src 是"控制码大块"、需要按行拆的串；post 拿到的 dst
// 已经是引擎物化后的形态（转义已折成真正码元），正是词典键的形态。

#include "ac1/text/text.h"

#include "ac1/core/hook.h"
#include "ac1/core/host.h" // host_flavor()：Dx9/Dx10 挂点与签名分栏
#include "ac1/core/log.h"
#include "ac1/core/mem.h"
#include "ac1/dict/dict.h"

#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>

#include "ac1/core/ctr.h"
#include "ac1/core/trace.h"
#include "ac1/core/str.h"
#include "inventory.h" // 同模块内部：字符串全记录（不进 include/，不跨模块）

// TEXT_REPLACE / SAMPLE_N 定义在 ac1/text/text.h（公开头，单测也要看得到）

namespace ac1 {
namespace text {
namespace {

// ---- 入口期望字节（install 时逐字节核对）----
const unsigned char kFunnelEntry[7] = { 0x6A, 0xFF, 0x68, 0xE9, 0x7A, 0x55, 0x01 };
const unsigned      kFunnelRva9 = 0x4A1920u;
// Dx10（0xBC3290）：SEH 指针值不同 ⇒ 签名分栏；
//   尾 ret（调用方清栈，调用点 add esp,8 实锤）⇒ __cdecl，与 Dx9 同，
//   detour 直接用同一个 hook_funnel，无需变体。
const unsigned char kFunnelEntry10[13] = { 0x6A, 0xFF, 0x68, 0xE9, 0xC9, 0x56, 0x01,
                                           0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 };
const unsigned      kFunnelRva10 = 0x7C3290u;

typedef void*(__cdecl* FunnelFn)(void* dst, const wchar_t* src);
FunnelFn g_origFunnel = NULL;

// 计数快照。多个线程会同时改，所以各字段都是 volatile LONG；用 core::Counters
// （ctr_inc / ctr_get），与 glyph 共用同一份实现。读侧只求"趋势"，不做一致性快照。
// 槽位顺序不许动 —— TextCounters 字段、状态行、以及单测的 topCount 都依赖它。
// g_c.next 不占 v[] 的任何一格；本模块拿它当「未命中 ASCII Top」的占槽游标（见 top_remember）。
enum {
    C_HITS = 0,
    C_REPLACED,
    C_MISS,
    C_MISS_ASCII,
    C_MISS_NONASCII,
    C_SKIPPED_GROW,
    C_FAULT,
    C_SKIP_EMPTY,
    C_SKIP_CJK,
    C_SKIP_DICT,
    C_SKIP_BADVIEW,
    C_SKIP_OTHER,
    C_SAMPLE_HIT,
    C_SAMPLE_MISS,
    COUNTER_SLOTS
};
core::Counters g_c;
struct CtrInit {
    CtrInit() { core::ctr_reset(&g_c, COUNTER_SLOTS); }
} g_ctrInit;

// Top 的占用数就是游标本身（游标只增、序号即下标 ⇒ 有效条目恒为 [0, top_count())）。
static int top_count() { return (int)InterlockedCompareExchange(&g_c.next, 0, 0); }

// ---- 闸门拒绝采样 ----
// 本钩在 core 钩执行迹（ac1/core/trace.h）里的槽位。-1 = 还没登记；
// 未登记时 hooktrace_enter/leave 直接空操作，功能不受影响。
static int g_traceSlot = -1;

// hook_funnel 每命中一次要走 wstr_view + 全串扫码元 + dict::canon + dict::lookup，
// 属热路径，故按采样记（首次 + 每 5000 条）。
const long kFunnelGateEvery = 5000;

// 采样直接读既有的 C_HITS（只读：不新增计数、也不改它的值）。
// 钩子的身份/命中计数归 core（HookSpec）管，这里只报"本模块决定了什么"。
void funnel_gate_log(const char* why, void* ra, int len)
{
    LONG h = (LONG)core::ctr_get(&g_c, C_HITS);
    if (h != 1 && (h % kFunnelGateEvery) != 0) return;
    core::log_line("[FUNNEL][闸门] #%ld 拒因=%s ret=%08X len=%d", (long)h, why, (unsigned)(uintptr_t)ra, len);
}

// ---- 「未命中 ASCII Top」：去重、最多 TOP_N 条、按首次出现顺序 ----
// 用途：量化"词典还缺哪些条目"（真机日志里的这份列表 ⇒ 交给 data/tools 的缺口工具生成补充条目）。
// 存的是 canon 之后的键（也就是真正拿去查表的那个形态），比原串更能说明缺的是什么。
// 固定大小、零动态分配 ⇒ 运行期不 malloc。
const int      TOP_N = 128;  // 最多留几条
const int      TOP_LEN = 24; // 每条最多留多少码元
static wchar_t g_top[TOP_N][TOP_LEN];
static int     g_topLen[TOP_N];

// 竞争纪律：先占槽再写（ctr_claim）——「读计数 → 判边界 → 写 → 最后自增」会让两个
//   线程抢到同一格并把计数顶过上界。inventory 的 inv_remember 要的是相反的顺序
//   （写条目在前、发布游标在最后，读侧才不会看到半条）⇒ 它用一把只护「新增」的
//   小锁把占槽与发布串起来，两条路径的形状不同、理由都在各自的文件里。
//   本表无「半条」问题：条目内容由 ctr_claim_n 的序号直接定位，写满即有效。
void top_remember(const wchar_t* key, int n)
{
    if (n <= 0) return;
    int cnt = top_count();
    if (cnt > TOP_N) cnt = TOP_N; // 满了就不再收（保持首次出现的顺序）
    if (n > TOP_LEN) n = TOP_LEN;
    for (int i = 0; i < cnt; i++) { // 去重
        if (g_topLen[i] != n) continue;
        if (memcmp(g_top[i], key, (size_t)n * sizeof(wchar_t)) == 0) return;
    }
    // 占槽：自增提到最前面 ⇒ 序号全局唯一，两个线程不会写同一格；满了返回 -1。
    // 用 core 的显式上界版本：默认的 ctr_claim 按计数槽数封顶（≤32），
    // 而本表有 TOP_N = 128 格 —— 用默认版会把 Top 上限悄悄砍成 32 条。
    const int s = core::ctr_claim_n(&g_c, TOP_N);
    if (s < 0) return;
    // 占到槽之后若发现与前一条重复，仍然写进去：槽已经占掉了，空着它会让 topCount
    // 高于有效条目数，detail() 就会读到 g_topLen[s] == 0 的未初始化条目。
    // 内容与前一条逐字节相同 ⇒ 多出来的那条是无害的重复（窗口只有「去重扫描 → 占槽」之间那几条指令）。
    memcpy(g_top[s], key, (size_t)n * sizeof(wchar_t));
    g_topLen[s] = n;
}

// 样本用缓冲：把 wchar 串转成 \uXXXX 形态（超出部分用 …(N 码元) 收尾）
void dump_wstr(char* out, size_t cap, const wchar_t* s, int n)
{
    if (!out || cap == 0) return; // 防御：调用方不会这么传，但契约上要有出口
    // 收尾标记先拼好、在循环里按它的长度预留：不预留的话循环会把缓冲吃到只剩 1~2 字节，
    //   标记永远写不上（截断就退化成无标记的静默截断）。标记拼在栈缓冲里、装得下才拷：
    //   原地 _snprintf 装不下时不补 NUL（core/str.h 的工具链陷阱），而 out[o..cap) 是
    //   未写入的栈字节，直接收口会把它们喂给下面的『%s』。
    char   tail[32];
    int    t = _snprintf(tail, sizeof(tail), "…(%d 码元)", n);
    size_t reserve = (t > 0 && (size_t)t + 1 <= sizeof(tail)) ? (size_t)t + 1 : 0; // 含尾 NUL
    if (reserve > cap - 1) reserve = 0; // 输出缓冲本身放不下标记 ⇒ 不预留
    size_t o = 0;
    int    shown = 0;
    for (int i = 0; i < n; i++) {
        char one[8];
        int  w = _snprintf(one, sizeof(one), "\\u%04X", (unsigned)(unsigned short)s[i]);
        if (w <= 0) break;
        if (o + (size_t)w + 1 + reserve >= cap) break;
        memcpy(out + o, one, (size_t)w);
        o += (size_t)w;
        shown++;
    }
    if (shown < n && reserve) {
        memcpy(out + o, tail, reserve); // 含尾 NUL
        return;
    }
    out[o] = 0; // o <= cap-1 恒成立（循环按 o+w+1+reserve < cap 收口；n<=0 时 o=0）
}

} // namespace

// post 阶段在匿名 namespace 之外：跨 TU 可见性由 src\path_internal.h 收口。
// ---- post 阶段（本模块真正的逻辑；钩子与单测调的都是它）----
// 这个函数里只放 POD 局部变量：/EHsc 下带对象析构的函数里不允许 __try（C2712）。
//   dst 是引擎的临时 wstring，随时可能被别的线程改动 ⇒ 读它必须 __try 兜底。
// 详见 src\path_internal.h。
//
// ---- forge 闸门 ----
//   装配层在启动时把 forge 核验结果经 set_forge_patch 注入（见 glyph/forgecheck.cpp）。
//   图集不在 ⇒ 本层只观测不替换（命中/未命中/样本/全记录照记，一个字节不写），
//   与 TEXT_REPLACE=0 同形 ⇒ 闸门关 = 纯原版界面。
static int g_forgePatch = 1; // 单测默认开；装配层每轮启动显式 set_forge_patch
void       set_forge_patch(int present) { g_forgePatch = present ? 1 : 0; }
// 「替换生效」的唯一判定口：编译开关与运行期闸门与关系，样本/清单/写入共用。
static int replace_effective() { return TEXT_REPLACE && g_forgePatch; }
void       run_post_counted(void* dst, void* ra)
{
    __try {
        core::ctr_inc(&g_c, C_HITS);

        core::WStrView v = core::wstr_view(dst); // 自带读门 + __try
        if (!v.ok) {
            core::ctr_inc(&g_c, C_SKIP_BADVIEW);
            funnel_gate_log("结构不可信", ra, -1);
            return;
        }
        if (v.len == 0) {
            core::ctr_inc(&g_c, C_SKIP_EMPTY);
            funnel_gate_log("空串", ra, 0);
            return;
        }

        // 闸门①：串里已经有 CJK（任一码元 ≥ 0x3000）⇒ 说明是上一轮留下的译文，幂等跳过。
        // 顺带在这同一趟里标出"含 ≥0x80 码位"（图标转义类）—— 后面的未命中分类要用。
        bool nonAscii = false;
        for (unsigned i = 0; i < v.len; i++) {
            unsigned short c = (unsigned short)v.buf[i];
            if (c >= 0x3000) {
                core::ctr_inc(&g_c, C_SKIP_CJK);
                funnel_gate_log("已含CJK", ra, (int)v.len);
                return;
            }
            if (c >= 0x80) nonAscii = true;
        }
        // 闸门②：词典没就绪 ⇒ 只观测
        if (!dict::ready()) {
            core::ctr_inc(&g_c, C_SKIP_DICT);
            funnel_gate_log("词典未就绪", ra, (int)v.len);
            return;
        }

        wchar_t key[DICT_KEY_MAX + 2];
        int     kn = dict::canon(v.buf, (int)v.len, key, DICT_KEY_MAX);
        if (kn <= 0) {
            core::ctr_inc(&g_c, C_SKIP_OTHER);
            return;
        }

        const dict::DictVal hit = dict::lookup(key, kn);
        if (!hit.p) {
            core::ctr_inc(&g_c, C_MISS);
            // 分类：纯 ASCII 的未命中 = 词典缺条目（可行动）；含 ≥0x80 的是图标转义类，本就不该翻。
            // 只有 ASCII 的才进 Top 列表 —— 那才是"该补哪条词典"的可行动信号。
            if (nonAscii)
                core::ctr_inc(&g_c, C_MISS_NONASCII);
            else {
                core::ctr_inc(&g_c, C_MISS_ASCII);
                top_remember(key, kn);
            }
            inv_remember(key, kn, INV_F_MISS | (nonAscii ? INV_F_NONASCII : 0), ra);
            if (core::ctr_get(&g_c, C_SAMPLE_MISS) < SAMPLE_N) {
                char txt[512];
                dump_wstr(txt, sizeof(txt), v.buf, (int)v.len);
                core::ctr_inc(&g_c, C_SAMPLE_MISS);
                core::log_line("[FUNNEL] 未命中样本 #%d ret=%08X len=%d 『%s』",
                               (int)core::ctr_get(&g_c, C_SAMPLE_MISS), (unsigned)(uintptr_t)ra, (int)v.len,
                               txt);
            }
            return;
        }

        // 走到这里必然命中：装载期拒绝空译文（ingest 的 SKIP_NOVAL），
        //   故 hit.p 非空时 len 恒 ≥ 1。
        const wchar_t* val = hit.p;
        const int      vlen = hit.len;

        if (core::ctr_get(&g_c, C_SAMPLE_HIT) < SAMPLE_N) {
            char txt[512];
            int  newLen = (int)v.len;
#if TEXT_REPLACE
            if (!replace_effective())
                newLen = (int)v.len; // forge 闸门关：与 TR0 同形（不替换）
            else if (v.res >= (unsigned)vlen)
                newLen = vlen;
            else
                newLen = -1; // 记成 "容量不足"
#endif
            dump_wstr(txt, sizeof(txt), v.buf, (int)v.len);
            core::ctr_inc(&g_c, C_SAMPLE_HIT);
            core::log_line("[FUNNEL] 命中样本 #%d ret=%08X len=%d→%d 命中=1 『%s』",
                           (int)core::ctr_get(&g_c, C_SAMPLE_HIT), (unsigned)(uintptr_t)ra, (int)v.len,
                           newLen, txt);
        }

        // 字符串全记录：命中的串也登记；"替换"标签与下面 #if 块同口径（能装下才算替换）
        inv_remember(key, kn,
                     INV_F_HIT | ((replace_effective() && v.res >= (unsigned)vlen) ? INV_F_REPLACED : 0), ra);

#if TEXT_REPLACE
        if (!replace_effective()) return; // forge 闸门关 ⇒ 只观测不替换（真·纯原版）
        if (v.res < (unsigned)vlen) {
            core::ctr_inc(&g_c, C_SKIPPED_GROW);
            return;
        } // 装不下：不动任何字节
        if (!core::mem_writable(v.buf, ((size_t)vlen + 1) * sizeof(wchar_t))) {
            core::ctr_inc(&g_c, C_SKIP_OTHER);
            return;
        }
        // 视图里的 buf 是 const（读语义）；上面 mem_writable 已确认这段可写，这里才去掉 const
        wchar_t* wbuf = (wchar_t*)v.buf;
        memcpy(wbuf, val, (size_t)vlen * sizeof(wchar_t));
        wbuf[vlen] = 0; // 尾 NUL（buf[_Myres] 本身是合法的）
        *(volatile unsigned*)((unsigned char*)dst + AC1_WSTR_OFF_SIZE) = (unsigned)vlen; // _Mysize
        // _Myres 一个字节都不动、不做等长填充：下游 assign 按 _Mysize 搬运 ⇒ 零填充
        core::ctr_inc(&g_c, C_REPLACED);
#endif
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        core::ctr_inc(&g_c, C_FAULT);
    }
}

namespace {

// noinline：_ReturnAddress() 依赖真实的栈帧，不能被内联掉
// （__declspec 必须写在指针返回类型之前，否则 MSVC 报 C2059）
__declspec(noinline) void* __cdecl hook_funnel(void* dst, const wchar_t* src)
{
    void* ra = _ReturnAddress(); // = 游戏里那个调用点的返回地址，用于反查是 24 个调用点里的哪个
    // 崩溃时能看出「崩的那一刻是漏斗在跑」，还能看出是哪个调用点。
    // track=1（不是只计数）：漏斗是本项目最可能出事的路径之一，量级只有几千次/轮。
    core::hooktrace_enter(g_traceSlot, (unsigned)(ULONG_PTR)ra, (unsigned)(ULONG_PTR)dst);
    void* r = g_origFunnel(dst, src); // 先放行 —— 语义完全不变
    run_post_counted(dst, ra);        // 再后处理
    core::hooktrace_leave(g_traceSlot);
    return r;
}

// ---- 漏斗的安装与观测（本模块对外 API 的实现）----
// 不再有 TextPath/注册表抽象：全项目只有这一条路径，为 N=1 维持虚接口 + 聚合
//   循环是纯间接税。将来真有第二条路径时，再从 git 历史捡回注册表模式。

static bool funnel_install()
{
    g_traceSlot = -1; // 未装上时 enter/leave 是空操作

    core::HookSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.name = "sub_8A1920 漏斗";
    spec.rva9 = kFunnelRva9;
    spec.rva10 = kFunnelRva10; // 0xBC3290（Dx10 真漏斗）
    spec.expect = kFunnelEntry;
    spec.expect_len = (int)sizeof(kFunnelEntry);
    spec.expect10 = kFunnelEntry10; // SEH 指针值不同 ⇒ 签名分栏
    spec.expect_len10 = (int)sizeof(kFunnelEntry10);
    spec.detour = (void*)&hook_funnel; // 两宿主同为 __cdecl（Dx10 尾 ret）
    spec.orig = (void**)&g_origFunnel;
    // 逐字节核对 + MinHook 落钩全在 core::hook_install_one 里，
    // 任何一步失败都只打日志、绝不硬装
    const int r = core::hook_install_one(spec, core::hook_base());
    // 槽位由 core 单点分配并回填 —— 本模块不自己登记（自己登记会让
    //   同一个钩在册上占两格）。
    if (r) {
        g_traceSlot = spec.traceSlot;
        core::hooktrace_set_policy(g_traceSlot, 2000, 200);
    }
    core::log_line("[FUNNEL] 配置：TEXT_REPLACE=%d（%s） forge 闸门=%s 词典匹配=%s 样本上限=%d", TEXT_REPLACE,
                   TEXT_REPLACE ? "**真替换**" : "只观测、不写任何字节",
                   g_forgePatch ? "开" : "**关（只观测不替换）**", "canon+精确相等", SAMPLE_N);
    return r != 0;
}

// 计数快照：明写逐字段拷贝（不用指针算术跨字段映射：那种写法在改动结构体时最容易悄悄错位）
static TextCounters snapshot()
{
    TextCounters s;
    s.hits = (int)core::ctr_get(&g_c, C_HITS);
    s.replaced = (int)core::ctr_get(&g_c, C_REPLACED);
    s.miss = (int)core::ctr_get(&g_c, C_MISS);
    s.missAscii = (int)core::ctr_get(&g_c, C_MISS_ASCII);
    s.missNonAscii = (int)core::ctr_get(&g_c, C_MISS_NONASCII);
    s.skippedGrow = (int)core::ctr_get(&g_c, C_SKIPPED_GROW);
    s.fault = (int)core::ctr_get(&g_c, C_FAULT);
    s.skipEmpty = (int)core::ctr_get(&g_c, C_SKIP_EMPTY);
    s.skipCjk = (int)core::ctr_get(&g_c, C_SKIP_CJK);
    s.skipDictNotReady = (int)core::ctr_get(&g_c, C_SKIP_DICT);
    s.skipBadView = (int)core::ctr_get(&g_c, C_SKIP_BADVIEW);
    s.skipOther = (int)core::ctr_get(&g_c, C_SKIP_OTHER);
    s.sampleHit = (int)core::ctr_get(&g_c, C_SAMPLE_HIT);
    s.sampleMiss = (int)core::ctr_get(&g_c, C_SAMPLE_MISS);
    s.topCount = top_count();
    return s;
}

static void funnel_status(char* out, int cap)
{
    const TextCounters k = snapshot();
    core::Str          s;
    core::str_init(&s, out, cap);
    core::str_addf(&s,
                   "FUNNEL 触发=%d 替换=%d 未命中=%d(ASCII %d/非ASCII %d) "
                   "跳过=空串 %d/含CJK %d/词典未就绪 %d/结构不可信 %d/其它 %d "
                   "容量不足=%d 异常=%d 样本=%d+%d/%d",
                   k.hits, k.replaced, k.miss, k.missAscii, k.missNonAscii, k.skipEmpty, k.skipCjk,
                   k.skipDictNotReady, k.skipBadView, k.skipOther, k.skippedGrow, k.fault, k.sampleHit,
                   k.sampleMiss, SAMPLE_N);
}

// 追加在主行后面那条：「未命中 ASCII Top」
// 每条都是 canon 之后的键（真正拿去查表的形态），\uXXXX 转义，≤TOP_LEN 码元。
static void funnel_detail(char* out, int cap)
{
    const TextCounters k = snapshot();
    int                n = k.topCount;
    if (n > TOP_N) n = TOP_N;
    // 整段走 core::Str（按实际写入量推进 len，永不越界、永不留半个 \u 转义）。
    //   「拿 _snprintf 返回值当写偏移」的写法是越界来源，见 core/str.h。
    core::Str s;
    core::str_init(&s, out, cap);
    core::str_addf(&s, "未命中 ASCII Top（去重，≤%d 条 / 每条 ≤%d 码元）：", TOP_N, TOP_LEN);
    if (n == 0) {
        core::str_addf(&s, "（空）");
        return;
    }
    for (int i = 0; i < n; i++) {
        core::str_addf(&s, "%s", i ? " ｜ " : "");
        for (int j = 0; j < g_topLen[i]; j++)
            core::str_addf(&s, "\\u%04X", (unsigned)(unsigned short)g_top[i][j]);
        if (core::str_full(&s)) break; // 满了就别再空转
    }
}

} // namespace

static int  g_installed = 0;
static char g_status[640] = "";
static char g_detail[1024] = "";

bool install()
{
    g_installed = funnel_install() ? 1 : 0;
    core::log_line("[文本] 漏斗钩：%s", g_installed ? "已装 ✓" : "**未装（只观测不上钩）**");
    return g_installed != 0;
}

TextCounters counters() { return snapshot(); }

const char* status_line()
{
    funnel_status(g_status, (int)sizeof(g_status));
    return g_status;
}

const char* detail_line()
{
    funnel_detail(g_detail, (int)sizeof(g_detail));
    return g_detail;
}

void funnel_run_post(void* dst, unsigned ra) { run_post_counted(dst, (void*)(uintptr_t)ra); }

} // namespace text
} // namespace ac1
