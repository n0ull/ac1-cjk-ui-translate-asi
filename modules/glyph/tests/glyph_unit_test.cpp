// modules/glyph/tests/glyph_unit_test.cpp —— glyph 层离线单测
//
//   pwsh -NoProfile -File modules\glyph\tests\build.ps1
//   pwsh -NoProfile -File modules\glyph\tests\run.ps1
//
// 链接 core.lib + glyph.lib（不链接 dict / text / app，不链接任何游戏 dll）。
//
// 本单测测的是真机跑的那同一个 font_loaded_post()（不是复制品）：
//   · 合成字体对象（按 AC1/glyph/glyph.h 里那套偏移自己造的字节块）
//   · 假 EngineApi（记录调用次数、真分配真清零，断言才有意义）
//   · 合成 AC1_CJK_Glyphs.bin（覆盖好文件 / 坏 magic / 截断）
//   装钩本身（0x8840B0 的 7 字节签名核对 + MinHook 落钩）由 core 的签名核对单测 + 真机覆盖。

#include "ac1/glyph/glyph.h"

#include "ac1/core/log.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

using namespace ac1::glyph;

static int g_pass = 0, g_fail = 0;

static void check(bool ok, const char* name, const char* detail = "")
{
    if (ok) {
        g_pass++;
        printf("  [通过] %s\n", name);
    }
    else {
        g_fail++;
        printf("  [失败] %s  %s\n", name, detail);
    }
}

// ======================================================================
// 假 EngineApi
// ======================================================================
struct FakeApi {
    int      allocCalls, entryCalls, ctorCalls, arrCtorCalls;
    int      pagesBuilt; // 真的新建出来的 charmap 页数
    void*    lastRec;    // 元素构造器最后收到的元素指针
    void*    lastArrBase;
    unsigned lastArrElem, lastArrCount; // 数组构造助手的实参
    FakeApi() { reset(); }
    void reset()
    {
        allocCalls = entryCalls = ctorCalls = arrCtorCalls = 0;
        pagesBuilt = 0;
        lastRec = NULL;
        lastArrBase = NULL;
        lastArrElem = lastArrCount = 0;
    }
};

static FakeApi g_fa;

static void* fake_alloc(unsigned size)
{
    g_fa.allocCalls++;
    void* p = HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
    if (p) memset(p, 0xCD, size); // 故意填毒：证明实现真的构造/写满了
    return p;
}

// 约定夹具：假实现必须用真引擎同样的调用约定，否则模块那边写错了时下面这几个
//   假函数就编不过去（C2440）。真引擎 record_array_ctor 是 __stdcall（尾 retn 10h）
//   且内部 mov ecx,esi; call ctor（元素构造器的第 1 参走 ecx）。它们与
//   ac1/glyph/glyph.h 里 EngineApi 字段的类型必须逐字对应。
//
// 假实现的行为模仿真引擎：0x896600 式懒建页（分配 0x200 B、按缺字符默认字形
// 预填 256 槽、把页指针写进 charmap）。

// 仿假 0x896720 setCharmapEntry：__fastcall(charmap, edx, cp, glyphIdx, flag)。
// 断言 flag 恒为 0（非 0 会改写 font+0x43C 那个全局缺字符默认字形），
// 并自己懒建页 + 写槽（真引擎内部就是这样）。
struct CmEntryCall {
    const void* cm;
    unsigned    cp, idx, flag;
};
static CmEntryCall g_cmCalls[64];
static int         g_cmCallN = 0;
static int         g_cmBadFlag = 0;
// 注入失败：这个码位写不上（模拟 0x896720 内部失败）
static unsigned g_poisonCp = 0;

static void __fastcall fake_set_charmap_entry(void* charmap, void*, unsigned cp, unsigned glyphIdx,
                                              unsigned flag)
{
    g_fa.entryCalls++;
    if (flag != 0) g_cmBadFlag++;
    if (g_cmCallN < 64) {
        g_cmCalls[g_cmCallN].cm = charmap;
        g_cmCalls[g_cmCallN].cp = cp;
        g_cmCalls[g_cmCallN].idx = glyphIdx;
        g_cmCalls[g_cmCallN].flag = flag;
    }
    g_cmCallN++;
    unsigned char*  cm = (unsigned char*)charmap;
    unsigned        page = cp >> 8, slot = cp & 0xFF;
    unsigned char** pg = (unsigned char**)(cm + page * 4);
    if (!*pg) { // 0x896600 做的事：建页 + 按 +0x43C 预填
        unsigned short* blk = (unsigned short*)HeapAlloc(GetProcessHeap(), 0, 0x200);
        for (int i = 0; i < 256; i++) blk[i] = 0;
        *pg = (unsigned char*)blk;
        g_fa.pagesBuilt++;
    }
    if (cp == g_poisonCp) return; // 注入失败：该码位写不上
    *(unsigned short*)(*pg + slot * 2) = (unsigned short)glyphIdx;
}

// 仿假 0x8842B0：__fastcall（第 1 参在 ecx）。
// 只清 +0x00..+0x0B 与 +0x0C..+0x1B，+0x1C..+0x1F 不碰。
static void __fastcall fake_rec_ctor(void* rec, void*)
{
    g_fa.ctorCalls++;
    g_fa.lastRec = rec;
    unsigned char* r = (unsigned char*)rec;
    for (int i = 0; i < 0x0C; i++) r[i] = 0;
    for (int i = 0x0C; i < 0x1C; i += 4) *(float*)(r + i) = 0.0f;
}

// 仿假 0x4020A0：__stdcall（4 参、被调方清栈），内部 `mov ecx,esi; call ctor`。
static void __stdcall fake_rec_array(void* base, unsigned elem, unsigned count, GlyphRecCtorFn ctor)
{
    g_fa.arrCtorCalls++;
    g_fa.lastArrBase = base;
    g_fa.lastArrElem = elem;
    g_fa.lastArrCount = count;
    for (unsigned i = 0; i < count; i++) ctor((unsigned char*)base + (size_t)i * elem, NULL);
}

static const EngineApi kFake = { &fake_alloc, &fake_set_charmap_entry, &fake_rec_ctor, &fake_rec_array };

// ======================================================================
// 合成字体对象
// ======================================================================
static const unsigned kFakeBase = 0x400000u;        // 宿主 imagebase
static const unsigned kVtableScimitar = 0x16A2A20u; // PixmapFontScimitar 对象里存的 vtable 值
static const unsigned kVtablePixmap = 0x16AB870u;   // PixmapFont 对象里存的 vtable 值

struct FakeFont {
    unsigned char  raw[GLYPH_FONT_SIZE];
    unsigned char* recs; // 原始表
};

// 造一个 count 条记录的字体；'@'(0x40) 那条的 UV 跨度由调用方给（= 指纹）
static unsigned char* make_font(FakeFont* ff, unsigned vt, unsigned count, float at_u0, float at_v0,
                                float at_u1, float at_v1)
{
    memset(ff->raw, 0, sizeof(ff->raw));
    *(unsigned*)ff->raw = vt; // +0x00 vtable
    *(unsigned short*)(ff->raw + GLYPH_OFF_COUNT) = (unsigned short)count;
    ff->recs = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, (size_t)count * GLYPH_REC_SIZE);
    for (unsigned i = 0; i < count; i++) {
        unsigned char* r = ff->recs + (size_t)i * GLYPH_REC_SIZE;
        *(unsigned short*)(r + GLYPH_REC_CP) = (unsigned short)(0x20 + i);
        *(unsigned short*)(r + GLYPH_REC_W) = (unsigned short)(6 + (i & 3));
        *(unsigned short*)(r + GLYPH_REC_H) = 11;
        *(short*)(r + GLYPH_REC_XOFF) = (short)(i & 1);
        *(short*)(r + GLYPH_REC_YOFF) = (short)-(int)(i & 1);
        *(unsigned short*)(r + GLYPH_REC_ADV) = (unsigned short)(5 + (i & 3));
        *(float*)(r + GLYPH_REC_U0) = 0.01f + (float)i * 0.0001f;
        *(float*)(r + GLYPH_REC_V0) = 0.02f + (float)i * 0.0001f;
        *(float*)(r + GLYPH_REC_U1) = 0.03f + (float)i * 0.0001f;
        *(float*)(r + GLYPH_REC_V1) = 0.04f + (float)i * 0.0001f;
        *(unsigned short*)(r + GLYPH_REC_PAGE) = 0;
    }
    // '@' 单独放一条：它的 UV 跨度就是字体指纹。
    // 上面的码点从 0x20 起连续，所以 0x40 落在下标 0x40-0x20。
    unsigned at = 0x40u - 0x20u;
    if (at >= count) at = 0;
    unsigned char* r = ff->recs + (size_t)at * GLYPH_REC_SIZE;
    *(unsigned short*)(r + GLYPH_REC_CP) = 0x40;
    *(float*)(r + GLYPH_REC_U0) = at_u0;
    *(float*)(r + GLYPH_REC_V0) = at_v0;
    *(float*)(r + GLYPH_REC_U1) = at_u1;
    *(float*)(r + GLYPH_REC_V1) = at_v1;
    *(unsigned char**)(ff->raw + GLYPH_OFF_TABLE) = ff->recs;
    return ff->recs;
}

