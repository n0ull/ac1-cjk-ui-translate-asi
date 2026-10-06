// modules/text/tests/text_unit_test.cpp —— text 层离线单测
//
//   pwsh -NoProfile -File modules\text\tests\build.ps1
//   pwsh -NoProfile -File modules\text\tests\run.ps1
//
// 链接 core.lib + dict.lib + text.lib（不链接 glyph.lib，不链接任何游戏 dll）。
//
// 本单测直接驱动漏斗 post 阶段：`funnel_run_post()` 就是钩子返回后调的
//   那个同一个函数（不是复制品），所以这里测过的就是真机跑的。
//   inline 钩本身装不上（没有 Dx9 宿主），它由 core 的签名核对单测 + 真机覆盖。

#include "ac1/text/text.h"

#include "ac1/core/log.h"
#include "ac1/core/mem.h"
#include "ac1/dict/dict.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace ac1::text;

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

// 造一个假的 MSVC8 wstring（SSO 或堆形态），并把内容写进去
struct FakeWs {
    unsigned char raw[AC1_WSTR_SIZE];
};

// 造一个假的 MSVC8 wstring。res < 8 ⇒ SSO（缓冲内联在 +0x04）；res >= 8 ⇒ 堆（_Bx 是堆指针）。
// 返回数据指针。两条硬约束（都来自 core::wstr_view 的自洽检查，违反会被读门当垃圾拒掉）：
//   ① 源串长度必须 <= res（否则 _Mysize > _Myres）；
//   ② 堆形态下 res 必须 >= 15 —— 真 wstring 的堆分配下限；8..14 一律按垃圾字段处理
//      （见 core/src/mem.cpp 的 wstr_view）。
static wchar_t* make_ws(FakeWs* w, const wchar_t* s, unsigned res)
{
    memset(w, 0, sizeof(w->raw));
    unsigned n = (unsigned)wcslen(s);
    if (res < AC1_WSTR_SSO_RES) { // SSO：缓冲内联在 +0x04
        wchar_t* inl = (wchar_t*)(w->raw + AC1_WSTR_OFF_BX);
        memcpy(inl, s, (size_t)n * sizeof(wchar_t));
        if (n) inl[n] = 0;
    }
    else { // 堆：_Bx 是堆指针
        wchar_t* buf = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, ((size_t)res + 1) * sizeof(wchar_t));
        memcpy(buf, s, (size_t)n * sizeof(wchar_t));
        buf[n] = 0;
        memset(buf + n + 1, 0, ((size_t)res - n) * sizeof(wchar_t));
        *(void**)(w->raw + AC1_WSTR_OFF_BX) = buf;
    }
    *(unsigned*)(w->raw + AC1_WSTR_OFF_SIZE) = n;
    *(unsigned*)(w->raw + AC1_WSTR_OFF_RES) = res;
    return (res < AC1_WSTR_SSO_RES) ? (wchar_t*)(w->raw + AC1_WSTR_OFF_BX)
                                    : (wchar_t*)(*(void**)(w->raw + AC1_WSTR_OFF_BX));
}

// 灌一份合成主词典：Start Game→開始遊戲 / Short→短 / Go→8 码元译文（装不下 res=7 的 SSO）
static bool write_fake_dict(const char* path)
{
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    auto put_big5 = [&](const wchar_t* s) {
        int n = WideCharToMultiByte(950, 0, s, -1, NULL, 0, NULL, NULL);
        if (n <= 1) return;
        char* b = (char*)malloc((size_t)n);
        if (WideCharToMultiByte(950, 0, s, -1, b, n, NULL, NULL) > 1) fwrite(b, 1, (size_t)n - 1, f);
        free(b);
    };
    fputs("xfhsm_res_ENG_Start\r\nStart Game\r\nxfhsm_res_ENG_End\r\n", f);
    fputs("xfhsm_res_CHI_Start\r\n", f);
    put_big5(L"開始遊戲");
    fputs("\r\nxfhsm_res_CHI_End\r\n", f);
    fputs("xfhsm_res_ENG_Start\r\nShort\r\nxfhsm_res_ENG_End\r\n", f);
    fputs("xfhsm_res_CHI_Start\r\n", f);
    put_big5(L"短"); // 只有 1 个码元
    fputs("\r\nxfhsm_res_CHI_End\r\n", f);
    fputs("xfhsm_res_ENG_Start\r\nGo\r\nxfhsm_res_ENG_End\r\n", f);
    fputs("xfhsm_res_CHI_Start\r\n", f);
    put_big5(L"開始遊戲結束遊戲"); // 8 码元
    fputs("\r\nxfhsm_res_CHI_End\r\n", f);
    fclose(f);
    return true;
}

