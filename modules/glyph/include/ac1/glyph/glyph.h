#pragma once

// modules/glyph —— 字形层
//
// 文本层能把界面上的串换成中文，但字库里没有对应字形 ⇒ 结果是方块/空白。
// glyph 层补上这一段：charmap 补页（码点 → 字形下标）+ 字形记录追加
// （新码点的度量 + UV 挂进 font+0x38 的表）+ 字形集数据（读 AC1_CJK_Glyphs.bin）。
//
// 本模块不碰 D3D/纹理：我们的图集在安装期打进 DataPC.forge，引擎自己加载。
// 唯一前提：补丁只在「我们的图集确实在 forge 里」时才打（否则 UV 缩放会把原版
// 文字搞花）——由启动期文件核验给出（check_forge_patch / forgecheck.cpp）。
//
// ===== 打补丁的时机：两条路径，缺一不可 =====
//
// 【快路径】钩 0x8840B0（字体对象 vtable 槽 7 = "加载 .mft"）的后置钩子：
// 返回后字体对象完全就绪、且不在任何绘制中，这时改 +0x34/+0x38/+0x3C 不会撞上
// 引擎正在遍历表的循环。入口 7 字节 `6A FF 68 1B 58 55 01`（push -1; push imm32），
// 签名 `bool __thiscall load(self, a0, a1, a2)`（3 栈参、ret 0Ch）；
// detour 写成 __fastcall(self, edx_unused, a0, a1, a2)，先原样转发，
// 只有返回值非 0 才打补丁。
//   但快路径不可靠：字体加载可能发生在钩子装好之前（亚秒级竞争），不能只依赖它。
//
// 【兜底路径】与时机无关的两步：发现 → 排队 → 应用
//   ① 0x8965D0（charmap 查表）后置钩：引擎每排一个字都查表，所以它一定会替我们
//      "发现"已存在的字体。先调 orig 拿原值（一个字不干预），再做发现、排队。
//      只读，一个字节都不写。
//   ② 0x799720（DrawText）前置钩：把队列里的字体逐个应用补丁，必须在 orig 之前——
//      换表必须发生在引擎任何一次读 font+0x38 之前。
//
// 铁律：查表钩子绝不能打补丁。绘制循环里引擎寄存器缓存着旧表指针；
//   在那里换表 ⇒ 新的字形下标在旧表上越界读。查表钩只做"发现 + 排队"；
//   只有 DrawText 的前置钩可以换表。
//
// ===== 编译开关 =====
// AC1_NO_FALLBACK（默认 0）：为 1 时不装兜底两条（0x8965D0 / 0x799720），
//   只装快路径 0x8840B0。代价：① 失去兜底（字体加载钩没赶上就中文全丢）；
//   ② 失去码位命中/漏字统计；③ 失去字体指纹样本。
//
#ifndef AC1_NO_FALLBACK
#define AC1_NO_FALLBACK 0
#endif

// ===== 两条硬约束（否则引擎析构会把野指针交给 magma 堆）======
//   · 引擎用引擎的 free 释放 font+0x38 指向的表 ⇒ 我们自建的表必须用引擎的
//     alloc 分配，绝不能 CRT malloc / 不能 new。
//   · charmap 页同理：页块由引擎析构逐页回收 ⇒ 必须让引擎自己建页：逐码位调
//     set_charmap_entry(charmap, cp, glyphIdx, flag=0)。
//   引擎函数全部经 EngineApi 注入（bind_engine / bind_engine_at），
//   这样本模块在没有游戏宿主的单测里也能跑（假 API）。
//
// 依赖：core（hook / log / host）。不依赖 text / dict / app。

