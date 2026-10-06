// modules/glyph/src/detours.cpp —— 三条钩的 detour + 发现 + DrawText 串诊断
//
// 铁律：查表钩绝不能打补丁。绘制循环里引擎寄存器缓存着旧表指针；
//   在那里换表 ⇒ 新的字形下标在旧表上越界读。查表钩只做"发现 + 排队"；
//   只有 DrawText 的前置钩可以换表。
//
// AC1_DRAW_DIAG（默认 0）：DrawText 串坐标诊断（「串 → 字体套 → 屏幕坐标」）。
//   1 = 打开。只读观测、有 400 条额度与去重环；平时关掉，抓布局问题时 -D 打开。

#include "ac1/glyph/glyph.h"

#include "ac1/core/log.h"
#include "ac1/core/trace.h"

#include "glyphsets.h"
#include "glyph_internal.h"

#include <windows.h>
#include <intrin.h>
#include <string.h>
#include <stdio.h>

#if !defined(AC1_DRAW_DIAG)
#define AC1_DRAW_DIAG 0
#endif

namespace ac1 {
namespace glyph {

#if AC1_DRAW_DIAG
static void diag_drawtext(const void* font, const unsigned short* s, unsigned x, unsigned y, unsigned n);
#endif

FontLoadFn   g_origFontLoad = NULL;
LookupOrig   g_origLookup = NULL;
DrawTextOrig g_origDrawText = NULL;

// 三个钩在 core 钩执行迹（ac1/core/trace.h）里的槽位。
//   策略按量级分档，见 glyph.cpp install() 的各分支（core 落钩成功后回填）。
int g_trFontLoad = -1, g_trLookup = -1, g_trDrawText = -1;

// noinline：detour 必须有真实的栈帧（_ReturnAddress 依赖真实的调用栈形状）。
// 命名空间作用域（不进匿名 namespace）：glyph.cpp 的 install() 要取它的地址填 HookSpec。
__declspec(noinline) bool __fastcall hook_font_load(void* self, void* edx_unused, unsigned a0, unsigned a1,
                                                    unsigned a2)
{
    core::hooktrace_enter(g_trFontLoad, (unsigned)(ULONG_PTR)self, 0);
    // 先原样转发：语义完全不变
    bool ok = g_origFontLoad(self, edx_unused, a0, a1, a2);
    // 只有加载成功才补丁 —— 失败时字体对象是半成品，动它没有意义
    if (ok) font_loaded_post(self);
    core::hooktrace_leave(g_trFontLoad);
    return ok;
}

// ======================================================================
// 兜底路径①：0x8965D0（charmap 查表）后置钩 —— 发现 + 排队，只读
//
// 为什么这里能"发现"：引擎每排一个字都要查一次表，所以只要字体已经被加载过、
// 且界面真的画过字，就一定会走到这里 —— 与"钩子装得早还是晚"无关。
// ======================================================================
static void discover_font(const void* font)
{
    if (!g_apiOk) return;               // 没有引擎 API 就没什么可排的
    if (!forge_patch_present()) return; // forge 闸门：补丁停用时不排队（apply_patch 另有兜底闸）
    // 拉黑过的字体立刻返回：查表钩每排一个字都调进来，不早退就是每帧重试+刷日志
    if (fail_listed(font)) return;
    // 形状不合格的（不是字体 / 还没加载完）在热路径上静默丢弃，不刷日志
    unsigned       count = 0, vt = 0;
    unsigned char* table = NULL;
    if (font_shape(font, &count, &table, &vt) != SHAPE_OK) return;

    int ri = registry_find(font);
    if (ri >= 0) {
        if (table == g_ourTable[ri]) return; // 我们的表还在 ⇒ 无事
        // 引擎把这个对象重置/重建了：我们追加的记录
        //   随旧块一起没了，charmap 里那些槽也指着不再存在的下标 ⇒ 必须重新补
        inc(C_REVIVED);
        if (queue_pending(font, g_setUsed[ri]))
            core::log_line("[字形] 字体 %p 的表被重置（现=%p 我们=%p）⇒ 重新排队（已排 %d）", font, table,
                           g_ourTable[ri], pend_count());
        return;
    }

    // 未登记 ⇒ 打一行指纹样本（和加载钩共用同一套采样位），再试着匹配
    sample_fingerprint(font);
    int si = match_set(font);
    if (si < 0) return; // 匹配不上就在这儿算了，不排队
    // 同一字体已在队列里时 queue_pending 返回 0 ⇒ 静默。
    //   否则每排一个字都会重打一遍这行，真机日志会刷成灾。
    if (queue_pending(font, si))
        core::log_line("[字形] 字体 %p 由查表发现（加载钩没赶上）⇒ 已排队，将在下一次 DrawText 前应用"
                       "（已排 %d，套#%d）",
                       font, pend_count(), si);
}

__declspec(noinline) unsigned short __fastcall hook_lookup(void* charmap, void* edx_unused, unsigned short C)
{
    // 先调 orig 拿原值 —— 引擎的返回值一个字都不干预
    // 上下文：a0 = charmap 对象（font = charmap - 0x3C），a1 = 被查的码位。
    //   崩溃时这两行就能定位到「是哪张字表的哪个字把引擎带崩的」。
    core::hooktrace_enter(g_trLookup, (unsigned)(ULONG_PTR)charmap, (unsigned)C);
    unsigned short r = g_origLookup(charmap, edx_unused, C);
    // 再做发现（只读 + 排队）
    discover_font((const unsigned char*)charmap - GLYPH_OFF_CHARMAP);
    // 最后记一次"这个码位被游戏查了"。必须排在发现之后：发现比它贵，
    //   而 C < 0x3000 时这里第一道闸就返回了，热路径成本 ≈ 一次比较。
    //   _ReturnAddress() 在这里是引擎里那个 call 点的返回地址（call 压栈、
    //   我们的 JMP 不动栈）⇒ 正好能反查"被谁查的"。
    cp_index_observe(C, (unsigned)(uintptr_t)_ReturnAddress());
    core::hooktrace_leave(g_trLookup);
    return r;
}

// ======================================================================
// 兜底路径②：0x799720（DrawText）前置钩 —— 真正换表的地方
//
// 必须在 orig 之前应用：换表的整个理由就是"赶在引擎读 font+0x38 之前"。
//   放回 orig 之后就失去了意义。
// ======================================================================
__declspec(noinline) bool __fastcall hook_drawtext(void* self, void* edx_unused, unsigned a0, unsigned a1,
                                                   unsigned a2, unsigned a3, unsigned a4, unsigned a5,
                                                   unsigned a6, unsigned a7)
{
    // 上下文：a0 = font 对象，a1 = 正在画的串指针。
    //   这是渲染热路径（一轮几万次），所以它的策略是 track=1 但不打明细行 ——
    //   进栈只为崩溃定位，打行会毁掉日志判读。
    core::hooktrace_enter(g_trDrawText, (unsigned)(ULONG_PTR)self, a0);
    // 先应用队列（换表），再转发 —— 顺序反过来这条路就没有意义了
    apply_pending();
#if AC1_DRAW_DIAG
    diag_drawtext(self, (const unsigned short*)(uintptr_t)a0, a1, a2, a3);
#endif
    bool r = g_origDrawText(self, edx_unused, a0, a1, a2, a3, a4, a5, a6, a7);
    core::hooktrace_leave(g_trDrawText);
    return r;
}

#if AC1_DRAW_DIAG
// ======================================================================
// 诊断：DrawText 的「串 → 字体套 → 绘制坐标」（AC1_DRAW_DIAG=1 时编译进热路径）
// ======================================================================
// 参数对应由 IDA 对 sub_799720 逐指令核定（注意 IDA 的 arg_4 其实是 [ebp+0x0C]）：
//   self(ecx) = 字体对象
//   a0         = const wchar_t*  ← 正在画的串
//   a1         = int → float   = x
//   a2         = int → float   = 基线 y
//   a3         = 码元个数
//   a6         = 与 0 比较的标志
// 两次绘制若 y 相同而文本不同 ⇒ 是布局把它们放到了同一处。
// 只在串里含 ≥0x3000 的码元时打（纯英文不关心）。
// 必须去重：加载界面那句「< 按任意鍵繼續 >」每帧在同一处画一次，
//   不去重的话 400 条额度在进游戏前就烧光，真正要看的对话框反而一行没有。
//   去重的键含文本哈希 —— 「两个不同的串画在同一个 y」正是要抓的线索，
//   只按 (font,x,y) 去重会把它一起吃掉。
#define DRAW_DIAG_MAX 400
#define DRAW_DIAG_RING 64
static volatile LONG g_diagN;

struct DiagSeen {
    const void* font;
    unsigned    x, y, h;
};
static DiagSeen      g_diagRing[DRAW_DIAG_RING];
static volatile LONG g_diagRingPos;

static bool diag_seen_before(const void* font, unsigned x, unsigned y, unsigned h)
{
    for (int i = 0; i < DRAW_DIAG_RING; i++)
        if (g_diagRing[i].font == font && g_diagRing[i].x == x && g_diagRing[i].y == y &&
            g_diagRing[i].h == h)
            return true;
    LONG      p = (LONG)InterlockedIncrement(&g_diagRingPos);
    DiagSeen& e = g_diagRing[(size_t)(p % DRAW_DIAG_RING)];
    e.font = font;
    e.x = x;
    e.y = y;
    e.h = h;
    return false;
}

static void diag_drawtext(const void* font, const unsigned short* s, unsigned x, unsigned y, unsigned n)
{
    (void)n;
    if (!s) return;
    // font 与 s 都是引擎的指针，读它们必须过读门 —— 单测驱动本函数时传的是假地址。
    //   a3 不是码元个数（实机读到 163052752 之类的大值）⇒ 一律按 NUL 截断。
    __try {
        unsigned short buf[16];
        unsigned       k = 0, h = 216613626u;
        bool           cjk = false;
        for (; k < 16; k++) {
            unsigned short c = s[k];
            if (!c) break;                      // NUL 终止
            if (c < 0x20 || c == 0xFFFF) break; // 明显不是正文
            buf[k] = c;
            if (c >= 0x3000) cjk = true;
            h = (h ^ c) * 16777619u;
        }
        if (!cjk) return;                            // 纯英文不关心
        if (diag_seen_before(font, x, y, h)) return; // 同一处重复绘制不重复打
        LONG d = (LONG)InterlockedIncrement(&g_diagN);
        if (d > DRAW_DIAG_MAX) return;
        // 终止性/不截断论证（改 buf 容量或删下面的守卫前必须重做）：k ≤ 16（buf 的容量）
        //   ⇒ o ≤ 96，每轮 _snprintf 的 room ≥ 64 ⇒ 永不截断、返回值恒为 6、每轮都写 NUL
        //   ⇒ 下面的『%s』读的是合法 C 串。cjk 闸门保证 k ≥ 1，所以 txt 也不会是未初始化串。
        char     txt[160];
        unsigned o = 0;
        for (unsigned i = 0; i < k && o + 12 < (unsigned)sizeof(txt); i++)
            o += (unsigned)_snprintf(txt + o, sizeof(txt) - o, "\\u%04X", (unsigned)buf[i]);
        core::log_line("[字形][Draw] font=%p 套#%d x=%d **y=%d** n=%u 『%s』", font, font_set_of((void*)font),
                       (int)x, (int)y, k, txt);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // 诊断读失败一律静默：它只是观测，绝不能影响渲染
    }
}
#endif // AC1_DRAW_DIAG

void set_hook_origins(LookupOrig lookup, DrawTextOrig drawtext)
{
    g_origLookup = lookup;
    g_origDrawText = drawtext;
}

} // namespace glyph
} // namespace ac1