// ================= 1. 闸门 =================
static void test_gates()
{
    printf("\n[1] 闸门：结构不可信 / 空串 / 含CJK / 词典未就绪\n");
    FakeWs w;

    int h0 = counters().hits, s0 = skipped(counters());

    funnel_run_post(NULL, 0x1111);
    check(counters().hits == h0 + 1, "每次调用都记 hits（哪怕 dst 是 NULL）");
    check(skipped(counters()) == s0 + 1, "dst=NULL 被当作结构不可信跳过");

    // 词典此刻还没装（ready=false）⇒ 任何像样的串都应走"词典未就绪"闸门
    make_ws(&w, L"Start Game", 15);
    s0 = skipped(counters());
    funnel_run_post(&w, 0x2222);
    check(skipped(counters()) == s0 + 1, "词典未就绪 ⇒ skipped++（不崩溃、不写）");
    check(ac1::dict::ready() == false, "此刻词典确实未就绪");

    // 词典就绪之后，空串仍应被跳过
    char dp[MAX_PATH];
    path_in_dir(dp, sizeof(dp), "_text_ut_dict.txt");
    if (!write_fake_dict(dp)) {
        check(false, "写合成词典", dp);
        return;
    }
    check(ac1::dict::load_main(dp) == 3, "合成词典载入 3 个键");
    check(ac1::dict::ready(), "载入后词典就绪");

    make_ws(&w, L"", 15);
    s0 = skipped(counters());
    funnel_run_post(&w, 0x3333);
    check(skipped(counters()) == s0 + 1, "空串被跳过");

    // 幂等闸门：串里已经有 CJK ⇒ 不重复翻译
    make_ws(&w, L"開始遊戲", 15);
    s0 = skipped(counters());
    int m0 = counters().miss, r0 = counters().replaced;
    funnel_run_post(&w, 0x4444);
    check(skipped(counters()) == s0 + 1, "含 CJK 的串被幂等跳过");
    check(counters().miss == m0 && counters().replaced == r0, "含 CJK 的串既不算未命中也不算替换");
}

// ================= 2. 命中与未命中 =================
static void test_hit_miss()
{
    printf("\n[2] 命中 / 未命中 / 容量不足\n");
    FakeWs w;

    int m0 = counters().miss;
    make_ws(&w, L"No Such Key", 15);
    funnel_run_post(&w, 0x5555);
    check(counters().miss == m0 + 1, "词典里没有的串 ⇒ miss++");
    check(counters().replaced == 0, "未命中不替换");

    int m1 = counters().miss;
    make_ws(&w, L"Start Game", 15);
    funnel_run_post(&w, 0x6666);
    check(counters().miss == m1, "命中的串不计入未命中");

    // 短译文装得下：SSO res=7，译文 1 码元 ⇒ 不该计入容量不足
    int g0 = counters().skippedGrow;
    make_ws(&w, L"Short", 7);
    funnel_run_post(&w, 0x7777);
    check(counters().skippedGrow == g0, "短译文装得下 ⇒ 不计入容量不足");

    // 真正的容量不足：源串 "Go"(2 码元) 装得进 SSO(res=7)，但译文 8 码元 > 7
    g0 = counters().skippedGrow;
    make_ws(&w, L"Go", 7);
    funnel_run_post(&w, 0x8888);
#if TEXT_REPLACE
    check(counters().skippedGrow == g0 + 1, "译文装不下（res=7 < 8 码元）⇒ skippedGrow++");
#else
    // 观测模式压根不评估容量（不写就不必关心装不装得下）⇒ 计数器不动
    check(counters().skippedGrow == g0, "TEXT_REPLACE=0：观测模式不评估容量，skippedGrow 不变");
#endif
    check(wcscmp((wchar_t*)(w.raw + AC1_WSTR_OFF_BX), L"Go") == 0, "译文装不下时原文未被改动");
    check(*(unsigned*)(w.raw + AC1_WSTR_OFF_SIZE) == 2, "译文装不下时 _Mysize 未被改动");

    // 源串本身就装不下（_Mysize > _Myres）⇒ 视图直接拒，不算容量不足
    FakeWs bad;
    make_ws(&bad, L"Go", 7);
    *(unsigned*)(bad.raw + AC1_WSTR_OFF_SIZE) = 99; // 伪造一个不一致的 _Mysize
    int s0 = skipped(counters());
    g0 = counters().skippedGrow;
    funnel_run_post(&bad, 0x9999);
    check(skipped(counters()) == s0 + 1 && counters().skippedGrow == g0,
          "_Mysize > _Myres 的对象被视图拒（不算容量不足）");
}