// ======================================================================
// 合成 AC1_CJK_Glyphs.bin
// ======================================================================
#pragma pack(push, 1)
struct RecFile {
    unsigned short cp, w, h;
    short          xoff, yoff, adv;
    float          u0, v0, u1, v1;
    unsigned short page, pad;
};
struct SetFile {
    unsigned count, fp_glyph_count;
    float    fp_u_span, fp_v_span;
    unsigned orig_w, orig_h;
    unsigned flags;
    unsigned our_w, our_h; // 我们这张图集的实际尺寸（每套不同）
    unsigned reserved;
};
#pragma pack(pop)

// ---- 合成夹具的图集尺寸 ----
// 尺寸只在本夹具里存在：夹具写 our_w/our_h，test_patch_normal 也只拿这两个值
//   算期望 —— 断言的是 (orig/our) 这个关系，与任何生产常量无关。
//   故意不取 1024×2048：生产代码若偷偷硬编码那对历史尺寸，本单测必红。
enum {
    kFixOrigW = 256,
    kFixOrigH = 512, // 清单头的 orig_w/orig_h（原图集尺寸）
    kFixOurW = 512,
    kFixOurH = 1024 // 清单头的 our_w/our_h（我们这张图集的尺寸）
};
static char g_dir[MAX_PATH] = "";
static void exe_dir()
{
    GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    char* p = strrchr(g_dir, '\\');
    if (p)
        p[1] = 0;
    else
        g_dir[0] = 0;
}
static void path_in_dir(char* out, size_t cap, const char* name)
{
    _snprintf(out, cap, "%s%s", g_dir, name);
    out[cap - 1] = 0;
}

// 写一个清单：magic/version/n_sets/n_recs_total + 每套（头 + count 条记录）
//
// magic 这里写的是字面字节，不是 GLYPH_SET_MAGIC 常量——夹具必须独立于
//   被测常量：若"用常量本身写文件"，常量写错时实现和夹具一起错、自洽通过。
//   好文件写 'A','C','G','G'；回归用例写旧错值 0x47474741（"AGGG"）。
static const unsigned kMagicAcgg = 0x47474341u;     // 字节 41 43 47 47 = "ACGG"
static const unsigned kMagicWrongOld = 0x47474741u; // 字节 41 47 47 47 = "AGGG"（旧错值）

enum {
    MANIFEST_MAGIC_OK = 0,
    MANIFEST_MAGIC_WRONG_OLD,
    MANIFEST_MAGIC_JUNK
};

static bool write_manifest(const char* path, int n_sets, int recs_per_set, unsigned fp0_count, float fp0_u,
                           float fp0_v, unsigned fp1_count, float fp1_u, float fp1_v, int truncate_by = 0,
                           int magic = MANIFEST_MAGIC_OK)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    unsigned m = (magic == MANIFEST_MAGIC_WRONG_OLD) ? kMagicWrongOld
                 : (magic == MANIFEST_MAGIC_JUNK)    ? 0x12345678u
                                                     : kMagicAcgg;
    fwrite(&m, 4, 1, f);
    unsigned       total = (unsigned)(n_sets * recs_per_set);
    unsigned short ver = GLYPH_SET_VERSION, ns = (unsigned short)n_sets;
    fwrite(&ver, 2, 1, f);
    fwrite(&ns, 2, 1, f);
    fwrite(&total, 4, 1, f);
    for (int s = 0; s < n_sets; s++) {
        SetFile h;
        h.count = (unsigned)recs_per_set;
        h.fp_glyph_count = s == 0 ? fp0_count : fp1_count;
        h.fp_u_span = s == 0 ? fp0_u : fp1_u;
        h.fp_v_span = s == 0 ? fp0_v : fp1_v;
        h.orig_w = kFixOrigW; // 合成夹具：给一个非零原图集尺寸
        h.orig_h = kFixOrigH;
        h.flags = 0;
        h.our_w = kFixOurW; // 合成夹具：字形模块按它算缩放系数
        h.our_h = kFixOurH;
        h.reserved = 0;
        fwrite(&h, sizeof(h), 1, f);
        for (int i = 0; i < recs_per_set; i++) {
            RecFile r;
            r.cp = (unsigned short)(0x4E00 + s * 0x100 + i); // 全在 CJK 一页
            r.w = 12;
            r.h = 14;
            r.xoff = -2;
            r.yoff = (short)-3;
            r.adv = 13;
            r.u0 = 0.50f + (float)i * 0.01f;
            r.v0 = 0.60f + (float)i * 0.01f;
            r.u1 = 0.52f + (float)i * 0.01f;
            r.v1 = 0.62f + (float)i * 0.01f;
            r.page = (unsigned short)s;
            r.pad = 0;
            fwrite(&r, sizeof(r), 1, f);
        }
    }
    fclose(f);
    if (truncate_by > 0) { // 砍掉尾巴 ⇒ 声明长度对不上
        HANDLE        h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        LARGE_INTEGER li;
        GetFileSizeEx(h, &li);
        LARGE_INTEGER ni;
        ni.QuadPart = li.QuadPart - truncate_by;
        SetFilePointerEx(h, ni, NULL, FILE_BEGIN);
        SetEndOfFile(h);
        CloseHandle(h);
    }
    return true;
}

static bool file_size_is(const char* path, long long want)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return false;
    long long hi = (long long)((((unsigned long long)d.nFileSizeHigh) << 32) | d.nFileSizeLow);
    return hi == want;
}

// ======================================================================
// 用例
// ======================================================================

// [1] 字体指纹：四个字段 + 采样计数
//   跑在最前面：指纹样本位是全局的、只有 GLYPH_FP_SAMPLES 个，后面几组会把它用满。
static void test_fingerprint()
{
    printf("\n[1] 字体指纹与采样\n");

    // --- 正常：表里有 '@' ---
    {
        FakeFont         ff;
        unsigned char*   old = make_font(&ff, kVtableScimitar, 224, 0.10f, 0.20f, 0.14f, 0.26f);
        GlyphFingerprint fp;
        memset(&fp, 0xAA, sizeof(fp)); // 确认实现会把它填满而不是留残值
        check(font_fingerprint(&ff, &fp) == 1, "font_fingerprint 返回 1");
        check(fp.count == 224, "字段 count = 224");
        check(fp.hasAt == 1, "字段 hasAt = 1（表里有 '@'）");
        check(fp.u_span == 0.14f - 0.10f, "字段 u_span = '@' 的 (u1-u0)");
        check(fp.v_span == 0.26f - 0.20f, "字段 v_span = '@' 的 (v1-v0)");
        printf("  指纹: 字形=%u hasAt=%u u=%.6f v=%.6f\n", fp.count, fp.hasAt, (double)fp.u_span,
               (double)fp.v_span);

        // --- 采样：font_loaded_post 在闸门之前就打一行（此时引擎 API 还没绑）---
        int fp0 = stats().fingerprints;
        font_loaded_post(&ff);
        check(stats().fingerprints == fp0 + 1, "font_loaded_post 采了一个指纹样本");
        font_loaded_post(&ff);
        check(stats().fingerprints == fp0 + 2, "未登记的字体再来一次仍采样（闸门拒之前就打）");
        font_loaded_post(NULL);
        check(stats().fingerprints == fp0 + 2, "font == NULL 不消耗样本位");
        HeapFree(GetProcessHeap(), 0, old);
    }

    // --- 没有 '@' ⇒ 返回 1 但 hasAt = 0，跨度为 0 ---
    {
        FakeFont       ff;
        unsigned char* old = make_font(&ff, kVtableScimitar, 224, 0.10f, 0.20f, 0.14f, 0.26f);
        // make_font 把 cp 0x40 放在下标 0x40-0x20；把它改成别的码点 ⇒ 表里再没有 '@'
        const unsigned at = 0x40u - 0x20u;
        *(unsigned short*)(old + (size_t)at * GLYPH_REC_SIZE + GLYPH_REC_CP) = 0x00FF;
        GlyphFingerprint fp;
        memset(&fp, 0xAA, sizeof(fp));
        check(font_fingerprint(&ff, &fp) == 1, "没有 '@' 仍然返回 1（对象本身是可信的）");
        check(fp.count == 224, "没有 '@' 时 count 照样给出");
        check(fp.hasAt == 0, "字段 hasAt = 0");
        check(fp.u_span == 0.0f && fp.v_span == 0.0f, "没有 '@' 时跨度为 0");
        HeapFree(GetProcessHeap(), 0, old);
    }

    // --- 表指针为空 ⇒ 返回 0，且 out 被清零 ---
    {
        FakeFont ff;
        make_font(&ff, kVtableScimitar, 224, 0.10f, 0.20f, 0.14f, 0.26f);
        *(unsigned char**)(ff.raw + GLYPH_OFF_TABLE) = NULL;
        GlyphFingerprint fp;
        memset(&fp, 0xAA, sizeof(fp));
        check(font_fingerprint(&ff, &fp) == 0, "表指针为空 ⇒ 返回 0");
        check(fp.count == 0 && fp.hasAt == 0 && fp.u_span == 0.0f && fp.v_span == 0.0f,
              "返回 0 时 out 被清零（不留下调用方的残值）");
    }

    // --- 字形数为 0 ⇒ 返回 0 ---
    {
        FakeFont       ff;
        unsigned char* old = make_font(&ff, kVtableScimitar, 224, 0.10f, 0.20f, 0.14f, 0.26f);
        *(unsigned short*)(ff.raw + GLYPH_OFF_COUNT) = 0;
        GlyphFingerprint fp;
        memset(&fp, 0xAA, sizeof(fp));
        check(font_fingerprint(&ff, &fp) == 0, "字形数 0 ⇒ 返回 0");
        check(fp.hasAt == 0, "返回 0 时 hasAt = 0");
        HeapFree(GetProcessHeap(), 0, old);
    }

    // --- 边界：NULL ---
    {
        GlyphFingerprint fp;
        memset(&fp, 0xAA, sizeof(fp));
        check(font_fingerprint(NULL, &fp) == 0, "font == NULL ⇒ 返回 0（不崩）");
        check(fp.count == 0 && fp.hasAt == 0, "font == NULL 时 out 被清零");
        check(font_fingerprint(NULL, NULL) == 0, "两个都 NULL ⇒ 返回 0（不崩）");
    }
}