namespace ac1 {
namespace glyph {

// ======================================================================
// 引擎对象布局（magma 字体对象，对象大小 0x45C）
//   公开出来是给单测用的：单测要按同一套偏移造合成字体。
//   Dx9 / Dx10 布局一致（实测）。下面的 enum 只列"代码会写"的字段。
//   另有两个只读字段（不写、碰了没好处）：
//     +0x43C  u16  缺字符默认字形（PC 上恒 0）
//     +0x454  u8   页数（PC 渲染不用）
// ======================================================================
enum {
    GLYPH_FONT_SIZE = 0x45C,  // 字体对象大小
    GLYPH_OFF_COUNT = 0x34,   // u16  字形数（u16，不是 dword）
    GLYPH_OFF_TABLE = 0x38,   // 记录表基址（一整块 count×0x20）
    GLYPH_OFF_CHARMAP = 0x3C, // 内联 256 项 dword 页指针数组，不是指针字段
    GLYPH_REC_SIZE = 0x20,    // 字形记录步长
    GLYPH_MAX_FONTS = 32,     // 幂等登记表容量（字体是长生命周期对象，够用）
    GLYPH_MAX_COUNT = 4096,   // 防御上限：字形数超过它就不碰
    GLYPH_FP_SAMPLES = 8,     // 指纹样本行上限：只对前这么多个字体打
    GLYPH_PENDING_MAX = 32,   // 待办队列容量
    GLYPH_PENDING_TRIES = 1   // 一个字体在队列里最多试几次
};

// 补丁失败的原因码（进失败名单时记下，日志里报出来；下标即原因码）
enum {
    GLYPH_FAIL_ALLOC = 1, // 新表分配失败
    GLYPH_FAIL_ENTRY = 2, // charmap 写槽没生效（set_charmap_entry 无返回值 ⇒ 回读校验发现）
    GLYPH_FAIL_FAULT = 3, // 打补丁途中异常（已回滚）
    GLYPH_FAIL_LIMIT = 4, // 字形数装不进 u16
    GLYPH_FAIL_SLOTS = 5  // 原因码取值的上界 +1
};

// 字形记录（0x20 字节/条）内的字段偏移。
//   +0x06/+0x08 是 i16（写入端 movsx）；+0x0A 写按 u16、读按 i16。
//   +0x1C 是图集页下标，PC 引擎 1（DrawText）从不读；+0x1E..+0x1F 保持 0。
enum {
    GLYPH_REC_CP = 0x00,
    GLYPH_REC_W = 0x02,
    GLYPH_REC_H = 0x04,
    GLYPH_REC_XOFF = 0x06,
    GLYPH_REC_YOFF = 0x08,
    GLYPH_REC_ADV = 0x0A,
    GLYPH_REC_U0 = 0x0C,
    GLYPH_REC_V0 = 0x10,
    GLYPH_REC_U1 = 0x14,
    GLYPH_REC_V1 = 0x18,
    GLYPH_REC_PAGE = 0x1C
};

// ======================================================================
// 引擎 API（注入式）
//
// glyph 唯一不能依赖的东西就是"游戏在不在"。真机上这些是引擎函数
// （bind_engine_at 的地址表，按宿主口味分栏），单测里是假实现。
// 两边共用同一个 font_loaded_post()。
//
// 调用约定直接写在类型里：约定错 ⇒ 单测的假实现都编不过。
//   各条依据（Dx9；Dx10 的等价物在 glyph.cpp 的地址表里）：
//   alloc        __cdecl (size, flags)   调用点 push 0 / push size / call + add esp,8
//   setEntry     __thiscall(ecx=charmap) + 3 栈参(cp,glyphIdx,flag)，retn 0Ch
//                ⇒ 等价的 __fastcall 形状是 (cm, edx_unused, cp, glyphIdx, flag)
//   record_ctor  __thiscall(ecx=rec)，尾 retn（无栈参）⇒ ≡ __fastcall(rec, edx_unused)
//   array_ctor   __stdcall(base, elem, count, ctor)，尾 retn 10h（被调方清栈），
//                且内部 mov ecx,esi; call ctor ⇒ 元素构造器的第 1 参走 ecx
// ======================================================================
typedef void(__fastcall* GlyphRecCtorFn)(void* rec, void* edx_unused);

struct EngineApi {
    void* (*alloc)(unsigned size); // → 引擎 alloc(size, flags=0)
    void(__fastcall* set_charmap_entry)(void* charmap, void* edx_unused, unsigned cp, unsigned glyphIdx,
                                        unsigned flag);
    GlyphRecCtorFn record_ctor;
    void(__stdcall* record_array_ctor)(void* base, unsigned elem_size, unsigned count, GlyphRecCtorFn ctor);
};

// 注入引擎 API。传 NULL = 解绑（之后 font_loaded_post 只计数不写）。
void bind_engine(const EngineApi* api);

// 按宿主基址填一份 EngineApi 并注入（地址表集中在 glyph.cpp）。
// base 传 0 = 用 core::hook_base()。返回 1 = 已绑定，0 = 基址为 0。
int bind_engine_at(unsigned base);

// 告知宿主基址：用来换算字体类 vtable 值（vtable 闸门用；Dx9 两套类 / Dx10 一套，
// 见 glyph.cpp set_host_base）。install(base) 与 bind_engine_at(base) 都会自动调它；
// 单测拿它喂一个假基址。
void set_host_base(unsigned base);

bool engine_bound();

// 「某个字体对象 = 清单里第几套」的对外出口。返回套下标；未登记返回 -1。
int font_set_of(const void* font);

// forge 补丁核验（启动期文件核验）：读 <asiDir>AC1_CJK_Forge.txt
// （tools/write_forge_sidecar.py 产出），比对 <asiDir>..\DataPC.forge 尺寸
// （sidecar flags&1 时再核 CRC32）。结果即整套字形补丁的总闸：1=启用 0=停用
// （原版零影响）。装配层在装钩前调它。
int  check_forge_patch(const char* asiDir);
void set_forge_patch(int present); // 装配层/单测直设；check_forge_patch 内部也走它
int  forge_patch_present();        // 当前闸门值（默认 0 = 安全）

// ======================================================================
// 字形集清单文件 AC1_CJK_Glyphs.bin
//
// 纯二进制，小端。全部偏移按字节：
//   头（12 B）
//     0x00  char  magic[4] = "ACGG"
//     0x04  u16   version = 1
//     0x06  u16   n_sets
//     0x08  u32   n_recs_total            （全部套的记录数之和，用来对账文件长度）
//   每套头（40 B = 10 个 u32），共 n_sets 套
//     +0x00 u32   count                   本套记录数 K
//     +0x04 u32   fp_glyph_count          指纹：期望的原字形数；0 = 不校验
//     +0x08 f32   fp_u_span               指纹：'@' 记录的 (u1-u0)
//     +0x0C f32   fp_v_span               指纹：'@' 记录的 (v1-v0)
//     +0x10 u32   orig_w                 该套原图集宽（原字形记录的 UV 要缩到它的左上角）
//     +0x14 u32   orig_h                 原图集高
//     +0x18 u32   flags                  保留（当前必须为 0）
//     +0x1C u32   our_w                  我们这张图集的宽（每套不同：按 em 定格子）
//     +0x20 u32   our_h                  图集高（按用量定；v 方向缩放系数用它）
//     +0x24 u32   reserved                （前向兼容，当前必须为 0）
//   原记录 UV 的缩放系数是 (orig_w/our_w, orig_h/our_h)，逐套取。
//   记录（0x20 = 32 B/条），共 Σcount 条
//     +0x00 u16 cp      +0x02 u16 w      +0x04 u16 h
//     +0x06 i16 xoff    +0x08 i16 yoff   +0x0A i16 adv
//     +0x0C f32 u0      +0x10 f32 v0     +0x14 f32 u1     +0x18 f32 v1
//     +0x1C u16 page    +0x1E u16 pad = 0
//
//   字段表逐项相加是 32 字节，与引擎的字形记录逐字节同构——"读文件 → 写进引擎表"
//   是一次 32 字节 memcpy。
//
// 指纹的意义：游戏里有多套字体，exe 侧无法枚举资源名。新记录的 UV 必须落在
// 那一套字体的图集里，所以用该字体 '@' 字形的 UV 跨度当指纹，逐套精确匹配，
// 匹配不上就一个字节都不写。
//
// 打包器必须把 fp_u_span / fp_v_span 写成 f32 的 u1-u0 / v1-v0 的结果，
// 不要写十进制字面量（f32 相减带舍入，与字面量差在第 7 位小数上）。
//
// v 的约定：引擎绘制时取 1-v（顶左原点翻转）。我们不改记录，打包器应当把
// 文件里的 v 写成引擎读出来的原值，指纹比对也按原值做。
// ======================================================================
enum {
    // 常量按"内存里那 4 个字节"写：文件头是 ASCII A C G G = 41 43 47 47，
    // 按小端 u32 读 = 0x47474341。
    GLYPH_SET_MAGIC = 0x47474341u, // "ACGG"（字节 41 43 47 47）
    GLYPH_SET_VERSION = 1,
    GLYPH_MAX_SETS = 8,     // 单文件最多几套
    GLYPH_MAX_RECS = 65536, // 单文件最多几条记录（≈2 MB 静态表）
    GLYPH_REC_FILE = 0x20   // 记录在文件里的字节数
};

// 清单解析的失败原因码（< 0）。日志里会写出来。
enum {
    GLYPH_MANIFEST_E_ARG = -1,     // 路径为空
    GLYPH_MANIFEST_E_OPEN = -2,    // 读不了（缺失 / 空 / 超限 / 读不满）
    GLYPH_MANIFEST_E_HEADER = -3,  // 太短 / 套数或记录数越界
    GLYPH_MANIFEST_E_MAGIC = -4,   // magic 不是 ACGG
    GLYPH_MANIFEST_E_VERSION = -5, // version 不是 1
    GLYPH_MANIFEST_E_TRUNC = -6,   // 中途截断（声明的记录数读不满）
    GLYPH_MANIFEST_E_LENGTH = -7   // 文件长度与声明对不上（多尾巴 / 少字节）
};

// 读入字形集清单。返回套数（>= 0），< 0 = 失败原因码。
// 失败时状态归零（不留半份数据），并打一行说明。
// 没有这个文件是正常状态（返回 E_OPEN）：钩子照装，只是每套都 no_match。
int load_manifest(const char* path);

// 清单文件名。这是本模块的数据契约（上面的逐字段布局描述的就是这个文件），
// 所以由 glyph 提供名字，装配层只负责把它拼到 <asiDir> 后面 —— 装配层不硬编码字面量。
const char* manifest_file_name();

// 已加载的套数
int set_count();

// ======================================================================
// 补丁入口（钩子与单测共用这一个函数）
//
// 闸门：forge 闸门开 / font 非空 / 引擎 API 已绑定 / vtable 是当前宿主的字体类 /
//       count > 0 / 表指针非空 / count ≤ GLYPH_MAX_COUNT / 字体没打过补丁。
// 任一不过 ⇒ 一个字节都不写；forge 闸门关只在整轮打一次日志、dup（已补过）只计数
//   不打行，其余拒绝路径各打一行。
// ======================================================================
void font_loaded_post(void* font);

// ======================================================================
// 字体指纹（供打包器 / 单测核对用）
//
// exe 里没有字体资源名可枚举，运行时创建了几个字体对象、每套的 UV 跨度是多少，
// 静态枚举不出来。所以模块在真机跑时把每个字体的跨度打进日志，打包器据此写
// fp_u_span / fp_v_span。'@'（0x40）几乎必然存在于任何拉丁字体 ⇒ 它的 UV 跨度
// 是最省事的指纹。只读：不写字体对象的任何字节。
// ======================================================================
struct GlyphFingerprint {
    unsigned count;  // 该字体字形数（font+0x34）
    unsigned hasAt;  // 1 = 表里找到了码点 0x40（'@'）
    float    u_span; // '@' 的 (u1-u0)；没找到时 0
    float    v_span; // '@' 的 (v1-v0)；没找到时 0
};

// 读字体对象算指纹。返回 1 = 取到（count > 0 且表指针非空，此时 hasAt 仍可能为 0）；
// 返回 0 = 取不到（font/out 为空、字形数为 0、表指针为空，或读的时候异常）。
// 取不到时 out 被清零。font 是引擎对象 ⇒ 内部用 __try 兜底。
int font_fingerprint(const void* font, GlyphFingerprint* out);

// ======================================================================
// 兜底路径的两个 detour（导出来是为了让单测能驱动同一个 detour，而不是复制品）
// 真机上它们由 core::hook_install_one 装上，orig 槽填 trampoline。
// 单测用 set_hook_origins() 塞自己的假 orig，然后直接调这两个 detour。
// 手工调它们在真机上没有意义（没有 orig 转发）。
// ======================================================================
typedef unsigned short(__fastcall* LookupOrig)(void* charmap, void* edx_unused, unsigned short C);
// DrawText 是 __thiscall + retn 20h = 8 个栈参；写成 __fastcall 的 10 参形状
// （ecx=self、edx 占位、其余 8 个按序入栈）与之完全对齐。
typedef bool(__fastcall* DrawTextOrig)(void* self, void* edx_unused, unsigned a0, unsigned a1, unsigned a2,
                                       unsigned a3, unsigned a4, unsigned a5, unsigned a6, unsigned a7);
void set_hook_origins(LookupOrig lookup, DrawTextOrig drawtext);

// 0x8965D0（charmap 查表）后置钩：先转发拿原值，再做"发现 + 排队"，最后原样返回。
// 只读：一个字节都不写。
unsigned short __fastcall hook_lookup(void* charmap, void* edx_unused, unsigned short C);

// 0x799720（DrawText）前置钩：先应用队列里的补丁，再转发给 orig 并原样返回它的值。
bool __fastcall hook_drawtext(void* self, void* edx_unused, unsigned a0, unsigned a1, unsigned a2,
                              unsigned a3, unsigned a4, unsigned a5, unsigned a6, unsigned a7);

// ======================================================================
// 状态
// ======================================================================
struct GlyphStats {
    int                hooks;                        // 已装钩子数（0..3）
    int                fontsPatched;                 // 已打补丁字体数
    int                glyphsAdded;                  // 追加字形数
    int                pagesCreated;                 // 新建 charmap 页数
    int                sets;                         // 已加载字形集套数
    int                fingerprints;                 // 打过指纹样本行的字体数（上限 GLYPH_FP_SAMPLES）
    int                pending;                      // 当前排在待办队列里的字体数（应用完就回落）
    int                revived;                      // 查表时发现引擎把已补丁的字体重置了 ⇒ 重新排队的次数
    int                appliedAtDraw;                // 由 DrawText 前置钩应用的次数
    int                dup;                          // 幂等命中（同一个字体又进来了）
    int                noMatch;                      // 指纹没匹配上任何一套
    int                rejectNull;                   // font == NULL
    int                rejectNoApi;                  // 引擎 API 未绑定
    int                rejectVtable;                 // vtable 不认识
    int                rejectCount0;                 // 字形数 == 0
    int                rejectNoTable;                // 表指针为空
    int                rejectTooBig;                 // 字形数超防御上限
    int                rejectFull;                   // 幂等登记表满（宁可不补，也不冒"重复追加"的风险）
    int                failAlloc;                    // 分配 / 写槽失败
    int                faults;                       // 读字体对象时异常
    int                failListed;                   // 进了失败名单的字体数（这些字体不再被排队/重试）
    int                failByCode[GLYPH_FAIL_SLOTS]; // 失败名单按原因码分桶（下标 = GLYPH_FAIL_*）
    int                cpHitKinds;                   // 被游戏查过的、我们映射了的码位种类数
    long               cpHitTotal;                   // 上述命中累计次数
    int                cpMissKinds;                  // 游戏想要、我们没映射的码位（≥0x3000）种类数
    unsigned long long legacyBytes;                  // 故意不释放的旧表字节数（见 glyph.cpp）
};
const GlyphStats& stats();

// 装钩（只装不卸：钩装在 core 的进程全局单例表里，运行期没有卸载路径）。
// install(base) 返回装上的项数（0..3：加载钩 / 查表钩 / DrawText 钩）。
// 拿不到基址、或引擎 API 没绑定 ⇒ 返回 0 + 日志说明，绝不硬装。
// 快路径（加载钩）装不上不影响兜底路径继续装——那正是加兜底的原因。
int install(unsigned base);

// 一行状态。写入调用方的缓冲，返回该缓冲。
const char* status(char* out, int cap);

// 码位命中/漏字的 Top 报表。由装配层的常驻循环（~10 秒）调；
// 内部只在"种类数变过"时才真的打日志 ⇒ 空转时一行都不打。
void report_tops();

} // namespace glyph
} // namespace ac1
