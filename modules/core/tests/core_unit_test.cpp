// modules/core/tests/core_unit_test.cpp —— core 层离线单测
//
//   pwsh -NoProfile -File modules\core\tests\build.ps1
//   pwsh -NoProfile -File modules\core\tests\run.ps1
//
// 只链接 core.lib（不链接 dict / text / glyph / 任何游戏 dll）。
// 覆盖：读门（可读/可写/分类）、wstring 视图（SSO + 堆 + 各种垃圾字段的拒绝）、
// 挂点签名逐字节核对、宿主守卫（在本单测进程里必然判"未知"）、日志开关。

#include "ac1/core/hook.h"
#include "ac1/core/ctr.h"
#include "ac1/core/trace.h"
#include "ac1/core/file.h"
#include "ac1/core/str.h"

#include "ac1/core/host.h"
#include "ac1/core/log.h"
#include "ac1/core/mem.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

using namespace ac1::core;

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

static char g_dir[MAX_PATH] = ""; // 本 exe 所在目录（带尾反斜杠）

static void exe_dir()
{
    GetModuleFileNameA(NULL, g_dir, MAX_PATH);
    char* p = strrchr(g_dir, '\\');
    if (p)
        p[1] = 0;
    else
        g_dir[0] = 0;
}

// ---- 造一个假的 MSVC8 wstring ----
struct FakeWs {
    unsigned char raw[AC1_WSTR_SIZE];
};

// SSO：缓冲内联在 +0x04，_Myres = 7
static void make_sso(FakeWs* w, const wchar_t* s)
{
    memset(w, 0, sizeof(w->raw));
    unsigned n = (unsigned)wcslen(s);
    memcpy(w->raw + AC1_WSTR_OFF_BX, s, (size_t)n * sizeof(wchar_t));
    if (n) ((wchar_t*)(w->raw + AC1_WSTR_OFF_BX))[n] = 0;
    *(unsigned*)(w->raw + AC1_WSTR_OFF_SIZE) = n;
    *(unsigned*)(w->raw + AC1_WSTR_OFF_RES) = 7;
}

// 堆形态：_Bx 是堆指针，res ≥ 15
static wchar_t* make_heap(FakeWs* w, const wchar_t* s, unsigned res)
{
    memset(w, 0, sizeof(w->raw));
    unsigned n = (unsigned)wcslen(s);
    wchar_t* buf = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, ((size_t)res + 1) * sizeof(wchar_t));
    if (!buf) return NULL;
    memcpy(buf, s, (size_t)n * sizeof(wchar_t));
    buf[n] = 0;
    memset(buf + n + 1, 0, ((size_t)res - n) * sizeof(wchar_t));
    *(void**)(w->raw + AC1_WSTR_OFF_BX) = buf;
    *(unsigned*)(w->raw + AC1_WSTR_OFF_SIZE) = n;
    *(unsigned*)(w->raw + AC1_WSTR_OFF_RES) = res;
    return buf;
}

// ================= 1. 读门 =================
static void test_mem_gate()
{
    printf("\n[1] 读门 mem_readable / mem_writable / mem_class\n");
    int stackVar = 0;
    check(mem_class(&stackVar) == MEMCLS_PRIVATE, "栈内存分类 = MEM_PRIVATE");
    check(mem_class(GetModuleHandleA(NULL)) == MEMCLS_IMAGE, "模块基址分类 = MEM_IMAGE");
    check(mem_class(NULL) == MEMCLS_UNKNOWN, "NULL 分类 = UNKNOWN");
    check(mem_class((void*)0x10) == MEMCLS_UNKNOWN, "野指针分类 = UNKNOWN");

    check(mem_readable(&stackVar, 4), "栈变量可读");
    check(mem_writable(&stackVar, 4), "栈变量可写");
    check(mem_readable(NULL, 4) == false, "NULL 不可读");
    check(mem_writable((void*)0x10, 4) == false, "野指针不可写");
    check(mem_readable(&stackVar, 0) == true, "长度 0 视为可读（不崩）");

    // 跨 region：把 .data 末尾到栈首之间的巨大区间交给读门，必须判否而不是崩
    check(mem_readable((void*)0x1000, 0x40000000u) == false, "巨大区间读门判否（不崩）");

    // 区间回绕的回归：b+n 越过 4G 上界时 e<b ⇒ while 一次都不跑。
    //   真实可达路径见 mem.cpp 的注释：垃圾 _Bx 靠近地址空间顶端 + 长串。
    check(mem_readable((void*)0xFFFFFFF0u, 0x100u) == false, "b+n 回绕 ⇒ 读门判否（不失效放行）");
    check(mem_writable((void*)0xFFFFFFF0u, 0x100u) == false, "b+n 回绕 ⇒ 写门判否（不失效放行）");

    // guard page 的回归：PAGE_GUARD(0x100) 落在 protect&0xFF 之外，被掩掉后
    //   PAGE_READWRITE|PAGE_GUARD 会落进 PAGE_READWRITE 分支 ⇒ 判成可读写。
    //   写侧误判的代价是 STATUS_GUARD_PAGE_VIOLATION(0x80000001) 直接终止进程，
    //   而这条判据正是 path_funnel 往引擎 wstring 写字之前的唯一授权。
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        void* g = VirtualAlloc(NULL, si.dwPageSize * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        check(g != NULL, "guard 用例：VirtualAlloc 两页成功");
        if (g) {
            DWORD      old = 0;
            const BOOL set = VirtualProtect(g, si.dwPageSize, PAGE_READWRITE | PAGE_GUARD, &old);
            check(set != 0, "guard 用例：首页置 PAGE_GUARD 成功");
            check(mem_readable(g, 16) == false, "★ guard page 判不可读（PAGE_GUARD 被 &0xFF 掩掉会误判）");
            check(mem_writable(g, 16) == false, "★ guard page 判不可写（误判会让写入直接终止进程）");
            check(mem_readable((const unsigned char*)g + si.dwPageSize, 16) == true,
                  "同一块里没加 guard 的第二页照常可读（判否针对 guard 本身，不是整块）");
            VirtualFree(g, 0, MEM_RELEASE);
        }
    }
}