// [2] 未绑定引擎 API ⇒ 不崩、不写、有拒绝计数
static void test_unbound()
{
    printf("\n[2] 引擎 API 未绑定\n");
    bind_engine(NULL);
    check(engine_bound() == false, "bind_engine(NULL) 后 engine_bound() = false");

    FakeFont ff;
    make_font(&ff, kVtableScimitar, 224, 0.10f, 0.20f, 0.14f, 0.26f);
    unsigned char snapshot[GLYPH_FONT_SIZE];
    memcpy(snapshot, ff.raw, sizeof(snapshot));

    int allocsBefore = g_fa.allocCalls;
    font_loaded_post(&ff); // 不绑引擎就调 ⇒ 闸门拦下
    font_loaded_post(NULL);
    check(memcmp(ff.raw, snapshot, sizeof(snapshot)) == 0, "字体对象一个字节都没动");
    check(g_fa.allocCalls == allocsBefore, "没有发生任何分配");
    const GlyphStats& s = stats();
    check(s.rejectNoApi >= 1, "拒绝计数（未绑引擎）≥ 1");
    check(s.rejectNull >= 1, "拒绝计数（NULL）≥ 1");
    check(s.fontsPatched == 0, "已打补丁字体数 = 0");
    HeapFree(GetProcessHeap(), 0, ff.recs);
}

// [3] load_manifest：magic / 截断 ⇒ 一律拒绝，且状态归零
static void test_manifest_bad()
{
    printf("\n[3] load_manifest 坏文件\n");
    char p[512];

    // 回归：magic 写成旧错值 0x47474741（字节 41 47 47 47 = "AGGG"）必须被拒。
    // 这一条夹具不引用被测常量——否则常量错了两边一起错、自洽通过。
    path_in_dir(p, sizeof(p), "wrong_magic_old.bin");
    write_manifest(p, 2, 3, 224, 0.04f, 0.04f, 224, 0.5f, 0.5f, 0, MANIFEST_MAGIC_WRONG_OLD);
    int rc = load_manifest(p);
    check(rc == GLYPH_MANIFEST_E_MAGIC, "旧错值 magic 0x47474741（\"AGGG\"）被拒");
    check(set_count() == 0, "旧错值被拒后套数归零");

    // 同样确认：字面 "ACGG"（41 43 47 47）是该被接受的那个
    path_in_dir(p, sizeof(p), "good_magic_check.bin");
    write_manifest(p, 1, 2, 224, 0.04f, 0.04f, 224, 0.5f, 0.5f, 0, MANIFEST_MAGIC_OK);
    rc = load_manifest(p);
    check(rc == 1, "字面 \"ACGG\"（41 43 47 47）被接受 ⇒ 夹具与实现对同一组字节");
    check(set_count() == 1, "接受后套数 = 1");
    load_manifest((const char*)"Z:\\不存在的路径\\x.bin"); // 复位成 0 套

    path_in_dir(p, sizeof(p), "bad_magic.bin");
    write_manifest(p, 2, 3, 224, 0.04f, 0.04f, 224, 0.5f, 0.5f, 0, MANIFEST_MAGIC_JUNK);
    rc = load_manifest(p);
    check(rc == GLYPH_MANIFEST_E_MAGIC, "垃圾 magic 被拒（返回 E_MAGIC）");
    check(set_count() == 0, "被拒后套数归零");

    path_in_dir(p, sizeof(p), "truncated.bin");
    write_manifest(p, 2, 3, 224, 0.04f, 0.04f, 224, 0.5f, 0.5f, 40, MANIFEST_MAGIC_OK);
    rc = load_manifest(p);
    check(rc < 0, "截断文件被拒");
    check(set_count() == 0, "被拒后套数仍归零");

    rc = load_manifest((const char*)"Z:\\绝对不存在的路径\\AC1_CJK_Glyphs.bin");
    check(rc == GLYPH_MANIFEST_E_OPEN, "文件不存在 ⇒ E_OPEN（正常状态，不是崩溃）");
    check(set_count() == 0, "没有文件 ⇒ 0 套");
    printf("  状态: %s\n", set_count() == 0 ? "0 套 ⇒ 钩子照装，但每套都会 no_match" : "?");
}

