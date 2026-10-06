#pragma once

// modules/core —— 钩执行迹（hook trace）
//
// 用途只有一个：崩溃时判断是哪个模块、哪个 hook 弄崩的。
//
// 常规日志覆盖不了这一刻：常驻状态行每 10 秒一轮，崩溃时那份可能已陈旧；而崩的就是
// 某个钩本身时，那个钩永远来不及给自己写日志 —— 要的信息必须在它进入时就埋下。
//
// 机制：每个钩进入时把「我是谁 + 两个上下文值」压进属于本线程的固定栈，
// 正常返回时弹出。进程随后发生未处理异常时，VEH（见 hooktrace_veh_install）
// 在异常分发时读栈——残留的正是「崩的那一刻由内到外的调用链」，写进崩溃日志。
// **次序契约**：报告里的 `#0` 恒为最内层（也就是崩点本身），编号越大越靠外；
//   栈内记录按进入次序存放（先进入的在下标 0），由 veh_dump / hooktrace_snapshot
//   统一逆序对外，两者口径一致。
// 栈空时明确写出「未记录到任何钩 ⇒ 崩在游戏自身」——这把「我们弄崩的」和
// 「游戏自己崩的」分开。
//
// 热路径成本：每钩进出各一次 GetCurrentThreadId()（x86 读 TEB，非系统调用）
// + ≤ TRACE_THREADS 槽线性探测 + 写 12 字节（HookRec）+ 改一次 depth。无 I/O、无分配、
// 无字符串格式化、无锁。
//
// 刻意不用 __declspec(thread)：x86 静态 TLS 依赖加载器为该模块建 TLS 目录，
// 而本插件由 ASI Loader 装入，加载方式未确认过——固定数组绕开这一点。
//
// 异常上下文里必须零分配、零 STL、零 CRT——VEH 的存在决定了本文件的全部形状。

#include <windows.h>

namespace ac1 {
namespace core {

// ---- 固定容量（编译期定死，运行期零分配）----
enum {
    TRACE_SLOTS_MAX = 32,  // 最多登记多少个钩
    TRACE_THREADS_MAX = 8, // 同时跟踪多少个线程（渲染线程 + 主线程 + …）
    TRACE_DEPTH_MAX = 24   // 单线程最深嵌套
};

// 一个钩在栈上留的记录。热路径上每多写一个字节都是钱。实际布局 12 字节
// （hookId 为 unsigned short，尾部对齐填充 2 字节）。
struct HookRec {
    unsigned short hookId; // 登记时拿到的槽位
    unsigned       a0, a1; // 两个上下文值。各钩自选，见下表。
};

// 各钩建议放什么进 a0/a1（只是建议，core 不关心语义）：
//     FUNNEL        → a0 = ret 地址（引擎里那个 call 点的返回地址）
//     DRAWTEXT      → a0 = font 指针
//     LOOKUP        → a0 = 查的码位 C
//     GLYPH_FONTLOAD → a0 = font 指针
struct HookPolicy {
    int track;     // 1 = 进面包屑栈（崩溃时可见）；0 = 只计数（热钩默认）
    int logEvery;  // 每 N 次记一条明细行；0 = 不记明细
    int logBudget; // 明细行总上限；<=0 表示不限
};

// 登记一个钩。返回槽位（>=0），重复登记同名返回已有槽位。
// 必须在该钩可能被调用之前完成（core 的 hook_install_one 成功时会自动登记）。
// 可在任意线程调用：冷路径，内部串行化；热路径（enter/leave）与 veh_dump 不上锁。
int hooktrace_register(const char* name, HookPolicy p);

// ---- 热路径。detour 顶上/底下各调一次。成对使用。----
void hooktrace_enter(int slot, unsigned a0, unsigned a1);
void hooktrace_leave(int slot);

// 调整一个已登记钩的采样策略（logEvery / logBudget）。
// 槽位由 core 单点分配并回填（HookSpec::traceSlot），模块没法在登记时交策略 ⇒
// 单独这个入口。logEvery<=0 不打明细行；logBudget<=0 不限。槽位无效时静默无操作。
void hooktrace_set_policy(int slot, int logEvery, int logBudget);

// 注册 VEH。必须在装任何钩之前调用（越早越好，它要盖住后面的所有异常）。
// 幂等：重复调用返回 1 且不重复注册。
int hooktrace_veh_install(const char* dir);

// 主动把当前钩栈写进崩溃日志（不依赖发生异常）。单测用。
void hooktrace_dump_now(const char* reason);

// 拆掉 VEH 并关掉崩溃日志句柄（单测用；生产路径不调用）。
void hooktrace_shutdown();

// ---- 只读观测 ----

// 幂等地确保已登记：已登记则原样保留并返回现有槽位（不覆盖策略）。
// core 的安装器在落钩成功时用它自动登记 ⇒ 凡是 core 装上的钩一定在册。
// 返回 -1 = 槽位已满（此时只是不追踪，功能不受影响）。
int hooktrace_ensure(const char* name, HookPolicy p);

// 默认策略：进面包屑栈（崩溃时可见）+ 不打明细行。
// 热钩可以先登记一个 track=0 的策略把它降级成「只计数」。
HookPolicy hooktrace_default_policy();

// 崩溃日志的完整路径（未就绪时返回空串）。装配层用它打一行「已就绪」。
const char* hooktrace_crash_path();

// 某个钩累计被进入多少次。
long hooktrace_count(int slot);

// 状态行片段：`HOOK FUNNEL=5096 DRAWTEXT=84210 …`。写入调用方缓冲并返回它；
// out==NULL 或 cap<=0 时不写任何字节、返回空串（调用方拿不到容量就别读缓冲）。
const char* hooktrace_status(char* out, int cap);

// 本进程登记过的钩数。
int hooktrace_slots();

// 单测用：把当前线程的栈复制出来（由内到外填 recs/nRecs —— recs[0] = 最内层，
// 与崩溃报告的 #0 同口径）。nMax 不够时保留最内层的 nMax 条。
// 返回实际条数；nMax<=0 时只返回条数不复制。
int hooktrace_snapshot(HookRec* recs, int nMax);

// 当前线程栈深（单测/诊断用）。
int hooktrace_depth();

} // namespace core
} // namespace ac1
