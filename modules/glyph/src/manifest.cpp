// modules/glyph/src/manifest.cpp —— 读 `AC1_CJK_Glyphs.bin`（字形集清单）
//
// 文件不存在是正常状态：钩子照装，只是每次匹配都 no_match ——
//   诚实比"看起来在工作"重要。
//
// 文件格式写在 ac1/glyph/glyph.h 里（那边是唯一的口径来源）。
// 任何一处对不上（magic / 版本 / 声明长度 / 实际长度 / 容量）⇒ 整套拒绝，
// 并且把已加载套数归零 —— 半份数据比零份数据危险得多。

#include "ac1/glyph/glyph.h"

#include "ac1/core/log.h"
#include "ac1/core/file.h"

#include "glyphsets.h"

// 本文件不直接 include <windows.h>：这里不碰任何 Win32 API
//   （整文件读取在 core::file_read_all 里，那里自带 windows.h）。
#include <string.h>
#include <stdio.h>

namespace ac1 {
namespace glyph {

GlyphSet      g_sets[GLYPH_MAX_SETS];
GlyphRec      g_recs[GLYPH_MAX_RECS];
volatile long g_nsets = 0;

// 清单文件名。字面量只此一处；app 用 glyph::manifest_file_name() 取，不自己写。
static const char kManifestName[] = "AC1_CJK_Glyphs.bin";
const char*       manifest_file_name() { return kManifestName; }

// 文件里每套的头：40 B。按字节 memcpy 进来，不依赖结构体填充。
#pragma pack(push, 1)
struct SetHdr {
    unsigned count;
    unsigned fp_glyph_count;
    float    fp_u_span;
    float    fp_v_span;
    unsigned orig_w; // 该套原图集宽（字形模块据此缩放原记录的 UV）
    unsigned orig_h; // 原图集高
    unsigned flags;  // 保留字段（当前必须为 0，见 glyph.h 的格式表）
    unsigned our_w;  // 我们这张图集的宽（每套不同：按 em 定格子、按用量定高度）
    unsigned our_h;  // 图集高 —— 原记录 UV 的 v 缩放系数要用它
    unsigned reserved;
};
#pragma pack(pop)

// 下面按 GLYPH_REC_FILE(0x20) 算步长、再 memcpy 进 g_recs（元素大小是
//   sizeof(GlyphRec)）。两者必须相等，否则每条记录多写/少写字节，
//   最坏越界 GLYPH_MAX_RECS×4 = 256 KB。用 static_assert 把这个不变量钉死。
static_assert(sizeof(GlyphRec) == (size_t)GLYPH_REC_FILE,
              "sizeof(GlyphRec) 必须 == GLYPH_REC_FILE(0x20)：解析器按它步进 memcpy");
static_assert(sizeof(SetHdr) == 40, "set header must be 40 bytes");

static const char* reason_text(int code)
{
    switch (code) {
    case GLYPH_MANIFEST_E_ARG: return "路径为空";
    case GLYPH_MANIFEST_E_OPEN: return "读不了（缺失 / 空 / 超限 / 读不满）";
    case GLYPH_MANIFEST_E_HEADER: return "文件头不可信（太短 / 套数或记录数越界）";
    case GLYPH_MANIFEST_E_MAGIC: return "magic 不是 ACGG";
    case GLYPH_MANIFEST_E_VERSION: return "版本不是 1";
    case GLYPH_MANIFEST_E_TRUNC: return "中途截断（声明的记录数读不满）";
    case GLYPH_MANIFEST_E_LENGTH: return "长度与声明对不上（多尾巴 / 少字节）";
    default: return "未知";
    }
}

// 整文件读取走 core::file_read_all（modules/core/include/ac1/core/file.h）。

int load_manifest(const char* path)
{
    // 先归零：任何失败路径都回到「0 套」，绝不留半份数据
    g_nsets = 0;
    // 码位索引跟着一起清：清单是空的 ⇒ 查表钩的命中/漏字统计整段短路
    cp_index_reset();

    if (!path || !path[0]) {
        core::log_line("[字形] · 清单路径为空 ⇒ 0 套（钩子照装，每套都会 no_match）");
        return GLYPH_MANIFEST_E_ARG;
    }

    unsigned char* buf = NULL;
    int            n = 0;
    if (!core::file_read_all(path, &buf, &n, 0)) {
        core::log_line(
            "[字形] · 清单不可用（不存在 / 空 / 超限 / 读失败）：%s ⇒ 0 套（安全状态：没有数据就没有补丁）",
            path);
        return GLYPH_MANIFEST_E_OPEN;
    }

    int rc = GLYPH_MANIFEST_E_HEADER;
    if (n < 12) { rc = GLYPH_MANIFEST_E_HEADER; }
    else if (*(const unsigned*)buf != GLYPH_SET_MAGIC) {
        rc = GLYPH_MANIFEST_E_MAGIC;
    }
    else {
        unsigned version = *(const unsigned short*)(buf + 4);
        unsigned n_sets = *(const unsigned short*)(buf + 6);
        unsigned n_recs = *(const unsigned*)(buf + 8);
        if (version != GLYPH_SET_VERSION) { rc = GLYPH_MANIFEST_E_VERSION; }
        else if (n_sets == 0 || n_sets > (unsigned)GLYPH_MAX_SETS || n_recs > (unsigned)GLYPH_MAX_RECS) {
            rc = GLYPH_MANIFEST_E_HEADER;
        }
        else {
            int off = 12, ns = 0, nr = 0;
            rc = 0; // 全部套都走完 ⇒ 成功
            for (unsigned i = 0; i < n_sets; i++) {
                if (off + (int)sizeof(SetHdr) > n) {
                    rc = GLYPH_MANIFEST_E_TRUNC;
                    break;
                }
                SetHdr h;
                memcpy(&h, buf + off, sizeof(SetHdr));
                off += (int)sizeof(SetHdr);
                if (h.count == 0 || h.count > (unsigned)GLYPH_MAX_RECS) {
                    rc = GLYPH_MANIFEST_E_HEADER;
                    break;
                }
                if ((unsigned)(nr + (int)h.count) > n_recs) {
                    rc = GLYPH_MANIFEST_E_TRUNC;
                    break;
                }
                size_t need = (size_t)h.count * (size_t)GLYPH_REC_FILE;
                if ((size_t)off + need > (size_t)n) {
                    rc = GLYPH_MANIFEST_E_TRUNC;
                    break;
                }
                g_sets[ns].count = h.count;
                g_sets[ns].fp_glyph_count = h.fp_glyph_count;
                g_sets[ns].fp_u_span = h.fp_u_span;
                g_sets[ns].fp_v_span = h.fp_v_span;
                g_sets[ns].orig_w = h.orig_w;
                g_sets[ns].orig_h = h.orig_h;
                g_sets[ns].flags = h.flags;
                g_sets[ns].our_w = h.our_w;
                g_sets[ns].our_h = h.our_h;
                g_sets[ns].first = nr;
                memcpy(g_recs + nr, buf + off, need);
                nr += (int)h.count;
                off += (int)need;
                ns++;
            }
            // 长度对账：声明的套数走完了，但文件里多出尾巴 / 少读了字节
            // ⇒ 文件与声明不一致，一样拒绝
            if (rc == 0 && (off != n || (unsigned)nr != n_recs)) rc = GLYPH_MANIFEST_E_LENGTH;
            if (rc == 0) g_nsets = ns;
        }
    }

    core::file_free(buf);

    if (rc < 0) {
        core::log_line("[字形] !! 清单 %s 被拒：%s（%d）⇒ 0 套", path, reason_text(rc), rc);
        g_nsets = 0;
        return rc;
    }

    // 码位索引：把所有套的码位去重排序，供查表钩二分。必须在成功之后才建 ——
    // 失败的清单不能留下一个半份索引（查表钩会拿它判命中）。
    for (int i = 0; i < (int)g_nsets; i++)
        for (unsigned k = 0; k < g_sets[i].count; k++) cp_index_add(g_recs[g_sets[i].first + (int)k].cp);
    cp_index_finish();

    unsigned total = 0;
    for (int i = 0; i < (int)g_nsets; i++) total += g_sets[i].count;
    core::log_line("[字形] · 清单 %s：%d 套 / 记录 %d 条 / 码位索引 %d 个", path, (int)g_nsets, (int)total,
                   (int)g_nAllCp);
    for (int i = 0; i < (int)g_nsets; i++)
        core::log_line("[字形]   套#%d 记录=%u 原字形=%s%u UV跨度=(%.7f, %.7f)", i, g_sets[i].count,
                       g_sets[i].fp_glyph_count ? "=" : "≠不限 ", g_sets[i].fp_glyph_count,
                       (double)g_sets[i].fp_u_span, (double)g_sets[i].fp_v_span);
    return (int)g_nsets;
}

int set_count() { return (int)g_nsets; }

} // namespace glyph
} // namespace ac1
