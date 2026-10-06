// modules/dict/tests/dict_unit_test.cpp —— dict 层离线单测
//
//   pwsh -NoProfile -File modules\dict\tests\build.ps1
//   pwsh -NoProfile -File modules\dict\tests\run.ps1
//   out\dict_unit_test.exe [主词典路径]
//
// 只链接 dict.lib（不链接 core / text / glyph / 任何游戏 dll）。
// 因为 dict 不依赖 core，它的日志出口是注入的 —— 这里注入一个 printf 出口，
// 顺便验证注入点本身。
//
// 覆盖：canon 规范化 / \xNNNN 转义 / CP950 解码 / 成对块解析 / 精确匹配 /
//       大小写开关 / 补充词典（同键覆盖、注释空行、坏行、非法 UTF-8）。
// 全程不碰游戏、不开 D3D、不写文件（日志走注入的 printf 出口）。
// 可选参数 = 真实主词典路径；给了就顺带跑一遍真实词典并只报统计（不参与成败）。

#include "ac1/dict/dict.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h> // wmemcpy / wmemcmp（兜底测试用显式码元数组）

using namespace ac1::dict;

// dict 的日志出口：注入一个 printf 版本（dict 本身不认识 core）
static void test_log_sink(const char* fmt, ...)
{
    printf("      |日志| ");
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

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

// 把 wchar 串按固定代码页 950写进文件（测试数据自产自销，不硬编码 Big5 字节）
static void put_big5(FILE* f, const wchar_t* s)
{
    int n = WideCharToMultiByte(950, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 1) return;
    char* b = (char*)malloc((size_t)n);
    if (!b) return;
    if (WideCharToMultiByte(950, 0, s, -1, b, n, NULL, NULL) > 1)
        fwrite(b, 1, (size_t)n - 1, f); // 去掉结尾 NUL
    free(b);
}

// 把宽串按 UTF-8 写进文件。译文里的 \xNN 转义不要在 C++ 字面量里手写，转换交给 API。
static void put_utf8(FILE* f, const wchar_t* w)
{
    int n = (int)wcslen(w);
    int need = WideCharToMultiByte(CP_UTF8, 0, w, n, NULL, 0, NULL, NULL);
    if (need <= 0) return;
    char buf[1024];
    if (need > (int)sizeof(buf)) return;
    WideCharToMultiByte(CP_UTF8, 0, w, n, buf, need, NULL, NULL);
    fwrite(buf, 1, need, f);
}

static bool wstr_eq(const wchar_t* v, int n, const wchar_t* want)
{
    if (!v) return false;
    int wn = (int)wcslen(want);
    if (wn != n) return false;
    return memcmp(v, want, (size_t)n * sizeof(wchar_t)) == 0;
}

static bool lookup_is(const wchar_t* key, const wchar_t* want)
{
    int     n = (int)wcslen(key);
    DictVal r = lookup(key, n);
    if (!r.p) {
        printf("         （键没查到: %ls）\n", key);
        return false;
    }
    if (!wstr_eq(r.p, r.len, want)) {
        printf("         （译文不符: 实际 %d 码元 =", r.len);
        for (int i = 0; i < r.len; i++) printf(" \\u%04X", (unsigned)(unsigned short)r.p[i]);
        printf("，期望 %d 码元 =", (int)wcslen(want));
        for (size_t i = 0; want[i]; i++) printf(" \\u%04X", (unsigned)(unsigned short)want[i]);
        printf("）\n");
        return false;
    }
    return true;
}

// ================= 0. 日志出口注入（dict 不依赖 core 的关键）=================
static volatile LONG g_sinkCalls = 0;
static void          counting_sink(const char* fmt, ...)
{
    (void)fmt;
    InterlockedIncrement(&g_sinkCalls);
}

static void test_log_sink()
{
    printf("\n[0] 日志出口注入（dict 无外部依赖的关键）\n");
    set_log(&counting_sink);
    check(g_sinkCalls == 0, "刚装上出口还没有日志行");

    char p[MAX_PATH];
    path_in_dir(p, sizeof(p), "_dict_ut_none.txt");
    load_main(p); // 打不开 ⇒ 必然打一行日志
    check(g_sinkCalls > 0, "注入的出口真的被调用了", "出口没被调用");

    set_log(NULL);
    long before = g_sinkCalls;
    load_main(p); // 没有出口 ⇒ 静默，不能崩
    check(g_sinkCalls == before, "没有出口时静默（不崩）");
    set_log(&test_log_sink);
}

// ================= 1. 词典未就绪的降级路径（必须最先跑：此刻表还是空的）=================
// lookup 的长度一律走它：dict::lookup 不认 n<0（不实现 canon 的约定），
//   手数字符数又容易错（ExtraSuppKey 是 12 不是 11）。
#define LK(k) lookup((k), (int)wcslen(k))

static void test_dict_init_missing()
{
    printf("\n[1] dict_init：词典缺失 ⇒ 报未就绪\n");
    bool ok = init(g_dir);
    check(ok == false, "本目录没有 LGCStringDict_01.txt ⇒ dict_init 返回 false");
    check(!ready(), "ready() 为假");
    check(main_path()[0] == 0, "主词典路径为空");
    check(status()[0] != 0, "状态摘要非空");
}

// ================= 10. 两种部署形态（分支判据）=================
// 用户要求先验这两个场景
//   形态 A「回退」：没有 AC1_Dict.txt ⇒ 走旧分支 = LG 主词典 + 额外补充词典
//   形态 B「有词典」：有 AC1_Dict.txt ⇒ 只读它，LG 主词典不读
//
// 用夹具而不是真实的 LG 文件：这里要验的是分支（读了哪个文件），
// 夹具一样能证明；而它自包含、每次都跑。真实文件的条数另行手验。
static void test_deploy_shapes()
{
    printf("\n[10] 两种部署形态：回退(无 AC1_Dict) vs 有词典(只读它)\n");
    char fakeRoot[MAX_PATH], fakeScripts[MAX_PATH], fakeLg[MAX_PATH];
    path_in_dir(fakeRoot, sizeof(fakeRoot), "fakedeploy");
    _snprintf(fakeScripts, sizeof(fakeScripts), "%s\\scripts", fakeRoot);
    _snprintf(fakeLg, sizeof(fakeLg), "%s\\LG_Data", fakeRoot);
    CreateDirectoryA(fakeRoot, NULL); // 父目录要先建：CreateDirectoryA 只建一级
    CreateDirectoryA(fakeScripts, NULL);
    CreateDirectoryA(fakeLg, NULL);

    char lgPath[MAX_PATH], supPath[MAX_PATH], usrPath[MAX_PATH];
    _snprintf(lgPath, sizeof(lgPath), "%s\\LGCStringDict_01.txt", fakeLg);
    _snprintf(supPath, sizeof(supPath), "%s\\AC1_CN_Dict_Supplement.txt", fakeScripts);
    _snprintf(usrPath, sizeof(usrPath), "%s\\AC1_Dict.txt", fakeScripts);

    // 夹具 LG 词典：两条 CP950 成对块（用 ascii_decode 那条路写，键是纯 ASCII）
    {
        FILE* f = fopen(lgPath, "wb");
        if (!f) {
            check(false, "写假 LG 词典", lgPath);
            return;
        }
        fputs("xfhsm_res_ENG_Start\r\nLgOnlyKey\r\nxfhsm_res_ENG_End\r\n", f);
        fputs("xfhsm_res_CHI_Start\r\n", f);
        put_big5(f, L"來自 LG");
        fputs("\r\nxfhsm_res_CHI_End\r\n", f);
        fclose(f);
    }
    // 额外补充词典（旧名）：一条独有的键，用来证明补充词典在形态 A 里生效
    {
        FILE* f = fopen(supPath, "wb");
        if (!f) {
            check(false, "写额外补充词典", supPath);
            return;
        }
        fputs("ExtraSuppKey\t", f);
        put_utf8(f, L"來自補充");
        fputs("\n", f);
        fclose(f);
    }

    // ---- 形态 A：回退（只有 LG + 额外补充，没有 AC1_Dict.txt）----
    ac1::dict::reset(); // 表必须先清，否则测的是上一轮残留
    DeleteFileA(usrPath);
    bool okA = init(fakeScripts);
    check(okA, "形态 A：无 AC1_Dict.txt ⇒ 仍就绪（走旧分支）");
    check(main_path()[0] != 0, "形态 A：**读了** LG 主词典");
    // 用 LK() 取长度，别手数 —— 我手错过一次（12 字的 "ExtraSuppKey" 数成 11）。
    //   不能用 lookup(key, -1)：它不实现 canon 那套「n<0 按 NUL 算长度」的约定，
    //   会静默返回 -1（看着像「键没查到」）。
    check(LK(L"LgOnlyKey").p != NULL, "形态 A：LG 的条目能查到");
    check(lookup_is(L"ExtraSuppKey", L"來自補充"), "形态 A：额外补充词典也生效");
    check(LK(L"UserDictKey").p == NULL, "形态 A：没有用户词典的条目");

    // ---- 形态 B：有词典（只读它，LG 不读）----
    {
        FILE* f = fopen(usrPath, "wb");
        if (!f) {
            check(false, "写 AC1_Dict.txt", usrPath);
            return;
        }
        fputs("UserDictKey\t", f);
        put_utf8(f, L"由用戶詞典");
        fputs("\n", f);
        fclose(f);
    }
    ac1::dict::reset(); // 同上：形态 B 必须在干净表上验
    bool okB = init(fakeScripts);
    check(okB, "形态 B：有 AC1_Dict.txt ⇒ 就绪");
    check(main_path()[0] == 0, "形态 B：**没读** LG 主词典");
    check(lookup_is(L"UserDictKey", L"由用戶詞典"), "形态 B：用户词典的译文生效");
    check(LK(L"LgOnlyKey").p == NULL, "★ 形态 B：LG 独有的键**查不到** ⇒ 不会被中文顶掉（这正是解耦的意义）");
    check(LK(L"ExtraSuppKey").p == NULL, "★ 形态 B：旧补充词典也不参与（被用户词典取代）");

    // 收尾
    DeleteFileA(usrPath);
    DeleteFileA(supPath);
    DeleteFileA(lgPath);
    RemoveDirectoryA(fakeScripts);
    RemoveDirectoryA(fakeLg);
    RemoveDirectoryA(fakeRoot);
}

// ================= 2. canon =================
static void test_canon()
{
    printf("\n[2] dict_canon 规范化\n");
    wchar_t out[64];

    int n;
    n = canon(L"  Start   Game  ", -1, out, 64);
    check(n == 10 && wstr_eq(out, n, L"Start Game"), "连续空白折叠 + 掐头去尾");

    n = canon(L"\t\r\n A \n B \t", -1, out, 64);
    check(n == 3 && wstr_eq(out, n, L"A B"), "TAB/CR/LF 全算空白");

    n = canon(L"   ", -1, out, 64);
    check(n == 0, "全空白 ⇒ 长度 0");

    n = canon(L"abc\0def", -1, out, 64);
    check(n == 3 && wstr_eq(out, n, L"abc"), "遇 NUL 视为串尾");

    n = canon(L"abcdefghij", -1, out, 4);
    check(n == -1, "超出 cap ⇒ -1");

    n = canon(L"x", 0, out, 64);
    check(n == 0, "长度 0 ⇒ 空");

    n = canon(L"MiXeD", -1, out, 64);
    check(n == 5 && wstr_eq(out, n, L"MiXeD"), "大小写原样保留（不归一化大小写）");
}

// ================= 2. 主词典（成对块 / 转义 / CP950）=================
static bool write_main_dict(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;

    // --- 块1：ENG 侧全用 \xNNNN（TAB 分隔的两行）⇒ 键应折叠成 "HELLO WORLD" ---
    //      注意 'E' 是 0x45、'L' 是 0x4C、'O' 是 0x4F（大写！小写会变成 hello）
    fputs("xfhsm_res_ENG_Start\r\n\t", f);
    fputs("\\x0048\\x0045\\x004C\\x004C\\x004F\tWORLD\r\nxfhsm_res_ENG_End\r\n", f);
    fputs("xfhsm_res_CHI_Start\r\n\t", f);
    put_big5(f, L"你好世界"); // CP950
    fputs("\r\nxfhsm_res_CHI_End\r\n", f);

    // --- 块2：普通 ASCII 键 ---
    fputs("xfhsm_res_ENG_Start\r\nStart Game\r\nxfhsm_res_ENG_End\r\n", f);
    fputs("xfhsm_res_CHI_Start\r\n", f);
    put_big5(f, L"開始遊戲");
    fputs("\r\nxfhsm_res_CHI_End\r\n", f);

    // --- 块3：键本身含 CJK ⇒ 必须被跳过（运行期会被 CJK 闸门拦掉）---
    fputs("xfhsm_res_ENG_Start\r\n\\x4E2D\\x6587\\r\nxfhsm_res_ENG_End\r\n", f);
    fputs("xfhsm_res_CHI_Start\r\n", f);
    put_big5(f, L"中文");
    fputs("\r\nxfhsm_res_CHI_End\r\n", f);

    // --- 块4：键长 4097 > DICT_KEY_MAX(4096) ⇒ 必须被跳过 ---
    fputs("xfhsm_res_ENG_Start\r\n", f);
    for (int i = 0; i < 4097; i++) fputc('A', f);
    fputs("\r\nxfhsm_res_ENG_End\r\n", f);
    fputs("xfhsm_res_CHI_Start\r\n", f);
    put_big5(f, L"太長");
    fputs("\r\nxfhsm_res_CHI_End\r\n", f);

    // --- 块5：与块2 键相同、译文不同 ⇒ 先到先得 ---
    fputs("xfhsm_res_ENG_Start\r\nStart Game\r\nxfhsm_res_ENG_End\r\n", f);
    fputs("xfhsm_res_CHI_Start\r\n", f);
    put_big5(f, L"重複條目");
    fputs("\r\nxfhsm_res_CHI_End\r\n", f);

    // --- 块6：CHI 侧给一个 Big5 非法字节 ⇒ 期望落 '?' ---
    fputs("xfhsm_res_ENG_Start\r\nBad Byte\r\nxfhsm_res_ENG_End\r\n", f);
    fputs("xfhsm_res_CHI_Start\r\n", f);
    fputc(0xFF, f);
    fputs("\r\nxfhsm_res_CHI_End\r\n", f);

    // --- 块7：CHI 侧用 \xNNNN 写码位（图标码位就是这么写的）---
    fputs("xfhsm_res_ENG_Start\r\nEsc Val\r\nxfhsm_res_ENG_End\r\n", f);
    fputs("xfhsm_res_CHI_Start\r\n\\x4E2D\\x6587\r\nxfhsm_res_CHI_End\r\n", f);

    // --- 块8：ENG 侧空 ⇒ 跳过 ---
    fputs("xfhsm_res_ENG_Start\r\n   \r\nxfhsm_res_ENG_End\r\n", f);
    fputs("xfhsm_res_CHI_Start\r\n", f);
    put_big5(f, L"空的");
    fputs("\r\nxfhsm_res_CHI_End\r\n", f);

    // --- 块9：CHI 侧带 U+2026 ⇒ 必须被改写成三个 ASCII 点 ---
    //   原版字体的 U+2026 字形是育碧占位图标（只有 title 自带该条目）。
    //     我们不接管那个码位，默认行为原样保留；只改译文侧。
    //   加块必须同步改 test_main_dict 里的 main_pairs(8→9) 与值池对账。
    fputs("xfhsm_res_ENG_Start\r\nLoading Data\r\nxfhsm_res_ENG_End\r\n", f);
    fputs("xfhsm_res_CHI_Start\r\n\\x4E2D\\x6587\\x2026\r\nxfhsm_res_CHI_End\r\n", f);

    fclose(f);
    return true;
}

static void test_main_dict()
{
    printf("\n[3] 主词典：成对块 / \\xNNNN / CP950\n");
    char path[MAX_PATH];
    path_in_dir(path, sizeof(path), "_dict_ut_main.txt");
    if (!write_main_dict(path)) {
        check(false, "写测试词典文件", path);
        return;
    }

    DictStats before = stats();
    load_main(path);
    const DictStats& a = stats();
    printf("  统计：成对块=%d 建键=%d 无译文=%d 键含CJK=%d 键过长=%d 重复=%d 值池满=%d 落'?'=%d\n",
           a.main_pairs, a.keys, a.main_noval, a.main_cjk, a.main_keylong, a.main_dup, a.main_valfull,
           a.decode_qmark);

    check(lookup_is(L"HELLO WORLD", L"你好世界"), "\\xNNNN 展开 + 物理行折叠后命中", "键没查到");
    check(lookup(L"HELLO\tWORLD", 11).p == NULL, "TAB 未折叠的键不命中（canon 确实在折叠）");
    check(lookup_is(L"Start Game", L"開始遊戲"), "CP950 解码正确（開始遊戲）");
    check(lookup_is(L"Esc Val", L"中文"), "CHI 侧 \\xNNNN 展开成码元");
    check(a.main_dup - before.main_dup == 1, "重复键被丢弃（先到先得）");
    check(a.main_cjk - before.main_cjk == 1, "键含 CJK 被跳过");
    // 上限是 4096：LG 里有 51 条 canon 后超 300 的真对白（最长 3,628 字符），
    //   300 会把它们静默丢掉。
    check(a.main_keylong - before.main_keylong == 1, "键过长(>4096)被跳过");
    // 反过来钉住「300~4096 的键要收」——防止上限被调回去
    {
        wchar_t buf[DICT_KEY_MAX + 2];
        int     k, i;
        for (i = 0; i < 1200; i++) buf[i] = L'A';
        buf[1200] = 0;
        k = canon(buf, 1200, buf, DICT_KEY_MAX);
        check(k == 1200, "1200 字符的键现在**装得下**（上限 4096）");
        for (i = 0; i < DICT_KEY_MAX + 1; i++) buf[i] = L'A';
        buf[DICT_KEY_MAX + 1] = 0;
        check(canon(buf, DICT_KEY_MAX + 1, buf, DICT_KEY_MAX) < 0, "超上限的键仍被拒（canon 返回 -1）");
    }
    check(a.main_noval - before.main_noval == 1, "空 ENG 块被跳过");
    check(a.main_pairs - before.main_pairs == 9, "成对块计数 = 9");

    // ---- 省略号规整：CHI 侧的 U+2026 必须变成三个 ASCII 点 ----
    //   块9 的值是 `中文…`（U+4E2D U+6587 U+2026）；取回时必须已经是 `中文...`。
    //   留着 U+2026 就会画成育碧占位图标（那是原版字体在该码位上的字形）。
    check(lookup_is(L"Loading Data", L"中文..."), "U+2026 → \"...\"（三个点，不留在译文里）");
    check(a.main_fallback[0] - before.main_fallback[0] == 1, "省略号规整计数 = 1");

    // 值池回收的回归测试（算术精确对账）。
    //   本夹具 9 个块里真正建键的 5 条，译文码元数：
    //     HELLO WORLD→你好世界(4) + Start Game→開始遊戲(4)
    //     + Esc Val→中文(2) + Bad Byte→'?'(1) + Loading Data→中文...(5) = 16
    //   块5 是与块2 同键的重复条目（重複條目 4 码元），它不该占池子。
    //   块9 的 U+2026 展开成 `...` 是 3 码元（不是 1）⇒ 这条断言同时盯着
    //   「入池检查按展开后长度算」——按 vlen 算会写溢出/漏字。
    check(a.valpool_used - before.valpool_used == 16,
          "值池只装真正存下的译文（重复键不白烧；U+2026 按展开后的 3 码元计）");

    // 块6：0xFF 在 Big5 里不是合法首字节 ⇒ 必须落 '?' 并计数
    //   （MB_ERR_INVALID_CHARS 下不再有「本机替换成 U+F8F8」这条侥幸路径）
    {
        check(lookup_is(L"Bad Byte", L"?"), "Big5 解不出的字节落 '?'（不是 U+F8F8）");
        check(a.decode_qmark - before.decode_qmark == 1, "落 '?' 计数 +1（旧口径下它是 0）");
    }

    // 返回值口径 = 本次新建的键条数：再载入同一份 ⇒ 全是重复键 ⇒ 0 条（旧实现返回表内总数）
    check(load_main(path) == 0, "第二次载入同一份 ⇒ 返回 0（口径 = 本次新建）");
}

// ================= 3b. 成对块：缺 CHI 的块不许偷下一块的译文 =================
static void test_main_pairing()
{
    printf("\n[3b] 主词典坏块：跨块偷配 + 跳块\n");
    char path[MAX_PATH];
    path_in_dir(path, sizeof(path), "_dict_ut_broken.txt");
    FILE* f = fopen(path, "wb");
    if (!f) {
        check(false, "写坏块夹具", path);
        return;
    }
    // 块1：ENG 有、CHI 整段缺失；块2：ENG+CHI 都正常。
    //   旧实现会让块1 偷走块2 的 CHI（错配）并把块2 整个跳掉（丢键），日志上看不出来。
    fputs("xfhsm_res_ENG_Start\r\nAAA BROKEN\r\nxfhsm_res_ENG_End\r\n", f);
    fputs("xfhsm_res_ENG_Start\r\nBBB GOOD\r\nxfhsm_res_ENG_End\r\n", f);
    fputs("xfhsm_res_CHI_Start\r\n", f);
    put_big5(f, L"好");
    fputs("\r\nxfhsm_res_CHI_End\r\n", f);
    fclose(f);

    DictStats before = stats();
    load_main(path);
    const DictStats& a = stats();
    check(a.main_pairs - before.main_pairs == 2, "两个 ENG 块都被数到（旧实现会漏掉被跳的那块）");
    check(a.main_noval - before.main_noval == 1, "缺 CHI 的块按「无译文」计（不再静默偷配）");
    check(LK(L"AAA BROKEN").p == NULL, "坏块的键没有入库");
    check(lookup_is(L"BBB GOOD", L"好"), "下一块的键仍配到自己的译文");
    DeleteFileA(path);
}

// ================= 3c. Big5 尾字节 0x5C 的合法汉字（許/功/蓋）=================
// 回归：`b[i+1]=='\\'` 的启发式会把尾字节 0x5C 的合法汉字拆成「替换符 + 字面反斜杠」，
//   且 U+FFFD 的替换语义（不设 MB_ERR_INVALID_CHARS）让 decode_qmark 恒 0、看不见。
static void test_big5_trail5c()
{
    printf("\n[3c] Big5 尾字节 0x5C：許/功/蓋 不许被拆\n");
    char path[MAX_PATH];
    path_in_dir(path, sizeof(path), "_dict_ut_b3.txt");
    FILE* f = fopen(path, "wb");
    if (!f) {
        check(false, "写 Big5 夹具", path);
        return;
    }
    fputs("xfhsm_res_ENG_Start\r\nTRAIL 5C\r\nxfhsm_res_ENG_End\r\n", f);
    fputs("xfhsm_res_CHI_Start\r\n", f);
    put_big5(f, L"許功蓋"); // 三个字的尾字节都是 0x5C
    fputs("\r\nxfhsm_res_CHI_End\r\n", f);
    fclose(f);

    DictStats before = stats();
    load_main(path);
    const DictStats& a = stats();
    check(a.decode_qmark - before.decode_qmark == 0, "没有解不出的字节（'?' 计数不涨）");
    check(lookup_is(L"TRAIL 5C", L"許功蓋"), "★ 尾字节 0x5C 的三个字逐码元正确", "值不对");
    DeleteFileA(path);
}

// ================= 3. 补充词典 =================
static bool write_supp_dict(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    // 补充词典固定按 UTF-8 写（测试数据自产自销，不硬编码字节）
    auto put_utf8 = [&](const wchar_t* s) {
        int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
        if (n <= 1) return;
        char* b = (char*)malloc((size_t)n);
        if (WideCharToMultiByte(CP_UTF8, 0, s, -1, b, n, NULL, NULL) > 1) fwrite(b, 1, (size_t)n - 1, f);
        free(b);
    };

    fputs("# \xE8\xAE\xAE\xE7\xBD\xAE\xE9\xA1\xB9\xE7\x9B\xAE\xE5\xBD\x95\xE6\xB3\xA8\xE9\x87\x8A\r\n",
          f); // UTF-8 注释
    fputs("\r\n", f);
    fputs("   \r\n", f);
    fputs("PROFILE\t", f);
    put_utf8(L"档案");
    fputs("\r\n", f); // 新键
    fputs("Start Game\t", f);
    put_utf8(L"開始新局");
    fputs("\r\n", f);                    // 覆盖主词典同键
    fputs("Esc\t\\x4E2D\\x6587\r\n", f); // 译文里的转义
    fputs("Same Key\t", f);
    put_utf8(L"第一");
    fputs("\r\n", f);
    fputs("Same Key\t", f);
    put_utf8(L"第二");
    fputs("\r\n", f);                    // 同文件内后写覆盖
    fputs("no tab on this line\r\n", f); // 无 TAB ⇒ 坏行
    fputs("Esc 2\tignored\r\n", f);      // 键里混了空格？不 —— 键 = "Esc 2"（合法）
    fputs("\\x4E2D\\x6587KEY\t", f);
    put_utf8(L"中文键");
    fputs("\r\n", f);                   // 键含 CJK（转义写法）⇒ 跳过
    fputs("Bad Utf8\t\xFF\xFE\r\n", f); // 译文非法 UTF-8
    fputs("Esc Val\t", f);
    put_utf8(L"中文");
    fputs("\r\n", f); // 与主词典同键同值 ⇒ 计入 sup_same
    fclose(f);
    return true;
}

static void test_supplement()
{
    printf("\n[4] 补充词典：UTF-8 / 同键覆盖 / 坏行\n");
    char path[MAX_PATH];
    path_in_dir(path, sizeof(path), "_dict_ut_supp.txt");
    if (!write_supp_dict(path)) {
        check(false, "写测试补充词典", path);
        return;
    }

    DictStats before = stats();
    load_supplement(path);
    const DictStats& a = stats();
    char             d[128];
    _snprintf(
        d, sizeof(d), "实际 lines=%d comment=%d empty=%d bad=%d keybad=%d valbad=%d new=%d over=%d same=%d",
        a.sup_lines - before.sup_lines, a.sup_comment - before.sup_comment, a.sup_empty - before.sup_empty,
        a.sup_bad - before.sup_bad, a.sup_keybad - before.sup_keybad, a.sup_valbad - before.sup_valbad,
        a.sup_new - before.sup_new, a.sup_over - before.sup_over, a.sup_same - before.sup_same);

    check(lookup_is(L"PROFILE", L"档案"), "补充词典 UTF-8 新键");
    check(lookup_is(L"Start Game", L"開始新局"), "补充词典**覆盖**主词典同键");
    check(lookup_is(L"Esc", L"中文"), "补充词典译文里的 \\xNNNN 展开");
    check(lookup_is(L"Same Key", L"第二"), "同文件内后写覆盖前写");
    check(lookup_is(L"Esc 2", L"ignored"), "键内含空格的普通条目");
    check(a.sup_lines - before.sup_lines == 10, "有效行计数 = 10", d);
    check(a.sup_comment - before.sup_comment == 1, "注释行被跳过", d);
    check(a.sup_empty - before.sup_empty == 2, "空行/空白行被跳过", d);
    check(a.sup_bad - before.sup_bad == 1, "无 TAB 的行被计坏行", d);
    check(a.sup_keybad - before.sup_keybad == 1, "键含 CJK 被跳过", d);
    check(a.sup_valbad - before.sup_valbad == 1, "非法 UTF-8 译文被跳过并计数", d);
    check(a.sup_over - before.sup_over == 2, "覆盖统计 = 2（主词典 1 + 同文件 1）", d);
    check(a.sup_new - before.sup_new == 4, "新建键统计 = 4", d);
    // 夹具里 "Esc Val\t中文" 与主词典同键同值 ⇒ sup_same 必须真的 = 1；
    //   退回 0 说明同值检测坏了。
    check(a.sup_same - before.sup_same == 1, "同键同值计入 sup_same（不是覆盖）", d);
    check(a.sup_keyfull - before.sup_keyfull == 0, "键表/键池未满（这类失败走 sup_keyfull，不算同值）", d);

    // 回归：同键同值的译文必须退回值池。写一个与当前表
    //   完全同值的单条词典重载 ⇒ 全 SAME ⇒ 值池零增长；涨了就说明 SAME 漏了 unwind。
    //   （不能拿本夹具直接重载：「Same Key」在文件里出现两次，重载第一行必然
    //   OVER —— 那是文件内后写覆盖语义的正确表现，不是泄漏。）
    char one[MAX_PATH];
    path_in_dir(one, sizeof(one), "_dict_ut_same.txt");
    FILE* f2 = fopen(one, "wb");
    if (f2) {
        fputs("Same Key\t", f2);
        put_utf8(f2, L"第二");
        fputs("\n", f2);
        fclose(f2);
        const int poolBefore = stats().valpool_used;
        load_supplement(one);
        check(stats().valpool_used == poolBefore, "全 SAME 重载 ⇒ 值池零增长（SAME 回滚）", d);
        DeleteFileA(one);
    }
}

// ================= 4b. UTF-8 BOM（首行判据与首条数据必须不受影响）=================
static void test_bom()
{
    printf("\n[4b] 补充词典带 UTF-8 BOM\n");
    char path[MAX_PATH];
    path_in_dir(path, sizeof(path), "_dict_ut_bom.txt");
    FILE* f = fopen(path, "wb");
    if (!f) {
        check(false, "写 BOM 夹具", path);
        return;
    }
    fputs("\xEF\xBB\xBF", f);                    // UTF-8 BOM
    fputs("# bom comment line (no tab)\r\n", f); // 首行：注释
    fputs("BomKey\t", f);
    put_utf8(f, L"由BOM后的第一條"); // 首条数据
    fputs("\r\n", f);
    fclose(f);

    DictStats before = stats();
    load_supplement(path);
    const DictStats& a = stats();
    check(lookup_is(L"BomKey", L"由BOM后的第一條"), "BOM 之后的首条数据正常入库");
    check(a.sup_comment - before.sup_comment == 1, "首行注释按注释计（旧实现把它算成坏行）");
    check(a.sup_bad - before.sup_bad == 0, "首行不再被计成「无TAB或空侧」");
    DeleteFileA(path);
}

// ================= 4c. 行长上限：键 ≤4096（canon）/ 值 ≤8192（解码）=================
// 回归：4096 固定缓冲把「确实过长」与「不是合法 UTF-8」共用一个返回 0，日志说错原因；
//   且用户路径的上限比主词典路径（8192）小一倍。现在两边对齐且分开计数。
static void test_length_limits()
{
    printf("\n[4c] 行长上限：键 ≤4096 / 值 ≤8192\n");
    char path[MAX_PATH];
    path_in_dir(path, sizeof(path), "_dict_ut_long.txt");
    FILE* f = fopen(path, "wb");
    if (!f) {
        check(false, "写超长夹具", path);
        return;
    }

    static char big[9100];
    memset(big, 'K', 5000); // ① 键 5000 码元：解码成功、canon 拒（>4096）
    fwrite(big, 1, 5000, f);
    fputs("\ttoo long key\r\n", f);
    fputs("LongVal\t", f); // ② 值 5000 码元：可入库（<=8192）
    memset(big, 'V', 5000);
    fwrite(big, 1, 5000, f);
    fputs("\r\n", f);
    fputs("TooLongVal\t", f); // ③ 值 9000 码元：>8192 ⇒ 「译文过长」
    memset(big, 'W', 9000);
    fwrite(big, 1, 9000, f);
    fputs("\r\n", f);
    memset(big, 'E', 4096); // ④ 边界：键 4096 + 值 8192
    fwrite(big, 1, 4096, f);
    fputs("\t", f);
    memset(big, 'F', 8192);
    fwrite(big, 1, 8192, f);
    fputs("\r\n", f);
    fclose(f);

    DictStats before = stats();
    load_supplement(path);
    const DictStats& a = stats();
    check(a.sup_keylong - before.sup_keylong == 1, "① 5000 码元的键 ⇒ 计入「键过长」");
    check(a.sup_keybad - before.sup_keybad == 0, "① 没有把它误记成「键非法」");
    check(a.sup_vallong - before.sup_vallong == 1, "③ 9000 码元的译文 ⇒ 计入「译文过长」");
    check(a.sup_valbad - before.sup_valbad == 0, "③ 没有把它误记成「译文解码失败」");
    {
        static wchar_t k[5001];
        for (int i = 0; i < 5000; i++) k[i] = L'K';
        check(lookup(k, 5000).p == NULL, "① 超长键查不到（表里装不下）");
    }
    {
        DictVal v = LK(L"LongVal");
        check(v.p != NULL && v.len == 5000, "② 5000 码元的译文能入库（上限 8192）");
    }
    {
        static wchar_t k[4097];
        for (int i = 0; i < 4096; i++) k[i] = L'E';
        DictVal v = lookup(k, 4096);
        check(v.p != NULL && v.len == 8192, "④ 边界：4096 键 + 8192 值都装得下");
        bool allF = (v.p != NULL);
        for (int i = 0; allF && i < v.len; i++)
            if (v.p[i] != L'F') allF = false;
        check(allF, "④ 值内容逐码元正确");
    }
    check(LK(L"TooLongVal").p == NULL, "③ 被丢的那条查不到（保留英文原文）");
    DeleteFileA(path);
}

// ================= 4d. 值池打满：丢条计数必须可见（必须最后跑）=================
static void test_pool_full_status()
{
    printf("\n[4d] 值池打满：丢条计数进 status 摘要\n");
    char path[MAX_PATH];
    path_in_dir(path, sizeof(path), "_dict_ut_full.txt");
    FILE* f = fopen(path, "wb");
    if (!f) {
        check(false, "写满池夹具", path);
        return;
    }

    // 按当前余量把池精确填满（每条 8192 码元 = 解码上限），最后再补一条必定装不下的
    static char line[8300];
    int         idx = 0;
    for (int remain = DICT_VALPOOL - stats().valpool_used; remain > 0;) {
        const int len = (remain >= 8192) ? 8192 : remain;
        char      key[32];
        _snprintf(key, sizeof(key), "FILL%03d\t", idx++);
        fputs(key, f);
        memset(line, 'x', (size_t)len);
        fwrite(line, 1, (size_t)len, f);
        fputs("\r\n", f);
        remain -= len;
    }
    fputs("FILL_OVER\t", f);
    memset(line, 'y', 8192);
    fwrite(line, 1, 8192, f);
    fputs("\r\n", f);
    fclose(f);

    DictStats s0 = stats();
    load_supplement(path);
    const DictStats& a = stats();
    check(a.sup_poolfull - s0.sup_poolfull >= 1, "值池满 ⇒ 至少一条被丢弃并计数");
    check(LK(L"FILL_OVER").p == NULL, "被丢的那条查不到（保留英文原文）");

    // status() 是 init() 里拼的（运行期只在启动时拼一次）⇒ 按真实时序再走一遍 init：
    //   在本测试目录放一份最小的 AC1_Dict.txt，init 加载它并重拼 status。
    char up[MAX_PATH];
    path_in_dir(up, sizeof(up), "AC1_Dict.txt");
    FILE* fu = fopen(up, "wb");
    if (fu) {
        fputs("# status rebuild probe\r\n", fu);
        fclose(fu);
    }
    init(g_dir);
    check(strstr(status(), "丢弃 值池满=") != NULL, "★ status 摘要带上丢弃计数（状态行/报表头可见）",
          status());
    DeleteFileA(up);
    DeleteFileA(path);
}

// ================= 4. 精确匹配语义（大小写差异由词典数据补全，不做兜底）=================
static void test_case_sensitivity()
{
    printf("\n[5] 精确匹配：大小写必须完全一致\n");
    int n = (int)wcslen(L"Start Game"); // 夹具里的键就是这个形态
    check(lookup(L"Start Game", n).p != NULL, "与词典键完全同形 ⇒ 命中");
    check(lookup(L"start game", n).p == NULL, "全小写 ⇒ **不命中**（不给模糊兜底；要支持就补词典条目）");
    check(lookup(L"START GAME", n).p == NULL, "全大写 ⇒ 不命中");
    check(lookup(L"", 0).p == NULL, "空键 ⇒ 未命中");
    check(lookup(L"ZZZ_NO_SUCH_KEY_ZZZ", 18).p == NULL, "不存在的键 ⇒ 未命中（p == NULL）");
}

// ================= 5b. lookup 的长度上界 =================
// 回归：n >= 65536 时 `k->len == (unsigned short)n` 的窄化比较可能让比较循环扫出键池。
//   缓冲区带**守卫页**：若上界护栏被改坏，越界读会立刻 AV，而不是读了超大 BSS 还静默返回 NULL。
static void test_lookup_bounds()
{
    printf("\n[5b] lookup 长度上界：n > DICT_KEY_MAX ⇒ 未命中\n");
    // 数组尾**正好贴**守卫页：任何越读（哪怕 2 字节）都会立刻 AV，而不是读进垫片里照样返回 NULL。
    const size_t   need = (size_t)(DICT_KEY_MAX + 1) * sizeof(wchar_t);
    const size_t   dataBytes = ((need + 4095) / 4096) * 4096;
    unsigned char* mem =
        (unsigned char*)VirtualAlloc(NULL, dataBytes + 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        check(false, "分配带守卫页的测试缓冲");
        return;
    }
    DWORD old = 0;
    VirtualProtect(mem + dataBytes, 4096, PAGE_NOACCESS, &old); // 守卫页：越界即 AV
    wchar_t* k = (wchar_t*)(mem + dataBytes - need);
    for (int i = 0; i < DICT_KEY_MAX + 1; i++) k[i] = L'Z';

    check(lookup(k, DICT_KEY_MAX + 1).p == NULL, "n = 4097 ⇒ 未命中");
    check(lookup(k, 70000).p == NULL, "★ n = 70000 ⇒ 未命中（不进 u16 窄化比较）");
    check(lookup(k, -1).p == NULL, "n < 0 ⇒ 未命中");
    check(lookup(NULL, 5).p == NULL, "key == NULL ⇒ 未命中");
    VirtualFree(mem, 0, MEM_RELEASE);
}

// ================= 6.5 兜底规整（直接调 API）=================
// 这一组是「加一条兜底只需三行」的兑现：不用去凑 CP950 夹具，
//   直接喂 wchar 串、断言出来什么。以后加规则就照这个模板写。
static void test_fallback()
{
    printf("\n[6.5] 译文兜底规整（src/fallback.cpp，规则表驱动）\n");
    printf("  规则数 = %d：", fallback_rule_count());
    for (int r = 0; r < fallback_rule_count(); r++) printf("%s%s", r ? " ｜ " : "", fallback_rule_name(r));
    printf("\n");
    check(fallback_rule_count() > 0, "至少有一条兜底规则");
    // 运行时镜像 fallback.cpp 的 static_assert：规则数超过 DictStats 的定长数组会静默丢统计
    check(fallback_rule_count() <= FALLBACK_MAX_RULES, "规则数 ≤ FALLBACK_MAX_RULES（计数数组容得下）");

    // 全部用显式码元数组，不写宽串字面量：源文件编码一旦不是 UTF-8，
    //   字面量里的 `…`/汉字会被按系统代码页拆成别的码元，测试就会变成测编译器。
    wchar_t b[64];
    for (int i = 0; i < 64; i++) b[i] = 0x5A5A; // 哨兵：证明结果没被多写

    // --- 不含任何目标字符 ⇒ 原样返回，长度不变 ---
    {
        const wchar_t in[] = { 0x5F00, 0x59CB, 0x6E38, 0x620F, 0 }; // 开始游戏
        wmemcpy(b, in, 5);
        int r = fallback_apply(b, 4, 64);
        check(r == 4 && wmemcmp(b, in, 5) == 0, "无命中的串：长度与内容都不变");
    }

    // --- len<0 契约：前 cap 个码元无 NUL ⇒ -1，且不越界读（审查 P2 回归）---
    {
        wchar_t       nb[6] = { L'a', L'b', L'c', L'd', L'e', L'x' }; // cap=5，前 5 个全非 NUL
        const wchar_t save = nb[5];                                   // canary：buf[cap] 不允许被碰
        check(fallback_apply(nb, -1, 5) == -1, "无 NUL 且装不下 ⇒ 按 NUL 算长度也拒");
        check(nb[5] == save, "canary 完好（NUL 扫描不越界读 buf[cap]）");
    }

    // --- 单个 U+2026 → ... ---
    {
        const wchar_t in[] = { 0x8A18, 0x5165, 0x2026, 0 };          // 载入…
        const wchar_t want[] = { 0x8A18, 0x5165, '.', '.', '.', 0 }; // 载入...
        wmemcpy(b, in, 4);
        int h0 = fallback_hits(0);
        int r = fallback_apply(b, 3, 64);
        check(r == 5, "U+2026 展开成三个点 ⇒ 长度 3→5");
        check(wmemcmp(b, want, 5) == 0, "内容变成「载入...」");
        // 契约：结果不是 NUL 结尾的，长度只由返回值给出 ⇒ 只比前 r 个
        check(b[5] == 0x5A5A, "b[5] 未被触碰（结果不补 NUL，长度只由返回值给出）");
        check(fallback_hits(0) == h0 + 1, "规则 0 命中计数 +1");
    }

    // --- 前后都有内容（证明就地展开没踩到前后）---
    {
        const wchar_t in[] = { 0x524D, 0x2026, 0x4E2D, 0x2026, 0x5F8C, 0 }; // 前…中…后
        const wchar_t want[] = { 0x524D, '.', '.', '.', 0x4E2D, '.', '.', '.', 0x5F8C, 0 };
        wmemcpy(b, in, 6);
        int r = fallback_apply(b, 5, 64);
        check(r == 9, "两个 U+2026 ⇒ 长度 5→9");
        check(wmemcmp(b, want, 9) == 0, "前后内容都完好（就地展开正确）");
    }

    // --- 连续三个（最坏重叠情形）---
    {
        const wchar_t in[] = { 0x2026, 0x2026, 0x2026, 0 };
        wmemcpy(b, in, 4);
        int r = fallback_apply(b, 3, 64);
        check(r == 9, "三个连排 U+2026 ⇒ 长度 3→9");
        int dots = 0;
        for (int i = 0; i < r; i++)
            if (b[i] == '.') dots++;
        check(dots == 9, "三个连排 ⇒ 九个点（重叠没出错）");
    }

    // --- 装不下 ⇒ 返回 -1，且原内容一个字节都没动（两趟分离的保证）---
    {
        const wchar_t in[] = { 0x8A18, 0x5165, 0x2026, 0 };
        wmemcpy(b, in, 4);
        int r = fallback_apply(b, 3, 3); // 装得下原文(3)，装不下展开后(5)
        check(r == -1, "cap 不够 ⇒ 返回 -1");
        check(wmemcmp(b, in, 4) == 0, "返回 -1 时原内容一个字节都没动");
    }

    // --- len < 0 ⇒ 按 NUL 自行算长度 ---
    {
        wchar_t in[] = { 0x2026, 0 };
        wmemcpy(b, in, 2);
        int r = fallback_apply(b, -1, 64);
        check(r == 3, "len<0 按 NUL 算长度（1 → 3）");
        check(b[0] == '.' && b[1] == '.' && b[2] == '.', "len<0 时内容也改对了");
    }

    // --- 越界参数不崩 ---
    check(fallback_apply(NULL, 3, 64) == -1, "buf=NULL ⇒ -1");
    check(fallback_hits(-1) == 0 && fallback_hits(999) == 0, "越界规则下标 ⇒ 0");
    check(strcmp(fallback_rule_name(-1), "?") == 0, "越界规则名 ⇒ \"?\"");
}

// ================= 6. 真实词典（可选）=================
static void test_real_dict(const char* realPath)
{
    printf("\n[6] 真实词典 %s（只报统计，不参与成败）\n", realPath);
    int n = load_main(realPath);
    printf("  载入键数 = %d\n", n);
    const DictStats& s = stats();
    printf(
        "  成对块=%d 建键=%d 键池=%d/%d 译文池=%d/%d 跳过：无译文=%d 键含CJK=%d 键过长=%d 重复=%d 落'?'=%d\n",
        s.main_pairs, s.keys, s.keypool_used, DICT_KEYPOOL, s.valpool_used, DICT_VALPOOL, s.main_noval,
        s.main_cjk, s.main_keylong, s.main_dup, s.decode_qmark);
    check(n > 0, "真实词典至少建出 1 个键");

    // ---- 省略号规整：U+2026 必须一个都不留在译文里 ----
    // 原版字体的 U+2026 字形是育碧占位图标（只有 title 自带该条目 ⇒ 查得到、
    // 不是回退到字形 0）。我们不接管那个码位，默认行为原样保留；
    // 只把译文里的 U+2026 换成三个 ASCII 点（英文原文本来就这么写）。
    // 真实词典侧只断言「规整确实在真实数据上发生了」——具体哪几条串留下
    // 取决于重复键去重等规则，不该把测试绑死在某一条上（规则本身由块9 夹具证明）。
    printf("  省略号规整改写 = %d 处\n", s.main_fallback[0]);
    check(s.main_fallback[0] > 0, "真实词典里确实有 U+2026 被改写（否则这条测试是空的）");
}

// ================= 9. 只有用户词典、没有 LG 主词典（必须最后跑）=================
// 这是「词典与原 LG 词典解耦」的判据
//   规格：用户给一份 `ENG<TAB>译文` 就行 —— 查到就替换，查不到留英文原文。
//   关键是不能再拿 LG 主词典（繁体中文）兜底，否则没翻的条目会露出中文。
//
// 必须最后跑：它调 init() 往全局表里塞键，还会覆盖同名键
//   （本文件早前的夹具已经把 Start Game 填成「開始遊戲」）。
//   放中间会污染后面那些按 before/after 差值对账的断言。
static void test_user_dict_only()
{
    printf("\n[9] 只有用户词典（无 LG 主词典）⇒ 仍就绪，且**不碰** LG 词典\n");
    char p[MAX_PATH];
    path_in_dir(p, sizeof(p), "AC1_Dict.txt");
    FILE* f = fopen(p, "wb");
    if (!f) {
        check(false, "写用户词典 AC1_Dict.txt", p);
        return;
    }
    fputs("Start Game\t\xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0\xe9\x96\x8b\xe5\xa7\x8b\n", f); // ゲーム開始
    // 译文里带真 U+2026（UTF-8 = E2 80 A6）。原版字体的这个码位上放着
    //   育碧的装饰图标（只有 title 自带该条目 ⇒ 是查得到、不是回退字形 0），
    //   所以必须改译文侧、且不接管那个码位。
    fputs("Load Map\t\xe8\xbd\xbd\xe5\x85\xa5\xe2\x80\xa6\n", f); // 载入…
    fputs("Exit\t\xe7\xb5\x82\xe4\xbb\xbb\n", f);                 // 終了
    fclose(f);

    bool ok = init(g_dir);
    check(ok == true, "只有 AC1_Dict.txt、没有 LGCStringDict_01.txt ⇒ **仍然就绪**");
    check(main_path()[0] == 0, "main_path() 为空 ⇒ 本轮**没读** LG 主词典");
    check(supp_path()[0] != 0, "supp_path() 指向用户词典");

    DictVal r = lookup(L"Start Game", 10);
    check(r.p != NULL, "用户词典里的键能查到");
    if (r.p) check(r.len == 5 && r.p[0] == 0x30B2 && r.p[1] == 0x30FC, "译文就是日语那一份（ゲーム開始）");

    // 回归：这条路径就是权威部署形态；兜底规整必须在它上面也执行
    //   （U+2026 码位上是育碧的装饰图标，不规整会画出图标）。
    //   断言取回的是三个 ASCII 点，且长度按展开后的 5 算（入池算术必须用 vlen2）。
    {
        // 键是纯 ASCII（游戏送来的英文串本来如此），U+2026 只出现在译文里。
        //   键侧走 expand_ascii（逐字节 1:1），译文侧走 MultiByteToWideChar(CP_UTF8) ——
        //   两条路不同，夹具不能拿非 ASCII 键去试。
        DictVal r2 = lookup(L"Load Map", 8);
        check(r2.p != NULL, "带 U+2026 译文的那条能查到");
        if (r2.p) {
            check(r2.len == 5, "展开后长度 3→5（入池用的是 vlen2，不是原长）");
            check(r2.len == 5 && r2.p[0] == 0x8F7D && r2.p[1] == 0x5165 && // 载入
                      r2.p[2] == '.' && r2.p[3] == '.' && r2.p[4] == '.',
                  "译文里的 U+2026 已换成三个 ASCII 点（游戏里就不会画成图标）");
        }
        check(stats().sup_fallback[0] > 0, "兜底规整在**用户词典**这条路径上确实执行了");
    }
    // 词典里没有的 ⇒ 保持英文（而不是回退成中文）
    check(lookup(L"Some String Nobody Translated", 28).p == NULL,
          "词典里没有的键查不到 ⇒ 运行期保留**英文原文**");

    DeleteFileA(p);
}

// ================= 11. 以 `#` 开头的键（游戏按键提示串）=================
// 判据的回归：「buf[a]=='#' 就是注释」会把真实数据当注释吞了。
//   游戏的 UI 串里有一批以 `#` 开头的（实测 106 条 / 占 7%）：
//       # Press ½ to select your Throwing Knives
//       # Ability Lost: Defense Break …
//   新判据：数据行必有 TAB，注释行永远没有。
static void test_hash_prefixed_keys()
{
    printf("\n[11] 以 `#` 开头的键：是数据，不是注释\n");
    char p[MAX_PATH];
    path_in_dir(p, sizeof(p), "AC1_Dict.txt");
    FILE* f = fopen(p, "wb");
    if (!f) {
        check(false, "写 AC1_Dict.txt", p);
        return;
    }
    fputs("# 这是真注释（无 TAB）\n", f);
    fputs("# Press Half to select your Throwing Knives\t", f);
    put_utf8(f, L"按左鍵選投擲飛刀");
    fputs("\n", f);
    fputs("# Ability Lost: Dodge\t", f);
    put_utf8(f, L"失去能力：閃避");
    fputs("\n", f);
    fputs("# 后面没有 TAB 的这一行才算注释\n", f);
    fputs("NormalKey\t正常條目\n", f);
    fclose(f);

    ac1::dict::reset();
    bool ok = init(g_dir);
    check(ok, "词典就绪");
    check(lookup_is(L"# Press Half to select your Throwing Knives", L"按左鍵選投擲飛刀"),
          "★ `#` 开头 + 有 TAB ⇒ **当数据**（`#` 且无 TAB 才当注释）");
    check(lookup_is(L"# Ability Lost: Dodge", L"失去能力：閃避"), "★ 同上，第二条");
    check(lookup_is(L"NormalKey", L"正常條目"), "普通键不受影响");
    check(lookup(L"# This is a real comment", -1).p == NULL ||
              lookup(L"# 这是真注释（无 TAB）", -1).p == NULL,
          "无 TAB 的 `#` 行仍然算注释（没被误当数据）");

    DeleteFileA(p);
}

// ================= 12. 译文转义：\n \t \\ \xNNNN =================
// 实测 LG 词典 2,992 个块里 529 个含反斜杠，全是 `\xNNNN`
//   （按键图标 `\x00A4`=¤、字面 `‰`=U+2030、控制码）。
//   游戏存多行文本用的是真实 LF，所以词典里的 `\n` 必须在写进游戏 wstring
//   之前变成真换行，否则两行标签被折成一行（排版丢失）。
//   顺序陷阱：`\\` 必须最先判，否则 `\n` 会被读成「反斜杠 + n」，
//   于是想写字面 `\n` 的人反而拿到换行。
static void test_value_escapes()
{
    printf("\n[12] 译文转义：\\n / \\t / \\\\ / \\xNNNN\n");
    char p[MAX_PATH];
    path_in_dir(p, sizeof(p), "AC1_Dict.txt");
    FILE* f = fopen(p, "wb");
    if (!f) {
        check(false, "写 AC1_Dict.txt", p);
        return;
    }
    // C++ 字面量里三者别混：`\t` = 真 TAB（分隔键与译文）；
    //   `\\n` = 「反斜杠 + n」——这才是要交给加载器解的转义；
    //   行尾的 `\n` 才是本行的换行符。
    fputs("EscNewline\tline1\\nline2\n", f);
    fputs("EscTab\ta\\tb\n", f);
    fputs("EscBackslash\tpath\\\\to\\\\file\n", f);
    fputs("EscHex\tA\\x00A4B\n", f);
    // 文件里写 x\\ny（两个反斜杠）⇒ 还原成 x + 反斜杠 + n + y，不是换行
    fputs("EscLiteralBsN\tx\\\\ny\n", f);
    fclose(f);

    ac1::dict::reset();
    init(g_dir);

    {
        DictVal v = lookup(L"EscNewline", (int)wcslen(L"EscNewline"));
        check(v.p && v.len == 11 && v.p[5] == L'\n' && v.p[6] == L'l',
              "`\\n` ⇒ **真换行**（否则多行标签排版会丢）");
    }
    {
        DictVal v = lookup(L"EscTab", (int)wcslen(L"EscTab"));
        check(v.p && v.len == 3 && v.p[1] == L'\t', "`\\t` ⇒ 真制表符");
    }
    {
        DictVal v = lookup(L"EscBackslash", (int)wcslen(L"EscBackslash"));
        // 值应是  path\to\file  = 13 码元
        check(v.p && v.len == 12 && v.p[4] == L'\\' && v.p[5] == L't',
              "`\\\\` ⇒ 字面反斜杠（path\\to\\file）");
    }
    {
        DictVal v = lookup(L"EscHex", (int)wcslen(L"EscHex"));
        check(v.p && v.len == 3 && v.p[1] == 0x00A4, "`\\xNNNN` ⇒ 指定码元（U+00A4，按键图标）");
    }
    {
        DictVal v = lookup(L"EscLiteralBsN", (int)wcslen(L"EscLiteralBsN"));
        // 文件里是 x \ \ n y（两个反斜杠）⇒ `\\` 先判、还原成一个反斜杠，
        // 于是结果是 x + 反斜杠 + n + y = 4 个码元，里面没有换行。
        // （若 `\n` 先判，这里会拿到换行 ⇒ 顺序陷阱就穿帮了。）
        check(v.p && v.len == 4 && v.p[1] == L'\\' && v.p[2] == L'n' && v.p[3] == L'y',
              "`\\\\n` ⇒ 4 码元 x\\ny，**不是换行**");
    }

    DeleteFileA(p);
}

// ================= 13. 键列的 UTF-8 解码 =================
// 守的是真机确认过的匹配失效：AC1_Dict.txt 键列里一个 UTF-8 的「ƒ」
//   （C6 92，2 字节）若按「每字节一个码元」装成 U+00C6 U+0092，而游戏发过来的是
//   一个 U+0192 ⇒ canon + 精确相等必然判不等 ⇒ 译文在、却一直「未命中」。
// 键列与译文列走同一套两步（UTF-8 解码 + unescape_value），所以游戏
//   自己的 `\xNNNN` 约定必须仍然有效 —— 两条写进同一个夹具，一起守。
//   `\/` 也一起钉住：反斜杠后不是那几个字符就原样透传。
static void test_key_column_utf8()
{
    printf("\n[13] 键列：UTF-8 多字节 + \\xNNNN 转义 + \\/ 透传\n");
    char p[MAX_PATH];
    path_in_dir(p, sizeof(p), "AC1_Dict.txt");
    FILE* f = fopen(p, "wb");
    if (!f) {
        check(false, "写 AC1_Dict.txt", p);
        return;
    }
    // 键列一律用 C 字节转义写：那就是被测的输入。用宽字面量往返一遍
    //   会把待修的 bug 一起转成正确的 UTF-8，等于什么都没测。
    fputs("Move \xC6\x92 to WALK.\t", f);
    put_utf8(f, L"走");
    fputs("\n", f);
    fputs("Alta\xC3\xAFr\t", f);
    put_utf8(f, L"高地");
    fputs("\n", f);
    fputs("Esc \\x0192 Key\t", f);
    put_utf8(f, L"转义键");
    fputs("\n", f); // 文件里是 6 个字面字符
    fputs("%1% \\/ %2% Saved\t", f);
    put_utf8(f, L"已保存");
    fputs("\n", f);
    fputs("Plain ASCII Key\t", f);
    put_utf8(f, L"纯ASCII");
    fputs("\n", f);
    fputs("Bad \xFF Key\t", f);
    put_utf8(f, L"不该出现");
    fputs("\n", f);
    fclose(f);

    ac1::dict::reset();
    bool             ok = init(g_dir);
    const DictStats& st = stats();
    char             d[128];
    _snprintf(d, sizeof(d), "实际 lines=%d new=%d keybad=%d", st.sup_lines, st.sup_new, st.sup_keybad);

    check(ok, "词典就绪", d);
    // THE bug：文件里 2 字节，键表里必须是 1 个 U+0192
    check(lookup_is(L"Move \x0192 to WALK.", L"走"),
          "★ 键列的 UTF-8 多字节 `ƒ` ⇒ 1 个 U+0192（拆成 2 个码元则永不命中）", d);
    // 拆成相邻字面量：C 的 `\x` 贪吃十六进制，贴着后面的字母最容易看走眼
    check(lookup_is(L"Alta\x00EF"
                    L"r",
                    L"高地"),
          "2 字节 UTF-8（É）⇒ 1 个 U+00EF", d);
    // 游戏的 `\xNNNN` 约定不能因为改解码器就失效
    {
        const wchar_t kEsc[] = { L'E', L's', L'c', L' ', (wchar_t)0x0192, L' ', L'K', L'e', L'y', 0 };
        check(lookup_is(kEsc, L"转义键"), "★ 键列的 `\\xNNNN` 转义**仍然**展开成 1 个码元", d);
    }
    check(lookup_is(L"%1% \\/ %2% Saved", L"已保存"), "`\\/` 不是转义 ⇒ 反斜杠原样保留", d);
    check(lookup_is(L"Plain ASCII Key", L"纯ASCII"), "纯 ASCII 键行为不变", d);
    check(st.sup_keybad == 1, "非法 UTF-8 的键被跳过并计入 sup_keybad", d);
    // 坏键若被「替换符兜底」放进来，这里会查到 "不该出现" —— 断言它压根没进表
    check(LK(L"Bad \xFFFD Key").p == NULL, "坏键那一行没进键表（占不到键槽）", d);

    DeleteFileA(p);
}

int main(int argc, char** argv)
{
    set_log(&test_log_sink); // 注入日志出口（dict 不依赖 core）
    exe_dir();
    SetConsoleOutputCP(65001); // 终端按 UTF-8 打印中文

    printf("ac1-chinese-translate 词典层单测（匹配 = canon + 精确相等，无模糊兜底）\n");
    printf("工作目录: %s\n", g_dir);

    test_log_sink(); // 日志出口注入点
    test_dict_init_missing();
    test_deploy_shapes(); // 必须在任何 load_* 之前：表无重置 API，被污染就查不准
    test_hash_prefixed_keys();
    test_value_escapes();   // 必须最先跑：此刻词典表还是空的
    test_key_column_utf8(); // 键列 UTF-8 / \xNNNN 两步，与译文列同源
    test_canon();
    test_main_dict();
    test_main_pairing();
    test_big5_trail5c();
    test_supplement();
    test_bom();
    test_length_limits();
    test_case_sensitivity();
    test_lookup_bounds();
    test_fallback();
    if (argc > 1) test_real_dict(argv[1]);
    test_user_dict_only();   // 必须最后：会覆盖同名键
    test_pool_full_status(); // 会把值池填满 ⇒ 排在所有还需要入池的用例之后

    char p1[MAX_PATH], p2[MAX_PATH];
    path_in_dir(p1, sizeof(p1), "_dict_ut_main.txt");
    path_in_dir(p2, sizeof(p2), "_dict_ut_supp.txt");
    DeleteFileA(p1);
    DeleteFileA(p2);

    printf("\n==== 通过 %d / 失败 %d ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
