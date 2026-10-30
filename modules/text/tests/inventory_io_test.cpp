// Intercept only inventory's Win32 writes; keep the production registration/flush path and real files.
#include <windows.h>
#include <stdio.h>
#include <string.h>

static unsigned     g_calls = 0, g_failAt = 0, g_shortAt = 0, g_zeroAt = 0;
static bool         g_byteWrites = false, g_keepFailing = false, g_registerDuringWrite = false;
static bool         g_registrationFinished = false;
static DWORD WINAPI register_during_write(void*);

static BOOL WINAPI inventory_write(HANDLE file, LPCVOID data, DWORD size, LPDWORD written,
                                   LPOVERLAPPED overlapped)
{
    ++g_calls;
    if (g_registerDuringWrite) {
        g_registerDuringWrite = false;
        HANDLE thread = CreateThread(NULL, 0, register_during_write, NULL, 0, NULL);
        if (thread) {
            g_registrationFinished = WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0;
            CloseHandle(thread);
        }
    }
    if (g_failAt && (g_calls == g_failAt || (g_keepFailing && g_calls > g_failAt))) {
        *written = 0;
        SetLastError(ERROR_DISK_FULL);
        return FALSE;
    }
    if (g_calls == g_zeroAt) {
        *written = 0;
        return TRUE;
    }
    if (g_byteWrites) size = 1;
    if (g_calls == g_shortAt) {
        const char* cjk = strstr((const char*)data, "中文");
        // Split inside UTF-8, so regenerating or repeating the prefix corrupts the final file.
        size = cjk ? (DWORD)(cjk - (const char*)data) + 1 : 1;
    }
    return WriteFile(file, data, size, written, overlapped);
}

#define WriteFile inventory_write
#include "../src/inventory.cpp"
#undef WriteFile

static DWORD WINAPI register_during_write(void*)
{
    ac1::text::inv_remember(L"Delta", 5, ac1::text::INV_F_MISS, (void*)0x1234);
    return 0;
}

static int  g_checks = 0, g_failures = 0;
static void check(bool ok, const char* message)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        printf("FAIL inventory I/O: %s\n", message);
    }
}

