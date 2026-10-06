// modules/glyph/src/engine_api.cpp —— 引擎函数适配：地址表 + shim + 绑定 + vtable 闸门
//
// 本模块唯一不能依赖的东西就是"游戏在不在"。真机上这些是引擎函数（地址表按
// 宿主口味分栏），单测里是假实现；两边共用同一个补丁实现（EngineApi 注入）。
//
// 调用约定直接写在类型里：约定错 ⇒ 单测的假实现都编不过。
//   各条依据（Dx9；Dx10 的等价物在同一张地址表里）：
//   alloc        __cdecl (size, flags)   调用点 push 0 / push size / call + add esp,8
//   free         __cdecl (p)             调用点 + add esp,4
//   setEntry     __thiscall(ecx=charmap) + 3 栈参(cp,glyphIdx,flag)，retn 0Ch
//                ⇒ 等价的 __fastcall 形状是 (cm, edx_unused, cp, glyphIdx, flag)
//   record_ctor  __thiscall(ecx=rec)，尾 retn（无栈参）⇒ ≡ __fastcall(rec, edx_unused)
//   array_ctor   __stdcall(base, elem, count, ctor)，尾 retn 10h（被调方清栈），
//                且内部 mov ecx,esi; call ctor ⇒ 元素构造器的第 1 参走 ecx
//
// 约定正确性的真实依据是反汇编证据，见 glyph.h 的约定表（逐条给出
//   add esp,8 / retn 0Ch / retn / retn 10h + mov ecx,esi; call ctor）。
//   编译器无法在 x86 上校验调用约定，唯一的防线是那张表 + 真机验证。

#include "ac1/glyph/glyph.h"

#include "ac1/core/hook.h"
#include "ac1/core/host.h" // host_flavor()：Dx9/Dx10 地址表分栏
#include "ac1/core/log.h"

#include "glyph_internal.h"

#include <windows.h>
#include <stdio.h>

