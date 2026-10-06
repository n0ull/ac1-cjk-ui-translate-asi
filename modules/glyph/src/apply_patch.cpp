// modules/glyph/src/apply_patch.cpp —— 补丁事务：形状闸门 → 建新表 → UV 缩放 → charmap → 回滚
//
// 补丁主体（加载钩 / DrawText 前置钩共用同一个函数）。
// 两条硬约束（否则引擎析构会把野指针交给 magma 堆）：
//   · 引擎用引擎的 free 释放 font+0x38 指向的表 ⇒ 我们自建的表必须用引擎的
//     alloc 分配，绝不能 CRT malloc / 不能 new。
//   · charmap 页同理：页块由引擎析构逐页回收 ⇒ 必须让引擎自己建页：逐码位调
//     set_charmap_entry(charmap, cp, glyphIdx, flag=0)。
//
// 为什么「换表」而不是「原地扩」：setGlyphs(0x8842F0) 全程序只有 2 个调用者
//   （.mft 加载器 + GlyphFont 析构）⇒ 加载完成后引擎永不重建这张表 ⇒
//   没有任何 API 能原地扩容。而 alloc / free 是成对的，引擎析构只会按当时的
//   font+0x38 释放一块 —— 所以"建新块 → 拷旧 → 加新 → 换指针"是唯一安全的形状。
//
// ===== 旧表不释放（有意的、有界的让渡）=====
// 引擎只会在析构时释放它自己那一刻 +0x38 指向的块。我们若在中途释放旧块，
// 就得赌"别处没有缓存过旧指针"—— 输了就是 use-after-free，崩在游戏里。
// 所以旧块留着不释放，代价是每个被补的字体泄漏 count×0x20 字节
// （典型 224×0x20 = 7 KB），一次性、可数、日志里逐项报出。
// 反过来，新块交给引擎，引擎析构时一定会正确释放它（同一个 magma 堆）。

#include "ac1/glyph/glyph.h"

#include "ac1/core/log.h"
#include "ac1/core/mem.h"

#include "glyphsets.h"
#include "glyph_internal.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>

