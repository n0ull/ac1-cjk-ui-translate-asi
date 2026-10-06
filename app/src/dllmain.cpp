// app/src/dllmain.cpp —— 装配层。整个 ac1_cjk.asi 只有这一个 .cpp。
//
// 本文件负责的范围：DllMain、工作线程入口、宿主守卫、字形清单路径的拼接、
// 各模块 install() 的依次调用，以及一条常驻状态循环。读门、挂点搬迁、词典解析、
// 文本替换都在 modules/ 里。
//
// 依赖：core / dict / text / glyph 的公开头；编译产物里除了本文件，
//       全部来自四个 .lib（见 app/build.ps1）。

#include "ac1/core/file.h"
#include "ac1/core/host.h"
#include "ac1/core/log.h"
#include "ac1/core/trace.h"
#include "ac1/core/mem.h"

#include "ac1/dict/dict.h"
#include "ac1/glyph/glyph.h"
#include "ac1/text/text.h"

#include <windows.h>

namespace {

char g_dir[MAX_PATH] = "";  // ASI 所在目录（带尾反斜杠）
char g_self[MAX_PATH] = ""; // ASI 完整路径

// 找到本 ASI 自己的路径（用本函数所在模块反查，不依赖任何全局句柄）
bool find_self()
{
    HMODULE hm = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(DWORD_PTR)&find_self, &hm) ||
        !hm)
        return false;
    const DWORD n = GetModuleFileNameA(hm, g_self, MAX_PATH);
    // 返回值等于 nSize 是「被截断」，不是成功。截断出来的 g_dir 指向一个不存在的
    //   目录 ⇒ 日志开不出来、forge 闸门全关，而状态行照报「已注入」，
    //   与下面 worker() 里那条「拿不到目录就不装任何东西」的承诺不符。
    // 按 fail-closed 处理：当作拿不到目录。
    if (!n || n >= MAX_PATH) return false;
    // 两块缓冲都是 MAX_PATH，等长定长拷贝，既不截断也不溢出。
    lstrcpynA(g_dir, g_self, MAX_PATH);
    char* slash = NULL;
    for (char* p = g_dir; *p; p++)
        if (*p == '\\') slash = p;
    // 找不到分隔符 ⇒ 推不出目录。已加载模块的 GetModuleFileNameA 必返回含盘符与
    //   反斜杠的绝对路径，所以这里是防御性分支；但它必须让 find_self 失败，
    //   否则会带着空目录继续往下走，四条路径全部退化成「相对游戏 CWD」。
    if (!slash) {
        g_dir[0] = 0;
        return false;
    }
    slash[1] = 0;
    return true;
}

