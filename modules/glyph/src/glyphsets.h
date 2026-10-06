// modules/glyph/src/glyphsets.h —— 同模块内部：字形集清单的落地存储
//
// 不进 include/，不跨模块：只有 glyph.cpp（打补丁）与 manifest.cpp（读文件）
// 需要看它。单测也不直接看 —— 它走 ac1/glyph/glyph.h 的公开接口断言。
//
// 静态数组而不是堆：glyph 在 DllMain 之后的 worker 线程里跑，但清单可能在
// 任何时候被读进来；固定大小 + 一次填满 ⇒ 没有"读到一半"的中间态。

#pragma once

#include "ac1/glyph/glyph.h"

namespace ac1 {
namespace glyph {

// 文件里的一条记录（恰好 0x20 = 32 字节，必须 #pragma pack(1)）。
// manifest.cpp 按 GLYPH_REC_FILE(0x20) 步进 memcpy，并有 static_assert 钉死 sizeof。
#pragma pack(push, 1)
struct GlyphRec {
    unsigned short cp, w, h;
    short          xoff, yoff, adv;
    float          u0, v0, u1, v1;
    unsigned short page, pad;
};
#pragma pack(pop)

// 一套字形。fp_* 是指纹：拿字体表里 '@' 记录的 UV 跨度去比对，
// 匹配不上就宁可不补（见 glyph.h 里"多套字体"那段说明）。
struct GlyphSet {
    unsigned count;          // 本套记录数 K
    unsigned fp_glyph_count; // 指纹：期望的原字形数；0 = 不校验
    float    fp_u_span;      // 指纹：'@' 的 (u1-u0)
    float    fp_v_span;      // 指纹：'@' 的 (v1-v0)
    // 该套原图集尺寸。原字形记录的 UV 是按原图集（0..1）算的 ⇒ 必须乘
    //   (orig_w/our_w, orig_h/our_h) 缩到我们图集的原字形区，否则英文/数字/符号
    //   会横跨整张图集采样到 CJK 格子 ⇒ 满屏乱码。
    unsigned orig_w, orig_h;
    // 保留字段（当前必须为 0，见 glyph.h 的格式表）。
    unsigned flags;
    // 我们这张图集的实际尺寸（每套不同：按 em 定格子）。
    unsigned our_w, our_h;
    int      first; // 在 g_recs 里的起始下标
};

// 落地存储（manifest.cpp 填，glyph.cpp 读）
extern GlyphSet      g_sets[GLYPH_MAX_SETS];
extern GlyphRec      g_recs[GLYPH_MAX_RECS];
extern volatile long g_nsets;

// ======================================================================
// 码位索引与命中统计（cpindex.cpp）
//
// 存在的理由：要定策略就得先知道"到底哪个码位被查、被谁查"。而不能从文本层的
//   字符串清单里找：含 CJK 的串会被漏斗的"含 CJK 闸门"提前跳过，压根进不了
//   那份清单 ⇒ 只有字形层自己记得住。
//
// 命中表：把清单里所有码位去重排序，二分查找。
//   · g_cpPage[] 是 1 位/码位（按 C>>6 分桶）的 O(1) 拒斥位图：热路径上先用它
//     挡掉"我们这一页什么都没有"的绝大多数请求，挡掉才轮到二分。
//   · g_cpHits[] 与 g_cpCaller[] 与 g_allCp 下标对齐，饱和在 0xFFFF。
// 漏字表：只记 `C >= 0x3000` 且不在我们表里的 —— 也就是"游戏想要、我们漏了的字"。
//   码位空间 0x3000..0xFFFF 只有 0xD000 项，直接索引（不做线性表），
//   否则热路径上要扫上万项。
// ======================================================================
extern unsigned short     g_allCp[GLYPH_MAX_RECS];    // 升序去重
extern unsigned short     g_cpHits[GLYPH_MAX_RECS];   // 与 g_allCp 下标对齐，饱和 0xFFFF
extern unsigned           g_cpCaller[GLYPH_MAX_RECS]; // 该码位第一次被查时的调用者返回地址
extern unsigned short     g_missHits[0xD000];         // 下标 = C - 0x3000，饱和 0xFFFF
extern unsigned           g_missFirst[0xD000];        // g_missHits 同下标，存第一次的调用者
extern unsigned long long g_cpPage[1024];             // 1 位/码位，C>>6 分桶
extern volatile long      g_nAllCp;

void cp_index_reset();                // 清单读失败 / 换文件时调用
void cp_index_add(unsigned short cp); // 逐条喂进去（去重在 finish 里做）
void cp_index_finish();               // 排序 + 去重 + 重建位图
int  cp_index_find(unsigned short C); // 二分；返回下标或 -1
// 查表钩热路径上唯一的入口：记一次"C 被查了"。C < 0x3000 立刻返回（零成本）。
void cp_index_observe(unsigned short C, unsigned caller);
int  cp_index_hit_kinds();
long cp_index_hit_total();
int  cp_index_miss_kinds();
void cp_index_report_tops(); // 内部有"种类数变过才打"的闸门

} // namespace glyph
} // namespace ac1