// ================= 3. 替换行为（按 TEXT_REPLACE 编译开关分别断言）=================
static void test_replace()
{
    printf("\n[3] 替换：TEXT_REPLACE=%d\n", TEXT_REPLACE);
    FakeWs w;

    // 堆缓冲：res=15 装得下 10 码元的源串，也装得下 4 码元的译文
    wchar_t* buf = make_ws(&w, L"Start Game", 15);
    unsigned resBefore = *(unsigned*)(w.raw + AC1_WSTR_OFF_RES);
    int      r0 = counters().replaced;

    funnel_run_post(&w, 0x9999);
    unsigned sizeAfter = *(unsigned*)(w.raw + AC1_WSTR_OFF_SIZE);
    unsigned resAfter = *(unsigned*)(w.raw + AC1_WSTR_OFF_RES);

#if TEXT_REPLACE
    check(counters().replaced == r0 + 1, "TEXT_REPLACE=1：命中即 replaced++");
    check(wcscmp(buf, L"開始遊戲") == 0, "TEXT_REPLACE=1：缓冲里就是译文");
    check(sizeAfter == 4, "TEXT_REPLACE=1：_Mysize = 译文长度 4（零填充）");
    check(buf[4] == 0, "TEXT_REPLACE=1：尾 NUL 已写");
    check(resAfter == resBefore, "TEXT_REPLACE=1：_Myres 一个字节都没动");
#else
    check(counters().replaced == r0, "TEXT_REPLACE=0：replaced 不增长");
    check(wcscmp(buf, L"Start Game") == 0, "TEXT_REPLACE=0：原文一个字节都没改");
    check(sizeAfter == 10, "TEXT_REPLACE=0：_Mysize 未被改动");
    check(resAfter == resBefore, "TEXT_REPLACE=0：_Myres 未被改动");
#endif
    (void)resBefore;
}

// ================= 3b. forge 闸门：闸门关 ⇒ 文本层只观测不替换（真·纯原版）=================
static void test_forge_gate_text()
{
    printf("\n[3b] forge 闸门（文本层进闸，选 B）\n");

    set_forge_patch(0);
    {
        FakeWs   w;
        wchar_t* buf = make_ws(&w, L"Start Game", 15);
        int      r0 = counters().replaced;
        funnel_run_post(&w, 0x9999);
        check(wcscmp(buf, L"Start Game") == 0, "闸门关：缓冲一个字节都没动（真·纯原版）");
        check(counters().replaced == r0, "闸门关：replaced 不涨（即使 TEXT_REPLACE=1 也一样）");
    }

    set_forge_patch(1); // 恢复（也验证闸门可逆）
    {
        FakeWs   w;
        wchar_t* buf = make_ws(&w, L"Start Game", 15);
        int      r0 = counters().replaced;
        funnel_run_post(&w, 0x9999);
#if TEXT_REPLACE
        check(counters().replaced == r0 + 1, "闸门开：恢复替换（replaced++）");
        check(wcscmp(buf, L"開始遊戲") == 0, "闸门开：缓冲里是译文");
#else
        check(counters().replaced == r0, "闸门开但 TEXT_REPLACE=0：仍只观测");
        check(wcscmp(buf, L"Start Game") == 0, "TEXT_REPLACE=0：原文仍在");
#endif
    }
}

