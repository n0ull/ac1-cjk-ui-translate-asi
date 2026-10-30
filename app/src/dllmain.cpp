// 装配层：同步初始化宿主、包与模块，完成后启动日志监控线程。

#include "ac1/core/file.h"
#include "ac1/core/host.h"
#include "ac1/core/log.h"
#include "ac1/core/trace.h"
#include "ac1/core/runtime_package.h"

#include "ac1/dict/dict.h"
#include "ac1/glyph/runtime.h"
#include "ac1/text/text.h"

#include <windows.h>

namespace {

char g_dir[MAX_PATH] = "";  // ASI 所在目录（带尾反斜杠）
char g_self[MAX_PATH] = ""; // ASI 完整路径

// Resident hooks borrow package views for the process lifetime.
ac1::core::RuntimePackage g_package;

void log_runtime_identity()
{
    unsigned char* data = NULL;
    unsigned char digest[32] = {};
    int bytes = 0;
    bool ok = ac1::core::file_read_all(g_self, &data, &bytes, ac1::core::FILE_READ_MAX_DEFAULT);
    if (ok) ok = ac1::core::runtime_sha256(data, (unsigned)bytes, digest);
    ac1::core::file_free(data);
    if (!ok) {
        ac1::core::log_warn("[RTP] ASI file identity unavailable");
        return;
    }
    char hex[65] = {};
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    // This identifies the adjacent file, not an attestation of loaded memory.
    ac1::core::log_line("[RTP] asi_sha256=%s", hex);
}

bool start_runtime_package()
{
    const ac1::core::HostFlavor host = ac1::core::host_flavor();
    if (host != ac1::core::HOST_FLAVOR_DX9 && host != ac1::core::HOST_FLAVOR_DX10) return false;
    char path[MAX_PATH];
    if (!ac1::core::path_join(path, sizeof(path), g_dir, "AC1_Language.bin")) return false;
    if (!g_package.open(path)) {
        ac1::core::log_warn("[RTP] package rejected: %s", g_package.error());
        return false;
    }
    char digest[65] = {};
    if (g_package.digest()) {
        static const char digits[] = "0123456789abcdef";
        for (unsigned i = 0; i < 32; ++i) {
            digest[i * 2] = digits[g_package.digest()[i] >> 4];
            digest[i * 2 + 1] = digits[g_package.digest()[i] & 15];
        }
    }
    ac1::core::log_line("[RTP] language=%s embedded_digest=%s package_version=2 dictionary_rules=2",
                        g_package.language_id(), digest);
    unsigned bytes = 0;
    const unsigned char* dictionary = g_package.dictionary(&bytes);
    ac1::dict::set_log(&ac1::core::log_line, &ac1::core::log_error);
    const bool dictionary_ready = ac1::dict::init_bytes(dictionary, (int)bytes);
    if (!dictionary_ready || ac1::dict::stats().rejected_lines)
        ac1::core::log_warn("[DICT] %s", ac1::dict::status());
    else
        ac1::core::log_line("[DICT] %s", ac1::dict::status());
    if (!dictionary_ready) return false;
    if (!ac1::glyph::runtime_bind(g_package)) return false;
    ac1::text::set_runtime_resources(&ac1::glyph::runtime_ready);
    if (!ac1::text::install() || !ac1::glyph::runtime_install(ac1::core::host_base())) return false;
    ac1::core::log_line("[RTP] single package active dictionary_keys=%d text_replace=%d", ac1::dict::stats().keys, TEXT_REPLACE);
    return true;
}

DWORD WINAPI monitor_runtime(LPVOID)
{
    for (;;) {
        char resource_status[512];
        ac1::glyph::runtime_status(resource_status, sizeof(resource_status));
        ac1::core::log_line("[状态] %s", resource_status);
        ac1::core::log_line("[状态] %s", ac1::text::status_line());
        ac1::core::log_line("[状态] %s", ac1::text::detail_line());
        ac1::text::inventory_tick(g_dir);
        Sleep(10000);
    }
}

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
    // A truncated path cannot identify the package or log directory.
    if (!n || n >= MAX_PATH) return false;
    // 两块缓冲都是 MAX_PATH，等长定长拷贝，既不截断也不溢出。
    lstrcpynA(g_dir, g_self, MAX_PATH);
    char* slash = NULL;
    for (char* p = g_dir; *p; p++)
        if (*p == '\\') slash = p;
    // 找不到分隔符 ⇒ 推不出目录。已加载模块的 GetModuleFileNameA 必返回含盘符与
    //   反斜杠的绝对路径，所以这里是防御性分支；但它必须让 find_self 失败，
    //   否则会带着空目录继续往下走，资源路径退化成「相对游戏 CWD」。
    if (!slash) {
        g_dir[0] = 0;
        return false;
    }
    slash[1] = 0;
    return true;
}

void start_plugin()
{
    // Resolve inputs relative to the ASI, never to the game working directory.
    if (!find_self()) {
        OutputDebugStringA("[ac1_cjk] 无法确定 ASI 自身目录，跳过全部安装。\n");
        return;
    }
    ac1::core::init_log(g_dir);

    ac1::core::log_line("================================================================");
    ac1::core::log_line("=== ac1_cjk 已注入 pid=%lu run=%s ===", (unsigned long)GetCurrentProcessId(),
                        ac1::core::log_run_stamp());
    ac1::core::log_line("[启动] ASI=%s", g_self);
    ac1::core::log_line("[启动] 日志=%s", ac1::core::log_path());
    log_runtime_identity();

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
        ac1::core::log_error("!! 宿主不受支持（%s）⇒ **不装任何钩**。", ac1::core::host_name());
        // 名字为空（路径被截断/拿不到）时上面那行看不出原因 ⇒ 把摘要一并打出来：
        //   大小=0 与 CRC32=00000000 指向「路径拿不到」，与真正的「版本不符」区分开。
        ac1::core::log_line("   %s", ac1::core::host_detail());
        ac1::core::log_line("=== ac1_cjk 结束（未装钩）===");
        return;
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

    ac1::core::log_line("[RTP] synchronous InitializeASI startup thread=%lu", (unsigned long)GetCurrentThreadId());
    if (!start_runtime_package()) {
        ac1::core::log_warn("[RTP] startup incomplete; resource and text mutations disabled");
        return;
    }
    HANDLE monitor = CreateThread(NULL, 0, monitor_runtime, NULL, 0, NULL);
    if (monitor) CloseHandle(monitor);
    else ac1::core::log_warn("[RTP] monitoring thread unavailable; installed hooks remain active");
}

BOOL CALLBACK initialize_runtime(PINIT_ONCE, PVOID, PVOID*)
{
    start_plugin();
    return TRUE;
}

} // namespace

// Ultimate ASI Loader calls this after LoadLibrary, outside the loader lock.
// Returning before hook installation would let the host load all four atlases
// while only the background initializer is queued.
extern "C" __declspec(dllexport) void InitializeASI()
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, initialize_runtime, NULL, NULL);
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}