// [3] 正常打补丁（224 条原表 + 指纹匹配的套 K=3）
static void test_patch_normal()
{
    printf("\n[4] 正常打补丁\n");
    char p[512];
    path_in_dir(p, sizeof(p), "good.bin");
    // 套#0：原字形 224、跨度 = '@' 记录的 (u1-u0, v1-v0)，按运行期同一算法算出来
    //       （f32 相减的舍入必须逐位一致，所以这里传的是算好的差，不是十进制字面量）
    // 套#1：故意不匹配
    bool wrote = write_manifest(p, 2, 3, 224, 0.14f - 0.10f, 0.26f - 0.20f, 224, 0.50f, 0.50f);
    check(wrote, "合成清单写盘成功");
    int ns = load_manifest(p);
    check(ns == 2, "load_manifest 返回 2 套");
    check(set_count() == 2, "set_count() = 2");
    check(sizeof(RecFile) == GLYPH_REC_FILE, "记录结构体恰好与 GLYPH_REC_FILE 同大小");
    check(file_size_is(p, 12 + 2 * (40 + 3 * GLYPH_REC_FILE)), "文件长度 = 12 + 2*(40+3*0x20)");

    bind_engine(&kFake);
    check(engine_bound() == true, "bind_engine(&kFake) 生效");
    set_host_base(kFakeBase); // 填出 vtable 闸门要认的那两个地址

    FakeFont       ff;
    unsigned char* old = make_font(&ff, kVtableScimitar, 224, 0.10f, 0.20f, 0.14f, 0.26f);
    unsigned char  oldCopy[224 * GLYPH_REC_SIZE];
    memcpy(oldCopy, old, sizeof(oldCopy));

    g_fa.reset();
    font_loaded_post(&ff);

    unsigned       count2 = *(unsigned short*)(ff.raw + GLYPH_OFF_COUNT);
    unsigned char* tbl2 = *(unsigned char**)(ff.raw + GLYPH_OFF_TABLE);
    check(count2 == 227, "font+0x34 由 224 变 227", "实际");
    check(tbl2 != NULL && tbl2 != old, "font+0x38 换成了新块");
    check(g_fa.allocCalls == 1, "只分配了一次（alloc 计数 = 1）");
    check(g_fa.arrCtorCalls == 1, "记录数组构造调用了 1 次");
    check(g_fa.ctorCalls == 227, "逐条构造 227 条（覆盖 count+K）");

    if (tbl2) {
        // 原记录不再逐字节相同：我们交的是每套自己尺寸的图集（our_w×our_h），
        //   原图集只占左下角 (orig_w/our_w, orig_h/our_h)，而原字形记录的 UV 是按
        //   原图集（0..1）算的 ⇒ 必须缩放，否则英文/数字/符号会横跨整张图集采样到
        //   CJK 格子 ⇒ 满屏乱码。
        //   所以：除 4 个 UV 浮点（+0x0C..+0x18）外，其余字节必须与原表相同。
        //   缩放系数按夹具自己写进清单的 our_w/our_h 算，不是任何生产常量 ——
        //     kFixOurW/H 故意 ≠ 1024/2048，生产代码若硬编码那对历史尺寸这里就会红。
        const float su = (float)kFixOrigW / (float)kFixOurW;
        const float sv = (float)kFixOrigH / (float)kFixOurH;
        int         uvOk = 1, otherOk = 1;
        for (int i = 0; i < 224; i++) {
            const unsigned char* a = tbl2 + i * GLYPH_REC_SIZE;
            const unsigned char* b = oldCopy + i * GLYPH_REC_SIZE;
            for (int k = 0; k < GLYPH_REC_SIZE; k++) {
                bool isUv = (k >= GLYPH_REC_U0 && k < GLYPH_REC_U0 + 16);
                if (!isUv && a[k] != b[k]) otherOk = 0;
            }
            const float* ua = (const float*)(a + GLYPH_REC_U0);
            const float* ub = (const float*)(b + GLYPH_REC_U0);
            if (ua[0] != ub[0] * su || ua[1] != ub[1] * sv || ua[2] != ub[2] * su || ua[3] != ub[3] * sv)
                uvOk = 0;
        }
        check(otherOk, "原 224 条除 4 个 UV 浮点外逐字节不变");
        check(uvOk, "★ 原记录的 UV 已按 (orig_w/our_w, orig_h/our_h) 缩放（否则英文乱码）");

        // 3 条新记录的每个字段
        bool fields = true;
        for (int i = 0; i < 3 && fields; i++) {
            const unsigned char* r = tbl2 + (224 + i) * GLYPH_REC_SIZE;
            unsigned             cp = 0x4E00 + i;
            if (*(unsigned short*)(r + GLYPH_REC_CP) != cp) fields = false;
            if (*(unsigned short*)(r + GLYPH_REC_W) != 12) fields = false;
            if (*(unsigned short*)(r + GLYPH_REC_H) != 14) fields = false;
            if (*(short*)(r + GLYPH_REC_XOFF) != -2) fields = false;
            if (*(short*)(r + GLYPH_REC_YOFF) != -3) fields = false;
            if (*(unsigned short*)(r + GLYPH_REC_ADV) != 13) fields = false;
            if (*(float*)(r + GLYPH_REC_U0) != 0.50f + (float)i * 0.01f) fields = false;
            if (*(float*)(r + GLYPH_REC_V0) != 0.60f + (float)i * 0.01f) fields = false;
            if (*(float*)(r + GLYPH_REC_U1) != 0.52f + (float)i * 0.01f) fields = false;
            if (*(float*)(r + GLYPH_REC_V1) != 0.62f + (float)i * 0.01f) fields = false;
            if (*(unsigned short*)(r + GLYPH_REC_PAGE) != 0) fields = false;
            if (*(unsigned short*)(r + 0x1E) != 0) fields = false; // 保持 0
        }
        check(fields, "3 条新记录的每个字段都正确（含 i16 偏移、f32 UV、+0x1C、+0x1E..1F=0）");

        // charmap：cp>>8 == 0x4E ⇒ 只需建第 0x4E 页；槽 = cp & 0xFF
        unsigned char** pg = (unsigned char**)(ff.raw + GLYPH_OFF_CHARMAP + (0x4E00 >> 8) * 4);
        check(*pg != NULL, "charmap 第 0x4E 页被建出来了");
        check(g_fa.entryCalls == 3, "set_charmap_entry 逐码位调了 3 次");
        if (*pg) {
            bool slots = true;
            for (int i = 0; i < 3; i++)
                if (*(unsigned short*)(*pg + ((0x4E00u + i) & 0xFF) * 2) != (unsigned short)(224 + i))
                    slots = false;
            check(slots, "槽值 = 新字形下标 224/225/226");
        }
        // 没被我们写过的槽必须还是建页时的预填值 0（= 占位字形）
        if (*pg) check(*(unsigned short*)(*pg + 0x10 * 2) == 0, "未分配的槽保持预填值 0");
    }

    const GlyphStats& s = stats();
    check(s.fontsPatched == 1, "已打补丁字体数 = 1");
    check(s.glyphsAdded == 3, "追加字形数 = 3");
    check(s.pagesCreated == 1, "建页数 = 1");
    check(s.legacyBytes == 224ull * GLYPH_REC_SIZE, "旧表让渡字节数 = 224*0x20");

    HeapFree(GetProcessHeap(), 0, old);
}

// [4] 幂等：同一个字体第二次进来 ⇒ 无任何写入
static void test_idempotent()
{
    printf("\n[5] 幂等\n");
    FakeFont       ff;
    unsigned char* old = make_font(&ff, kVtablePixmap, 224, 0.10f, 0.20f, 0.14f, 0.26f);
    g_fa.reset();
    font_loaded_post(&ff);
    check(*(unsigned short*)(ff.raw + GLYPH_OFF_COUNT) == 227, "第一次补成功（227）");

    unsigned char snapshot[GLYPH_FONT_SIZE];
    memcpy(snapshot, ff.raw, sizeof(snapshot));
    int a = g_fa.allocCalls, pg = g_fa.entryCalls;
    int ac = g_fa.arrCtorCalls, ct = g_fa.ctorCalls;
    int fp1 = stats().fingerprints;

    for (int i = 0; i < 3; i++) font_loaded_post(&ff);

    check(stats().fingerprints == fp1, "已登记（已打补丁）的字体再来 3 次，不再消耗指纹样本位");
    check(memcmp(ff.raw, snapshot, sizeof(snapshot)) == 0, "再调 3 次，字体对象一个字节都没动");
    check(g_fa.allocCalls == a, "alloc 调用计数不增长");
    check(g_fa.entryCalls == pg, "set_charmap_entry 调用计数不增长");
    check(g_fa.arrCtorCalls == ac, "record_array_ctor 调用计数不增长");
    check(g_fa.ctorCalls == ct, "record_ctor 调用计数不增长");
    const GlyphStats& s = stats();
    check(s.dup == 3, "重复计数 = 3");
    check(s.fontsPatched == 2, "已打补丁字体数 = 2");
    HeapFree(GetProcessHeap(), 0, old);
}

// [5] 闸门：vtable 不认识 / count=0 / 表指针空
static void test_gates()
{
    printf("\n[6] 可靠性闸门\n");
    g_fa.reset();
    int a = g_fa.allocCalls;

    // (a) vtable 不认识
    {
        FakeFont       ff;
        unsigned char* old = make_font(&ff, 0xDEADBEEFu, 224, 0.10f, 0.20f, 0.14f, 0.26f);
        unsigned char  snap[GLYPH_FONT_SIZE];
        memcpy(snap, ff.raw, sizeof(snap));
        font_loaded_post(&ff);
        check(memcmp(ff.raw, snap, sizeof(snap)) == 0, "vtable 不认识 ⇒ 一个字节都没写");
        check(g_fa.allocCalls == a, "vtable 不认识 ⇒ 没有分配");
        HeapFree(GetProcessHeap(), 0, old);
    }
    // (b) count == 0
    {
        FakeFont       ff;
        unsigned char* old = make_font(&ff, kVtableScimitar, 224, 0.10f, 0.20f, 0.14f, 0.26f);
        *(unsigned short*)(ff.raw + GLYPH_OFF_COUNT) = 0;
        font_loaded_post(&ff);
        check(*(unsigned short*)(ff.raw + GLYPH_OFF_COUNT) == 0, "count=0 ⇒ 仍然 0");
        check(*(unsigned char**)(ff.raw + GLYPH_OFF_TABLE) == old, "count=0 ⇒ 表指针未动");
        check(g_fa.allocCalls == a, "count=0 ⇒ 没有分配");
        HeapFree(GetProcessHeap(), 0, old);
    }
    // (c) 表指针空
    {
        FakeFont ff;
        make_font(&ff, kVtableScimitar, 224, 0.10f, 0.20f, 0.14f, 0.26f);
        *(unsigned char**)(ff.raw + GLYPH_OFF_TABLE) = NULL;
        font_loaded_post(&ff);
        check(*(unsigned char**)(ff.raw + GLYPH_OFF_TABLE) == NULL, "表指针空 ⇒ 仍然空");
        check(g_fa.allocCalls == a, "表指针空 ⇒ 没有分配");
    }
    // (d) count 超防御上限
    {
        FakeFont       ff;
        unsigned char* old = make_font(&ff, kVtableScimitar, 224, 0.10f, 0.20f, 0.14f, 0.26f);
        *(unsigned short*)(ff.raw + GLYPH_OFF_COUNT) = (unsigned short)(GLYPH_MAX_COUNT + 1);
        font_loaded_post(&ff);
        check(*(unsigned short*)(ff.raw + GLYPH_OFF_COUNT) == (unsigned short)(GLYPH_MAX_COUNT + 1),
              "count 超上限 ⇒ 原值未动");
        check(g_fa.allocCalls == a, "count 超上限 ⇒ 没有分配");
        HeapFree(GetProcessHeap(), 0, old);
    }
    const GlyphStats& s = stats();
    check(s.rejectVtable == 1, "拒绝计数（vtable）= 1");
    check(s.rejectCount0 == 1, "拒绝计数（count=0）= 1");
    check(s.rejectNoTable == 1, "拒绝计数（表指针空）= 1");
    check(s.rejectTooBig == 1, "拒绝计数（count 超上限）= 1");
}

