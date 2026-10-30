// Loader replay: background work is deliberately unscheduled until the game
// consumes its fonts. Initialization must finish on the loader's own call.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

namespace {
unsigned checks = 0, failures = 0, opened = 0, threads = 0;
bool reject_package = false, dictionary_initialized = false, bound = false;
bool text_hook = false, resource_hook = false, cpu_fonts_ready = false;
unsigned translated = 0;
bool dx10 = false;
bool reject_dictionary = false, partial_dictionary = false, dictionary_warning = false;
char dictionary_log[512] = {};
bool identity_logged = false, identity_warning = false, reject_identity = false;
void capture_dictionary_log(bool warning, const char* format, va_list args)
{
    if (!strcmp(format, "[RTP] asi_sha256=%s")) {
        const char* hex = va_arg(args, const char*);
        identity_logged = strlen(hex) == 64 && hex[0] == 'a' && hex[1] == 'b';
        return;
    }
    if (!strcmp(format, "[RTP] ASI file identity unavailable")) identity_warning = warning;
    if (strncmp(format, "[DICT] ", 7)) return;
    dictionary_warning = warning;
    vsnprintf(dictionary_log, sizeof(dictionary_log), format, args);
}
HANDLE WINAPI queued_thread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD)
{
    ++threads;
    return CreateEventW(NULL, TRUE, FALSE, NULL);
}
void check(bool ok, const char* message)
{
    ++checks;
    if (!ok) ++failures;
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
}
}

#define CreateThread queued_thread
#include "../src/dllmain.cpp"
#undef CreateThread