// ================= 2. wstring 视图 =================
static void test_wstr_view()
{
    printf("\n[2] wstr_view 视图\n");
    WStrView v;

    v = wstr_view(NULL);
    check(!v.ok, "NULL 对象 ⇒ ok=false");

    FakeWs w;

    make_sso(&w, L"Hi");
    v = wstr_view(&w);
    check(v.ok && v.len == 2 && v.res == 7, "SSO：len/res 读对");
    check(v.ok && v.buf == (const wchar_t*)(w.raw + AC1_WSTR_OFF_BX), "SSO：缓冲指针 = 对象内 +0x04");
    check(v.ok && wcscmp(v.buf, L"Hi") == 0, "SSO：内容可读");

    wchar_t* buf = make_heap(&w, L"Start Game", 15);
    v = wstr_view(&w);
    check(v.ok && v.len == 10 && v.res == 15, "堆：len/res 读对");
    check(v.ok && v.buf == buf, "堆：缓冲指针 = _Bx 里的堆指针");
    check(v.ok && wcscmp(v.buf, L"Start Game") == 0, "堆：内容可读");

    // _Mysize > _Myres ⇒ 不可信
    make_sso(&w, L"Hi");
    *(unsigned*)(w.raw + AC1_WSTR_OFF_SIZE) = 99;
    check(!wstr_view(&w).ok, "_Mysize > _Myres 被拒");

    // _Myres 超上限 ⇒ 不可信
    make_sso(&w, L"Hi");
    *(unsigned*)(w.raw + AC1_WSTR_OFF_RES) = 0x500000u;
    check(!wstr_view(&w).ok, "_Myres > 0x400000 被拒");

    // 堆形态但 _Myres 落在 [8,15) ⇒ 不可信
    make_heap(&w, L"x", 15);
    *(unsigned*)(w.raw + AC1_WSTR_OFF_RES) = 10;
    check(!wstr_view(&w).ok, "堆形态 _Myres=10（<15）被拒");

    // _Bx 指向未提交内存 ⇒ 读门拦下
    make_heap(&w, L"x", 15);
    *(void**)(w.raw + AC1_WSTR_OFF_BX) = (void*)0x1000;
    check(!wstr_view(&w).ok, "缓冲指向未提交内存被读门拦下");

    // 野对象指针
    check(!wstr_view((void*)0x10).ok, "野对象指针被拒");
    check(mem_faults() == 0, "全程读门异常计数 = 0", "有异常被兜住");
}

// ================= 3. 挂点签名核对 =================
static void test_hook_signature()
{
    printf("\n[3] hook_bytes_match 逐字节核对\n");
    static const unsigned char entry[7] = { 0x6A, 0xFF, 0x68, 0xE9, 0x7A, 0x55, 0x01 };
    int                        bad = -2;

    check(hook_bytes_match(entry, entry, 7, &bad) == 1 && bad == -1, "完全符合 ⇒ 1，firstBad=-1");
    check(hook_bytes_match(entry, NULL, 7, &bad) == 0, "expect=NULL ⇒ 0");
    check(hook_bytes_match(NULL, entry, 7, &bad) == 0, "at=NULL ⇒ 0");
    check(hook_bytes_match(entry, entry, 0, &bad) == 1, "长度 0 ⇒ 1");

    unsigned char one[7];
    memcpy(one, entry, 7);
    one[3] = 0x68 ^ 0x01; // 第 3 字节不符
    bad = -2;
    check(hook_bytes_match(one, entry, 7, &bad) == 0 && bad == 3, "第 3 字节不符 ⇒ firstBad=3");

    memcpy(one, entry, 7);
    one[6] = 0x00; // 最后一个字节不符
    bad = -2;
    check(hook_bytes_match(one, entry, 7, &bad) == 0 && bad == 6, "末字节不符 ⇒ firstBad=6");

    // 宿主不适用 ⇒ 装不上（绝不硬装）
    HookSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.name = "test";
    spec.rva9 = 0x4A1920;
    spec.rva10 = 0;
    spec.expect = entry;
    spec.expect_len = 7;
    hook_set_base(0x400000);
    printf("  （当前宿主口味 = %d；本单测进程必然不是 Dx9）\n", (int)host_flavor());
    check(host_flavor() != HOST_FLAVOR_DX9, "单测进程被判为非 Dx9 宿主");
    check(hook_rva(spec) == 0, "口味不适用 ⇒ RVA 取 0");
    check(hook_install_one(spec, 0x400000) == 0, "口味不适用 ⇒ 不装钩（返回 0）");
    check(hook_bytes_match(one, entry, 7, &bad) == 0, "签名不符的字节仍然被识别为不符");

    // 签名列闸门（hook_sig_ok）。它排在宿主判定之前，所以在本单测里直接可打，
    //   不必绕道「装不上钩」去间接观察。
    bad = -2;
    check(hook_bytes_match(entry, entry, 0, &bad) == 1 && bad == -1,
          "核对器对零长度签名是放行的（for 不跑 ⇒ 直接返回 1）——这正是要挡它的原因");
    {
        HookSpec s;
        memset(&s, 0, sizeof(s));
        s.name = "sig";
        s.expect = entry;
        s.expect_len = HOOK_SIG_MIN;
        check(hook_sig_ok(s) == 1, "签名恰好等于下限 ⇒ 可用");
        s.expect_len = HOOK_SIG_MIN - 1;
        check(hook_sig_ok(s) == 0, "短于下限 1 字节 ⇒ 不可用（MinHook 要改 5 字节）");
        s.expect_len = 0;
        check(hook_sig_ok(s) == 0, "零长度签名 ⇒ 不可用");
        s.expect = NULL;
        s.expect_len = 7;
        check(hook_sig_ok(s) == 0, "签名指针为空 ⇒ 不可用");
        s.expect = entry;
        s.expect_len = 7;
        s.expect10 = entry;
        s.expect_len10 = 0;
        check(hook_sig_ok(s) == 0, "给了 expect10 却没给 expect_len10 ⇒ 不可用（Dx10 上闸门会失效）");
        s.expect_len10 = 13;
        check(hook_sig_ok(s) == 1, "两列都合规 ⇒ 可用");
        s.expect10 = NULL;
        check(hook_sig_ok(s) == 1, "expect10 为 NULL 时只看 expect 这一列");
        // 装钩路径上也确实被挡住了。返回值分不出是哪道闸门挡的（宿主判定也会返回 0），
        //   「谁先谁后」是读代码得出的事实，不是这条断言证明的。
        s.expect_len = 0;
        check(hook_install_one(s, 0x400000) == 0, "签名列不合规的条目装不上钩");
    }
}