static bool read_inventory(const char* path, char* buffer, DWORD capacity)
{
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    BOOL  ok = ReadFile(file, buffer, capacity - 1, &got, NULL);
    CloseHandle(file);
    buffer[got] = 0;
    return ok != FALSE;
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const char* scenario = argv[1];
    int         firstDone = 0, firstHeader = 1;
    bool        openFailure = false;
    if (!strcmp(scenario, "header-fail")) {
        g_failAt = 1;
        firstHeader = 0;
    }
    else if (!strcmp(scenario, "entry-fail")) {
        g_failAt = 3;
        firstDone = 1;
    }
    else if (!strcmp(scenario, "header-partial")) {
        g_shortAt = 1;
        g_failAt = 2;
        firstHeader = 0;
    }
    else if (!strcmp(scenario, "entry-partial")) {
        g_shortAt = 3;
        g_failAt = 4;
        firstDone = 1;
    }
    else if (!strcmp(scenario, "short-write")) {
        g_byteWrites = true;
        firstDone = 3;
    }
    else if (!strcmp(scenario, "zero-write")) {
        g_zeroAt = 2;
    }
    else if (!strcmp(scenario, "repeat-fail")) {
        g_failAt = 3;
        g_keepFailing = true;
        firstDone = 1;
    }
    else if (!strcmp(scenario, "registration")) {
        g_registerDuringWrite = true;
        firstDone = 3;
    }
    else if (!strcmp(scenario, "open-fail")) {
        openFailure = true;
        firstHeader = 0;
    }
    else
        return 2;

    char executable[MAX_PATH], directory[MAX_PATH], sub[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameA(NULL, executable, sizeof(executable));
    char* slash = strrchr(executable, '\\');
    if (!slash) return 2;
    *slash = 0;
    int len = _snprintf(directory, sizeof(directory), "%s\\inventory-%s-%lu", executable, scenario,
                        GetCurrentProcessId());
    if (len <= 0 || len >= (int)sizeof(directory) || !CreateDirectoryA(directory, NULL)) return 2;
    _snprintf(sub, sizeof(sub), "%s\\AC1_Runtime", directory);
    if (!CreateDirectoryA(sub, NULL)) return 2;
    _snprintf(path, sizeof(path), "%s\\strings_.txt", sub);

    HANDLE blocker = INVALID_HANDLE_VALUE;
    if (openFailure) {
        blocker = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        check(blocker != INVALID_HANDLE_VALUE, "exclusive file blocks opening");
    }
    using namespace ac1::text;
    check(!strcmp(flag_tag(INV_F_HIT | INV_F_UNCHANGED), "同值 "),
          "unchanged takes precedence over a dictionary hit");
    check(!strcmp(flag_tag(INV_F_MISS | INV_F_NONASCII), "未命中·非ASCII"),
          "non-ASCII is an observation, not an icon classification");
    inv_remember(L"Alpha", 5, INV_F_MISS, (void*)0x1234);
    inv_remember(L"中文 Beta", 7, INV_F_MISS, (void*)0x1234);
    inv_remember(L"Gamma", 5, INV_F_MISS, (void*)0x1234);
    inventory_tick(directory);
    check(g_invDone == firstDone, "only complete entry lines advance the cursor");
    check(g_hdrDone == firstHeader, "only a complete header is committed");
    if (blocker != INVALID_HANDLE_VALUE) CloseHandle(blocker);
    if (g_failAt || g_zeroAt) {
        BOOL removed = DeleteFileA(path);
        check(!removed && GetLastError() == ERROR_SHARING_VIOLATION,
              "pending output keeps the same file protected across retries");
    }

    if (!strcmp(scenario, "entry-partial")) { inv_remember(L"中文 Beta", 7, INV_F_MISS, (void*)0x1234); }
    if (g_keepFailing) {
        unsigned before = g_calls;
        for (int i = 0; i < 3; ++i) inventory_tick(directory);
        check(g_calls == before + 3, "persistent failure performs one write attempt per tick");
        check(g_invDone == firstDone, "persistent failure does not discard pending entries");
        HANDLE thread = CreateThread(NULL, 0, register_during_write, NULL, 0, NULL);
        bool   finished = thread && WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0;
        check(finished && g_invN == 4, "registration progresses while output is failing");
        if (thread) CloseHandle(thread);
    }
    if (!strcmp(scenario, "registration")) {
        check(g_registrationFinished, "disk I/O does not hold the registration lock");
        check(g_invN == 4, "a new entry can be published during a flush");
    }

    g_failAt = g_zeroAt = g_shortAt = 0;
    g_keepFailing = g_byteWrites = false;
    inventory_tick(directory);
    check(g_invDone == g_invN && g_hdrDone, "retry drains all published entries");
    char contents[2048] = {};
    check(read_inventory(path, contents, sizeof(contents)), "result can be read from disk");
    const char* headerEnd = strstr(contents, "\r\n");
    check(!strncmp(contents, "# === run ", 10) && headerEnd,
          "exactly one complete run header starts the file");
    const char* expected = "[×1] 未命中  caller=00001234 Alpha\r\n"
                           "[×1] 未命中  caller=00001234 中文 Beta\r\n"
                           "[×1] 未命中  caller=00001234 Gamma\r\n";
    char        rows[1024];
    strcpy(rows, expected);
    if (g_invN == 4) strcat(rows, "[×1] 未命中  caller=00001234 Delta\r\n");
    check(headerEnd && !strcmp(headerEnd + 2, rows),
          "UTF-8 rows are complete, ordered, neither missing nor duplicated");
    unsigned before = g_calls;
    inventory_tick(directory);
    check(g_calls == before, "a drained inventory does not write again");
    check(DeleteFileA(path) != FALSE, "successful flush releases the file handle");
    RemoveDirectoryA(sub);
    RemoveDirectoryA(directory);
    printf("inventory I/O %s: checks=%d failures=%d\n", scenario, g_checks, g_failures);
    return g_failures ? 1 : 0;
}