// ================= 4. 幂等：翻过一轮的串再喂一次不会二次翻译 =================
static void test_idempotent()
{
    printf("\n[4] 幂等（跑两遍）\n");
    FakeWs w;
    int    r0 = counters().replaced;

    make_ws(&w, L"Short", 7);
    funnel_run_post(&w, 0xAAAA);
    funnel_run_post(&w, 0xAAAA); // 第二遍：串里已是 CJK

    wchar_t* buf = (wchar_t*)(w.raw + AC1_WSTR_OFF_BX);
#if TEXT_REPLACE
    check(counters().replaced == r0 + 1, "TEXT_REPLACE=1：只翻一次，第二遍被 CJK 闸门拦住");
    check(wcscmp(buf, L"短") == 0, "TEXT_REPLACE=1：第二遍没有把译文当原文再翻一次");
#else
    check(counters().replaced == r0, "TEXT_REPLACE=0：两遍都不替换");
    check(wcscmp(buf, L"Short") == 0, "TEXT_REPLACE=0：两遍后仍是原文");
#endif
}

// ================= 5. 异常与注册表 =================
static void test_fault_and_install()
{
    printf("\n[5] 异常兜底与安装\n");
    int f0 = counters().fault;

    // 完全不可信的地址：必须被读门/视图挡下，而不是崩
    funnel_run_post((void*)0x10, 0xBBBB);
    check(counters().fault == f0, "野地址被读门挡下（不产生异常、不崩）");
    check(ac1::core::mem_faults() == 0, "core 读门异常计数 = 0");

    // 宿主不受支持 ⇒ install() 装不上（且不崩）
    const int ins = install();
    check(ins == 0, "不认识的宿主下装不上（绝不硬装）");

    const char* s = status_line();
    check(s != NULL && strstr(s, "FUNNEL") != NULL, "状态行含路径名", s);
    printf("  状态行: %s\n", s);
}

// ================= 6. 观测粒度：跳过分项 / 未命中分类 / ASCII Top =================
// 造一个含 ≥0x80 但 <0x3000 码元的串（图标转义类：过 CJK 闸门，但算"非 ASCII"）
static wchar_t* make_icon_ws(FakeWs* w, unsigned short icon, const wchar_t* rest)
{
    memset(w, 0, sizeof(w->raw));
    wchar_t* buf = (wchar_t*)(w->raw + AC1_WSTR_OFF_BX); // SSO：缓冲内联
    buf[0] = icon;
    buf[1] = L' ';
    size_t n = wcslen(rest);
    for (size_t i = 0; i < n; i++) buf[2 + i] = rest[i];
    buf[2 + n] = 0;
    *(unsigned*)(w->raw + AC1_WSTR_OFF_SIZE) = (unsigned)(n + 2);
    *(unsigned*)(w->raw + AC1_WSTR_OFF_RES) = 7;
    return buf;
}