namespace ac1 {
namespace core {
bool file_read_all(const char*, unsigned char** data, int* bytes, int)
{
    static unsigned char fixture[] = "identity fixture";
    *data = reject_identity ? NULL : fixture;
    *bytes = reject_identity ? 0 : sizeof(fixture);
    return !reject_identity;
}
void file_free(void*) {}
bool runtime_sha256(const void*, unsigned, unsigned char digest[32])
{
    memset(digest, 0xAB, 32);
    return true;
}
RuntimePackage::RuntimePackage() : data_(NULL), dictionary_(NULL), dictionary_bytes_(0), error_("fixture") {}
RuntimePackage::~RuntimePackage() {}
bool RuntimePackage::open(const char*) { ++opened; return !reject_package; }
const unsigned char* RuntimePackage::dictionary(unsigned* bytes) const
{
    static const unsigned char data[] = "OPTIONS\ttranslation\n";
    *bytes = sizeof(data) - 1;
    return data;
}
bool path_join(char* out, int cap, const char* dir, const char* name)
{
    if (strlen(dir) + strlen(name) >= (size_t)cap) return false;
    strcpy_s(out, cap, dir);
    strcat_s(out, cap, name);
    return true;
}
void init_log(const char*) {}
void log_line(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    capture_dictionary_log(false, format, args);
    va_end(args);
}
void log_warn(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    capture_dictionary_log(true, format, args);
    va_end(args);
}
void log_error(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    capture_dictionary_log(true, format, args);
    va_end(args);
}
const char* log_run_stamp() { return "fixture"; }
const char* log_path() { return "fixture.log"; }
bool host_check() { return true; }
HostFlavor host_flavor() { return dx10 ? HOST_FLAVOR_DX10 : HOST_FLAVOR_DX9; }
unsigned host_base() { return 0x400000; }
const char* host_name() { return "fixture"; }
const char* host_detail() { return "fixture"; }
int hooktrace_veh_install(const char*) { return 1; }
const char* hooktrace_crash_path() { return "fixture-crash.log"; }
}
namespace dict {
void set_log(LogFn, LogFn) {}
bool init_bytes(const unsigned char*, int) { dictionary_initialized = !reject_dictionary; return dictionary_initialized; }
const DictStats& stats()
{
    static DictStats value = {};
    value.keys = reject_dictionary ? 0 : 1;
    value.rejected_lines = reject_dictionary || partial_dictionary ? 1 : 0;
    return value;
}
const char* status()
{
    if (reject_dictionary) return "result=rejected reason=unsupported_value_unit failure_line=5";
    if (partial_dictionary) return "result=partial reason=invalid_utf8_value first_rejected_line=3";
    return "result=clean reason=none";
}
}
namespace glyph {
bool runtime_bind(const core::RuntimePackage&) { bound = dictionary_initialized; return bound; }
bool runtime_install(unsigned) { resource_hook = bound; return resource_hook; }
bool runtime_ready() { return cpu_fonts_ready; }
void runtime_status(char* output, int cap) { if (cap) output[0] = 0; }
}
namespace text {
RuntimeResourcesReady readiness = NULL;
void set_runtime_resources(RuntimeResourcesReady callback) { readiness = callback; }
bool install() { text_hook = dictionary_initialized && readiness; return text_hook; }
const char* status_line() { return "fixture"; }
const char* detail_line() { return "fixture"; }
void inventory_tick(const char*) {}
}
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dx10") == 0) dx10 = true;
        if (strcmp(argv[i], "--reject-package") == 0) reject_package = true;
        if (strcmp(argv[i], "--reject-dictionary") == 0) reject_dictionary = true;
        if (strcmp(argv[i], "--partial-dictionary") == 0) partial_dictionary = true;
    }
    printf("Startup host: %s\n", dx10 ? "Dx10" : "Dx9");
    check(DllMain(GetModuleHandleW(NULL), DLL_PROCESS_ATTACH, NULL) != FALSE, "loader attaches ASI");
    check(opened == 0 && threads == 0, "DllMain neither initializes nor queues a racing startup thread");
    typedef void (__cdecl* InitializeFn)();
    InitializeFn initialize = (InitializeFn)GetProcAddress(GetModuleHandleW(NULL), "InitializeASI");
    check(initialize != NULL, "loader resolves the undecorated InitializeASI entry");
    if (initialize) initialize();
    check(identity_logged, "startup records a full lowercase ASI file digest");
    reject_identity = true;
    log_runtime_identity();
    check(identity_warning, "identity read failure is explicit and does not alter installed hooks");
    reject_identity = false;
    if (reject_package) {
        check(opened == 1 && !text_hook && !resource_hook && threads == 0,
              "rejected package returns before game hooks or monitoring are published");
    } else if (reject_dictionary) {
        check(opened == 1 && !bound && !text_hook && !resource_hook && threads == 0,
              "rejected dictionary returns before resource binding, hooks or monitoring");
        check(dictionary_warning && strstr(dictionary_log, "result=rejected") &&
              strstr(dictionary_log, "reason=unsupported_value_unit failure_line=5"),
              "rejected dictionary preserves reason and line in a warning");
    } else {
        check(dictionary_warning == partial_dictionary &&
              strstr(dictionary_log, partial_dictionary ? "result=partial" : "result=clean"),
              "dictionary outcome reaches the startup log with matching severity");
        // This is the native event order from the English-only run. No queued
        // callback is dispatched before the host loads its font resources.
        cpu_fonts_ready = resource_hook;
        if (text_hook && ac1::text::readiness && ac1::text::readiness()) ++translated;
        printf("Host consumes fonts: prepared=%u replacements=%u\n", cpu_fonts_ready ? 4u : 0u, translated);
        check(opened == 1 && resource_hook && text_hook,
              "package and hooks are ready when the loader resumes the game");
        check(cpu_fonts_ready && translated == 1,
              "first resource/text consumption reaches CJK instead of remaining entirely English");
        check(threads == 1, "only monitoring is queued after initialization completes");
    }
    if (initialize) initialize();
    check(opened == 1 && threads == (reject_package || reject_dictionary ? 0u : 1u),
          "repeated loader initialization does not reopen the package or install hooks twice");
    printf("Startup replay: checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
