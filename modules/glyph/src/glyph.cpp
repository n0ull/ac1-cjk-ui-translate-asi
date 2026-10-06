// modules/glyph/src/glyph.cpp —— 字形层公共 API：装钩 / 状态行 / 统计汇总
//
// 事实依据：全部偏移/地址为反汇编实测（Dx9 / Dx10 两套均已实机验证），
//   逐处出注在下面的挂点常量表与 glyph.h 的约定表里。
//
// ===== 打补丁的三条路径 =====
//
//   快路径  0x8840B0 字体加载（后置钩）：赶上了就一步到位。
//           它不可靠：字体对象的加载可能发生在钩子装好之前
//             ⇒ 表现为"钩子已装 ✓ 但 字体=0 指纹=0/8"。
//   兜底①   0x8965D0 charmap 查表（后置钩）：只读。引擎每排一个字都查一次表，
//           所以只要字体加载过且画过字就一定会走到这里 —— 与装钩早晚无关。
//           发现未登记且指纹能匹配的字体 ⇒ 排队。
//   兜底②   0x799720 DrawText（前置钩）：把队列里的字体逐个应用，在 orig 之前。
//
// 铁律：查表钩绝不能打补丁。绘制循环里引擎寄存器缓存着旧表指针，
//   在那里换表会让新下标在旧表上越界读。查表钩只做"发现+排队"，
//   只有 DrawText 的前置钩可以换表。
//
// ===== 怎么选字形集 =====
// 运行时可能有多套字体（textpc / title / bold / techno，exe 侧无法枚举）。
// 新记录的 UV 必须落在那一套字体自己的图集里，否则会画到别人的字形上。
// 所以用该字体表里 '@' 字形的 UV 跨度 (u1-u0, v1-v0) 当指纹逐套精确匹配
// （1e-6 刻度的定点整数比较），再对 fp_glyph_count。一套都不匹配 ⇒ 不写。
//
// 实现切分（机制各自成文件，见 glyph_internal.h 顶部的文件地图）：
//   engine_api.cpp  引擎函数适配   font_track.cpp  生命周期三表 + 指纹
//   apply_patch.cpp 补丁事务       detours.cpp     三条钩 + 发现 + 诊断

#include "ac1/glyph/glyph.h"

#include "ac1/core/hook.h"
#include "ac1/core/host.h" // host_flavor()：Dx9/Dx10 挂点与签名分栏
#include "ac1/core/log.h"
#include "ac1/core/str.h"
#include "ac1/core/ctr.h"
#include "ac1/core/trace.h"

#include "glyphsets.h"
#include "glyph_internal.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>