// ================= 3b. hook_hexdump（签名不符诊断的格式化器） =================
// 这段代码只在「宿主版本不符」时才跑（单测进程 flavor 恒 NONE，进不去那条分支），
//   所以它是本文件唯一一处「出事才有输出」的代码 —— 用自测缝把它变成可回归的。
static void test_hook_hexdump()
{
    printf("\n[3b] hook_hexdump 逐字节格式化\n");
    static const unsigned char b3[3] = { 0x6A, 0xFF, 0x68 };
    char                       b[64];

    b[0] = 'X';
    b[1] = 0;
    check(hook_hexdump(b, (int)sizeof(b), NULL, 3) == 0 && b[0] == 0, "p==NULL ⇒ 空串、返回 0");
    check(hook_hexdump(b, (int)sizeof(b), b3, 0) == 0 && b[0] == 0, "n==0 ⇒ 空串、返回 0");
    check(hook_hexdump(b, (int)sizeof(b), b3, -1) == 0 && b[0] == 0, "n<0 ⇒ 空串、返回 0");

    struct Guarded {
        char pre[8];
        char buf[16];
        char post[8];
    };
    Guarded g;
    memset(&g, 0xCD, sizeof(g));
    check(hook_hexdump(NULL, 64, b3, 3) == 0, "out==NULL ⇒ 返回 0");
    check(hook_hexdump(g.buf, 0, b3, 3) == 0, "cap==0 ⇒ 返回 0");
    check(hook_hexdump(g.buf, -1, b3, 3) == 0, "cap<0 ⇒ 返回 0");
    bool intact = true;
    for (int i = 0; i < 8; i++) {
        if ((unsigned char)g.pre[i] != 0xCD) intact = false;
        if ((unsigned char)g.post[i] != 0xCD) intact = false;
    }
    check(intact, "★ 容量不可用时不写任何字节（前后 canary 完好）");

    check(hook_hexdump(g.buf, 9, b3, 2) == 2 && strcmp(g.buf, "6A FF") == 0,
          "恰好放满：cap=9/n=2 ⇒ \"6A FF\"", g.buf);
    check(hook_hexdump(g.buf, 9, b3, 3) == 2 && strcmp(g.buf, "6A FF") == 0, "★ 截断按整字节、末尾不留空格",
          g.buf);
    {
        bool intact2 = true;
        for (int i = 0; i < 8; i++) {
            if ((unsigned char)g.pre[i] != 0xCD) intact2 = false;
            if ((unsigned char)g.post[i] != 0xCD) intact2 = false;
        }
        check(intact2, "★ 截断只写缓冲内（前后 canary 完好）");
    }
    check(hook_hexdump(b, 5, b3, 3) == 0 && b[0] == 0, "一格都放不下 ⇒ 空串、返回 0", b);

    // 生产最长签名（Dx10 字体加载 13 字节实测值，见 glyph.cpp 的 kFontLoadEntry10）
    static const unsigned char e13[13] = { 0x6A, 0xFF, 0x68, 0x4B, 0xFE, 0x56, 0x01,
                                           0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 };
    check(hook_hexdump(b, (int)sizeof(b), e13, 13) == 13, "13 字节签名全量格式化");
    check(strcmp(b, "6A FF 68 4B FE 56 01 64 A1 00 00 00 00") == 0, "内容逐字节正确", b);

    // 本仓多处截断处理（str / log / dict / dump_wstr）都建立在这条 CRT 语义上：
    //   截断时 _snprintf 写满 count 字节、不补 NUL、返回负数。换工具链要在这里炸。
    char t[5] = { 0 }; // 第 5 字节留给 NUL：这条断言失败时 detail 里的 %s 才不会越界读
    memset(t, 'Z', 4);
    const int r = _snprintf(t, 4, "%s", "abcdef");
    check(r < 0 && t[3] == 'd', "★ MSVC _snprintf 截断语义：负返回且不补 NUL（前提被钉住）", t);
}

// ================= 4. 宿主守卫 =================
static void test_host_guard()
{
    printf("\n[4] 宿主守卫\n");
    bool ok = host_check();
    check(ok == false, "单测 exe 不是 Dx9 ⇒ host_check() = false");
    check(host_flavor() == HOST_FLAVOR_NONE, "口味 = NONE");
    check(host_name()[0] != 0, "宿主文件名非空");
    check(host_exe()[0] != 0, "宿主完整路径非空");
    check(host_detail()[0] != 0, "宿主摘要非空");
    printf("  摘要: %s\n", host_detail());
    check(host_base() != 0, "主模块基址非 0");
    check(host_file_size() > 0, "主 exe 文件大小 > 0");
    check(host_crc32() != 0, "主 exe CRC32 算得出来（自算值，非基准 E8936C99）");

    // ---- 宿主表 ----
    check(host_table_size() >= 1, "宿主表至少登记了一门宿主");
    check(host_crc_reference() == 0,
          "★ 本单测 exe 不在表里 ⇒ 基准 CRC 为 0（没有基准可比，不拿别的口味的基准凑）");
    check(strcmp(host_flavor_name(), "未知") == 0, "未识别宿主的口味名是「未知」", host_flavor_name());
    // 未识别宿主时，摘要里那一格必须说「未知」而不是「不一致 ⚠」
    check(strstr(host_detail(), "未知") != 0, "★ 未识别宿主时摘要写「未知」，不虚报「不一致」",
          host_detail());
    check(strstr(host_detail(), "口味=未知") != 0, "摘要含「口味=未知」", host_detail());

    // 摘要的形状一个字没动 —— README §9 的验收判据逐字比对这一行。
    // 这里只能验形状（Dx9 那一行走不到：单测进程不是那个 exe）。
    check(strstr(host_detail(), "exe=") != 0 && strstr(host_detail(), " 基址=0x") != 0 &&
              strstr(host_detail(), " 字节 CRC32=") != 0 && strstr(host_detail(), "（基准 ") != 0 &&
              strstr(host_detail(), "） 口味=") != 0,
          "★ 摘要的字段与顺序一字未改（Dx9 实机上仍会打出 README 记的那一行）", host_detail());
    // 再验一次「表里那一行的 exe 名必须是真名」：Dx9 行的名字必须是那个 exe。
    // （防的是有人把表里的名字写错却还留着 flavor ⇒ 在错的宿主上返回 true。）
    check(host_flavor() == HOST_FLAVOR_NONE, "表里没有本单测的 exe ⇒ 不认作任何口味（宁可漏装也不误装）");
}