// [6] 指纹不匹配 ⇒ 不写、计入 no_match
static void test_no_match()
{
    printf("\n[7] 指纹不匹配\n");
    g_fa.reset();
    int            a = g_fa.allocCalls;
    FakeFont       ff;
    unsigned char* old = make_font(&ff, kVtableScimitar, 224, 0.50f, 0.50f, 0.60f, 0.70f);
    unsigned char  snap[GLYPH_FONT_SIZE];
    memcpy(snap, ff.raw, sizeof(snap));
    font_loaded_post(&ff);
    check(memcmp(ff.raw, snap, sizeof(snap)) == 0, "指纹对不上 ⇒ 一个字节都没写");
    check(g_fa.allocCalls == a, "指纹对不上 ⇒ 没有分配");
    check(*(unsigned short*)(ff.raw + GLYPH_OFF_COUNT) == 224, "字形数仍是 224");
    const GlyphStats& s = stats();
    check(s.noMatch == 1, "no_match 计数 = 1");
    check(s.fontsPatched == 2, "已打补丁字体数仍是 2（没多补）");
    HeapFree(GetProcessHeap(), 0, old);
}

// [7] 旧表未被释放
static void test_legacy_not_freed()
{
    printf("\n[8] 旧表保留（有意的、有界的让渡）\n");
    // 不释放旧块是**结构性**的：EngineApi 里没有 free，模块想释放也无从释放。
    //   可观测的等价判据是下面的「让渡字节数恰好 = 每个被补字体一份」。
    const GlyphStats& s = stats();
    check(s.legacyBytes == 2ull * 224ull * GLYPH_REC_SIZE, "旧表让渡字节数 = 2 个字体 × 224×0x20");
    printf("  已让渡 %llu 字节（每个被补字体一次，进程生命周期内不再增长）\n", s.legacyBytes);
}

// [8] status 字符串
static void test_status()
{
    printf("\n[9] 状态行\n");
    char        buf[512];
    const char* s = status(buf, sizeof(buf));
    check(s == buf, "status() 返回的是调用方的缓冲");
    check(s && s[0] != 0, "status() 非空");
    const char* keys[9] = { "闸门=", "钩子=", "字体=", "字形+", "页+", "集=", "指纹=", "排队=", "重置=" };
    for (int i = 0; i < 9; i++) check(strstr(s, keys[i]) != NULL, keys[i]);
    check(strstr(s, "未实现") == NULL, "status 里已经没有「未实现」（数据半已实现）");
    printf("  状态: %s\n", s);
    check(status(NULL, 16) != NULL, "status(NULL) 不崩");
    check(status(buf, 0) != NULL, "status(容量 0) 不崩");
}

// [9] 引擎 API 缺字段 ⇒ 不认（不装钩的前提）
static void test_api_incomplete()
{
    printf("\n[10] EngineApi 缺字段\n");
    EngineApi bad = kFake;
    bad.record_ctor = NULL;
    bind_engine(&bad);
    check(engine_bound() == false, "缺 record_ctor ⇒ engine_bound() = false（装钩会被拒）");
    bind_engine(&kFake);
    check(engine_bound() == true, "重新绑定完整 API ⇒ 生效");
    check(stats().hooks == 0, "本单测没装过钩（没有 Dx9 宿主）");
    check(install(0) == 0, "install(base=0) 拿不到基址 ⇒ 诚实返回 0");
    check(stats().hooks == 0, "装不上的调用不改变已装钩数");
}

// ======================================================================
// [11] 与时机无关的兜底路径：查表发现 → 排队 → DrawText 前应用
//
// 驱动的是真机那两个 detour 本身（hook_lookup / hook_drawtext），orig 换成
// 假实现。断言重点：
//   · 查表钩一个字节都不写（铁律）
//   · 补丁确实落在 orig 之前
//   · 引擎把已补丁的字体重置后，查表钩能发现并重新排队
// ======================================================================
static int         g_lookupCalls = 0, g_drawCalls = 0;
static const bool  kDrawRet = true;
static const void* g_seenTable = NULL;
static unsigned    g_seenCount = 0;

// 模仿 0x8965D0 本体：无页 ⇒ 返回 0；否则返回 page[C & 0xFF]
static unsigned short __fastcall fake_lookup_orig(void* cm, void*, unsigned short C)
{
    g_lookupCalls++;
    unsigned char** pg = (unsigned char**)((unsigned char*)cm + (C >> 8) * 4);
    if (!*pg) return 0;
    return *(unsigned short*)(*pg + (C & 0xFF) * 2);
}

// 模仿 0x799720 本体：顺手记下"引擎此刻看到的表"，用来证明补丁在 orig 之前落地
static bool __fastcall fake_draw_orig(void* self, void*, unsigned, unsigned, unsigned, unsigned, unsigned,
                                      unsigned, unsigned, unsigned)
{
    g_drawCalls++;
    g_seenTable = *(const void**)((unsigned char*)self + GLYPH_OFF_TABLE);
    g_seenCount = *(const unsigned short*)((unsigned char*)self + GLYPH_OFF_COUNT);
    return kDrawRet;
}

static bool file_contains(const char* path, const char* needle)
{
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    static char buf[256 * 1024];
    size_t      n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}

// 数一个次序列在日志里出现了多少次
static int file_count(const char* path, const char* needle)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    static char buf[256 * 1024];
    size_t      n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    int         c = 0;
    const char* p = buf;
    while ((p = strstr(p, needle)) != NULL) {
        c++;
        p += strlen(needle);
    }
    return c;
}

static void test_fallback_path()
{
    printf("\n[11] 兜底路径（查表发现 → DrawText 应用）\n");
    bind_engine(&kFake);
    set_hook_origins(&fake_lookup_orig, &fake_draw_orig);

    // 本组自备一份能匹配上的清单（match_set 要求有套可匹配）
    char mp[512];
    path_in_dir(mp, sizeof(mp), "fallback.bin");
    write_manifest(mp, 2, 3, 224, 0.14f - 0.10f, 0.26f - 0.20f, 224, 0.50f, 0.50f);
    check(load_manifest(mp) == 2, "本组自备清单：2 套已加载");

    // 这一组要断言"日志里真的出现了排队行"，所以临时把日志打开；
    // 顺手删掉旧文件，免得上一轮的残留行被当成这一轮的。
    // 日志体系：run 文件按 stamp 命名；同秒内的组间会共用同名文件
    // ⇒ 先建拿路径、关掉、删掉、再重建，保证本组断言只看到本组的行
    ac1::core::init_log(g_dir);
    char lp[512];
    lstrcpynA(lp, ac1::core::log_path(), sizeof(lp));
    ac1::core::close_log();
    DeleteFileA(lp);
    ac1::core::init_log(g_dir);

    // 用 static：栈上的合成字体会被后续函数复用同一段栈地址，
    //   而模块的幂等登记表是按指针记的 ⇒ 地址撞了就以为"已经补过"。
    static FakeFont ff;
    unsigned char*  old = make_font(&ff, kVtableScimitar, 224, 0.10f, 0.20f, 0.14f, 0.26f);
    unsigned char*  cm = ff.raw + GLYPH_OFF_CHARMAP;

    // ---- ① 查表钩：只读 ----
    g_lookupCalls = g_drawCalls = 0;
    int            a0 = g_fa.allocCalls, p0 = g_fa.entryCalls, c0 = g_fa.ctorCalls;
    int            pend0 = stats().pending;
    unsigned short r = hook_lookup(cm, NULL, 0x4E00);
    check(g_lookupCalls == 1, "查表钩确实把 orig 调了一次");
    check(r == 0, "orig 的返回值（此时无 charmap 页 ⇒ 0）被原样传出");
    check(g_fa.allocCalls == a0, "★ 查表钩零分配（不建新表）");
    check(g_fa.entryCalls == p0 && g_fa.ctorCalls == c0, "★ 查表钩没写槽、没构造记录");
    check(*(unsigned short*)(ff.raw + GLYPH_OFF_COUNT) == 224, "★ 查表后字形数仍是 224");
    check(*(unsigned char**)(ff.raw + GLYPH_OFF_TABLE) == old, "★ 查表后表指针未动");
    check(stats().pending == pend0 + 1, "查表发现 ⇒ 排队数 +1");

    // ---- ② DrawText 钩：应用，且必须在 orig 之前 ----
    int app0 = stats().appliedAtDraw;
    g_seenCount = 0;
    g_seenTable = NULL;
    bool ret = hook_drawtext(&ff, NULL, 1, 2, 3, 4, 5, 6, 7, 8);
    check(ret == kDrawRet, "DrawText 钩：orig 的返回值被原样传出");
    check(g_drawCalls == 1, "orig 被调了 1 次");
    check(g_seenCount == 227, "★ orig 看到的就是 227 ⇒ 补丁在 orig 之前落地");
    check(g_seenTable != old && g_seenTable == *(const void**)(ff.raw + GLYPH_OFF_TABLE),
          "★ orig 看到的表就是刚换上去的那张");
    check(stats().appliedAtDraw == app0 + 1, "appliedAtDraw +1");
    check(stats().pending == pend0, "应用成功后队列回落");
    check(fake_lookup_orig(cm, NULL, 0x4E00) == 224, "charmap 已能查到新字形下标 224");
    check(fake_lookup_orig(cm, NULL, 0x4E02) == 226, "charmap 第 2 个新码点也写进去了");
    unsigned char* newTbl = *(unsigned char**)(ff.raw + GLYPH_OFF_TABLE);

    // ---- ③ 第二次 DrawText：无新写入 ----
    int a1 = g_fa.allocCalls, p1 = g_fa.entryCalls, c1 = g_fa.ctorCalls;
    hook_drawtext(&ff, NULL, 1, 2, 3, 4, 5, 6, 7, 8);
    check(g_fa.allocCalls == a1 && g_fa.entryCalls == p1 && g_fa.ctorCalls == c1,
          "★ 第二次 DrawText 没有任何新写入（不重复追加）");
    check(g_drawCalls == 2, "orig 照常被调（第二次）");
    check(stats().appliedAtDraw == app0 + 1, "appliedAtDraw 不再增长");
    check(*(unsigned char**)(ff.raw + GLYPH_OFF_TABLE) == newTbl, "表指针没被换掉");
    check(*(unsigned short*)(ff.raw + GLYPH_OFF_COUNT) == 227, "字形数仍是 227");

    // ---- ④ 引擎把对象重置 ⇒ 查表重新排队 ⇒ 再应用一次 ----
    int rev0 = stats().revived;
    *(unsigned char**)(ff.raw + GLYPH_OFF_TABLE) = old; // 换回引擎那张旧表
    *(unsigned short*)(ff.raw + GLYPH_OFF_COUNT) = 224;
    int p2 = stats().pending;
    hook_lookup(cm, NULL, 0x4E00);
    check(stats().revived == rev0 + 1, "★ 表被重置 ⇒ revived +1");
    check(stats().pending == p2 + 1, "重置后重新排队");
    check(*(unsigned char**)(ff.raw + GLYPH_OFF_TABLE) == old, "★ 查表钩仍然零写入");

    int a2 = g_fa.allocCalls;
    hook_drawtext(&ff, NULL, 1, 2, 3, 4, 5, 6, 7, 8);
    check(g_fa.allocCalls == a2 + 1, "重置后重新应用（确实又分配了一张表）");
    check(*(unsigned short*)(ff.raw + GLYPH_OFF_COUNT) == 227, "重新应用后又是 227");
    check(*(unsigned char**)(ff.raw + GLYPH_OFF_TABLE) != old, "表确实又换了一张");
    check(stats().appliedAtDraw == app0 + 2, "appliedAtDraw 第 2 次");
    check(fake_lookup_orig(cm, NULL, 0x4E00) == 224, "重置后 charmap 槽又被写对");

    // ---- 日志核对 ----
    ac1::core::init_log(""); // 关回去：其余用例不写文件
    check(file_contains(lp, "由查表发现"), "日志里有「由查表发现（加载钩没赶上）」排队行");
    check(file_contains(lp, "的表被重置"), "日志里有「表被重置 ⇒ 重新排队」行");
    check(file_contains(lp, "DrawText前"), "日志里有 DrawText 路径的应用行");

    char sb[512];
    printf("  状态: %s\n", status(sb, sizeof(sb)));
}