DWORD WINAPI worker(LPVOID)
{
    // 拿不到自己的目录 ⇒ init_log / dict::init / 字形清单 / forge sidecar 四条路径
    // 全按进程当前工作目录解析且必然落空 ⇒ 不装任何东西；此时 core 日志尚未打开，出口只有调试器。
    if (!find_self()) {
        OutputDebugStringA("[ac1_cjk] 无法确定 ASI 自身目录，跳过全部安装。\n");
        return 0;
    }
    ac1::core::init_log(g_dir);

    ac1::core::log_line("================================================================");
    ac1::core::log_line("=== ac1_cjk 已注入 pid=%lu run=%s ===", (unsigned long)GetCurrentProcessId(),
                        ac1::core::log_run_stamp());
    ac1::core::log_line("[启动] ASI=%s", g_self);
    ac1::core::log_line("[启动] 日志=%s", ac1::core::log_path());

    // ---- 宿主守卫：不认识的宿主到此为止，一个钩都不装 ----
    // 守卫必须早于一切安装，这是硬数据依赖而不是时序偏好：
    //   hook_rva()（core/src/hook.cpp:82-88）直接 switch (host_flavor())，
    //   而 host_flavor()（core/src/host.cpp:129）只是 return g_flavor，没有惰性初始化，
    //   初值 HOST_FLAVOR_NONE，只有 host_check() 内部才按 exe 名改写它。
    // 守卫挪到装钩之后 ⇒ 每条 HookSpec 都在 hook.cpp:111-114 的
    //   「当前宿主没有适用的 RVA 列」上跳过，一个钩都装不上。
    // 顺带一提，守卫本身是启动路径上最重的一步（要逐字节 CRC 整个宿主 exe，
    //   Dx10 那边约 24 MB），所以「晚装」的代价是亚秒级，不是秒级。
    if (!ac1::core::host_check()) {
        ac1::core::log_line("!! 宿主不受支持（%s）⇒ **不装任何钩**。", ac1::core::host_name());
        // 名字为空（路径被截断/拿不到）时上面那行看不出原因 ⇒ 把摘要一并打出来：
        //   大小=0 与 CRC32=00000000 指向「路径拿不到」，与真正的「版本不符」区分开。
        ac1::core::log_line("   %s", ac1::core::host_detail());
        ac1::core::log_line("=== ac1_cjk 结束（未装钩）===");
        return 0;
    }
    ac1::core::log_line("[启动] 宿主 ✓ %s", ac1::core::host_detail());

    // 崩溃取证设施：装 VEH，必须早于所有钩——它要在异常分发时读出
    //「崩的那一刻我们的哪个钩在栈上」。详见 modules/core/include/ac1/core/trace.h。
    const int   vehOk = ac1::core::hooktrace_veh_install(g_dir);
    const char* crashPath = ac1::core::hooktrace_crash_path();
    if (vehOk && crashPath[0]) {
        ac1::core::log_line("[启动] 崩溃转储已就绪（%s）⇒ 崩了会在那里写下当时的钩栈", crashPath);
    }
    else {
        ac1::core::log_warn("[启动] 崩溃转储不可用（veh=%d path=\"%s\"）⇒ 崩溃时不会产生转储。", vehOk,
                            crashPath);
    }

    // forge 补丁核验（启动期文件核验）：读 sidecar、比对 forge 尺寸，
    // 结果即整套字形补丁 + 文本替换的总闸（两层同闸，闸门关 = 纯原版界面）。
    // 必须在装钩之前——闸门值由 glyph 层与 text 层读取。
    // 核验一行的判决日志由 glyph 模块自己打。
    const int forgePatch = ac1::glyph::check_forge_patch(g_dir);
    ac1::text::set_forge_patch(forgePatch);

    // ---- 接线：dict 不依赖 core，日志/WARN 两个出口由装配层注入 ----
    ac1::dict::set_log(&ac1::core::log_line);
    ac1::dict::set_warn_log(&ac1::core::log_warn);

    // ---- 依次装各模块（顺序即依赖顺序：词典先就绪，文本层才谈得上替换）----
    ac1::dict::init(g_dir);

    // 字形层：引擎 API 的地址表与调用约定适配收在 glyph 模块内部。
    // 基址传 0：bind_engine_at / install 内部都是 `base ? base : hook_base()`，
    // 而 hook_base() 又回落 host_base() ⇒ 与在装配层显式取一次完全同值。
    // 装配层不需要知道这条回退链，也就不用为此多引一个 core/hook.h。
    ac1::glyph::bind_engine_at(0);

    // 字形集清单：缺文件是正常状态（0 套 ⇒ 每套都 no_match，钩子照装）
    char glyphManifest[MAX_PATH];
    if (ac1::core::path_join(glyphManifest, (int)sizeof(glyphManifest), g_dir,
                             ac1::glyph::manifest_file_name())) {
        ac1::glyph::load_manifest(glyphManifest);
    }
    else {
        ac1::core::log_warn("[启动] 字形清单路径拼接失败 ⇒ 不施加任何字形补丁。");
    }

    const int textInstalled = ac1::text::install();
    const int glyphInstalled = ac1::glyph::install(0);

    // 容量按被调用方的最坏情况给，不按实测典型值：
    //   glyph::status() 三段格式串典型 251~252 字节（UTF-8，汉字 3 字节），
    //   字段全满约 363 字节 ⇒ 256 会在「任何一个计数器多一位」时静默截掉末段
    //   （拉黑=N 让渡=NNNB）。core::Str 满了只补 NUL，不告警、不计数、不记日志。
    //   hooktrace_status() 当前 4 个钩约 135 字节，32 槽满载约 1200 字节。
    // 两处统一给 512，覆盖真实形态并留足余量。
    char glyphStatus[512];
    char hookStatus[512];
    ac1::glyph::status(glyphStatus, sizeof(glyphStatus));
    ac1::core::log_line("[启动] 模块就位：词典=%s 文本=%s 字形=%d（%s）",
                        ac1::dict::ready() ? "就绪" : "**未就绪（只观测）**",
                        textInstalled ? "漏斗已装" : "**漏斗未装**", glyphInstalled, glyphStatus);

    for (;;) {
        Sleep(10000);
        ac1::core::log_line("[状态] %s", ac1::text::status_line());
        ac1::core::log_line("[状态] %s", ac1::text::detail_line());
        ac1::text::inventory_tick(g_dir); // 字符串全记录：新增条目刷盘
        ac1::core::log_line("[状态] 词典 %s", ac1::dict::ready() ? ac1::dict::status() : "未就绪（只观测）");
        // 字形状态每轮重新取：它的计数随运行期增长
        ac1::glyph::status(glyphStatus, sizeof(glyphStatus));
        // 码位命中/漏字的 Top：内部有"种类数变过才打"的闸门 ⇒ 空转时一行都不打
        ac1::glyph::report_tops();
        // 钩执行迹（core 的）：每个钩累计被进入多少次，回答「哪些钩是活的」；
        // crash 日志回答「崩的那一刻是谁」——两者配套。
        ac1::core::hooktrace_status(hookStatus, (int)sizeof(hookStatus));
        ac1::core::log_line("[状态] %s ｜ %s ｜ 读门异常=%lu 丢弃日志=%lu", glyphStatus, hookStatus,
                            ac1::core::mem_faults(), ac1::core::log_dropped());
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