namespace ac1 {
namespace glyph {

// ======================================================================
// 引擎 API 的绑定（地址表集中在这一个地方）
//
// 真址 = base + RVA，RVA = VA − imagebase(0x400000)。
// ======================================================================
typedef void*(__cdecl* RawAlloc)(unsigned size, unsigned flags);
typedef void(__thiscall* RawSetEntry)(void* charmap, unsigned cp, unsigned idx, unsigned flag);
typedef void(__thiscall* RawRecCtor)(void* rec);
typedef void(__stdcall* RawRecArray)(void* base, unsigned elem, unsigned count, GlyphRecCtorFn ctor);

static RawAlloc    g_rawAlloc = NULL;
static RawSetEntry g_rawSetEntry = NULL;
static RawRecCtor  g_rawRecCtor = NULL;
static RawRecArray g_rawRecArray = NULL;

// 按宿主口味分栏（两套地址都经 capstone 逐调用点复核过形状）：
//   Dx10 的 alloc/setCharmapEntry 与 Dx9 同形；record_ctor 把 0x20
//   记录逐字段清零（与 Dx9 同语义）；record_array_ctor 走引擎自己调的跳板。
static const struct {
    unsigned alloc, set_entry, rec_ctor, rec_arr_ctor;
} kEngineRva9 = {
    0x787980u - 0x400000u, // magma 分配
    0x896720u - 0x400000u, // setCharmapEntry（不是 0x896600）
    0x8842B0u - 0x400000u, // 字形记录 ctor
    0x40104Bu - 0x400000u  // 记录数组构造助手（跳板 → 0x4020A0）
};
static const struct {
    unsigned alloc, set_entry, rec_ctor, rec_arr_ctor;
} kEngineRva10 = {
    0x8D5370u - 0x400000u, // magma 分配（push 0; push size; call + add esp,8 同形）
    0xBF8910u - 0x400000u, // setCharmapEntry（thiscall+3 栈参+retn 0Ch 同形）
    0xBEC550u - 0x400000u, // 字形记录 ctor（0x20 记录逐字段清零同语义）
    0x4010AFu - 0x400000u  // 记录数组构造助手（跳板 → 0x402090）
};

// ---- 真正需要适配的只剩一个 shim ----
//   alloc：引擎是两参 (size, flags)，EngineApi 是单参 size。
//   setCharmapEntry：引擎是 __thiscall(ecx + 3 栈参)，EngineApi 要等价的
//     __fastcall(ecx + edx + 3 栈参) —— __thiscall 的第 2 参在栈上、
//     __fastcall 在 edx 上，不是同一种调用序列，必须过 shim。
//   record_ctor（__thiscall(ecx)，0 栈参，0 清栈 ≡ __fastcall(ecx,edx)）与
//   record_array_ctor（__stdcall，尾 retn 10h）的形状与 EngineApi 字段
//   完全一致 ⇒ 直接赋值。
static void* __cdecl   shim_alloc(unsigned size) { return g_rawAlloc(size, 0u); }
static void __fastcall shim_set_entry(void* charmap, void* edx_unused, unsigned cp, unsigned glyphIdx,
                                      unsigned flag)
{ g_rawSetEntry(charmap, cp, glyphIdx, flag); }

EngineApi g_api = { NULL, NULL, NULL, NULL };
bool      g_apiOk = false;

// 字体类对象里第一个 dword 存的 vtable 值（VA），按宿主口味分栏：
//   Dx9:  0x16A2A20 = PixmapFontScimitar / 0x16AB870 = PixmapFont
//         （ctor `C7 06 ..` 实锤，两套类共用 load 函数 slot7=0x8840B0）
//   Dx10: 0x16D4998 = 实机四套字体对象都带它的那个值（两个 ctor `C7 06` 实锤）；
//         0x1716B2C = PixmapFont（离线 RTTI 链解析值，兜底）。Dx10 没有
//         PixmapFontScimitar ⇒ 两个槽位是两个不同的值，都接受。
// 别把"槽的地址"当成 vtable 值写进来（那是 vtable 基址 + 0x1C）。
// 闸门用来挡住"根本不是字体对象"的那次调用。
static unsigned g_vtable[2] = { 0, 0 };

bool vtable_ok(unsigned vt)
{
    for (int i = 0; i < 2; i++)
        if (g_vtable[i] && vt == g_vtable[i]) return true;
    return false;
}

// 告知宿主基址：用来换算字体类 vtable 值（vtable 闸门用；Dx9 两套类 / Dx10 一套）。
// install(base) 与 bind_engine_at(base) 都会自动调它；单测拿它喂一个假基址。
void set_host_base(unsigned base)
{
    if (!base) return;
    if (core::host_flavor() == core::HOST_FLAVOR_DX10) {
        g_vtable[0] = base + (0x16D4998u - 0x400000u); // 实机：四套字体对象都带它
                                                       //   （两个 ctor `C7 06` 实锤 @0x8E5595/0x8E572F）
        g_vtable[1] = base + (0x1716B2Cu - 0x400000u); // PixmapFont（RTTI 解析值，兜底）
    }
    else {
        g_vtable[0] = base + (0x16A2A20u - 0x400000u); // PixmapFontScimitar
        g_vtable[1] = base + (0x16AB870u - 0x400000u); // PixmapFont
    }
}

// 注入引擎 API。传 NULL = 解绑（之后 font_loaded_post 只计数不写）。
void bind_engine(const EngineApi* api)
{
    if (!api) {
        g_apiOk = false;
        return;
    }
    g_api = *api;
    g_apiOk = (g_api.alloc && g_api.set_charmap_entry && g_api.record_ctor && g_api.record_array_ctor);
    if (!g_apiOk)
        core::log_line("[字形] !! bind_engine：EngineApi 缺字段（alloc / set_charmap_entry / "
                       "record_ctor / record_array_ctor 为必填）⇒ 不会装钩");
}

// 按宿主基址填一份 EngineApi 并注入（地址表集中在上面两张表里）。base 传 0 =
// 用 core::hook_base()。返回 1 = 已绑定，0 = 基址为 0。
int bind_engine_at(unsigned base)
{
    unsigned b = base ? base : core::hook_base();
    if (!b) {
        core::log_line("[字形] !! 拿不到宿主基址 ⇒ 引擎 API 未绑定");
        g_apiOk = false;
        return 0;
    }
    set_host_base(b);
    // 引擎地址表按宿主口味选（Dx9/Dx10 两张都已实测）
    const bool dx10 = (core::host_flavor() == core::HOST_FLAVOR_DX10);
    g_rawAlloc = (RawAlloc)(b + (dx10 ? kEngineRva10.alloc : kEngineRva9.alloc));
    g_rawSetEntry = (RawSetEntry)(b + (dx10 ? kEngineRva10.set_entry : kEngineRva9.set_entry));
    g_rawRecCtor = (RawRecCtor)(b + (dx10 ? kEngineRva10.rec_ctor : kEngineRva9.rec_ctor));
    g_rawRecArray = (RawRecArray)(b + (dx10 ? kEngineRva10.rec_arr_ctor : kEngineRva9.rec_arr_ctor));

    EngineApi api;
    api.alloc = &shim_alloc;
    api.set_charmap_entry = &shim_set_entry; // __thiscall → __fastcall
    // 这两个与 EngineApi 字段形状一致（__thiscall(ecx) ≡ __fastcall(ecx,edx)；
    // 数组构造器同为 __stdcall），所以直接赋值。
    // 调用约定在 x86 上无法由编译器校验：这里只能靠 glyph.h 约定表里的
    //   反汇编证据。真要改约定，先改那张表、再真机验，别指望编译期拦住。
    api.record_ctor = (GlyphRecCtorFn)g_rawRecCtor;
    api.record_array_ctor = (void(__stdcall*)(void*, unsigned, unsigned, GlyphRecCtorFn))g_rawRecArray;
    bind_engine(&api);
    return g_apiOk ? 1 : 0;
}

bool engine_bound() { return g_apiOk; }

} // namespace glyph
} // namespace ac1
