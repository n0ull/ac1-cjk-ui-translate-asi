// modules/core/src/hook.cpp
//
// 落钩机制 = MinHook（third_party/minhook/，BSD，源文件在本模块编译进 core.lib）。
// 本模块没有任何自研的字节改写/搬迁代码：MinHook 用 hde32 量出指令边界并对
// 相对位移重定位。
//
// 本模块保留的、MinHook 不做的事只有一件：写字节之前先逐字节核对签名。
//   MinHook 只看地址可不可执行，不看那里是不是你以为的代码。宿主版本一换，
//   签名不符的地址照样挂得上 ⇒ 崩溃。所以核对留在这里，日志也留在这里。
//
// MinHook 是进程全局单例（g_hooks 一张表、MH_Initialize 全进程只成功一次）。
//   所以它的源文件只编一次（在本模块），全仓库别处不得再编第二份 —— 两份实现
//   各有一张互不可见的表，谁都看不见对方改过的字节。

#include "ac1/core/hook.h"

#include "ac1/core/host.h"
#include "ac1/core/log.h"
#include "ac1/core/trace.h"
#include "ac1/core/mem.h"

#include "MinHook.h"

#include <string.h>
#include <stdio.h>

namespace ac1 {
namespace core {
namespace {

unsigned g_baseOverride = 0; // 0 = 跟随 host_base()

// MH_Initialize 只做一次。MH_ERROR_ALREADY_INITIALIZED 不是失败，
// 是同一个进程全局单例已被初始化过。
bool g_mhReady = false;

// 建钩之前必须已经初始化过。返回 1 = 可以往下走。
// MH_ERROR_ALREADY_INITIALIZED 一律当成功 —— 见上面 g_mhReady 的说明。
int ensure_minhook(const char* who)
{
    if (g_mhReady) return 1;
    const MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        log_line("[钩] !! MH_Initialize 失败（%s）⇒ %s 跳过", who, MH_StatusToString(st));
        return 0;
    }
    g_mhReady = true;
    if (st == MH_ERROR_ALREADY_INITIALIZED)
        log_line("[钩] MinHook 已由同进程别的模块初始化过 ⇒ 沿用同一个单例");
    return 1;
}

} // namespace

// 签名不符诊断的唯一格式化器；契约见 hook.h。纯函数 ⇒ 离线单测直接打它
//   （这段代码只在「宿主版本不符」时才跑，之前没有任何断言兜着）。
int hook_hexdump(char* out, int cap, const unsigned char* p, int n)
{
    if (!out || cap <= 0) return 0; // 拿不到容量 ⇒ 一个字节都不写
    out[0] = 0;
    if (!p || n <= 0) return 0;

    const size_t c = (size_t)cap;
    size_t       o = 0;
    for (int i = 0; i < n && o + 5 < c; i++) {
        // MSVC 的 _snprintf 截断时返回**负数**（不是 ISO 的「本想写的长度」）。
        //   不判它就直接 (size_t) 转换，o 会变成天文数字、循环退出后 out[o-1] 越界。
        const int w = _snprintf(out + o, c - o, "%02X ", (unsigned)p[i]);
        if (w < 0) break;
        o += (size_t)w;
    }
    if (o) out[o - 1] = 0; // 替掉最后一个空格；每轮恒写 3 字节 ⇒ o 是 3 的倍数
    return (int)(o / 3);   // 实际格式化的输入字节数；< n ⇔ 被 cap 截断
}

int hook_bytes_match(const void* at, const unsigned char* expect, int n, int* firstBad)
{
    if (firstBad) *firstBad = -1;
    if (!at || !expect || n < 0) return 0;
    const unsigned char* a = (const unsigned char*)at;
    for (int i = 0; i < n; i++) {
        if (a[i] != expect[i]) {
            if (firstBad) *firstBad = i;
            return 0;
        }
    }
    return 1;
}

unsigned hook_rva(const HookSpec& spec)
{
    switch (host_flavor()) {
    case HOST_FLAVOR_DX9: return spec.rva9;
    case HOST_FLAVOR_DX10: return spec.rva10;
    default: return 0; // 非本插件支持的宿主 ⇒ 一律不挂
    }
}

int hook_sig_ok(const HookSpec& spec)
{
    if (!spec.expect || spec.expect_len < HOOK_SIG_MIN) return 0;
    if (spec.expect10 && spec.expect_len10 < HOOK_SIG_MIN) return 0;
    return 1;
}

void     hook_set_base(unsigned b) { g_baseOverride = b; }
unsigned hook_base() { return g_baseOverride ? g_baseOverride : host_base(); }

int hook_install_one(HookSpec& spec, unsigned base)
{
    // 签名列先于宿主判定校验：口径不对的条目无论装在哪门宿主上都不该走到落钩。
    //   短于 HOOK_SIG_MIN 时 hook_bytes_match 一次循环都不跑、直接返回「全部符合」，
    //   闸门形同虚设，而 MinHook 仍会改掉 HOOK_SIG_MIN 个字节。
    if (!hook_sig_ok(spec)) {
        log_line("[钩] %-14s 跳过：入口签名列不合规（Dx9 %d 字节 / Dx10 %d 字节，下限 %d）", spec.name,
                 spec.expect_len, spec.expect_len10, HOOK_SIG_MIN);
        return 0;
    }
    const unsigned rva = hook_rva(spec);
    if (!rva) {
        log_line("[钩] %-14s 跳过：当前宿主没有适用的 RVA 列", spec.name);
        return 0;
    }
    if (!spec.detour || !spec.orig) {
        log_line("[钩] %-14s 跳过：挂点表条目不完整（detour/orig 为空）", spec.name);
        return 0;
    }

    unsigned char* target = (unsigned char*)(base + rva);

    // 期望字节按宿主口味选列：Dx10 且另给了 expect10 ⇒ 用 expect10；否则共用 expect。
    const unsigned char* exp = spec.expect;
    int                  explen = spec.expect_len;
    if (host_flavor() == HOST_FLAVOR_DX10 && spec.expect10) {
        exp = spec.expect10;
        explen = spec.expect_len10;
    }

    // 目标页必须是本模块映像里已提交的代码页
    if (mem_class(target) != MEMCLS_IMAGE) {
        log_line("[钩] %-14s 跳过：目标 %08X 所在内存不是映像（宿主版本不符？）", spec.name, base + rva);
        return 0;
    }
    if (!mem_readable(target, (size_t)explen)) {
        log_line("[钩] %-14s 跳过：目标 %08X 的 %d 字节读不出来", spec.name, base + rva, explen);
        return 0;
    }

    // 核对在建钩之前：MinHook 不会替你问"这里是不是你以为的代码"，
    //   签名不符就必须停在这里，一行日志，绝不硬装。
    int firstBad = -1;
    if (!hook_bytes_match(target, exp, explen, &firstBad)) {
        char want[64], got[64];
        hook_hexdump(want, (int)sizeof(want), exp, explen);
        hook_hexdump(got, (int)sizeof(got), target, explen);
        log_line("[钩] !! %-14s 签名不符（不硬装）：RVA %08X / VA %08X，第 %d 字节起不符 期望[%s] 实际[%s]",
                 spec.name, rva, base + rva, firstBad, want, got);
        return 0;
    }

    if (!ensure_minhook(spec.name)) return 0;

    // 建钩（此时还没改任何字节）→ 使能。MH_CreateHook 填 *ppOriginal：
    // 那是 MinHook 生成的 trampoline，detour 靠它转发原函数。
    const MH_STATUS sc = MH_CreateHook(target, spec.detour, spec.orig);
    if (sc != MH_OK) {
        log_line("[钩] !! %-14s MH_CreateHook 失败（%s）⇒ 不装", spec.name, MH_StatusToString(sc));
        return 0;
    }
    // Enable 必须查返回值：失败时钩子其实没生效——"报了成功却没生效"比
    //   "明确失败"贵得多。
    const MH_STATUS se = MH_EnableHook(target);
    if (se != MH_OK) {
        log_line("[钩] !! %-14s MH_EnableHook 失败（%s）⇒ 钩子没生效 ⇒ 撤掉并按未装处理", spec.name,
                 MH_StatusToString(se));
        MH_RemoveHook(target);
        return 0;
    }

    // 落钩成功 ⇒ 登记进钩执行迹，并把槽位回填给调用方。
    //   单点分配：登记用 spec.name 这一个名字，detour 拿回填的槽位去 enter/leave。
    spec.traceSlot = hooktrace_ensure(spec.name, hooktrace_default_policy());

    log_line("[钩] %-14s 已装 ✓ RVA %08X / VA %08X 签名[%d 字节逐字节符合] trampoline %p", spec.name, rva,
             base + rva, explen, *spec.orig);
    return 1;
}

} // namespace core
} // namespace ac1