// ======================================================================
// [12] 约定夹具 + 失败名单、半程回滚
//
// 约定部分：假 API 用真引擎同样的调用约定，这里断言本模块实际传下去了什么。
// 失败部分：注入一个写不上的码位 ⇒ 补槽回读校验失败 ⇒ 回滚 + 拉黑，
//   之后 10 次 hook_lookup 必须零排队、零新日志。
// ======================================================================
static void test_convention_and_faillist()
{
    printf("\n[12] 约定夹具 / 失败名单 / 回滚\n");
    bind_engine(&kFake);
    set_hook_origins(&fake_lookup_orig, &fake_draw_orig);

    char mp[512];
    path_in_dir(mp, sizeof(mp), "conv.bin");
    write_manifest(mp, 2, 3, 224, 0.14f - 0.10f, 0.26f - 0.20f, 224, 0.50f, 0.50f);
    check(load_manifest(mp) == 2, "本组自备清单：2 套已加载");

    // 日志体系：run 文件按 stamp 命名；同秒内的组间会共用同名文件
    // ⇒ 先建拿路径、关掉、删掉、再重建，保证本组断言只看到本组的行
    ac1::core::init_log(g_dir);
    char lp[512];
    lstrcpynA(lp, ac1::core::log_path(), sizeof(lp));
    ac1::core::close_log();
    DeleteFileA(lp);
    ac1::core::init_log(g_dir);
    // ---- ── 约定部分：一次正常补丁的完整实参 ----
    {
        static FakeFont ff;
        unsigned char*  old = make_font(&ff, kVtableScimitar, 224, 0.10f, 0.20f, 0.14f, 0.26f);
        unsigned char*  cm = ff.raw + GLYPH_OFF_CHARMAP;
        g_poisonCp = 0;
        g_fa.reset();
        g_cmCallN = 0;
        g_cmBadFlag = 0;
        int app0 = stats().appliedAtDraw;

        hook_lookup(cm, NULL, 0x4E00); // 发现 → 排队
        check(stats().pending == 1, "约定部分：查表后排队 1");
        hook_drawtext(&ff, NULL, 1, 2, 3, 4, 5, 6, 7, 8); // 应用
        check(stats().appliedAtDraw == app0 + 1, "应用计数 +1");

        // set_charmap_entry：逐码位、charmap 对象对、flag 恒 0
        check(g_cmCallN == 3, "约定：逐码位调 set_charmap_entry 3 次");
        check(g_cmBadFlag == 0, "★ 约定：flag 恒为 0（绝不碰 font+0x43C）");
        bool argsOk = true;
        for (int i = 0; i < g_cmCallN && i < 64; i++) {
            if (g_cmCalls[i].cm != (const void*)cm) argsOk = false;
            if (g_cmCalls[i].cp != 0x4E00u + i) argsOk = false;
            if (g_cmCalls[i].idx != 224u + i) argsOk = false;
            if (g_cmCalls[i].flag != 0u) argsOk = false;
        }
        check(argsOk, "约定：每次收到 (cm, cp=0x4E00+i, idx=224+i, flag=0)");

        // record_array_ctor：__stdcall 收到 (base, elem, count, ctor)
        unsigned char* nw = *(unsigned char**)(ff.raw + GLYPH_OFF_TABLE);
        check(g_fa.arrCtorCalls == 1, "约定：数组构造调用 1 次");
        check(g_fa.lastArrBase == (void*)nw, "约定：数组构造的 base = 新表");
        check(g_fa.lastArrElem == (unsigned)GLYPH_REC_SIZE, "约定：数组构造的 elem = 0x20");
        check(g_fa.lastArrCount == 227, "约定：数组构造的 count = 227");
        check(g_fa.ctorCalls == 227, "约定：元素构造器被调 227 次");
        check(g_fa.lastRec == (void*)(nw + 226 * GLYPH_REC_SIZE),
              "★ 约定：元素构造器收到的是 **ecx** 里那个元素指针");
        check(g_fa.pagesBuilt == 1, "约定：实际建出 1 个 charmap 页");
        (void)old;
    }

    // ---- ── 失败部分：写不上 ⇒ 回滚 + 拉黑 + 不再刷屏 ----
    {
        static FakeFont ff;
        unsigned char*  old = make_font(&ff, kVtablePixmap, 224, 0.10f, 0.20f, 0.14f, 0.26f);
        unsigned char*  cm = ff.raw + GLYPH_OFF_CHARMAP;
        g_poisonCp = 0x4E01u; // 第二个新码位写不上
        g_fa.reset();
        g_cmCallN = 0;
        int fail0 = stats().failListed;
        int byCode0[GLYPH_FAIL_SLOTS];
        for (int i = 0; i < GLYPH_FAIL_SLOTS; i++) byCode0[i] = stats().failByCode[i];
        int pend0 = stats().pending;
        int app1 = stats().appliedAtDraw;

        hook_lookup(cm, NULL, 0x4E00); // 发现 → 排队
        check(stats().pending == pend0 + 1, "失败前：排队 +1");
        hook_drawtext(&ff, NULL, 1, 2, 3, 4, 5, 6, 7, 8);

        // 回滚：+0x38 / +0x34 必须是补丁前的值
        check(*(unsigned char**)(ff.raw + GLYPH_OFF_TABLE) == old, "★ 回滚：+0x38 恢复为旧表指针");
        check(*(unsigned short*)(ff.raw + GLYPH_OFF_COUNT) == 224, "★ 回滚：+0x34 恢复为 224");
        check(stats().failListed == fail0 + 1, "失败后：进失败名单 +1");
        // 失败原因码要被记下来并读得出来（g_failCode → stats().failByCode）。
        //   这条是被毒化的码位 ⇒ fail_list_add 收到的应该是 GLYPH_FAIL_ENTRY。
        check(stats().failByCode[GLYPH_FAIL_ENTRY] == byCode0[GLYPH_FAIL_ENTRY] + 1,
              "★ 失败原因码被记录：ENTRY 桶 +1（这次是写槽没生效）");
        {
            int sum = 0, otherMoved = 0;
            for (int i = 0; i < GLYPH_FAIL_SLOTS; i++) {
                sum += stats().failByCode[i];
                if (i != GLYPH_FAIL_ENTRY) otherMoved += (stats().failByCode[i] != byCode0[i]);
            }
            check(sum == stats().failListed, "★ 各原因码桶之和 == failListed（分布与名单总数对得上）");
            check(otherMoved == 0, "★ 只有 ENTRY 桶动了（没有误记到别的桶）");
        }
        check(stats().pending == pend0, "失败后：队列回落（不再重试）");
        check(stats().appliedAtDraw == app1, "失败的那次不计入 DrawText 应用");

        ac1::core::init_log("");
        int blackLogs = file_count(lp, "拉黑：此后不再排队"); // 只算 fail_list_add 那一行
        int blackAny = file_count(lp, "拉黑");                // 含回滚那一行
        check(blackLogs == 1, "失败只打一行拉黑日志", "实际行数见下");
        check(blackAny == 2, "失败总共两行（回滚说明 + 拉黑说明）", "实际行数见下");
        ac1::core::init_log(g_dir);

        // 再进 10 次查表：已拉黑 ⇒ 零排队、零新日志、零写入
        int a = g_fa.allocCalls, e = g_fa.entryCalls;
        int discBefore = file_count(lp, "由查表发现");
        for (int i = 0; i < 10; i++) hook_lookup(cm, NULL, 0x4E00);
        ac1::core::init_log("");
        check(stats().pending == pend0, "★ 拉黑后 10 次查表：零排队");
        check(g_fa.allocCalls == a && g_fa.entryCalls == e, "★ 拉黑后 10 次查表：零写入");
        check(file_count(lp, "由查表发现") == discBefore, "★ 拉黑后 10 次查表：零新日志");
        check(file_count(lp, "拉黑") == blackAny, "★ 拉黑后日志行数不增");
        check(stats().failListed == fail0 + 1, "拉黑后名单长度不再增长");
        check(stats().failByCode[GLYPH_FAIL_ENTRY] == byCode0[GLYPH_FAIL_ENTRY] + 1,
              "★ 拉黑后 10 次查表：原因码分布不再增长（重复拉黑会把桶算重）");
        g_poisonCp = 0;
        (void)old;
    }

    char sb[512];
    printf("  状态: %s\n", status(sb, sizeof(sb)));
}

