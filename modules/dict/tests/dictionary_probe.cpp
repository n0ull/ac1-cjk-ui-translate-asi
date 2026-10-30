#include "ac1/core/file.h"
#include "ac1/dict/dict.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vector>

namespace {

unsigned g_errorLogs = 0;

void capture_log(const char* format, va_list args)
{
    char message[2048] = {};
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    printf("log=%s\n", message);
}

void capture_info(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    capture_log(format, args);
    va_end(args);
}

void capture_error(const char* format, ...)
{
    ++g_errorLogs;
    va_list args;
    va_start(args, format);
    capture_log(format, args);
    va_end(args);
}

bool read_input(const char* path, std::vector<unsigned char>& bytes)
{
    FILE* file = NULL;
    if (fopen_s(&file, path, "rb") || !file) return false;
    bool ok = fseek(file, 0, SEEK_END) == 0;
    const long size = ok ? ftell(file) : -1;
    // One byte above the limit exercises init_bytes' own input-size rejection.
    ok = size >= 0 && size <= ac1::core::FILE_READ_MAX_DEFAULT + 1;
    if (ok) {
        bytes.resize((size_t)size);
        ok = fseek(file, 0, SEEK_SET) == 0;
        if (ok && size) ok = fread(&bytes[0], 1, (size_t)size, file) == (size_t)size;
    }
    return fclose(file) == 0 && ok;
}

bool export_queries(FILE* input, FILE* output)
{
    unsigned size = 0;
    wchar_t key[DICT_KEY_MAX] = {};
    for (;;) {
        const size_t got = fread(&size, 1, sizeof(size), input);
        if (!got) return !ferror(input) && !ferror(output);
        if (got != sizeof(size)) return false;
        if (!size || size > DICT_KEY_MAX || fread(key, sizeof(wchar_t), size, input) != size)
            return false;
        const ac1::dict::DictVal value = ac1::dict::lookup(key, (int)size);
        const unsigned lengths[] = { size, (unsigned)value.len };
        if (fwrite(lengths, sizeof(lengths), 1, output) != 1 ||
            fwrite(key, sizeof(wchar_t), size, output) != size) return false;
        if (value.len && fwrite(value.p, sizeof(wchar_t), (size_t)value.len, output) != (size_t)value.len)
            return false;
    }
}

} // namespace

int main(int argc, char** argv)
{
    static_assert(sizeof(wchar_t) == 2, "Probe exports Windows UTF-16 code units");
    if (argc != 4 && argc != 5) return 2;
    if (argc == 5 && strcmp(argv[4], "--seed")) return 2;
    ac1::dict::set_log(&capture_info, &capture_error);
    if (argc == 5) {
        const unsigned char seed[] = "Prior\told\n";
        if (!ac1::dict::init_bytes(seed, sizeof(seed) - 1)) return 2;
        g_errorLogs = 0;
    }
    std::vector<unsigned char> bytes;
    if (!read_input(argv[1], bytes)) return 2;
    const bool accepted = ac1::dict::init_bytes(bytes.empty() ? NULL : &bytes[0], (int)bytes.size());
    printf("%s\n", ac1::dict::status());
    const ac1::dict::DictStats& stats = ac1::dict::stats();
    // This test protocol observes public behavior; it does not decode dictionary rows.
    const unsigned header[] = {
        0x31424441, 2, ac1::dict::RULES_VERSION, (unsigned)accepted, (unsigned)ac1::dict::ready(),
        (unsigned)stats.keys, (unsigned)stats.keypool_used, (unsigned)stats.valpool_used,
        (unsigned)stats.sup_lines, (unsigned)stats.sup_new, (unsigned)stats.sup_duplicate,
        (unsigned)stats.sup_comment, (unsigned)stats.sup_empty,
        (unsigned)stats.sup_bad, (unsigned)stats.sup_keybad, (unsigned)stats.sup_valbad,
        (unsigned)stats.sup_poolfull, (unsigned)stats.sup_keyfull, (unsigned)stats.sup_keylong,
        (unsigned)stats.sup_vallong, (unsigned)stats.sup_fallback_full, g_errorLogs
    };
    FILE* queries = NULL;
    FILE* output = NULL;
    if (fopen_s(&queries, argv[2], "rb") || !queries) return 2;
    if (fopen_s(&output, argv[3], "wb") || !output) {
        fclose(queries);
        return 2;
    }
    bool ok = fwrite(header, sizeof(header), 1, output) == 1 && export_queries(queries, output);
    ok = fclose(queries) == 0 && ok;
    ok = fclose(output) == 0 && ok;
    return ok ? 0 : 2;
}
