// modules/core/src/trace.cpp
//
// 钩执行迹 = 每线程面包屑栈 + 崩溃转储。
//
// 本文件里禁止出现：需要展开的对象（C++ 异常/dtor）、堆分配、CRT 的
//   stdio、以及任何可能阻塞的东西。理由：
//     · hooktrace_enter/leave 跑在渲染线程与引擎回调里；
//     · veh_dump 跑在异常分发过程中，那是最不能分配、不能死锁的场合。
//   全部状态是文件作用域的定长数组（BSS），零堆。

#include "ac1/core/trace.h"

#include "ac1/core/log.h"
#include "ac1/core/file.h"
#include "ac1/core/mem.h"
#include "ac1/core/str.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace ac1 {
namespace core {
namespace {

// ---- 登记表 ----
struct SlotDef {
    const char*   name;
    HookPolicy    p;
    volatile LONG hits;   // 进入次数
    volatile LONG logged; // 已打的明细行数
};

SlotDef       g_slots[TRACE_SLOTS_MAX];
volatile LONG g_nslots = 0;

// 登记表写锁：只被 hooktrace_register 使用（冷路径：装配期/单测各登记一次）。
//   热路径（enter/leave）与 veh_dump 一律不上锁 —— 它们只读 g_nslots 以下的格，
//   而本锁保证「扫重 → 填格 → 发布 g_nslots」整段串行 ⇒ 它们脚下「行 < g_nslots
//   即完整」的不变量成立。没有它，两个线程会读到同一个 n、同写 g_slots[n]：一个登记
//   被覆盖丢失，计数与策略互相踩；g_nslots 也不再等于真实行数（状态行出现幽灵行，
//   最坏会超过 TRACE_SLOTS_MAX）。
// 0 = 空闲、1 = 持有。临界区里只有几次存储（日志在锁外打），不值得引入内核锁；
//   YieldProcessor = _mm_pause（winnt.h，<windows.h> 已引入）。
volatile LONG g_regLock = 0;

void reg_lock()
{
    while (InterlockedCompareExchange(&g_regLock, 1, 0) != 0) YieldProcessor();
}
void reg_unlock() { InterlockedExchange(&g_regLock, 0); }

// ---- 每线程栈 ----
struct ThreadStack {
    volatile LONG claimed; // 0 = 未认领，1 = 已认领
    DWORD         tid;
    volatile LONG depth;    // 当前深度（进入 +1，离开 -1）
    volatile LONG overflow; // 超出 TRACE_DEPTH_MAX 的次数（诊断用）
    HookRec       rec[TRACE_DEPTH_MAX];
};

ThreadStack g_ts[TRACE_THREADS_MAX];

// ---- 崩溃日志 ----
HANDLE        g_crash = INVALID_HANDLE_VALUE;
char          g_crashPath[MAX_PATH] = "";
PVOID         g_veh = NULL;
volatile LONG g_dumping = 0;    // 防转储自己再 fault 时递归
volatile LONG g_crashDumps = 0; // 转储总次数

// 认领当前线程的栈槽。线程数超上限时返回 NULL ——
// 此时只丢追踪，不影响任何功能（钩子照常工作，只是崩溃时看不到它）。
ThreadStack* ts_self()
{
    const DWORD tid = GetCurrentThreadId();
    for (int i = 0; i < TRACE_THREADS_MAX; i++) {
        if (g_ts[i].claimed == 1 && g_ts[i].tid == tid) return &g_ts[i];
    }
    for (int i = 0; i < TRACE_THREADS_MAX; i++) {
        // 认领用一次 CAS 把 claimed 由 0 翻成 1，之后才写 tid。并发读者只会看到两种
        //   状态：claimed==1 而 tid 仍是 0（BSS 初值），或 claimed==1 而 tid 已就位。
        //   GetCurrentThreadId() 永不返回 0 ⇒ 前一种不会误命中本线程，只会跳过该槽；
        //   槽位一经认领不再回收，下次调用必定重新找到它。x86 是 TSO，存储不重排。
        if (InterlockedCompareExchange(&g_ts[i].claimed, 1, 0) == 0) {
            g_ts[i].tid = tid;
            g_ts[i].depth = 0;
            g_ts[i].overflow = 0;
            return &g_ts[i];
        }
    }
    return NULL; // 线程槽用尽
}

ThreadStack* ts_find(DWORD tid)
{
    for (int i = 0; i < TRACE_THREADS_MAX; i++)
        if (g_ts[i].claimed && g_ts[i].tid == tid) return &g_ts[i];
    return NULL;
}

const SlotDef* slot_at(int slot)
{
    // 负号与越界都要挡。g_nslots 恒 ≤ TRACE_SLOTS_MAX（登记时封顶），故不必再单独测 MAX。
    if (slot < 0 || (LONG)slot >= g_nslots) return NULL;
    return &g_slots[slot];
}

// ---- VEH ----

// 只处理「致命且与我们相关」的异常码。其余一律放过。
bool worth_reporting(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:    // 0xC0000005 —— 绝大多数
    case EXCEPTION_STACK_OVERFLOW:      // 0xC00000FD
    case EXCEPTION_ILLEGAL_INSTRUCTION: // 0xC000001D
    case EXCEPTION_PRIV_INSTRUCTION:    // 0xC0000096
    case EXCEPTION_IN_PAGE_ERROR:       // 0xC0000006
    case 0xC0000409:                    // STATUS_STACK_BUFFER_OVERRUN
    case 0xC0000374:                    // STATUS_HEAP_CORRUPTION
        return true;
    default: return false;
    }
}

// 纯 Win32 追加写：VEH 里不能用 core::log_line（它拿 CRITICAL_SECTION，
// 崩溃瞬间可能正被别人持着 ⇒ 死锁），也不能用 CRT。
void raw_write(const char* s, int n)
{
    HANDLE h = g_crash;
    if (h == INVALID_HANDLE_VALUE || !s || n <= 0) return;
    DWORD wr = 0;
    if (!WriteFile(h, s, (DWORD)n, &wr, NULL)) return;
    FlushFileBuffers(h); // 必须落盘：崩了之后没人再替我们 flush
}

void veh_dump(DWORD code, PVOID addr, DWORD tid)
{
    if (InterlockedCompareExchange(&g_dumping, 1, 0) != 0) return; // 已在转储
    InterlockedIncrement(&g_crashDumps);

    char      buf[2048];
    char*     p = buf;
    const int cap = (int)sizeof(buf);

    // 用 core::Str 组装（它保证不越界），再一次性 raw_write。
    Str s;
    str_init(&s, p, cap);
    str_addf(&s, "\r\n[崩溃] ===== AC1_CJK 捕获 =====\r\n");
    str_addf(&s, "[崩溃] code=%08X addr=%08X tid=%08X 转储=%ld\r\n", (unsigned)code,
             (unsigned)(ULONG_PTR)addr, (unsigned)tid, (long)g_crashDumps);

    ThreadStack* ts = ts_find(tid);
    int          depth = 0;
    if (ts) depth = (int)ts->depth;

    if (depth <= 0) { str_addf(&s, "[崩溃] ★ 未记录到任何钩 ⇒ 崩在游戏自身，不是我们的钩\r\n"); }
    else {
        // depth>0 ⇒ ts 必非空（depth 只在 ts 非空时被赋值）
        str_addf(&s, "[崩溃] 钩栈（由内到外，共 %d 层；溢出丢弃 %ld 次）：\r\n", depth, (long)ts->overflow);
        // rec[] 的下标是进入次序 ⇒ rec[0] 最先进入（最外层）、rec[depth-1] 才是崩点。
        //   这里**逆序**打印并把 #0 固定成最内层：钩栈存在的唯一用途就是定位
        //   「哪个钩弄崩的」，顺着打印会把最不可能出事的入口钩排在 #0。
        //   溢出的那几层没有记录（enter 只写 dep ≤ TRACE_DEPTH_MAX），
        //   此时最内层仍是 rec[TRACE_DEPTH_MAX-1]，上界靠 n 收口。
        const int n = (depth < TRACE_DEPTH_MAX) ? depth : TRACE_DEPTH_MAX;
        for (int i = n - 1; i >= 0; i--) {
            const HookRec* r = &ts->rec[i];
            const SlotDef* d = slot_at((int)r->hookId);
            // 这段跑在真实崩溃的异常分发里，r->hookId 可能是脏的：判空不是多余，
            //   它的作用是让「崩溃报告」不至于自己二次崩溃。
            str_addf(&s, "[崩溃]   #%d %-14s a0=0x%08X a1=0x%08X 累计=%ld\r\n", n - 1 - i, d ? d->name : "?",
                     r->a0, r->a1, d ? (long)d->hits : 0L);
        }
    }
    str_addf(&s, "[崩溃] ===== 结束 =====\r\n");

    raw_write(buf, str_len(&s));

    InterlockedExchange(&g_dumping, 0);
}

LONG CALLBACK veh(PEXCEPTION_POINTERS ep)
{
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (!worth_reporting(code)) return EXCEPTION_CONTINUE_SEARCH;

    PVOID addr = NULL;
    DWORD tid = GetCurrentThreadId();
    if (ep->ExceptionRecord->NumberParameters >= 2)
        addr = (PVOID)(ULONG_PTR)ep->ExceptionRecord->ExceptionInformation[1];
    else if (ep->ContextRecord)
        addr = (PVOID)(ULONG_PTR)ep->ContextRecord->Eip;

    veh_dump(code, addr, tid);

    // 永远继续搜索：我们是旁观者，绝不改变崩溃语义。
    //   返回 CONTINUE_EXECUTION 会吞掉异常、改变行为 —— 那比没有这个 VEH 危险得多。
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

// ======================================================================
// 登记
// ======================================================================

int hooktrace_register(const char* name, HookPolicy p)
{
    const char* nm = name ? name : "?";
    reg_lock();              // 本函数是 g_nslots 的唯一写者；并发调用在这里串行化
    const LONG n = g_nslots; // 锁内快照：持锁期间没有第二个写者 ⇒ 不会过期
    int        slot = -1;
    for (int i = 0; i < n && i < TRACE_SLOTS_MAX; i++) {
        if (g_slots[i].name && strcmp(g_slots[i].name, nm) == 0) { // 幂等
            slot = i;
            break;
        }
    }
    int full = 0;
    if (slot < 0) {
        if (n >= TRACE_SLOTS_MAX)
            full = 1;
        else {
            g_slots[n].name = nm;
            g_slots[n].p = p;
            g_slots[n].hits = 0;
            g_slots[n].logged = 0;
            InterlockedExchangeAdd(&g_nslots, 1); // 发布在填格之后 ⇒ 读者看不到半初始化行
            slot = (int)n;
        }
    }
    reg_unlock(); // 唯一出口：本函数内不许再出现提前 return（否则后续登记会永久自旋）
    // 日志（含文件 I/O）放在锁外：自旋锁里只做几次存储。
    if (full) log_line("[迹] !! 登记 %s 失败：槽位已满（%d）", nm, TRACE_SLOTS_MAX);
    return slot;
}

// 已登记则原样返回、**不覆盖策略**（register 的幂等分支就是这个行为，无需在这里再扫一遍）。
int hooktrace_ensure(const char* name, HookPolicy p) { return hooktrace_register(name, p); }

HookPolicy hooktrace_default_policy()
{
    HookPolicy p;
    p.track = 1;    // 进面包屑栈：崩溃时能看出是它
    p.logEvery = 0; // 默认不打明细行（热钩逐次打会刷屏，见 trace.h 开头）
    p.logBudget = 0;
    return p;
}

const char* hooktrace_crash_path() { return g_crashPath; }

void hooktrace_set_policy(int slot, int logEvery, int logBudget)
{
    if (slot < 0 || slot >= (int)g_nslots) return; // 上界由 hooktrace_register 单点封顶
    g_slots[slot].p.logEvery = logEvery;
    g_slots[slot].p.logBudget = logBudget;
    InterlockedExchange(&g_slots[slot].logged, 0);
}

// ======================================================================
// 热路径
// ======================================================================

void hooktrace_enter(int slot, unsigned a0, unsigned a1)
{
    const SlotDef* d = slot_at(slot);
    if (!d) return;

    const LONG hits = InterlockedIncrement((volatile LONG*)&d->hits);
    // 明细行：冷钩每次都记、热钩按 logEvery 采样，且受 budget 封顶。
    // 判定与写日志都放在这里做，但只有真的要打时才付打日志的钱
    //   （status 判断是几个整数比较）。
    // 策略字段一次快照：hooktrace_set_policy 会并发改写它们，重复读可能让下面的 %
    //   撞上 0（同一字段第一次读非 0、第二次读成 0 ⇒ 整数除零）。
    const int every = d->p.logEvery;
    const int budget = d->p.logBudget;
    if (every > 0) {
        const bool hitEvery = (every == 1) || (hits % every) == 0;
        // budget 数的是已打出的行数，不是进入次数——两者混用会让上限在第一次
        //   采样点之前就被用光。
        // 名额靠 CAS 抢 ⇒ 上限精确：多线程同时到达时只有一个能把它从 <budget 推到
        //   budget。先无锁读一次做快筛，真到了采样点才付原子操作的钱。
        if (hitEvery) {
            LONG k = 0;
            if (budget <= 0) { k = InterlockedIncrement((volatile LONG*)&d->logged); }
            else {
                for (;;) {
                    const LONG cur = InterlockedCompareExchange((volatile LONG*)&d->logged, 0, 0);
                    if (cur >= budget) break; // 名额已用尽
                    if (InterlockedCompareExchange((volatile LONG*)&d->logged, cur + 1, cur) == cur) {
                        k = cur + 1;
                        break;
                    }
                }
            }
            if (k) log_line("[迹] %-14s #%ld/%d a0=0x%08X a1=0x%08X", d->name, (long)hits, (int)k, a0, a1);
        }
    }

    if (!d->p.track) return;

    ThreadStack* ts = ts_self();
    if (!ts) return;
    const LONG dep = InterlockedIncrement(&ts->depth);
    if (dep >= 1 && dep <= TRACE_DEPTH_MAX) {
        // 下标 = 进入次序：dep==1 是**最先**进入的那个钩 ⇒ rec[0] 是最外层，
        //   rec[depth-1] 才最靠近崩溃点。对外一律以 rec[depth-1] 为 #0
        //   （由内到外），见 veh_dump 与 hooktrace_snapshot。
        HookRec* r = &ts->rec[dep - 1];
        r->hookId = (unsigned short)slot;
        r->a0 = a0;
        r->a1 = a1;
    }
    else if (dep > TRACE_DEPTH_MAX) {
        InterlockedIncrement(&ts->overflow);
    }
}

void hooktrace_leave(int slot)
{
    const SlotDef* d = slot_at(slot);
    // 必须与 enter 对称。
    //   enter 在 track==0 时提前返回、根本没碰 depth；若 leave 不做同样的判断，
    //   就会凭空减一层 ⇒ 深度变负、栈永远弹不干净 ⇒ 崩溃转储把早已返回的钩全列进去，
    //   正好毁掉这套东西唯一的价值。同理非法槽位也不能减。
    if (!d || !d->p.track) return;
    ThreadStack* ts = ts_self();
    if (!ts) return;
    InterlockedDecrement(&ts->depth);
}

// ======================================================================
// 崩溃转储
// ======================================================================

int hooktrace_veh_install(const char* dir)
{
    if (g_veh) return 1; // 幂等
    if (dir && dir[0]) {
        // 路径拼装带容量自检（装不下就返回失败），拼不出来时 g_crashPath 留空 ——
        //   调用方看到空路径就会报「崩溃转储不可用」，而不是拿到一个被截断的文件名。
        char sub[MAX_PATH], name[64];
        _snprintf(name, sizeof(name) - 1, "crash_%s.log", log_run_stamp()); // 与 run_<stamp>.log 同一轮
        name[sizeof(name) - 1] = 0;
        if (path_join(sub, (int)sizeof(sub), dir, "AC1_CJK") &&
            path_join(g_crashPath, (int)sizeof(g_crashPath), sub, name)) {
            // 预先打开句柄：VEH 里不能 CreateFile（可能已在 loader lock 或文件系统异常态）。
            //   没打开成功就把路径清空 —— 转储会静默变成「什么都不写」，
            //   而调用方以为自己在记日志。
            CreateDirectoryA(sub, NULL); // 与 log.cpp 同一个产出目录（幂等）
            g_crash = CreateFileA(g_crashPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (g_crash == INVALID_HANDLE_VALUE) g_crashPath[0] = 0;
        }
    }
    g_veh = AddVectoredExceptionHandler(1, veh); // 优先级 1：尽量排在最前
    return g_veh != NULL;
}

void hooktrace_dump_now(const char* reason)
{
    veh_dump(0xDEADBEEFu, NULL, GetCurrentThreadId());
    if (reason) log_line("[迹] 已转储当前钩栈：%s", reason);
}

void hooktrace_shutdown()
{
    if (g_veh) {
        RemoveVectoredExceptionHandler(g_veh);
        g_veh = NULL;
    }
    if (g_crash != INVALID_HANDLE_VALUE) {
        CloseHandle(g_crash);
        g_crash = INVALID_HANDLE_VALUE;
    }
    g_crashPath[0] = 0;
}

// ======================================================================
// 观测
// ======================================================================

long hooktrace_count(int slot)
{
    const SlotDef* d = slot_at(slot);
    return d ? (long)d->hits : 0;
}

int hooktrace_slots() { return (int)g_nslots; }

const char* hooktrace_status(char* out, int cap)
{
    if (!out || cap <= 0) return ""; // 容量不可用 ⇒ 返回空串，绝不让调用方读到未写入的缓冲
    Str s;
    str_init(&s, out, cap);
    str_addf(&s, "HOOK(%d)", (int)g_nslots);
    const LONG n = g_nslots;
    for (int i = 0; i < n && i < TRACE_SLOTS_MAX; i++) {
        str_addf(&s, " %s=%ld", g_slots[i].name ? g_slots[i].name : "?",
                 (long)InterlockedCompareExchange((volatile LONG*)&g_slots[i].hits, 0, 0));
    }
    if (g_crashDumps) str_addf(&s, " 崩溃转储=%ld", (long)g_crashDumps);
    return out;
}

// 由内到外填：recs[0] = 最内层（崩点那一层），与 veh_dump 的 #0 同口径。
//   nMax 不够时保留**最内层**的 nMax 条（离崩点最近的那几层才是要看的东西）。
int hooktrace_snapshot(HookRec* recs, int nMax)
{
    ThreadStack* ts = ts_self();
    if (!ts) return 0;
    int total = (int)ts->depth;
    if (total > TRACE_DEPTH_MAX) total = TRACE_DEPTH_MAX;
    if (total < 0) total = 0;
    int take = total;
    if (nMax > 0 && recs && nMax < take) take = nMax;
    for (int i = 0; i < take; i++) recs[i] = ts->rec[total - 1 - i];
    return take;
}

int hooktrace_depth()
{
    ThreadStack* ts = ts_self();
    return ts ? (int)ts->depth : 0;
}

} // namespace core
} // namespace ac1