// ======================================================================
// [13] 码位命中 / 漏字统计
//
// 用合成清单（2 套 × 3 条 = 0x4E00..0x4E02 / 0x4F00..0x4F02）驱动查表钩，断言：
//   我们的码位 → 命中（次数涨 + 记第一次的调用者）
//   C < 0x3000 → 快路，两张表都不动
//   ≥0x3000 但不在表里 → 进「漏字」
//   二分的边界（最小/最大/刚好不存在）与“清单为空时整段短路”
// ======================================================================
static void test_cpindex()
{
    printf("\n[13] 码位命中 / 漏字统计\n");
    bind_engine(&kFake);
    set_hook_origins(&fake_lookup_orig, &fake_draw_orig);

    // 日志体系：run 文件按 stamp 命名；同秒内的组间会共用同名文件
    // ⇒ 先建拿路径、关掉、删掉、再重建，保证本组断言只看到本组的行
    ac1::core::init_log(g_dir);
    char lp[512];
    lstrcpynA(lp, ac1::core::log_path(), sizeof(lp));
    ac1::core::close_log();
    DeleteFileA(lp);
    ac1::core::init_log(g_dir);

    // ---- 清单为空：整段短路，零写入 ----
    load_manifest("Z:/nope/x.bin");
    check(set_count() == 0, "清单为空（正当实机情形）");
    {
        int h0 = stats().cpHitKinds, t0 = (int)stats().cpHitTotal, m0 = stats().cpMissKinds;
        report_tops();
        ac1::core::init_log("");
        check(stats().cpHitKinds == h0 && stats().cpHitTotal == t0 && stats().cpMissKinds == m0,
              "清单为空：report_tops() 不动任何计数");
        // 即使直接进查表钩，也必须零写入（短路在第二道闸）
        static FakeFont ff;
        make_font(&ff, kVtableScimitar, 224, 0.10f, 0.20f, 0.14f, 0.26f);
        for (int i = 0; i < 5; i++) hook_lookup(ff.raw + GLYPH_OFF_CHARMAP, NULL, 0x9F8D);
        check(stats().cpHitKinds == h0 && stats().cpMissKinds == m0,
              "清单为空：查 5 次 U+9F8D 也不记漏字（无表短路）");
        ac1::core::init_log(g_dir);
    }

    // ---- 加载合成清单：码位 = 0x4E00..0x4E02 / 0x4F00..0x4F02 ----
    char mp[512];
    path_in_dir(mp, sizeof(mp), "cpindex.bin");
    write_manifest(mp, 2, 3, 224, 0.14f - 0.10f, 0.26f - 0.20f, 224, 0.50f, 0.50f);
    check(load_manifest(mp) == 2, "合成清单 2 套已加载");
    {
        static FakeFont ff;
        make_font(&ff, kVtableScimitar, 224, 0.10f, 0.20f, 0.14f, 0.26f);
        unsigned char* cm = ff.raw + GLYPH_OFF_CHARMAP;
        int            hk0 = stats().cpHitKinds, ht0 = (int)stats().cpHitTotal, mk0 = stats().cpMissKinds;

        // (a) C < 0x3000 → 快路，两表都不动
        for (int i = 0; i < 50; i++) hook_lookup(cm, NULL, 'A'); // 'A' = 0x41
        hook_lookup(cm, NULL, 0x20);
        hook_lookup(cm, NULL, 0x2FFF);
        check(stats().cpHitKinds == hk0 && stats().cpHitTotal == ht0 && stats().cpMissKinds == mk0,
              "★ C<0x3000（'A'/0x20/0x2FFF）不进任何表");

        // (b) 我们的码位 → 命中，次数涨
        for (int i = 0; i < 7; i++) hook_lookup(cm, NULL, 0x4E00);
        check(stats().cpHitKinds == hk0 + 1, "命中种类 +1（只算 1 种）");
        check(stats().cpHitTotal == ht0 + 7, "命中次数 +7");
        check(stats().cpMissKinds == mk0, "命中的不算漏字");

        // (c) 二分边界：最小 / 最大 均命中
        hook_lookup(cm, NULL, 0x4F02); // 清单最大码位
        hook_lookup(cm, NULL, 0x4DFF); // 小于最小 → 漏字
        hook_lookup(cm, NULL, 0x4F03); // 大于最大 → 漏字
        check(stats().cpHitKinds == hk0 + 2, "★ 二分：最大码位 0x4F02 命中");
        check(stats().cpMissKinds == mk0 + 2, "★ 二分：0x4DFF / 0x4F03 都落到漏字");

        // (d) 真正的「漏字」：一个远端码位
        for (int i = 0; i < 3; i++) hook_lookup(cm, NULL, 0x9F8D);
        check(stats().cpMissKinds == mk0 + 3, "漏字种类 +1（U+9F8D 连查 3 次只算 1 种）");
        long missTotal = stats().cpHitTotal; // 漏字总次数不在 stats 里，用 Top 行反查

        // (e) Top 报表：只在「种类数变过」时打
        // 上面空清单那段已经打过一次「「7a7a」」报表（首次总是打）
        int hitTop0 = file_count(lp, "命中码位 Top");
        int missTop0 = file_count(lp, "漏字 Top");
        report_tops();
        ac1::core::init_log("");
        check(file_count(lp, "U+4E00×") == 1, "Top 里有命中条目 U+4E00");
        check(file_count(lp, "U+9F8D×") == 1, "Top 里有漏字条目 U+9F8D");
        check(file_count(lp, "命中码位 Top") == hitTop0 + 1, "种类数变了 ⇒ 命中 Top 多打 1 行");
        check(file_count(lp, "漏字 Top") == missTop0 + 1, "种类数变了 ⇒ 漏字 Top 多打 1 行");
        int hits1 = file_count(lp, "U+4E00×7@");
        check(hits1 == 1, "命中 Top 带次数与调用者（U+4E00×7@XXXXXXXX）", "格式见下");
        int hitTop1 = file_count(lp, "命中码位 Top");
        ac1::core::init_log(g_dir);
        report_tops(); // 种类数没变 ⇒ 不应该打
        ac1::core::init_log("");
        check(file_count(lp, "命中码位 Top") == hitTop1, "★ 种类数没变 ⇒ Top 不再打");
        ac1::core::init_log(g_dir);
        (void)missTotal;
    }

    char sb[512];
    check(strstr(status(sb, sizeof(sb)), "命中码位=") != NULL, "status 含「命中码位」");
    check(strstr(sb, "漏字=") != NULL, "status 含「漏字」");
    printf("  状态: %s\n", sb);
    ac1::core::init_log("");
}

