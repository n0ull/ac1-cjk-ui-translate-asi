// modules/core/src/log.cpp —— 日志体系（等级 / 分类 / 单次 run；见 log.h 文件头）
#include "ac1/core/log.h"
#include "ac1/core/file.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace ac1 {
namespace core {
namespace {

char             g_path[MAX_PATH] = ""; // run_<stamp>.log 完整路径
char             g_stamp[32] = "";      // "YYYYMMDD_HHMMSS"
HANDLE           g_file = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_cs;
INIT_ONCE        g_csOnce = INIT_ONCE_STATIC_INIT;
volatile LONG    g_dropped = 0;

BOOL CALLBACK init_cs_once(PINIT_ONCE, PVOID, PVOID*)
{
    InitializeCriticalSection(&g_cs);
    return TRUE;
}

// INIT_ONCE 自带「只执行一次 + 对其余线程可见」。自建就绪标志不行：标志先置位、
//   临界区后初始化时，第二个线程会看到标志已就绪而临界区尚未初始化。
void ensure_cs() { InitOnceExecuteOnce(&g_csOnce, init_cs_once, NULL, NULL); }

void make_stamp(char* out, int cap)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf(out, (size_t)cap - 1, "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
              st.wMinute, st.wSecond);
    out[cap - 1] = 0;
}

// 统一的写行内核：level 名字 + 日期时间 + 消息体
//   autoErr=1：**渲染后的消息体**里含 "!!" 标记 ⇒ 提级到 ERR。
//   判据必须落在消息体上、而不是格式串上：字典这类经日志出口注入的模块走的是
//   g_log("%s", buf)，格式串恒为 "%s"，而 "[词典] !!" 里的标记写在 buf 里；
//   直接调 log_line 的模块也把标记写在中文类别前缀之后 ⇒ 只看 fmt[0..1] 会让
//   除 "!! " 开头那几处以外的所有事故行停在 INFO（实机日志 267 行全 INFO）。
void write_line(const char* lvl, int autoErr, const char* fmt, va_list ap)
{
    ensure_cs();
    EnterCriticalSection(&g_cs);
    HANDLE h = g_file;
    if (h == INVALID_HANDLE_VALUE) {
        InterlockedIncrement(&g_dropped);
        LeaveCriticalSection(&g_cs);
        return;
    }

    char body[2048];
    body[0] = 0; // CRT 一个字节都没写就返回负数时（非法格式串），消息体必须是空串而不是栈垃圾
    int n = _vsnprintf(body, sizeof(body) - 2, fmt, ap);
    // MSVC 的 _vsnprintf 截断时把 count 字节写满、不补 NUL、返回负数（见 str.cpp 的工具链
    //   陷阱）⇒ 两条路都自己补 NUL。只清 body[0] 会把已写好的消息体整条丢掉、只剩前缀。
    if (n < 0 || (size_t)n >= sizeof(body) - 2) body[sizeof(body) - 2] = 0;
    // 提级判据用 strstr 而不是比首字节：标记可以落在类别前缀之后（见本函数头）。
    if (autoErr && strstr(body, "!!")) lvl = "ERR";

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2208];
    int  m = _snprintf(line, sizeof(line), "[%04d-%02d-%02d %02d:%02d:%02d][%s] ", st.wYear, st.wMonth,
                       st.wDay, st.wHour, st.wMinute, st.wSecond, lvl);
    if (m < 0) m = 0;
    lstrcpynA(line + m, body, sizeof(line) - (size_t)m);
    size_t len = lstrlenA(line);
    line[len++] = '\r';
    line[len++] = '\n';
    if (len >= sizeof(line)) len = sizeof(line) - 1;

    DWORD wr = 0;
    WriteFile(h, line, (DWORD)len, &wr, NULL);
    LeaveCriticalSection(&g_cs);
}

// 关掉当前句柄。init_log 换 run 与 close_log 共用；调用方已持有 g_cs。
void close_file_locked()
{
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

} // namespace

void init_log(const char* dir)
{
    // 路径在锁外拼好：每一步都带容量自检，装不下就返回失败，而不是拿一个被截断的
    //   文件名去 CreateFile（那样 log_path() 会报出与实际打开的文件不同的名字）。
    //   拼不出来时不落文件、log_path() 留空（与传空串同形），调用方看得到「日志没开」。
    char newDir[MAX_PATH] = "", newPath[MAX_PATH] = "", newStamp[32] = "";
    int  built = 0;
    if (dir && dir[0]) {
        char sub[MAX_PATH], name[64];
        built = path_join(sub, (int)sizeof(sub), dir, "AC1_CJK");
        if (built) {
            make_stamp(newStamp, (int)sizeof(newStamp));
            _snprintf(name, sizeof(name) - 1, "run_%s.log", newStamp);
            name[sizeof(name) - 1] = 0;
            built = path_join(newPath, (int)sizeof(newPath), sub, name);
            if (built) lstrcpynA(newDir, sub, (int)sizeof(newDir));
        }
    }

    ensure_cs();
    EnterCriticalSection(&g_cs);
    close_file_locked();
    g_path[0] = 0;
    g_stamp[0] = 0;
    if (built) {
        lstrcpynA(g_path, newPath, (int)sizeof(g_path));
        lstrcpynA(g_stamp, newStamp, (int)sizeof(g_stamp));
        CreateDirectoryA(newDir, NULL); // 已存在 ⇒ ERROR_ALREADY_EXISTS，忽略
        g_file = CreateFileA(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_file == INVALID_HANDLE_VALUE) g_path[0] = 0;
    }
    LeaveCriticalSection(&g_cs);
}

void close_log()
{
    ensure_cs();
    EnterCriticalSection(&g_cs);
    close_file_locked();
    LeaveCriticalSection(&g_cs);
}

const char* log_run_stamp() { return g_stamp; }

void log_line(const char* fmt, ...)
{
    // 等级由 write_line 按消息体里的 "!!" 标记决定（autoErr=1），这里只给默认档。
    va_list ap;
    va_start(ap, fmt);
    write_line("INFO", 1, fmt, ap);
    va_end(ap);
}

void log_warn(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    write_line("WARN", 0, fmt, ap); // 显式档：不受 "!!" 标记影响
    va_end(ap);
}

const char* log_path() { return g_path; }

unsigned long log_dropped() { return (unsigned long)g_dropped; }

} // namespace core
} // namespace ac1
