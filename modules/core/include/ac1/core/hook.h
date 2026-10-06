#pragma once

// modules/core —— 挂点表（数据驱动）
//
// 一个挂点 = 一行静态数据：名字、按宿主口味分栏的 RVA、期望入口字节、detour、
// 原函数回填位。安装时逐字节核对期望字节；任何一条不符就跳过并打日志，绝不硬装
// （硬装到签名不符的地址 = 往未知指令流里跳 5 个字节）。
//
// RVA 与签名按宿主口味分栏：rva9 = Dx9（imagebase 0x400000），rva10 = Dx10
// （0 = 未确认 ⇒ 不装）；expect10 为 NULL = 与 expect 相同（两宿主同字节）。
// 用哪一列由 host.h 的口味判定决定（本模块内部直接问 host_flavor()）。
//
// 落钩机制：MinHook（third_party/minhook，只读）。它是进程全局单例，只编进
// core.lib 一份——任何地方不许再编第二份，否则两张钩子表互不可见。
// 搬迁/回填由 MinHook 的 hde32 负责；本模块只负责它不做的那件事：写前核对签名。
//
// 本模块只装不卸。core 不依赖任何其它模块。

#include <windows.h>

namespace ac1 {
namespace core {

enum {
    HOOK_SIG_MIN = 5 // 入口签名核对的下限字节数：MinHook 的跳转至少要改 5 字节，
                     // 核对量小于它 ⇒ 有字节没被核对过
};

struct HookSpec {
    const char*          name;       // 日志里的挂点名
    unsigned             rva9;       // Dx9 宿主 RVA（VA - 0x400000）；0 = 不适用
    unsigned             rva10;      // Dx10 宿主 RVA；0 = 未确认
    const unsigned char* expect;     // 期望入口字节（Dx9；两宿主同字节时共用）
    int                  expect_len; // 参与核对的字节数（下限 HOOK_SIG_MIN，hook_install_one 会挡）
    const unsigned char* expect10;   // Dx10 的期望字节；NULL = 与 expect 相同
    int                  expect_len10;
    void*                detour;    // 我们的钩子
    void**               orig;      // MinHook 回填的 trampoline 入口
    int                  traceSlot; // 出参：落钩成功后 core 回填钩执行迹槽位（<0 = 登记失败，
                                    //   只是不追踪，功能不受影响）
};

// 挂点表的签名列是否可用（纯函数，无副作用 ⇒ 离线单测直接打它）。
// 两列都要有字节，且都不短于 HOOK_SIG_MIN；expect10 为 NULL 时只看 expect 这一列。
// 返回 1 = 可用，0 = 不该拿它去落钩。
int hook_sig_ok(const HookSpec& spec);

// 逐字节核对（纯函数，无副作用 ⇒ 离线单测直接打它）。
// 返回 1 = 全部符合；0 = 不符，*firstBad = 第一个不符的字节下标（-1 = 全部符合）。
int hook_bytes_match(const void* at, const unsigned char* expect, int n, int* firstBad);

// 把 p[0..n) 格式化成 "6A FF 68"（大写十六进制、单空格分隔、无尾空格）写入 out。
// 纯函数（只写 out、无全局状态）⇒ 离线单测直接打它。
// 返回实际格式化的输入字节数 k（0 <= k <= n）；k < n ⇔ 因 cap 不足被截断。
// cap > 0 时 out 恒为合法 C 串（含 p==NULL / n<=0 / 一个字节都放不下 / 截断四种情形）；
// cap <= 0 或 out == NULL 时一个字节都不写、返回 0。
// 内部用 CRT 的 _snprintf ⇒ 不得在 VEH/异常分发路径里调（那条路径的约束见 trace.h）。
int hook_hexdump(char* out, int cap, const unsigned char* p, int n);

// 取当前宿主适用的一列 RVA；不适用返回 0
unsigned hook_rva(const HookSpec& spec);

// 装一条。返回 1 = 已装；0 = 跳过（日志已写）。跳过原因：当前宿主无 RVA 列 /
// 条目不完整 / 签名列不合规（见 hook_sig_ok）/ 目标不是已提交映像页或读不出来 /
// 签名不符 / MinHook 失败（初始化/建钩/使能任一步 MH_STATUS ≠ MH_OK）。
// base 传 0 = 用 hook_base()。
// 注意 spec 不是 const：落钩成功后 core 把钩执行迹槽位写进 spec.traceSlot。
int hook_install_one(HookSpec& spec, unsigned base);

// 显式设置基址（0 = 跟随 host_base()）。单测用它喂假基址。
void     hook_set_base(unsigned b);
unsigned hook_base();

} // namespace core
} // namespace ac1