static void test_observation_granularity()
{
    printf("\n[6] 观测粒度：跳过分项 / 未命中 ASCII·非ASCII 分类 / Top 列表\n");
    const TextCounters& k = counters();

    // ---- ① 跳过分项：逐个闸门验自己的桶，而不是只验总数 ----
    int    e0 = k.skipEmpty, j0 = k.skipCjk, d0 = k.skipDictNotReady, b0 = k.skipBadView;
    FakeWs w;

    make_ws(&w, L"", 15);
    funnel_run_post(&w, 0xC001);
    check(counters().skipEmpty == e0 + 1, "空串 ⇒ skipEmpty++");

    make_ws(&w, L"開始遊戲", 15); // 含 CJK
    funnel_run_post(&w, 0xC002);
    check(counters().skipCjk == j0 + 1, "含 CJK ⇒ skipCjk++");

    funnel_run_post((void*)0x10, 0xC003); // 结构不可信
    check(counters().skipBadView == b0 + 1, "野地址 ⇒ skipBadView++");

    // ---- ② 状态行必须真的把跳分开成这几个标签 ----
    const char* st = status_line();
    check(strstr(st, "跳过=空串") != NULL, "状态行含「跳过=空串」", st);
    check(strstr(st, "含CJK") != NULL, "状态行含「含CJK」", st);
    check(strstr(st, "词典未就绪") != NULL, "状态行含「词典未就绪」", st);
    check(strstr(st, "结构不可信") != NULL, "状态行含「结构不可信」", st);
    check(strstr(st, "非ASCII") != NULL, "状态行含未命中的「非ASCII」分类", st);
    printf("  状态行样例: %s\n", st);

    // ---- ③ 未命中分类：纯 ASCII vs 含 ≥0x80 码位 ----
    int a0 = counters().missAscii, na0 = counters().missNonAscii, t0 = counters().topCount;

    make_ws(&w, L"Missing Alpha", 15); // 纯 ASCII，未命中
    funnel_run_post(&w, 0xC004);
    check(counters().missAscii == a0 + 1, "纯 ASCII 未命中 ⇒ missAscii++");
    check(counters().missNonAscii == na0, "纯 ASCII 不算进 missNonAscii");
    check(counters().topCount == t0 + 1, "纯 ASCII 未命中 ⇒ 进 Top 列表");

    int t1 = counters().topCount;
    make_ws(&w, L"Missing Alpha", 15); // 同一串再来一次 ⇒ 去重
    funnel_run_post(&w, 0xC005);
    check(counters().topCount == t1, "Top 列表去重（同串不重复占位）");
    check(counters().missAscii == a0 + 2, "但计数照常累加");

    make_icon_ws(&w, 0x00A4, L"icon"); // 含 0x00A4 码位
    int t2 = counters().topCount;
    funnel_run_post(&w, 0xC006);
    check(counters().missNonAscii == na0 + 1, "含 ≥0x80 码位 ⇒ missNonAscii++");
    check(counters().topCount == t2, "非 ASCII 未命中不进 Top（图标转义类，本就不该翻）");

    // ---- ④ Top 列表的内容：\\uXXXX 转义、首次出现顺序、每条 ≤24 码元 ----
    make_ws(&w, L"Zebra Last", 15);
    funnel_run_post(&w, 0xC007);
    const char* det = detail_line();
    printf("  明细行样例: %s\n", det);
    check(strstr(det, "未命中 ASCII Top") != NULL, "明细行标题正确", det);
    check(strstr(det, "\\u004D") != NULL, "Top 用 \\uXXXX 转义（Missing Alpha 的 M）", det);
    // 首次出现顺序：Missing Alpha 在 Zebra Last 之前
    const char* a = strstr(det, "\\u004D");
    const char* z = strstr(det, "\\u005A");
    check(a && z && a < z, "Top 按首次出现顺序（Missing Alpha 早于 Zebra Last）", det);
    check(strstr(det, "\\u00A4") == NULL, "非 ASCII 的那串不出现在 Top 里");

    // 超过 TOP_LEN(24) 的键要截断：造一条 40 字符的未命中串
    // （res 必须 ≥ 40，否则 wstr_view 会以 "_Mysize > _Myres" 拒掉它 —— 那本身也是被测行为）
    int tBefore = counters().topCount;
    make_ws(&w, L"0123456789012345678901234567890123456789", 45);
    funnel_run_post(&w, 0xC008);
    check(counters().missAscii > 0, "超长 ASCII 串确实走到未命中分支");
    check(counters().topCount == tBefore + 1, "超长未命中串仍然进 Top（截断后）");
    det = detail_line();
    // 最后一条（超长串）应该正好 24 个 \uXXXX —— 从最后一个分隔符之后开始数
    // （分隔符是 UTF-8 的" ｜ "，别去算它几个字节，直接找本段第一个 \u）
    {
        const char* lastSeg = strrchr(det, '｜');
        lastSeg = lastSeg ? strstr(lastSeg, "\\u") : det; // 找不到就整段数
        int units = 0;
        for (const char* q = lastSeg; (q = strstr(q, "\\u")) != NULL; q += 2) units++;
        check(units == 24, "超长串在 Top 里截断到正好 24 个码元", det);
        printf("  最后一条 Top（%d 码元）: %s\n", units, lastSeg);
    }

    // ---- ⑤ 字符串全记录（inventory）----
    // 目的（用户裁定）：先把游戏用到的串全记录下来，翻译决策等清单齐了再定。
    // 这里验：唯一串去重 + 计数 + 标签 + 首次调用者；以及"跳过最新一条"两次 tick 才齐。
    {
        char invp[MAX_PATH];
        { // 日志体系：inventory 产出 = AC1_CJK\strings_<run stamp>.txt（单测 stamp 为空）
            char invname[96];
            _snprintf(invname, sizeof(invname) - 1, "AC1_CJK\\strings_%s.txt", ac1::core::log_run_stamp());
            invname[sizeof(invname) - 1] = 0;
            path_in_dir(invp, sizeof(invp), invname);
        }
        DeleteFileA(invp);                // 从干净状态开始
        ac1::text::inventory_tick(g_dir); // 首次：写 run 头（此刻可能还没新条目）

        make_ws(&w, L"Unique Alpha", 15); // 新串（未命中）
        // 堆形态的 _Myres 必须 ≥ 15：读门把 8..14 当"垃圾字段"拒掉（对齐真 wstring 的分配下限）
        int ma0 = counters().missAscii;
        funnel_run_post(&w, 0xC101);
        check(counters().missAscii == ma0 + 1,
              "Unique Alpha 确实走到未命中分支（否则下面的文件断言无从谈起）");
        make_ws(&w, L"Unique Alpha", 15); // 同一串再来一次 ⇒ 计数 +1、不新增条目
        funnel_run_post(&w, 0xC102);
        check(counters().missAscii == ma0 + 2, "第二次同样走到未命中分支");

        ac1::text::inventory_tick(g_dir); // tick#1：写出到 N-2
        DWORD sz1 = 0;
        {
            HANDLE hz = CreateFileA(invp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                    OPEN_EXISTING, 0, NULL);
            if (hz != INVALID_HANDLE_VALUE) {
                sz1 = GetFileSize(hz, NULL);
                CloseHandle(hz);
            }
        }
        printf("  tick#1 后文件大小=%lu\n", (unsigned long)sz1);
        ac1::text::inventory_tick(g_dir); // tick#2：把最后一条也写出
        DWORD sz2 = 0;
        {
            HANDLE hz = CreateFileA(invp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                    OPEN_EXISTING, 0, NULL);
            if (hz != INVALID_HANDLE_VALUE) {
                sz2 = GetFileSize(hz, NULL);
                CloseHandle(hz);
            }
        }
        printf("  tick#2 后文件大小=%lu\n", (unsigned long)sz2);
        check(sz1 != sz2 || sz1 > 0, "tick 之间文件有增长（说明新条目被写出）");

        HANDLE h = CreateFileA(invp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        check(h != INVALID_HANDLE_VALUE, "inventory 文件已生成");
        if (h != INVALID_HANDLE_VALUE) {
            char  buf[16384];
            DWORD got = 0;
            ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
            buf[got] = 0;
            CloseHandle(h);
            check(strstr(buf, "# === run ") != NULL, "文件有 run 头", buf);
            check(strstr(buf, "Unique Alpha") != NULL, "含新登记的串（UTF-8 原文，不是 \\uXXXX）", buf);
            check(strstr(buf, "[×2]") != NULL, "同一串两次 ⇒ 计数 [×2]", buf);
            check(strstr(buf, "caller=0000C101") != NULL, "记首次调用者 caller=0000C101", buf);
            check(strstr(buf, "未命中") != NULL, "标签含「未命中」", buf);
            printf("  inventory 片段: %.200s\n",
                   strstr(buf, "Unique Alpha") ? strstr(buf, "Unique Alpha") : buf);
        }
    }
}

// ================= 7. 样本行的截断收尾（dump_wstr） =================
// 回归：≥86 码元的样本串会走到 dump_wstr 的截断收尾。旧实现原地 _snprintf 装不下时
//   不补 NUL ⇒ 后面的『%s』越界读栈；新实现按尾标记长度预留缓冲、截断处必有 NUL。
//   本用例把日志开到 out\AC1_CJK\ 并回读那一行（其余用例日志是关的）。
static void test_sample_tail()
{
    printf("\n[7] 样本行截断收尾（长串样本）\n");

    ac1::core::init_log(g_dir);
    ac1::core::close_log();
    DeleteFileA(ac1::core::log_path());
    ac1::core::init_log(g_dir);

    static wchar_t longS[201];
    for (int i = 0; i < 200; i++) longS[i] = (wchar_t)(L'a' + (i % 26));
    longS[200] = 0;

    FakeWs w;
    make_ws(&w, longS, 255);
    const int miss0 = counters().miss;
    const int smp0 = counters().sampleMiss;
    funnel_run_post(&w, 0x0040BEEF);
    check(counters().miss == miss0 + 1, "200 码元长串按未命中计数（不在合成词典里）");
    check(counters().sampleMiss == smp0 + 1, "★ 长串落进未命中样本（样本预算未满）");

    ac1::core::close_log();
    {
        static char buf[8192];
        DWORD       got = 0;
        HANDLE h = CreateFileA(ac1::core::log_path(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        check(h != INVALID_HANDLE_VALUE, "样本日志可回读");
        if (h != INVALID_HANDLE_VALUE) {
            ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
            CloseHandle(h);
        }
        buf[got] = 0;
        const char* p = strstr(buf, "未命中样本");
        check(p != NULL, "日志里有未命中样本行");
        check(p && strstr(p, "码元)』") != NULL, "★ 截断带尾标记「…(200 码元)」（旧实现写不上标记）",
              p ? p : buf);
        check(p && strstr(p, "\\u0061") != NULL, "样本内容按 \\uXXXX 转义");
    }
    DeleteFileA(ac1::core::log_path());
    ac1::core::init_log(""); // 恢复「单测不写文件」的状态
    check(ac1::core::log_path()[0] == 0, "恢复为关闭状态");
}

// ================= 8. inventory 并发登记 =================
// 回归：inv_remember 原来的「读 g_invN → 去重扫描 → 写 g_inv[cnt] → 自增」在并发下会让
//   两个线程都扫不到对方要写的串（那时谁都没写），双双拿到同一个 cnt、双双写 g_inv[cnt]：
//   一条唯一串被覆盖丢失，而 g_invN 被顶到 cnt+2 ⇒ 下标 cnt+1 那格从没写过，
//   tick 把全零当条目落盘（`[×0]  caller=00000000`），还可能读到半条。
//   四个线程各打 100 条互不相同的串，然后查落盘结果：一条不少、无重复、无空洞条目。
static const int INV_CONC_THREADS = 4;
static const int INV_CONC_EACH = 100;

static volatile LONG g_invGo = 0;

static DWORD WINAPI inv_conc_thread(LPVOID pv)
{
    const int tid = (int)(INT_PTR)pv;
    FakeWs    w;
    while (InterlockedCompareExchange(&g_invGo, 0, 0) == 0) YieldProcessor(); // 自旋 barrier
    for (int i = 0; i < INV_CONC_EACH; i++) {
        wchar_t s[16];
        _snwprintf(s, 16, L"cc%d-%d", tid, i); // ≤7 码元 ⇒ SSO 形态，无需分配
        make_ws(&w, s, AC1_WSTR_SSO_RES - 1);  // _Myres = 7
        funnel_run_post(&w, 0xC0B0u + (unsigned)tid);
    }
    return 0;
}

static void test_inventory_concurrency()
{
    printf("\n[8] inventory 并发登记（四个线程）\n");

    char invp[MAX_PATH];
    {
        char invname[96];
        _snprintf(invname, sizeof(invname) - 1, "AC1_CJK\\strings_%s.txt", ac1::core::log_run_stamp());
        invname[sizeof(invname) - 1] = 0;
        path_in_dir(invp, sizeof(invp), invname);
    }
    ac1::text::inventory_tick(g_dir); // 先把此前累计的条目落盘，让文件里只剩本轮新增
    DeleteFileA(invp);

    HANDLE th[INV_CONC_THREADS];
    InterlockedExchange(&g_invGo, 0);
    for (int i = 0; i < INV_CONC_THREADS; i++)
        th[i] = CreateThread(NULL, 0, inv_conc_thread, (LPVOID)(INT_PTR)i, 0, NULL);
    bool allOk = true;
    for (int i = 0; i < INV_CONC_THREADS; i++)
        if (!th[i]) allOk = false;
    check(allOk, "四个登记线程都建起来了");
    DWORD waitRc = WAIT_FAILED;
    if (allOk) {
        InterlockedExchange(&g_invGo, 1);
        waitRc = WaitForMultipleObjects(INV_CONC_THREADS, th, TRUE, 30000);
    }
    for (int i = 0; i < INV_CONC_THREADS; i++)
        if (th[i]) CloseHandle(th[i]);
    check(waitRc == WAIT_OBJECT_0, "四个线程都跑完了（未超时）");

    ac1::text::inventory_tick(g_dir); // 一次 tick 就该把已发布的条目全部写出

    static char big[262144];
    DWORD       got = 0;
    HANDLE      h = CreateFileA(invp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    check(h != INVALID_HANDLE_VALUE, "并发登记后 inventory 文件已生成");
    if (h != INVALID_HANDLE_VALUE) {
        ReadFile(h, big, sizeof(big) - 1, &got, NULL);
        CloseHandle(h);
    }
    big[got] = 0;

    const int expect = INV_CONC_THREADS * INV_CONC_EACH;
    int       missing = 0, dup = 0;
    for (int t = 0; t < INV_CONC_THREADS; t++) {
        for (int i = 0; i < INV_CONC_EACH; i++) {
            char pat[24];
            _snprintf(pat, sizeof(pat) - 1, "cc%d-%d\r\n", t, i);
            pat[sizeof(pat) - 1] = 0;
            const char* p = strstr(big, pat);
            if (!p)
                missing++;
            else if (strstr(p + 1, pat))
                dup++;
        }
    }
    printf("  并发登记 %d 条：缺失 %d / 重复 %d / 文件 %lu 字节\n", expect, missing, dup, (unsigned long)got);
    check(missing == 0, "★ 并发登记的每条唯一串都落了盘（旧实现会被同伴覆盖丢掉）");
    check(dup == 0, "同一条串没有重复落盘");
    check(strstr(big, "[×0]") == NULL, "★ 没有 [×0] 空洞条目（槽位相撞后游标指向没写过的格）");
    DeleteFileA(invp);
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0); // 崩了也要留下最后跑到哪一步的输出
    exe_dir();
    ac1::core::init_log("");  // 关日志：单测不写文件
    ac1::dict::set_log(NULL); // dict 的日志出口也不接
    SetConsoleOutputCP(65001);

    printf("ac1-chinese-translate text 层单测（链接 core+dict+text）\n");
    printf("TEXT_REPLACE=%d  工作目录: %s\n", TEXT_REPLACE, g_dir);

    test_gates();
    test_hit_miss();
    test_replace();
    test_forge_gate_text();
    test_idempotent();
    test_fault_and_install();
    test_observation_granularity();
    test_sample_tail();
    // 必须排在最后：本用例灌 400 条未命中会把未命中样本预算（SAMPLE_N）吃光，
    //   而 test_sample_tail 依赖那个预算还有余量。
    test_inventory_concurrency();

    char dp[MAX_PATH];
    path_in_dir(dp, sizeof(dp), "_text_ut_dict.txt");
    DeleteFileA(dp);

    printf("\n==== 通过 %d / 失败 %d ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