namespace ac1 {
namespace glyph {

// ---- 三条挂点的入口期望字节（装钩时逐字节核对；不符 ⇒ 跳过，绝不硬装）----

// ① 快路径 0x8840B0：RVA = VA 0x8840B0 − 0x400000
//   入口 = `push -1; push imm32`，两条完整指令、无相对位移
const unsigned char kFontLoadEntry[7] = { 0x6A, 0xFF, 0x68, 0x1B, 0x58, 0x55, 0x01 };
const unsigned      kFontLoadRva9 = 0x4840B0u;
// Dx10（0xBEC350，PixmapFont 槽7，vt @0x1716B2C）：SEH 指针值不同 ⇒ 签名分栏
const unsigned char kFontLoadEntry10[13] = { 0x6A, 0xFF, 0x68, 0x4B, 0xFE, 0x56, 0x01,
                                             0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 };
const unsigned      kFontLoadRva10 = 0x7EC350u;

// ② 兜底·发现 0x8965D0：RVA 0x4965D0
//   入口 = `movzx edx,word ptr [esp+4]; mov eax,edx`，两条完整指令、无相对位移；
//   函数尾 `retn 4` ⇒ 1 个栈参 ⇒ detour 写成 __fastcall(self, edx_unused, C)
const unsigned char kLookupEntry[7] = { 0x0F, 0xB7, 0x54, 0x24, 0x04, 0x8B, 0xC2 };
const unsigned      kLookupRva9 = 0x4965D0u;
// Dx10（0xBF87C0）：入口字节与 Dx9 逐字节相同 ⇒ expect10 留空（共用 kLookupEntry）
const unsigned kLookupRva10 = 0x7F87C0u;

// ③ 兜底·应用 0x799720：RVA 0x399720
//   入口前 8 字节 = `push ebp; mov ebp,esp; and esp,-10h; push -1`
//   （四条完整、位置无关）⇒ expect_len = 8（MinHook 落钩的下限是 5 字节）。
//   期望字节必须与宿主里的真实入口逐字节一致：这张表是 core 唯一拦在
//     "硬装到签名不符的地址"前面的关卡（MinHook 自己不看代码，只看地址可不可执行）。
//   函数尾 `retn 20h`（0x799D8C）⇒ 8 个栈参 ⇒ detour 写 __fastcall(self, edx, a0..a7)
const unsigned char kDrawTextEntry[8] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x6A, 0xFF };
const unsigned      kDrawTextRva9 = 0x399720u;
// Dx10（0x8E81F0）：前 8 字节与 Dx9 逐字节相同（SEH 指针在第 8 字节之后，不进签名）
const unsigned kDrawTextRva10 = 0x4E81F0u;

static volatile LONG g_installed = 0; // 装上的钩子数（0..3）

int install(unsigned base)
{
#if AC1_NO_FALLBACK
    if ((int)g_installed >= 1) return (int)g_installed;
#else
    if ((int)g_installed >= 3) return (int)g_installed;
#endif
    if (!g_apiOk) {
        // 诚实：没有引擎 API 就装不上（装了也调不动引擎的分配器）
        core::log_line("[字形] 引擎 API 未绑定 ⇒ **不装钩**（装了也无法用引擎的分配器建表）");
        return 0;
    }
    unsigned b = base ? base : core::hook_base();
    if (!b) {
        core::log_line("[字形] 拿不到宿主基址 ⇒ **不装钩**");
        return 0;
    }
    set_host_base(b);
    // 钩执行迹的槽位一律由 core 在落钩成功后回填（HookSpec::traceSlot），
    //   本模块不自己登记（自己登记会让同一个钩在册上占两格）。
    //   登记完再按各钩实测量级调采样：core 的默认是「进栈 + 不打明细行」。
    //     查表钩  27904 次/轮 ⇒ 每 5000 次记一行
    //     DrawText 渲染热路径 ⇒ 完全不打明细行（diag 已负责细节，AC1_DRAW_DIAG 开关）
    //     字体加载 一轮只有几次 ⇒ 每次都记
    g_trLookup = g_trDrawText = g_trFontLoad = -1;

    // 三条路径各装各的：某一条装不上不牵连其它两条（逐字节核对 + MinHook
    // 落钩都在 core::hook_install_one 里，任何一步失败都只打日志、绝不硬装）
    core::HookSpec spec;
    memset(&spec, 0, sizeof(spec));
    int n = 0;

#if AC1_NO_FALLBACK
    // 兜底路径已被 -DAC1_NO_FALLBACK=1 关闭（见 glyph.h 顶部的开关说明）。
    //   只装快路径 0x8840B0 —— 代价是失去兜底 + 码位统计 + 指纹样本。
    core::log_line("[字形] ★ AC1_NO_FALLBACK=1 ⇒ **不装** 0x8965D0 查表 / 0x799720 DrawText"
                   "（少 2 个钩）。代价：① 无兜底（字体加载钩没赶上就中文全丢）"
                   "② 无「命中码位/漏字」统计 ③ 无字体指纹样本。");
#else
    // ① 兜底·发现：0x8965D0 charmap 查表（后置）
    spec.name = "0x8965D0 查表·发现";
    spec.rva9 = kLookupRva9;
    spec.rva10 = kLookupRva10; // 0xBF87C0：入口字节两宿主逐字节相同
    spec.expect = kLookupEntry;
    spec.expect_len = (int)sizeof(kLookupEntry);
    spec.detour = (void*)&hook_lookup;
    spec.orig = (void**)&g_origLookup;
    if (core::hook_install_one(spec, b)) {
        g_trLookup = spec.traceSlot;
        // 每 5000 次进入记一行、上限 100 行。实测一轮进入 28 万～59 万次 ⇒ 采样点
        //   远超 100 个，所以**上限先到**：第 50 万次进入之后就不再出明细行。
        //   想要覆盖整轮就把上限调大（内存换日志行数，日志不会被丢）。
        core::hooktrace_set_policy(g_trLookup, 5000, 100);
    }
    n += (g_trLookup >= 0);

    // ② 兜底·应用：0x799720 DrawText（前置）。`kDrawTextEntry` 8 字节 =
    //    `push ebp; mov ebp,esp; and esp,-16; push -1`（四条完整、位置无关的指令，
    //    1+2+3+2=8）⇒ MinHook 量得出指令边界，不需要改 core。
    //    勿写成 6：6 与真实入口不符，签名核对会挡下来（直接跳过，不硬装）。
    memset(&spec, 0, sizeof(spec));
    spec.name = "0x799720 DrawText·应用";
    spec.rva9 = kDrawTextRva9;
    spec.rva10 = kDrawTextRva10; // 0x8E81F0：签名前 8 字节两宿主逐字节相同
    spec.expect = kDrawTextEntry;
    spec.expect_len = (int)sizeof(kDrawTextEntry);
    spec.detour = (void*)&hook_drawtext;
    spec.orig = (void**)&g_origDrawText;
    if (core::hook_install_one(spec, b)) {
        g_trDrawText = spec.traceSlot;
        core::hooktrace_set_policy(g_trDrawText, 0, 0); // 渲染热路径 ⇒ 完全不打明细行
    }
    n += (g_trDrawText >= 0);
#endif // AC1_NO_FALLBACK —— 到此为止只关掉 ①②（兜底两条）。
       //   ③ 快路径（0x8840B0）必须在开关之外：它才是真正写字形表的那条，
       //   关掉它 = 中文字形一个都打不出来。

    // ③ 快路径：0x8840B0 字体加载（后置）——字形表补丁的唯一入口，任何配置下都必须装。
    memset(&spec, 0, sizeof(spec));
    spec.name = "0x8840B0 字体加载";
    spec.rva9 = kFontLoadRva9;
    spec.rva10 = kFontLoadRva10;      // 0xBEC350（PixmapFont 槽7）
    spec.expect10 = kFontLoadEntry10; // SEH 指针值不同 ⇒ 签名分栏
    spec.expect_len10 = (int)sizeof(kFontLoadEntry10);
    spec.expect = kFontLoadEntry;
    spec.expect_len = (int)sizeof(kFontLoadEntry);
    spec.detour = (void*)&hook_font_load;
    spec.orig = (void**)&g_origFontLoad;
    if (core::hook_install_one(spec, b)) {
        g_trFontLoad = spec.traceSlot;
        core::hooktrace_set_policy(g_trFontLoad, 1, 200); // 一轮几次 ⇒ 每次都记
    }
    n += (g_trFontLoad >= 0);

    g_installed = n;
    core::log_line("[字形] 配置：清单=<脚本目录>AC1_CJK_Glyphs.bin（缺文件=0 套=不写）"
                   " 指纹='@' 的 UV 跨度 装钩=%d/3（查表·发现 + DrawText·应用 + 字体加载）"
                   " 已加载套=%d 幂等表=%d/%d 队列=%d/%d",
                   n, (int)g_nsets, reg_count(), (int)GLYPH_MAX_FONTS, pend_count(), (int)GLYPH_PENDING_MAX);
    return n;
}

const GlyphStats& stats()
{
    // 明写逐字段拷贝（不用指针算术跨字段映射：那种写法在改动结构体时最容易悄悄错位）
    static GlyphStats s;
    s.hooks = (int)g_installed;
    s.fontsPatched = (int)core::ctr_get(&g_ctr, C_FONTS);
    s.glyphsAdded = (int)core::ctr_get(&g_ctr, C_GLYPHS);
    s.pagesCreated = (int)core::ctr_get(&g_ctr, C_PAGES);
    s.sets = (int)g_nsets;
    s.fingerprints = (int)g_fpSamples;
    s.pending = pend_count();
    s.revived = (int)core::ctr_get(&g_ctr, C_REVIVED);
    s.appliedAtDraw = (int)core::ctr_get(&g_ctr, C_APPLIED_DRAW);
    s.dup = (int)core::ctr_get(&g_ctr, C_DUP);
    s.noMatch = (int)core::ctr_get(&g_ctr, C_NOMATCH);
    s.rejectNull = (int)core::ctr_get(&g_ctr, C_REJ_NULL);
    s.rejectNoApi = (int)core::ctr_get(&g_ctr, C_REJ_NOAPI);
    s.rejectVtable = (int)core::ctr_get(&g_ctr, C_REJ_VTABLE);
    s.rejectCount0 = (int)core::ctr_get(&g_ctr, C_REJ_COUNT0);
    s.rejectNoTable = (int)core::ctr_get(&g_ctr, C_REJ_NOTABLE);
    s.rejectTooBig = (int)core::ctr_get(&g_ctr, C_REJ_TOOBIG);
    s.rejectFull = (int)core::ctr_get(&g_ctr, C_REJ_FULL);
    s.failAlloc = (int)core::ctr_get(&g_ctr, C_FAILALLOC);
    s.faults = (int)core::ctr_get(&g_ctr, C_FAULT);
    s.failListed = (int)g_nfail;
    // 失败名单的原因码分布（g_failCode → failByCode）。g_nfail 只在
    //   n < GLYPH_MAX_FONTS 时才自增 ⇒ 它天然封顶在 32，与 g_failCode 同长；
    //   这里仍取一次 min 兜底，万一将来有人改了那个自增条件也不会越界读。
    for (int i = 0; i < GLYPH_FAIL_SLOTS; i++) s.failByCode[i] = 0; // stats() 复用同一静态对象 ⇒ 必须先清
    int nfail = (int)g_nfail;
    if (nfail > GLYPH_MAX_FONTS) nfail = GLYPH_MAX_FONTS;
    for (int i = 0; i < nfail; i++) {
        int c = g_failCode[i];
        if (c < 0 || c >= GLYPH_FAIL_SLOTS) c = 0; // 越界码归"未知"桶，与 switch 的 default 同口径
        s.failByCode[c]++;
    }
    s.cpHitKinds = cp_index_hit_kinds();
    s.cpHitTotal = cp_index_hit_total();
    s.cpMissKinds = cp_index_miss_kinds();
    s.legacyBytes = (unsigned long long)g_legacyBytes;
    return s;
}

const char* status(char* out, int cap)
{
    if (!out || cap <= 0) return "";
    const GlyphStats& s = stats();
    // 装配层给 512 字节缓冲：这行必须装得下（实测典型值 ~240 字节 UTF-8）。
    // 为了腾出位置给码位统计，把拒绝原因压成 3 类 —— 逐项明细在日志里查，
    // 状态行只回答"有没有被拒、被谁拒"。
    // 用 core::Str 累加（拿 _snprintf 返回值当写偏移是越界来源，见 core/str.h）。
    //   分三段追加；行首「闸门=」段是 forge 核验结果。
    core::Str w;
    core::str_init(&w, out, cap);
    core::str_addf(&w,
                   "GLYPH 闸门=%s 钩子=%d/3 字体=%d 字形+%d 页+%d 集=%d 指纹=%d/%d 排队=%d 重置=%d "
                   "Draw应用=%d 命中码位=%d种/%ld次 漏字=%d种",
                   forge_patch_present() ? "开" : "**关**", s.hooks, s.fontsPatched, s.glyphsAdded,
                   s.pagesCreated, s.sets, s.fingerprints, (int)GLYPH_FP_SAMPLES, s.pending, s.revived,
                   s.appliedAtDraw, s.cpHitKinds, s.cpHitTotal, s.cpMissKinds);
    core::str_addf(&w, " | 拒绝=%d(未绑 %d/vt %d/其它 %d) 无匹配=%d 重复=%d 分配失败=%d 异常=%d",
                   s.rejectNull + s.rejectNoApi + s.rejectVtable + s.rejectCount0 + s.rejectNoTable +
                       s.rejectTooBig + s.rejectFull,
                   s.rejectNoApi, s.rejectVtable,
                   s.rejectCount0 + s.rejectNoTable + s.rejectTooBig + s.rejectFull, s.noMatch, s.dup,
                   s.failAlloc, s.faults);
    core::str_addf(&w, " 拉黑=%d 让渡=%lluB", s.failListed, s.legacyBytes);
    out[cap - 1] = 0;
    return out;
}

void report_tops() { cp_index_report_tops(); }

} // namespace glyph
} // namespace ac1