namespace ac1 {
namespace glyph {

// 故意不释放的旧表字节数（累加），见文件头「旧表不释放」
//   __declspec(align(8))：InterlockedExchangeAdd64 要求 8 字节对齐
__declspec(align(8)) volatile long long g_legacyBytes = 0;

static void write_rec(unsigned char* rec, const GlyphRec& g)
{
    *(unsigned short*)(rec + GLYPH_REC_CP) = g.cp;
    *(unsigned short*)(rec + GLYPH_REC_W) = g.w;
    *(unsigned short*)(rec + GLYPH_REC_H) = g.h;
    *(short*)(rec + GLYPH_REC_XOFF) = g.xoff;                        // i16：写入端引擎用 movsx
    *(short*)(rec + GLYPH_REC_YOFF) = g.yoff;                        // i16
    *(unsigned short*)(rec + GLYPH_REC_ADV) = (unsigned short)g.adv; // 写 u16、读 i16
    *(float*)(rec + GLYPH_REC_U0) = g.u0;
    *(float*)(rec + GLYPH_REC_V0) = g.v0;
    *(float*)(rec + GLYPH_REC_U1) = g.u1;
    *(float*)(rec + GLYPH_REC_V1) = g.v1;
    *(unsigned short*)(rec + GLYPH_REC_PAGE) = g.page; // 页下标：PC 引擎1 不读
    *(unsigned short*)(rec + 0x1E) = 0;                // ctor 与装载器都不写 ⇒ 保持 0
}

// ======================================================================
// 形状闸门：条件的唯一一份实现，三条路径（加载钩 / 查表钩 / 应用）都走它。
// 静默，不打日志、不计数 —— 报不报由调用方决定（热路径不打日志）。
// ======================================================================
int font_shape(const void* font, unsigned* outCount, unsigned char** outTable, unsigned* outVt)
{
    if (!font) return SHAPE_NULL;
    __try {
        const unsigned char* f = (const unsigned char*)font;
        unsigned             vt = *(const unsigned*)f;
        unsigned             count = *(const unsigned short*)(f + GLYPH_OFF_COUNT);
        const unsigned char* table = *(const unsigned char* const*)(f + GLYPH_OFF_TABLE);
        if (outVt) *outVt = vt;
        if (outCount) *outCount = count;
        if (outTable) *outTable = (unsigned char*)table;
        if (!vtable_ok(vt)) return SHAPE_VTABLE;
        if (count == 0) return SHAPE_COUNT0;
        if (!table) return SHAPE_NOTABLE;
        if (count > (unsigned)GLYPH_MAX_COUNT) return SHAPE_TOOBIG;
        return SHAPE_OK;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return SHAPE_FAULT;
    }
}

// 把 SHAPE_* 翻译成"计数 + 一行日志"。加载钩（低频）用它；查表钩直接忽略。
void report_shape_reject(int sh, const void* font, unsigned vt, unsigned count)
{
    switch (sh) {
    case SHAPE_VTABLE:
        inc(C_REJ_VTABLE);
        core::log_line("[字形] 字体 %p vtable=%08X 不认识 ⇒ 不打补丁", font, vt);
        break;
    case SHAPE_COUNT0:
        inc(C_REJ_COUNT0);
        core::log_line("[字形] 字体 %p 字形数=0 ⇒ 不打补丁", font);
        break;
    case SHAPE_NOTABLE:
        inc(C_REJ_NOTABLE);
        core::log_line("[字形] 字体 %p 字形表指针为空 ⇒ 不打补丁", font);
        break;
    case SHAPE_TOOBIG:
        inc(C_REJ_TOOBIG);
        core::log_line("[字形] 字体 %p 字形数=%u > 上限 %d ⇒ 不打补丁（不认识的布局）", font, count,
                       (int)GLYPH_MAX_COUNT);
        break;
    case SHAPE_FAULT:
        inc(C_FAULT);
        core::log_line("[字形] !! 读字体 %p 时异常 ⇒ 本字体不补丁", font);
        break;
    default: break;
    }
}

// ======================================================================
// 补丁主体（加载钩 / DrawText 前置钩共用同一个函数）
//   本函数自己只负责：形状闸门 → 已经补过就返回 → 建新块 → 拷旧 → 写新 →
//   发布指针 → 补 charmap → 登记。返回 1 = 这次真的换了表。
// ======================================================================
int apply_patch(const void* font, int setIdx, const char* how)
{
    if (!forge_patch_present()) return 0; // forge 闸门（纵深防御；主闸在 font_loaded_post / 排队处）
    if (!g_apiOk) return 0;
    unsigned       count = 0, vt = 0;
    unsigned char* table = NULL;
    if (font_shape(font, &count, &table, &vt) != SHAPE_OK) return 0; // 静默：调用方决定报不报
    if (setIdx < 0 || setIdx >= (int)g_nsets) return 0;

    // 已经补过、且引擎没有把对象重置（表指针仍是我们换上去那块）⇒ 无事可做
    int ri = registry_find(font);
    if (ri >= 0 && table == g_ourTable[ri]) return 0;

    // 登记表满了还继续补，就有可能给同一个字体追加第二遍 ⇒ 宁可不做
    if (ri < 0 && reg_count() >= GLYPH_MAX_FONTS) {
        inc(C_REJ_FULL);
        core::log_line("[字形] !! 幂等登记表已满（%d 条）⇒ 字体 %p 不打补丁", reg_count(), font);
        return 0;
    }

    // 回滚用的快照：换表是最后一步发布，但补槽可能失败，失败必须把
    // +0x38/+0x34 放回去，绝不能给引擎留一个"表已换、charmap 不全"的半程状态。
    const unsigned char* oldTable = table;
    const unsigned       oldCount = count;
    int                  published = 0;

    __try {
        unsigned char*  f = (unsigned char*)font;
        const GlyphSet& set = g_sets[setIdx];
        unsigned        K = set.count;
        if (K == 0) return 0;
        if (count + K > 0xFFFFu) { // +0x34 是 u16，装不下就绝不能写
            inc(C_REJ_TOOBIG);
            core::log_line("[字形] 字体 %p：%u+%u 超出 u16 字形数上限 ⇒ 不打补丁", f, count, K);
            fail_list_add(font, GLYPH_FAIL_LIMIT);
            return 0;
        }
        unsigned total = count + K;

        // ---- ① 新块 + 逐条构造 + 拷旧 + 写新 ----
        //   引擎释放这张表时走 0x787A20，所以必须用 0x787980 分配（magma 堆）
        unsigned char* nw = (unsigned char*)g_api.alloc(total * (unsigned)GLYPH_REC_SIZE);
        if (!nw) {
            inc(C_FAILALLOC);
            core::log_line("[字形] !! 字体 %p：新表分配失败（%u 字节）⇒ 不打补丁", f,
                           total * (unsigned)GLYPH_REC_SIZE);
            fail_list_add(font, GLYPH_FAIL_ALLOC);
            return 0;
        }
        // 0x4020A0 是 __stdcall(base, elem, count, ctor)，且用 `mov ecx,esi; call ctor`
        // 调元素构造器 ⇒ ctor 的第 1 参在 ecx。EngineApi 的字段类型就是
        // 按这个形状写的（见 engine_api.cpp 文件头的约定表），这里不再有任何 reinterpret。
        g_api.record_array_ctor(nw, (unsigned)GLYPH_REC_SIZE, total, g_api.record_ctor);
        memcpy(nw, table, (size_t)count * (size_t)GLYPH_REC_SIZE);
        // 把原字形记录的 UV 缩到我们图集里原图集所占的那一块。
        //   原记录的 UV 是按原图集尺寸（0..1）算的；我们的图集更大，
        //   原图集只占左下角 (orig_w/our_w, orig_h/our_h) ⇒ 不缩放就会横跨整张
        //   图集采样，扫到 CJK 格子与空白 ⇒ 英文/数字/符号满屏乱码。
        //   （CJK 记录的 UV 由打包器直接按我们的图集算好，不在此列。）
        //   forge 路线下此处无条件缩放：能走到 apply_patch 说明 forge 闸门
        //     已开（我们的图集就在 forge 里）。
        {
            const unsigned OW = g_sets[setIdx].orig_w, OH = g_sets[setIdx].orig_h;
            if (OW && OH) {
                // 缩放系数必须用本套图集的实际尺寸，不能硬编码 1024/2048：
                //   图集现在是按 em 定格子、按用量定高度，每套都不同。
                const unsigned NW = g_sets[setIdx].our_w, NH = g_sets[setIdx].our_h;
                const float    su = NW ? (float)OW / (float)NW : 1.0f;
                const float    sv = NH ? (float)OH / (float)NH : 1.0f;
                for (unsigned r = 0; r < count; r++) {
                    float* uv = (float*)(nw + (size_t)r * GLYPH_REC_SIZE + GLYPH_REC_U0);
                    uv[0] *= su;
                    uv[1] *= sv;
                    uv[2] *= su;
                    uv[3] *= sv;
                }
            }
        }
        for (unsigned i = 0; i < K; i++)
            write_rec(nw + (size_t)(count + i) * (size_t)GLYPH_REC_SIZE, g_recs[set.first + (int)i]);

        // 度量自检（只读，不改任何东西）：把原表的拉丁记录与我们追加的 CJK 记录
        //   在同一张表、同一套单位里并排打出来。
        { // ★ 不做"只打一次"闸门：一次性标志会被"日志尚关闭"的那次调用先消费掉，
            //   真正该看度量的那轮反而一行不打。这段是纯只读探针，每次补丁都打。
            const unsigned char* L = table;
            unsigned             lwMax = 0, lhMax = 0, lyoMax = 0;
            int                  ladvMin = 0x7FFF, ladvMax = 0, ladvSum = 0, ladvN = 0;
            for (unsigned r = 0; r < count; r++) {
                const unsigned char* p = L + (size_t)r * GLYPH_REC_SIZE;
                unsigned             cp = *(const unsigned short*)(p + GLYPH_REC_CP);
                unsigned             w = *(const unsigned short*)(p + GLYPH_REC_W);
                unsigned             h = *(const unsigned short*)(p + GLYPH_REC_H);
                int                  xo = *(const short*)(p + GLYPH_REC_XOFF);
                int                  yo = *(const short*)(p + GLYPH_REC_YOFF);
                int                  ad = *(const short*)(p + GLYPH_REC_ADV);
                if (w > lwMax) lwMax = w;
                if (h > lhMax) lhMax = h;
                if (yo > (int)lyoMax) lyoMax = yo;
                if (ad > 0) {
                    if (ad < ladvMin) ladvMin = ad;
                    if (ad > ladvMax) ladvMax = ad;
                    ladvSum += ad;
                    ladvN++;
                }
                if (cp == 0x40)
                    core::log_line("[度量] 套#%d 拉丁 '@'：w=%u h=%u xo=%d yo=%d adv=%d", setIdx, w, h, xo,
                                   yo, ad);
            }
            core::log_line("[度量] 套#%d 原表 %u 条：w<=%u h<=%u yo<=%u adv %d~%d 均值%d", setIdx, count,
                           lwMax, lhMax, lyoMax, ladvMin, ladvMax, ladvN ? ladvSum / ladvN : 0);
            for (int i = 0; i < 2 && i < (int)K; i++) {
                const unsigned char* p = nw + (size_t)(count + i) * GLYPH_REC_SIZE;
                core::log_line("[度量] 套#%d CJK#%d U+%04X：w=%u h=%u xo=%d yo=%d adv=%d", setIdx, i,
                               *(const unsigned short*)(p + GLYPH_REC_CP),
                               *(const unsigned short*)(p + GLYPH_REC_W),
                               *(const unsigned short*)(p + GLYPH_REC_H), *(const short*)(p + GLYPH_REC_XOFF),
                               *(const short*)(p + GLYPH_REC_YOFF), *(const short*)(p + GLYPH_REC_ADV));
            }
        }

        // 最后一步才发布新指针：在这之前引擎读到的永远是自洽的旧表
        *(unsigned char**)(f + GLYPH_OFF_TABLE) = nw;
        *(unsigned short*)(f + GLYPH_OFF_COUNT) = (unsigned short)total;
        published = 1;

        // 旧块不释放（见文件头）；代价逐项记账
        InterlockedExchangeAdd64(&g_legacyBytes, (LONGLONG)count * (LONGLONG)GLYPH_REC_SIZE);

        // ---- ② 补 charmap：逐码位调 setCharmapEntry(flag=0) ----
        //   它自己懒建页（内部调 0x896600）+ 写槽，完全替代"先建页再逐槽写"。
        //   flag 恒传 0：非 0 会走 `mov [ecx+400h], di` 改写 font+0x43C
        //     （全局缺字符默认字形）—— 我们绝不碰那个字段。
        //   setCharmapEntry 没有返回值 ⇒ 每写一槽都回读确认它真的生效了；
        //     没生效就整笔回滚 + 拉黑，而不是留半程状态给引擎。
        unsigned char* cm = f + GLYPH_OFF_CHARMAP; // 内联 256 项 dword 数组
        unsigned       pagesHere = 0, done = 0;
        int            fail = 0;
        for (unsigned i = 0; i < K; i++) {
            const GlyphRec& g = g_recs[set.first + (int)i];
            unsigned        cp = g.cp;
            unsigned        page = cp >> 8;
            unsigned        slot = cp & 0xFF;
            unsigned char** pg = (unsigned char**)(cm + page * 4);
            bool            hadPage = (*pg != NULL);
            g_api.set_charmap_entry(cm, NULL, cp, count + i, 0);
            if (!hadPage && *pg) {
                pagesHere++;
                inc(C_PAGES);
            }
            unsigned short got = *pg ? *(unsigned short*)(*pg + slot * 2) : 0xFFFFu;
            if (got != (unsigned short)(count + i)) {
                inc(C_FAILALLOC);
                core::log_line("[字形] !! 字体 %p：charmap 槽 U+%04X 没写上（期望 %u 实得 %u）"
                               "⇒ 回滚并拉黑",
                               f, cp, count + i, (unsigned)got);
                fail = GLYPH_FAIL_ENTRY;
                break;
            }
            done++;
        }
        if (fail) {
            // 半程回滚：把 +0x38 / +0x34 放回补丁前的样子。
            // （已经写进 charmap 的槽指向的是旧表里不存在的下标；拉黑之后我们
            //   不会再碰这个字体，引擎自己的越界检查 0x881B43 会兜住。）
            if (published) {
                *(unsigned char**)(f + GLYPH_OFF_TABLE) = (unsigned char*)oldTable;
                *(unsigned short*)(f + GLYPH_OFF_COUNT) = (unsigned short)oldCount;
            }
            fail_list_add(font, fail);
            return 0;
        }

        char sn[16];
        _snprintf(sn, sizeof(sn), "套#%d", setIdx);
        core::log_line("[字形][%s] 字体 %p vtable=%08X 字形 %u→%u（+%d）页 %d 条 集=%s", how, f, vt, count,
                       total, (int)K, (int)pagesHere, sn);

        inc(C_FONTS);
        core::ctr_add(&g_ctr, C_GLYPHS, (long)done);

        // ---- 登记（幂等表 + 我们的表指针）----
        if (ri >= 0) { registry_set_table(font, nw, setIdx); }
        else if (!registry_add(font, nw, setIdx)) {
            // 占槽失败 = 在上面那次「登记表已满」检查之后、这一行之前，另一个线程
            // 恰好把最后几格占满了。补丁已经发布到字体上，不能就这么放手（没登记的
            // 字体会被重新发现、重新补一遍）。按半程回滚处理，与上面那条失败路径同款。
            inc(C_REJ_FULL);
            *(unsigned char**)(f + GLYPH_OFF_TABLE) = (unsigned char*)oldTable;
            *(unsigned short*)(f + GLYPH_OFF_COUNT) = (unsigned short)oldCount;
            core::log_line("[字形] !! 字体 %p：登记时被并发占满了幂等表 ⇒ 回滚，不打补丁", f);
            return 0;
        }
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        inc(C_FAULT);
        core::log_line("[字形] !! 打补丁时异常（.EXCEPTION_%08X）⇒ 字体 %p 回滚并拉黑",
                       (unsigned)GetExceptionCode(), font);
        // 异常可能发生在发布之后 ⇒ 同样要回滚，否则引擎拿到半程状态。
        // 这一段刻意不放在 __try 里：回滚本身不该再抛。
        if (published) {
            unsigned char* f = (unsigned char*)font;
            *(unsigned char**)(f + GLYPH_OFF_TABLE) = (unsigned char*)oldTable;
            *(unsigned short*)(f + GLYPH_OFF_COUNT) = (unsigned short)oldCount;
        }
        fail_list_add(font, GLYPH_FAIL_FAULT);
        return 0;
    }
}

// ======================================================================
// 快路径：0x8840B0 字体加载的后置钩子（补丁入口；detours.cpp 的 hook_font_load 调它）
//
// 它单独不够：字体对象的加载可能发生在钩子装好之前（实机两轮里一轮赶上、
//   一轮没赶上）。它只是"赶上了就更好"的那条路；真正的保障是两条兜底路径。
// 这里只放 POD 局部变量：/EHsc 下带对象析构的函数里不允许 __try（C2712）。
//   font 是引擎对象，形状我们只"信"到闸门那一层，再往后必须 __try 兜底。
// ======================================================================
void font_loaded_post(void* font)
{
    if (!font) {
        inc(C_REJ_NULL);
        core::log_line("[字形] 字体 NULL ⇒ 不打补丁");
        return;
    }
    // 拉黑过的字体直接跳过（加载钩也是每次加载调一次，别让它反复试）
    if (fail_listed(font)) return;

    // ---- 指纹采样：先于一切闸门与匹配，只读不写 ----
    //   exe 里枚举不出字体资源名 ⇒ 每套字体的跨度只能靠这行日志收上去，
    //   喂给离线打包器写 AC1_CJK_Glyphs.bin 的 fp_u_span / fp_v_span。
    sample_fingerprint(font);

    if (!g_apiOk) {
        inc(C_REJ_NOAPI);
        core::log_line("[字形] 引擎 API 未绑定 ⇒ 不打补丁（没有引擎的分配器，"
                       "自建的块引擎释放不了）");
        return;
    }

    // ---- forge 闸门：补丁未检出 ⇒ 整套不打（指纹采样/拒绝计数照常，仍喂离线工具）----
    //   闸门语义与判决日志见 forgecheck.cpp；这里只放行/拦停，每轮启动只报一次。
    if (!forge_patch_present()) {
        static int s_gateLogged = 0;
        if (!s_gateLogged) {
            s_gateLogged = 1;
            core::log_line("[字形] forge 补丁未检出 ⇒ 整套字形补丁停用（原版不受影响）");
        }
        return;
    }

    unsigned       count = 0, vt = 0;
    unsigned char* table = NULL;
    int            sh = font_shape(font, &count, &table, &vt);
    if (sh != SHAPE_OK) {
        report_shape_reject(sh, font, vt, count);
        return;
    }

    // ---- 闸门：同一个字体只补一次 ----
    if (registry_find(font) >= 0) {
        inc(C_DUP);
        return;
    } // 幂等：静默，不重复追加

    // ---- 选字形集（指纹）----
    int si = match_set(font);
    if (si < 0) {
        inc(C_NOMATCH);
        core::log_line("[字形] 字体 %p vtable=%08X 字形 %u：没有匹配的字形集（已加载 %d 套）"
                       "⇒ 不打补丁",
                       font, vt, count, (int)g_nsets);
        return;
    }
    apply_patch(font, si, "加载钩");
}

} // namespace glyph
} // namespace ac1