// ================= 5. 日志体系（等级 / run stamp / 单次 run 文件） =================
static void test_log()
{
    printf("\n[5] 日志体系\n");
    unsigned long d0 = log_dropped();
    init_log(g_dir);
    check(log_path()[0] != 0, "init_log 后有路径");
    check(strstr(log_path(), "AC1_CJK\\run_") != NULL, "路径 = AC1_CJK\\run_<stamp>.log", log_path());
    check(strstr(log_path(), ".log") != NULL, "路径以 .log 结尾");
    check(lstrlenA(log_run_stamp()) == 15, "run stamp 是 YYYYMMDD_HHMMSS（15 字符）", log_run_stamp());
    check(log_run_stamp()[8] == '_', "run stamp 第 9 位是下划线");
    check(GetFileAttributesA(log_path()) != INVALID_FILE_ATTRIBUTES, "run 日志文件已落盘");

    // 三个等级各写一行 + "!!" 标记的两处形态，然后回读全文断言行格式
    log_line("[单测] core 日志通路自检 pid=%lu", (unsigned long)GetCurrentProcessId());
    log_line("!! 单测错误样例");
    log_line("[单测] !! 类别前缀之后的标记");
    log_line("%s", "[词典] !! 经注入出口的预格式化消息体");
    log_warn("[单测] 降级样例");
    check(log_dropped() == d0, "写日志没有丢行");
    close_log();
    {
        char   buf[8192];
        DWORD  got = 0;
        HANDLE h = CreateFileA(log_path(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        check(h != INVALID_HANDLE_VALUE, "关闭后仍能回读日志文件");
        if (h != INVALID_HANDLE_VALUE) {
            ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
            CloseHandle(h);
        }
        buf[got] = 0;
        check(strstr(buf, "][INFO] [单测] core 日志通路自检") != NULL, "INFO 行带等级列", buf);
        check(strstr(buf, "][ERR] !! 单测错误样例") != NULL, "★ 兼容壳：!! 开头自动归 ERR");
        // 回归：判据曾经只看格式串的 fmt[0..1]。类别前缀之后写标记的真实形态
        //   （[词典] !! / [字形] !! …）与经 set_log 注入的 "%s" 形态都会停在 INFO
        //   ⇒ 全树 23 处「要行动的事故」里只有 4 处真能打 ERR（实机 267 行全 INFO）。
        check(strstr(buf, "][ERR] [单测] !! 类别前缀之后的标记") != NULL, "★ 标记在类别前缀之后也归 ERR");
        check(strstr(buf, "][ERR] [词典] !! 经注入出口的预格式化消息体") != NULL,
              "★ 经注入出口（fmt=\"%s\"）的消息体带标记同样归 ERR");
        check(strstr(buf, "][WARN] [单测] 降级样例") != NULL, "log_warn 写出 WARN 级");
        check(buf[0] == '[' && buf[5] == '-' && buf[8] == '-' && buf[11] == ' ',
              "行首是 [YYYY-MM-DD HH:MM:SS] 日期时间", buf);
    }
    DeleteFileA(log_path());

    init_log(""); // 关掉
    check(log_path()[0] == 0, "init_log(\"\") 后路径清空");
    check(log_run_stamp()[0] == 0, "init_log(\"\") 后 run stamp 清空");
    log_line("[单测] 这一行应该被丢弃");
    check(log_dropped() == d0 + 1, "关闭后写日志计入丢弃次数");
    close_log();

    // 目录长到装不下 ⇒ 不落文件、log_path() 留空。半截文件名会让 log_path() 报出
    //   与实际打开的文件不同的名字 —— 这条断言就是那个失效模式的回归网。
    {
        char longDir[MAX_PATH];
        memset(longDir, 'D', sizeof(longDir) - 1);
        longDir[sizeof(longDir) - 1] = 0;
        longDir[sizeof(longDir) - 2] = '\\';
        longDir[sizeof(longDir) - 3] = 0;
        longDir[0] = 'C';
        init_log(longDir);
        check(log_path()[0] == 0, "目录过长 ⇒ 拼不出路径，日志保持关闭（不产生被截断的文件名）");
        check(log_run_stamp()[0] == 0, "目录过长 ⇒ run stamp 也留空");
        close_log();
    }
}

// ================= 5b. 超长日志行（截断必须保留消息体） =================
// 回归：截断时旧实现清 body[0] ⇒ 整条消息体变成空串（日志里只剩 `[时间戳][LVL] `）。
static void test_log_truncation()
{
    printf("\n[5b] 超长日志行（截断保留）\n");

    init_log(g_dir);
    close_log();
    DeleteFileA(log_path()); // 同一 run 内重开：清掉前面用例写下的内容，便于整文件读回
    init_log(g_dir);

    static char big[2400];
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    log_line("[单测] %s", big);
    close_log();

    static char buf[8192];
    DWORD       got = 0;
    HANDLE h = CreateFileA(log_path(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    check(h != INVALID_HANDLE_VALUE, "截断日志可回读");
    if (h != INVALID_HANDLE_VALUE) {
        ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
        CloseHandle(h);
    }
    buf[got] = 0;

    const char* p = strstr(buf, "][INFO] [单测] ");
    check(p != NULL, "截断行仍有时间戳/等级/前缀");
    check(p && strstr(p, "AAAA") != NULL, "★ 消息体被保留（旧实现这里只剩前缀）", p ? p : buf);
    check(p && strlen(p) > 2000, "截断行仍带着大段消息体（≈2046 B）");
    DeleteFileA(log_path());
    init_log(""); // 恢复「单测不写文件」的状态
    check(log_path()[0] == 0, "恢复为关闭状态");
}

// ================= 6. 安全字符串累加器（str） =================
// 重点是截断路径：「拿 _snprintf 返回值当偏移」在缓冲不够时返回值 > cap ⇒
//   (cap - o) 无符号下溢成巨数 ⇒ 越界写。Str 按实际写入量推进，不许有这个形状。
static void test_str()
{
    printf("\n[6] Str 安全累加器\n");

    // 正常追加
    char buf[64];
    Str  s;
    str_init(&s, buf, (int)sizeof(buf));
    str_addf(&s, "键=%d/", 42);
    str_addf(&s, "值=%s", "abc");
    check(strcmp(buf, "键=42/值=abc") == 0, "两次追加结果正确", buf);
    check(str_len(&s) == (int)strlen(buf), "str_len 与实际长度一致");

    // 截断：小 cap + 长格式串（旧写法这里会越界写）。
    //   canary 用前后都有余量的结构体，才能真正测出越界
    //   （只声明 char[8] 再去读 [8..15] 是读越界，测不出任何东西）。
    struct Guarded {
        char pre[8];
        char buf[8];
        char post[8];
    };
    Guarded g;
    memset(&g, 0xCD, sizeof(g));
    Str t;
    str_init(&t, g.buf, (int)sizeof(g.buf));
    str_addf(&t, "%s", "0123456789ABCDEF"); // 16 字节内容，cap 只有 8
    char d1[160];
    _snprintf(d1, sizeof(d1), "实际尾字节=%02X strlen=%d", (unsigned char)g.buf[7], (int)strlen(g.buf));
    check(g.buf[7] == 0, "截断后末尾仍是 NUL", d1);
    check(strlen(g.buf) < sizeof(g.buf), "截断后长度未超出 cap", d1);
    bool guard_intact = true;
    for (int i = 0; i < 8; i++) {
        if ((unsigned char)g.pre[i] != 0xCD) guard_intact = false;
        if ((unsigned char)g.post[i] != 0xCD) guard_intact = false;
    }
    check(guard_intact, "★ 截断没有写到缓冲之外（前后 canary 完好）");

    // 满了之后再追加：内容不变，NUL 仍在
    char full[4];
    Str  f;
    str_init(&f, full, (int)sizeof(full));
    str_addf(&f, "abcdefghij"); // cap=4
    const int lenBefore = str_len(&f);
    str_addf(&f, "MORE");
    char d2[160];
    _snprintf(d2, sizeof(d2), "追加前=%d 追加后=%d 内容=\"%.8s\"", lenBefore, str_len(&f), full);
    check(str_len(&f) == lenBefore, "缓冲已满时再追加不改长度", d2);
    check(str_full(&f), "str_full 报满");
    check(full[sizeof(full) - 1] == 0, "满时仍保留 NUL");

    // 不可用态：NULL 缓冲 / 零容量 ⇒ 全部空操作且不崩
    Str z;
    str_init(&z, NULL, 0);
    str_addf(&z, "x=%d", 1);
    check(str_len(&z) == 0, "NULL 缓冲进入不可用态");
    str_init(&z, full, 0);
    str_addf(&z, "x=%d", 1);
    check(str_len(&z) == 0, "零容量进入不可用态");
}

// ================= 7. 原子计数组（ctr） =================
static void test_ctr()
{
    printf("\n[7] Counters 原子计数组与占槽\n");
    Counters c;
    ctr_reset(&c, 4);
    check(ctr_count(&c) == 4, "槽数按参数归一");

    ctr_inc(&c, 0);
    ctr_inc(&c, 0);
    ctr_add(&c, 1, 5);
    ctr_add(&c, 1, -2);
    check(ctr_get(&c, 0) == 2, "槽 0 自增两次 = 2");
    check(ctr_get(&c, 1) == 3, "槽 1 加 5 减 2 = 3");

    // 越界：静默丢弃，绝不越界写
    ctr_inc(&c, -1);
    ctr_inc(&c, 4);
    ctr_inc(&c, 9999);
    check(ctr_get(&c, 0) == 2 && ctr_get(&c, 1) == 3, "越界自增被丢弃，不污染合法槽");
    ctr_inc(NULL, 0); // 不崩
    check(ctr_get(NULL, 0) == 0, "NULL 实例读出 0");

    // 上限保护：n 超界按 COUNTER_SLOTS_MAX 处理
    Counters big;
    ctr_reset(&big, COUNTER_SLOTS_MAX + 10);
    check(ctr_count(&big) == COUNTER_SLOTS_MAX, "n 超上限被夹到 COUNTER_SLOTS_MAX");
    ctr_inc(&big, COUNTER_SLOTS_MAX - 1);
    check(ctr_get(&big, COUNTER_SLOTS_MAX - 1) == 1, "上限槽可用");

    // 占槽：每次拿到唯一序号，满了返回 -1（「读计数→判边界→写→最后自增」没有这个保证）
    Counters t;
    ctr_reset(&t, 3);
    int s0 = ctr_claim(&t);
    int s1 = ctr_claim(&t);
    int s2 = ctr_claim(&t);
    int s3 = ctr_claim(&t);
    check(s0 == 0 && s1 == 1 && s2 == 2, "占槽依次拿到 0/1/2");
    check(s3 == -1, "占满后返回 -1");

    // reset 之后游标归零（否则复用同一个实例会永远占不到槽）
    ctr_reset(&t, 3);
    check(ctr_claim(&t) == 0, "ctr_reset 后占槽游标归零");

    // 快照
    Counters s2c;
    ctr_reset(&s2c, 4);
    ctr_add(&s2c, 2, 7);
    long snap[6];
    ctr_snapshot(&s2c, snap, 6);
    check(snap[2] == 7 && snap[3] == 0 && snap[4] == 0 && snap[5] == 0, "快照取到值且超出部分补 0");
}

// ================= 8. 文件读取与路径拼接（file） =================
static void test_file()
{
    printf("\n[8] file_read_all / file_exists / path_join\n");

    char p[MAX_PATH];
    if (!path_join(p, (int)sizeof(p), g_dir, "_core_file_test.bin")) {
        check(false, "path_join 拼出测试文件路径");
        return;
    }
    check(strstr(p, "_core_file_test.bin") != 0, "path_join 结果含文件名", p);

    // g_dir 自带尾反斜杠 ⇒ 不该再补一个
    int doubles = 0;
    for (size_t i = 0; p[i + 1]; i++)
        if (p[i] == '\\' && p[i + 1] == '\\') doubles++;
    check(doubles == 0, "目录已带尾反斜杠时不重复补", p);

    // 不带尾反斜杠 ⇒ 必须补
    char noTail[MAX_PATH];
    lstrcpynA(noTail, g_dir, MAX_PATH);
    size_t nl = strlen(noTail);
    if (nl && noTail[nl - 1] == '\\') noTail[nl - 1] = 0;
    char p2[MAX_PATH];
    check(path_join(p2, (int)sizeof(p2), noTail, "x.bin"), "无尾斜杠也能拼");
    check(strstr(p2, "\\x.bin") != 0, "补上了分隔符", p2);

    // 装不下 ⇒ 返回 0，且不会写坏缓冲
    char tiny[8];
    memset(tiny, 0xCD, sizeof(tiny));
    check(path_join(tiny, (int)sizeof(tiny), "C:\\a\\very\\long\\directory", "file.bin") == 0,
          "容量不足时 path_join 返回 0");
    check(tiny[0] == 0, "失败时缓冲被置空而不是留下半截路径");

    check(!file_exists("Z:/nope/definitely-missing.bin"), "不存在的文件判否");

    // 写一份已知内容再读回来，逐字节核对
    HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        check(false, "创建测试文件", p);
        return;
    }
    const char* payload = "AC1_CJK_core_file_probe";
    DWORD       wr = 0;
    WriteFile(h, payload, (DWORD)strlen(payload), &wr, NULL);
    CloseHandle(h);

    check(file_exists(p), "刚写的文件存在");

    unsigned char* buf = NULL;
    int            n = 0;
    check(file_read_all(p, &buf, &n, 0), "file_read_all 读出");
    check(buf != NULL && n == (int)strlen(payload), "读回长度正确");
    check(buf && memcmp(buf, payload, (size_t)n) == 0, "读回内容逐字节相同");
    file_free(buf);

    // 超上限按失败处理（不截断）
    unsigned char* capped = NULL;
    int            cn = 0; // 不能叫 small：windows.h 把它定义成 char
    check(file_read_all(p, &capped, &cn, 4) == false, "超过 maxBytes 按失败处理");
    check(capped == NULL && cn == 0, "失败时 out/outN 被清干净");

    // 空文件按失败处理（0 字节不是有效输入）
    HANDLE e = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (e != INVALID_HANDLE_VALUE) CloseHandle(e);
    unsigned char* eb = NULL;
    int            en = 0;
    check(file_read_all(p, &eb, &en, 0) == false, "空文件按失败处理");
    check(eb == NULL, "空文件不交出缓冲");

    file_read_all("Z:/nope/missing.bin", &buf, &n, 0);
    check(buf == NULL, "打不开的文件交出 NULL");

    DeleteFileA(p);
}

// ================= 9. 钩执行迹：面包屑栈（纯内存） =================
static int g_tracked, g_countOnly, g_tracked2;

static void test_trace_stack()
{
    printf("\n[9] 钩执行迹 · 面包屑栈\n");

    HookPolicy p;
    p.track = 1;
    p.logEvery = 0;
    p.logBudget = 0;
    g_tracked = hooktrace_register("UNIT_TRACK", p);
    p.track = 0;
    g_countOnly = hooktrace_register("UNIT_COUNT", p);
    p.track = 1; // 第二个进栈的钩：只有两层都 track=1 才测得出栈的次序
    g_tracked2 = hooktrace_register("UNIT_TRACK2", p);
    check(g_tracked >= 0 && g_countOnly >= 0 && g_tracked2 >= 0, "三个钩都登记成功");
    check(g_tracked2 != g_tracked, "两个进栈的钩拿到不同槽位");
    check(hooktrace_register("UNIT_TRACK", p) == g_tracked, "同名重复登记幂等（返回原槽）");
    check(hooktrace_slots() >= 2, "已登记数 >= 2");

    const long c0 = hooktrace_count(g_tracked);
    const long d0 = hooktrace_depth();

    // 进出配对 ⇒ 深度回到原处
    hooktrace_enter(g_tracked, 0xAAAA0001u, 0xBBBB0002u);
    check(hooktrace_depth() == d0 + 1, "enter 使深度 +1");
    hooktrace_leave(g_tracked);
    check(hooktrace_depth() == d0, "★ leave 使深度回到原处（不减会永远弹不干净）");
    check(hooktrace_count(g_tracked) == c0 + 1, "计数 +1");

    // 嵌套：两层，转储要能读出顺序
    hooktrace_enter(g_tracked, 0x11111111u, 0);
    hooktrace_enter(g_countOnly, 0x22222222u, 0); // countOnly 不进栈
    check(hooktrace_depth() == d0 + 1, "track=0 的钩不进栈（深度只 +1）");
    check(hooktrace_count(g_countOnly) >= 1, "track=0 的钩仍然计数");
    {
        HookRec   recs[TRACE_DEPTH_MAX];
        const int n = hooktrace_snapshot(recs, TRACE_DEPTH_MAX);
        check(n == d0 + 1, "快照条数与深度一致", "n");
        check(n > 0 && recs[0].hookId == (unsigned short)g_tracked, "栈里唯一的那个钩在 recs[0]");
        check(n > 0 && recs[0].a0 == 0x11111111u, "上下文值 a0 原样带出");
    }
    hooktrace_leave(g_countOnly);

    // ★ 两层**都进栈**时的次序：recs[0] 必须是后进入的那个（最内层 = 崩点）。
    //   旧实现顺着 rec[] 填，而 rec[0] 是最先进入的入口钩 ⇒ recs[0] 反而是最外层：
    //   崩溃报告把「最不可能出事的那个」排在 #0，排查方向整个反过来。
    //   上面那条单层断言在任何次序下都成立（栈里只有一个钩），所以它抓不到这个错
    //   —— 必须两层都 track=1 才测得出。
    hooktrace_enter(g_tracked2, 0x33333333u, 0);
    {
        HookRec   recs[TRACE_DEPTH_MAX];
        const int n = hooktrace_snapshot(recs, TRACE_DEPTH_MAX);
        check(n == d0 + 2, "两层都进栈 ⇒ 快照 2 条", "n");
        check(n == d0 + 2 && recs[0].hookId == (unsigned short)g_tracked2 && recs[0].a0 == 0x33333333u,
              "★ recs[0] = 最内层（后进入的那个钩）");
        check(n == d0 + 2 && recs[1].hookId == (unsigned short)g_tracked, "recs[1] = 外层（先进入的那个钩）");
    }
    // nMax 不够时保留最内层的那些层（离崩点最近的才是要看的）
    {
        HookRec   recs[TRACE_DEPTH_MAX];
        const int n = hooktrace_snapshot(recs, 1);
        check(n == 1 && recs[0].hookId == (unsigned short)g_tracked2, "nMax=1 时留下的是最内层那一层");
    }
    hooktrace_leave(g_tracked2);
    hooktrace_leave(g_tracked);
    check(hooktrace_depth() == d0, "两层都退出后深度归零");

    // 栈溢出：只丢记录，不越界、也不影响 depth 的正确性
    for (int i = 0; i < TRACE_DEPTH_MAX + 5; i++) hooktrace_enter(g_tracked, (unsigned)i, 0);
    check(hooktrace_depth() == d0 + TRACE_DEPTH_MAX + 5, "★ 深度可以超过上限（记录被丢弃）");
    {
        HookRec   recs[TRACE_DEPTH_MAX];
        const int n = hooktrace_snapshot(recs, TRACE_DEPTH_MAX);
        check(n <= TRACE_DEPTH_MAX, "快照永远不超过数组上限");
    }
    for (int i = 0; i < TRACE_DEPTH_MAX + 5; i++) hooktrace_leave(g_tracked);
    check(hooktrace_depth() == d0, "溢出部分也能完整退回");

    // 非法槽位：静默无操作

    // 显式上界版：表比计数数组大是正常用法（text 的 Top 表 128 格 vs 十几槽）
    Counters w;
    ctr_reset(&w, 4); // 计数器只用 4 格
    check(ctr_count(&w) == 4, "w 的计数槽数 = 4");
    check(ctr_claim(&w) == 0, "默认 claim 按计数槽数封顶时拿到 0");
    check(ctr_claim(&w) == 1, "再拿一个 1");
    // 显式上界 8：与计数槽数无关
    check(ctr_claim_n(&w, 8) == 2, "★ 显式上界版不受计数槽数限制");
    check(ctr_claim_n(&w, 8) == 3, "继续拿到 3");
    check(ctr_claim_n(&w, 8) == 4, "★ 可以越过计数槽数（表 8 格 > 计数 4 格）");
    check(ctr_claim_n(&w, 8) == 5, "继续拿到 5");
    check(ctr_claim_n(&w, 8) == 6, "继续拿到 6");
    check(ctr_claim_n(&w, 8) == 7, "拿到上界内最后一格 7");
    check(ctr_claim_n(&w, 8) == -1, "★ 超过显式上界返回 -1");
    check(ctr_claim_n(NULL, 8) == -1, "NULL 实例返回 -1");
    check(ctr_claim_n(&w, 0) == -1, "cap=0 回退到计数槽数 4（游标已走到 8 ⇒ 满，返回 -1）");
    hooktrace_enter(-1, 0, 0);
    hooktrace_enter(9999, 0, 0);
    hooktrace_leave(-1);
    check(hooktrace_depth() == d0, "非法槽位不改变栈");
    check(hooktrace_count(-1) == 0, "非法槽位计数读出 0");
}

// ================= 9b. 并发登记（登记表写锁） =================
// 回归：hooktrace_register 的「读 g_nslots → 扫重 → 填格 → 最后自增」在并发下会让两个
//   线程写同一格（丢一个登记、计数合并），边界上还会把 g_nslots 顶过 TRACE_SLOTS_MAX。
static volatile LONG g_concGo = 0;
static int           g_concSlot[4];

static DWORD WINAPI conc_register_thread(LPVOID pv)
{
    static const char* names[4] = { "CONC_0", "CONC_1", "CONC_2", "CONC_3" };
    const int          i = (int)(INT_PTR)pv;
    while (InterlockedCompareExchange(&g_concGo, 0, 0) == 0) YieldProcessor(); // 自旋 barrier
    HookPolicy p;
    p.track = 1;
    p.logEvery = 0;
    p.logBudget = 0;
    g_concSlot[i] = hooktrace_register(names[i], p);
    return 0;
}

static void test_trace_concurrent_register()
{
    printf("\n[9b] 并发登记（同一张表，四个线程）\n");
    const int base = hooktrace_slots();

    HANDLE th[4];
    InterlockedExchange(&g_concGo, 0);
    for (int i = 0; i < 4; i++)
        th[i] = CreateThread(NULL, 0, conc_register_thread, (LPVOID)(INT_PTR)i, 0, NULL);
    const bool allOk = th[0] && th[1] && th[2] && th[3];
    check(allOk, "四个登记线程都建起来了");
    if (allOk) {
        InterlockedExchange(&g_concGo, 1);
        WaitForMultipleObjects(4, th, TRUE, 10000);
    }
    for (int i = 0; i < 4; i++)
        if (th[i]) CloseHandle(th[i]);

    bool valid = true, uniq = true;
    for (int i = 0; i < 4; i++) {
        if (g_concSlot[i] < 0) valid = false;
        for (int j = i + 1; j < 4; j++)
            if (g_concSlot[i] == g_concSlot[j]) uniq = false;
    }
    check(valid, "四个线程都拿到有效槽位");
    check(uniq, "★ 槽位两两不同（旧实现会两线程写同一格、丢一个登记）");
    check(hooktrace_slots() == base + 4, "已登记数 = 基数 + 4（每个登记各占一格）");
    check(hooktrace_slots() <= TRACE_SLOTS_MAX, "★ 登记数恒不超过表容量（旧的 check-then-act 会顶过界）");

    char buf[1024];
    hooktrace_status(buf, (int)sizeof(buf));
    int named = 0;
    for (int i = 0; i < 4; i++) {
        char pat[16];
        _snprintf(pat, sizeof(pat) - 1, "CONC_%d=", i);
        pat[sizeof(pat) - 1] = 0;
        if (strstr(buf, pat)) named++;
    }
    check(named == 4, "状态行里四个名字各出现一次");
    check(strstr(buf, "?=") == NULL, "没有幽灵行（未填槽的 ?=0）");
}

// ================= 10. 明细行采样（logEvery × logBudget）=================
// logBudget 数的是**已打出的行数**。曾把它写成「进入次数」：热钩（每 N 次一行、
// 上限 B 行）在第 N 次进入时已经累计了 N 次进入，而上限只有 B ⇒ within 恒假
// ⇒ 一行都打不出来（真机上漏斗与查表钩各跑了几十万次进入，明细行为 0）。
// 这条断言在那个版本上会红。
static void test_trace_sampling()
{
    printf("\n[10] 明细行采样\n");
    HookPolicy p;
    p.track = 0;      // 不进栈，本节只验采样
    p.logEvery = 100; // 每 100 次进入记一行
    p.logBudget = 5;  // 最多 5 行
    const int slot = hooktrace_register("UNIT_SAMPLED", p);
    check(slot >= 0, "采样钩登记成功");
    // init_log 是追加打开（OPEN_ALWAYS），同秒重跑会续写上一次的 run_<stamp>.log
    // ⇒ 先建目录拿到路径、关掉、删干净、再重新开。
    init_log(g_dir);
    close_log();
    DeleteFileA(log_path());
    init_log(g_dir);
    for (int i = 0; i < 1000; i++) hooktrace_enter(slot, (unsigned)i, 0);
    check(hooktrace_count(slot) == 1000, "进入 1000 次", "count");
    close_log();

    char   buf[16384];
    DWORD  got = 0;
    HANDLE h = CreateFileA(log_path(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
        CloseHandle(h);
    }
    buf[got] = 0;
    int lines = 0;
    for (const char* q = buf; (q = strstr(q, "[迹] UNIT_SAMPLED")) != NULL; q++) lines++;
    check(lines == 5, "每 100 次一行、上限 5 行 ⇒ 恰好打出 5 行（旧实现打出 0 行）");
    DeleteFileA(log_path());
    // 只数行数不够：打进 #1..#5 的实现同样能凑够 5 行。相位由 #<进入次数>/<行号> 钉住。
    check(strstr(buf, "#100/1 a0=") != NULL, "第 1 行落在第 100 次进入");
    check(strstr(buf, "#500/5 a0=") != NULL, "第 5 行落在第 500 次进入");
    check(strstr(buf, "#1/") == NULL, "第 1 次进入不打（不是每次都记）");
    check(strstr(buf, "#600/") == NULL, "上限 5 ⇒ 第 600 次进入不再打");
}

// ================= 11. 崩溃转储：真触发一次访问违例 =================
//
// VEH 在栈展开之前运行，转储完仍返回 EXCEPTION_CONTINUE_SEARCH ⇒ 同一次异常接着被
//   __except 接住、进程照常活着。所以本节可以**在进程内**触发真 AV 走完整条链路
//   （真 AV ⇒ 真 VEH ⇒ 真 WriteFile），不必 fork 子进程去死、也不必解析退出码。
static bool read_crash_log(char* out, int cap)
{
    const char* p = hooktrace_crash_path(); // AC1_CJK\crash_<run stamp>.log（日志体系）
    HANDLE      h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD      got = 0;
    const BOOL ok = ReadFile(h, out, (DWORD)cap - 1, &got, NULL);
    CloseHandle(h);
    out[got] = 0;
    return ok != FALSE;
}

static void crash_log_delete() { DeleteFileA(hooktrace_crash_path()); }

static void test_trace_crash_dump()
{
    printf("\n[11] 崩溃转储（真触发一次访问违例）\n");

    // 必须在装 VEH 之前删：VEH 装的时候就把崩溃日志句柄打开了，
    //   而那个句柄没带 FILE_SHARE_DELETE ⇒ 之后再 DeleteFile 会因共享冲突失败，
    //   于是「上一轮的残留内容」会被误当成本轮转储读出来。
    crash_log_delete();
    check(hooktrace_veh_install(g_dir) == 1, "VEH 注册成功");
    check(hooktrace_veh_install(g_dir) == 1, "VEH 重复注册是幂等的（不重复挂钩）");
    {
        const char* pp = hooktrace_crash_path();
        check(GetFileAttributesA(pp) != INVALID_FILE_ATTRIBUTES,
              "VEH 安装时崩溃日志句柄已打开（否则转储无处可写）", pp);
    }

    HookPolicy p;
    p.track = 1;
    p.logEvery = 0;
    p.logBudget = 0;
    const int a = hooktrace_register("SELF_PROBE", p);

    // ---- 真 AV：VEH 转储 → 返回 CONTINUE_SEARCH → __except 接住 ----
    hooktrace_enter(a, 0xCAFEBABEu, 0x12345678u);
    __try {
        volatile int* bad = (volatile int*)0x10; // 必定不可读
        *bad = 1;
        check(false, "★ 这一行不该打印（AV 应当被 __except 接住）");
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        check(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION, "确实是一次访问违例（0xC0000005）");
    }

    {
        static char content[8192];
        const bool  got = read_crash_log(content, (int)sizeof(content));
        check(got, "★ 真 AV 触发了 VEH 并落了盘", g_dir);
        check(strstr(content, "AC1_CJK") != 0, "转储里有抬头", content);
        check(strstr(content, "C0000005") != 0, "★ 转储里记下了真实的异常码", content);
        check(strstr(content, "SELF_PROBE") != 0,
              "★★ 转储里有崩溃时正在执行的那个钩名 —— 这正是本设施存在的理由", content);
        check(strstr(content, "0xCAFEBABE") != 0, "转储里有该钩的上下文值 a0", content);
        check(strstr(content, "0x12345678") != 0, "转储里有上下文值 a1", content);
    }

    // ---- 异常被接住之后，钩栈必须还是原样（VEH 是旁观者，不能改变行为）----
    //  此时还没 hooktrace_leave：钩仍在栈上，VEH 若擅自清栈这条就会红。
    {
        static char content[8192];
        crash_log_delete();
        hooktrace_dump_now("AV 之后");
        read_crash_log(content, (int)sizeof(content));
        check(strstr(content, "SELF_PROBE") != 0, "★ 接住 AV 之后钩仍在栈上（VEH 没有把栈清掉）", content);
        check(strstr(content, "游戏自身") == 0, "此时不该出现「空栈」结论", content);
    }

    // ---- 钩已退出 ⇒ 明确写出「崩在游戏自身，不是我们的钩」----
    hooktrace_leave(a);
    {
        static char content[8192];
        crash_log_delete();
        hooktrace_dump_now("空栈");
        read_crash_log(content, (int)sizeof(content));
        check(strstr(content, "游戏自身") != 0, "★ 栈空时明确写出「不是我们的钩」", content);
    }

    hooktrace_shutdown();

    // ---- 拆掉 VEH 之后：崩溃日志既不会再增长，也不会被自动转储 ----
    //  这条必须放在 shutdown 之后：句柄开着的时候 DeleteFile 会因共享冲突失败，
    //   上一轮残留内容会被误读成「本轮又写了一次」。
    crash_log_delete();
    {
        static char probe[8192];
        check(!read_crash_log(probe, (int)sizeof(probe)), "shutdown 之后普通调用不产生任何转储（不刷屏）");
    }

    // shutdown 之后 VEH 确实摘掉了：再崩一次不该再产生转储
    crash_log_delete();
    hooktrace_enter(a, 0x11u, 0);
    __try {
        volatile int* bad = (volatile int*)0x10;
        *bad = 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    hooktrace_leave(a);
    {
        static char content[8192];
        check(!read_crash_log(content, (int)sizeof(content)),
              "★ shutdown 之后 VEH 真的摘掉了（异常被接住但不再转储）");
    }
    crash_log_delete();
}

int main()
{
    exe_dir();
    SetConsoleOutputCP(65001);
    printf("ac1-chinese-translate core 层单测（只链接 core.lib）\n");
    printf("工作目录: %s\n", g_dir);

    test_mem_gate();
    test_wstr_view();
    test_hook_signature();
    test_hook_hexdump();
    test_host_guard();
    test_log();
    test_log_truncation();
    test_str();
    test_ctr();
    test_file();
    test_trace_stack();
    test_trace_concurrent_register();
    test_trace_sampling();
    test_trace_crash_dump();

    printf("\n==== 通过 %d / 失败 %d ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