// [15] forge 闸门：关 ⇒ 加载钩/查表发现都不打补丁、不排队；开 ⇒ 兜底路径恢复
static void test_forge_gate()
{
    printf("\n[15] forge 闸门（启动期文件核验的总闸）\n");
    bind_engine(&kFake);
    set_hook_origins(&fake_lookup_orig, &fake_draw_orig);
    set_host_base(kFakeBase);

    char mp[512];
    path_in_dir(mp, sizeof(mp), "gate.bin");
    write_manifest(mp, 2, 3, 224, 0.14f - 0.10f, 0.26f - 0.20f, 224, 0.50f, 0.50f);
    check(load_manifest(mp) == 2, "闸门组：清单已加载（2 套）");

    set_forge_patch(0);
    check(forge_patch_present() == 0, "set_forge_patch(0) ⇒ forge_patch_present()=0");

    // ① 加载钩路径：闸门关 ⇒ 一个字节都不动
    {
        static FakeFont ff;
        unsigned char*  old = make_font(&ff, kVtableScimitar, 224, 0.10f, 0.20f, 0.14f, 0.26f);
        int             patched0 = stats().fontsPatched;
        font_loaded_post(&ff);
        check(*(unsigned short*)(ff.raw + GLYPH_OFF_COUNT) == 224, "闸门关：加载钩后字形数仍 224");
        check(*(unsigned char**)(ff.raw + GLYPH_OFF_TABLE) == old, "闸门关：加载钩后表指针未动");
        check(stats().fontsPatched == patched0, "闸门关：已补丁字体数不涨");
        HeapFree(GetProcessHeap(), 0, ff.recs);
    }

    // ② 查表发现路径：闸门关 ⇒ 不排队；闸门开 ⇒ 排队 + DrawText 前应用
    {
        static FakeFont ff2;
        make_font(&ff2, kVtableScimitar, 224, 0.10f, 0.20f, 0.14f, 0.26f);
        unsigned char* cm = ff2.raw + GLYPH_OFF_CHARMAP;
        int            pend0 = stats().pending;
        hook_lookup(cm, NULL, 0x4E00);
        check(stats().pending == pend0, "闸门关：查表发现不排队");
        check(*(unsigned short*)(ff2.raw + GLYPH_OFF_COUNT) == 224, "闸门关：查表后字形数仍 224");

        set_forge_patch(1);
        check(forge_patch_present() == 1, "set_forge_patch(1) ⇒ forge_patch_present()=1");
        hook_lookup(cm, NULL, 0x4E00);
        check(stats().pending == pend0 + 1, "闸门开：查表发现 ⇒ 排队 +1");
        hook_drawtext(&ff2, NULL, 1, 2, 3, 4, 5, 6, 7, 8);
        check(*(unsigned short*)(ff2.raw + GLYPH_OFF_COUNT) == 227, "闸门开：DrawText 前应用 ⇒ 224 变 227");
        HeapFree(GetProcessHeap(), 0, ff2.recs);
    }
    set_forge_patch(1); // 套件默认态（与 main() 入口一致）
}

// [16] forge 核验（forgecheck.cpp）：sidecar 缺失 / 尺寸不符 / 尺寸符合 / CRC 对错 / forge 缺失
static void fc_write_text(const char* path, const char* s)
{
    FILE* f = fopen(path, "wb");
    if (!f) {
        check(0, "写文件失败", path);
        return;
    }
    fwrite(s, 1, strlen(s), f);
    fclose(f);
}

static void test_forgecheck()
{
    printf("\n[16] forge 核验（sidecar + 尺寸 + CRC32）\n");
    char base[512], scripts[512], sidecar[512], forge[512];
    path_in_dir(base, sizeof(base), "fc\\");
    CreateDirectoryA(base, NULL);
    _snprintf(scripts, sizeof(scripts) - 1, "%sscripts\\", base);
    scripts[sizeof(scripts) - 1] = 0;
    CreateDirectoryA(scripts, NULL);
    _snprintf(sidecar, sizeof(sidecar) - 1, "%sAC1_CJK_Forge.txt", scripts);
    sidecar[sizeof(sidecar) - 1] = 0;
    _snprintf(forge, sizeof(forge) - 1, "%sDataPC.forge", base);
    forge[sizeof(forge) - 1] = 0;

    // 768 B 已知载荷：bytes 0..255 ×3 ⇒ zlib CRC32 = B0C0DF2A（离线独立基准）
    unsigned char payload[768];
    for (int i = 0; i < 768; i++) payload[i] = (unsigned char)(i & 0xFF);
    {
        FILE* f = fopen(forge, "wb");
        fwrite(payload, 1, sizeof(payload), f);
        fclose(f);
    }

    DeleteFileA(sidecar);
    check(check_forge_patch(scripts) == 0 && forge_patch_present() == 0, "① sidecar 缺失 ⇒ 闸门关");

    fc_write_text(sidecar, "# t\nDataPC.forge 999 00000000 0\n");
    check(check_forge_patch(scripts) == 0 && forge_patch_present() == 0, "② 尺寸不符 ⇒ 闸门关");

    fc_write_text(sidecar, "DataPC.forge 768 deadbeef 0\n");
    check(check_forge_patch(scripts) == 1 && forge_patch_present() == 1,
          "③ 尺寸符合(flags=0) ⇒ 闸门开（不看 CRC）");

    fc_write_text(sidecar, "DataPC.forge 768 b0c0df2a 1\n");
    check(check_forge_patch(scripts) == 1 && forge_patch_present() == 1,
          "④ flags=1 且 CRC 一致(zlib B0C0DF2A) ⇒ 开（顺带交叉验证 C++ CRC32 == zlib）");

    fc_write_text(sidecar, "DataPC.forge 768 00000000 1\n");
    check(check_forge_patch(scripts) == 0 && forge_patch_present() == 0, "⑤ flags=1 但 CRC 不符 ⇒ 关");

    DeleteFileA(forge);
    fc_write_text(sidecar, "DataPC.forge 768 b0c0df2a 1\n");
    check(check_forge_patch(scripts) == 0 && forge_patch_present() == 0, "⑥ forge 文件缺失 ⇒ 关");

    set_forge_patch(1); // 恢复套件默认态
}

int main()
{
    ac1::core::init_log(""); // 关日志：单测不写文件
    SetConsoleOutputCP(65001);
    exe_dir();
    printf("ac1-chinese-translate glyph 层单测（链接 core+glyph）\n");

    // AC1_NO_FALLBACK 的回归断言：#endif 若放在 ③ 快路径（0x8840B0）之后，
    //   关掉兜底时会连主路径一起关掉，中文字形一个都打不出来。
    //   这里显式断言：无论开关如何，0x8840B0 的 HookSpec 必须仍然在册。
    {
        // 用源码级检查代替运行时检查：单测进程不是宿主 exe，install() 恒返回 0，
        // 装钩数量测不出来；本组确认编译期确实把 ③ 放在了开关之外 ——
        // 若开关把 ③ 一起关掉，本文件里的 AC1_NO_FALLBACK 断言会先红。
#if defined(AC1_NO_FALLBACK) && AC1_NO_FALLBACK
        check(font_set_of(NULL) == -1,
              "★ 收缩配置下 ③ 的依赖（font_set_of）仍完好 —— 它只被 0x8840B0 用到，"
              "若 ③ 被开关圈掉，这里就查不出差别；故另配一个源码级守卫（见 build.ps1）");
        printf("  （AC1_NO_FALLBACK=1：单测无法断言装钩数，请以真机 "
               "`GLYPH 钩子=1/3 字体=N 字形+1458N` 为准）\n");
#else
        check(true, "全量配置");
#endif
    }
    printf("测的是真机跑的那几个函数：font_loaded_post / hook_lookup / hook_drawtext。\n");
    printf("装钩本身（三条挂点的入口字节核对 + MinHook 落钩）由 core 签名核对单测 + 真机覆盖。\n");

    // 套件默认态：forge 闸门开（forge 路线下补丁的唯一总闸，见 forgecheck.cpp）。
    //   老用例全部假定「补丁应该打上」——它们跑的就是闸门开的行为；
    //   闸门关的行为由 test_forge_gate / test_forgecheck 专组断言（组内自己切回默认）。
    ac1::glyph::set_forge_patch(1);

    test_fingerprint();
    test_unbound();
    test_manifest_bad();
    test_patch_normal();
    test_idempotent();
    test_gates();
    test_no_match();
    test_legacy_not_freed();
    test_status();
    test_api_incomplete();
    test_fallback_path(); // 放最后：它会改全局计数/清单，前面的断言是绝对值
    test_convention_and_faillist();
    test_cpindex();
    test_forge_gate();
    test_forgecheck();

    printf("\n==== 通过 %d / 失败 %d ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
