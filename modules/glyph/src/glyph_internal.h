// modules/glyph/src/glyph_internal.h —— 同模块内部的接缝（不进 include/，不跨模块）
//
// glyph.cpp 物理切分后的跨文件契约：五个 .cpp 共享的 statics 在这里 extern，
// 模式与 glyphsets.h 相同。只许 glyph 模块自己的 .cpp 引它。
//
// 切分边界（机制各自成文件，见各 .cpp 文件头）：
//   glyph.cpp       公共 API：install/stats/status/report_tops + 挂点常量表
//   engine_api.cpp  引擎函数适配（地址表/shim/绑定/vtable 闸门）
//   font_track.cpp  字体生命周期：计数 + 登记表 + 待办队列 + 失败名单 + 指纹
//   apply_patch.cpp 补丁事务（形状闸门 → 建新表 → UV 缩放 → charmap → 回滚）
//   detours.cpp     三条钩的 detour + 发现 + DrawText 串诊断（AC1_DRAW_DIAG 开关）

#pragma once

#include "ac1/glyph/glyph.h"

#include "ac1/core/ctr.h"

namespace ac1 {
namespace glyph {

// ---- engine_api.cpp ----
extern EngineApi g_api;
extern bool      g_apiOk;
bool             vtable_ok(unsigned vt);

// ---- font_track.cpp：计数 + 生命周期三表 + 指纹 ----
// 槽位编号与顺序不许改 —— stats() 的逐字段拷贝、状态行、单测断言全都按这个顺序读。
enum {
    C_FONTS = 0,
    C_GLYPHS,
    C_PAGES,
    C_DUP,
    C_NOMATCH,
    C_REVIVED,
    C_APPLIED_DRAW,
    C_REJ_NULL,
    C_REJ_NOAPI,
    C_REJ_VTABLE,
    C_REJ_COUNT0,
    C_REJ_NOTABLE,
    C_REJ_TOOBIG,
    C_REJ_FULL,
    C_FAILALLOC,
    C_FAULT,
    COUNTER_SLOTS
};
extern core::Counters g_ctr;
void                  inc(int slot);

extern void* g_fonts[GLYPH_MAX_FONTS];
extern void* g_ourTable[GLYPH_MAX_FONTS];
extern int   g_setUsed[GLYPH_MAX_FONTS];
int          reg_count();
int          registry_find(const void* font);
int          registry_add(const void* font, const void* ourTable, int setIdx);
void         registry_set_table(const void* font, const void* ourTable, int setIdx);

int  pend_count();
void pend_set_count(int n);
int  queue_pending(const void* font, int setIdx);
void apply_pending();

extern const void*   g_failList[GLYPH_MAX_FONTS];
extern int           g_failCode[GLYPH_MAX_FONTS];
extern volatile LONG g_nfail;
bool                 fail_listed(const void* font);
void                 fail_list_add(const void* font, int code);

extern volatile LONG g_fpSamples;
void                 sample_fingerprint(const void* font);

// 选字形集（'@' UV 跨度指纹）；补丁与发现两条路共用。
int match_set(const void* font);

// ---- detours.cpp：trampoline、detour 与执行迹槽位（install 取地址、core 回填槽位）----
typedef bool(__fastcall* FontLoadFn)(void* self, void* edx_unused, unsigned a0, unsigned a1, unsigned a2);
extern FontLoadFn         g_origFontLoad;
extern LookupOrig         g_origLookup;
extern DrawTextOrig       g_origDrawText;
extern int                g_trFontLoad, g_trLookup, g_trDrawText;
bool __fastcall           hook_font_load(void* self, void* edx_unused, unsigned a0, unsigned a1, unsigned a2);
unsigned short __fastcall hook_lookup(void* charmap, void* edx_unused, unsigned short C);
bool __fastcall           hook_drawtext(void* self, void* edx_unused, unsigned a0, unsigned a1, unsigned a2,
                                        unsigned a3, unsigned a4, unsigned a5, unsigned a6, unsigned a7);

// ---- apply_patch.cpp：补丁事务 + 旧表让渡记账 ----
enum {
    SHAPE_OK = 0,
    SHAPE_NULL,
    SHAPE_VTABLE,
    SHAPE_COUNT0,
    SHAPE_NOTABLE,
    SHAPE_TOOBIG,
    SHAPE_FAULT
};
int  font_shape(const void* font, unsigned* outCount, unsigned char** outTable, unsigned* outVt);
void report_shape_reject(int sh, const void* font, unsigned vt, unsigned count);
int  apply_patch(const void* font, int setIdx, const char* how);
extern __declspec(align(8)) volatile long long g_legacyBytes;

} // namespace glyph
} // namespace ac1
