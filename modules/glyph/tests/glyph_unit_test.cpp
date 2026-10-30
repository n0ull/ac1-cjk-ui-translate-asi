// Synthetic resource transactions exercise production mutation code without game assets.
#include "../src/runtime.cpp"
#include <math.h>

using namespace ac1;
using namespace ac1::glyph;
namespace {
int passed = 0, failed = 0;
bool allocation_fails = false, map_fails = false;
unsigned allocation_calls = 0;
unsigned short page[256] = {};

void check(bool ok, const char* message)
{
    if (ok) ++passed; else ++failed;
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
}
void* __cdecl allocate(unsigned bytes, unsigned)
{
    ++allocation_calls;
    return allocation_fails ? NULL : HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
}
void __cdecl release(void* pointer) { if (pointer) HeapFree(GetProcessHeap(), 0, pointer); }
void __stdcall construct(void* data, unsigned stride, unsigned count, GlyphRecCtorFn)
{
    memset(data, 0, stride * count);
}
void __fastcall set_entry(void*, void*, unsigned cp, unsigned slot, unsigned)
{
    if (!map_fails) page[cp & 255] = (unsigned short)slot;
}
int __fastcall advance(void* font, void*, unsigned cp)
{
    unsigned char* table = *(unsigned char**)((unsigned char*)font + 0x38);
    return *(short*)(table + page[cp & 255] * 32 + 10);
}

void test_transaction(bool refuse_allocation, bool refuse_mapping)
{
    unsigned char font[0x45C] = {}, appended[32] = {};
    unsigned char* original = (unsigned char*)allocate(32, 0);
    if (!original) { check(false, "fixture allocation"); return; }
    const float uv[] = {0, 0, 1, 1};
    memcpy(original + 12, uv, sizeof(uv));
    // Native serialized contract: cp/width/height/xoff/yoff/advance, then UV.
    const unsigned short fields[] = {0x4E2D, 24, 24, 0, 2, 29};
    memcpy(appended, fields, sizeof(fields));
    core::RuntimeFont source = {};
    source.name = "fixture";
    source.original_count = source.appended_count = 1;
    source.original_width = source.original_height = 256;
    source.width = source.height = 1024;
    source.records = appended;
    Plan plan = {}; plan.source = &source;
    FontState state = {}; state.font = font; state.table = original; state.count = 1; state.plan = &plan;
    *(unsigned short*)(font + 0x34) = 1;
    *(unsigned char**)(font + 0x38) = original;
    memset(page, 0, sizeof(page));
    *(unsigned*)(font + 0x3C + 0x4E * 4) = (unsigned)page;
    allocation_fails = refuse_allocation; map_fails = refuse_mapping;
    g_font_failed = 0; g_retained_bytes = 0;
    append_font(state);
    const bool rejected = refuse_allocation || refuse_mapping;
    check((g_font_failed != 0) == rejected, "failed transaction disables further text mutation");
    if (rejected) {
        check(*(unsigned short*)(font + 0x34) == 1, "rejection preserves original count");
        check(*(unsigned char**)(font + 0x38) == original, "rejection restores original table");
        check(page[0x2D] == 0, "rejection does not publish a new charmap slot");
    } else {
        check(*(unsigned short*)(font + 0x34) == 2 && state.appended, "success publishes one appended record");
        check(page[0x2D] == 1 && advance(font, NULL, 0x4E2D) == 29, "native lookup reaches appended metrics");
        check(memcmp(state.table + 32, appended, 32) == 0, "serialized appended record is preserved");
        check(fabs(*(float*)(state.table + 20) - 0.25f) < 0.0001f, "original UV span follows atlas expansion");
        unsigned char* published = state.table;
        append_font(state);
        check(state.table == published && *(unsigned short*)(font + 0x34) == 2, "repeated preparation is idempotent");
    }
    if (state.retained) release(state.retained);
    release(state.table);
    allocation_fails = map_fails = false;
}

void test_no_append()
{
    unsigned char original[32] = {}, font[0x45C] = {};
    const float uv[] = {0, 0, 1, 1};
    memcpy(original + 12, uv, sizeof(uv));
    core::RuntimeFont source = {};
    source.original_count = 1;
    source.original_width = source.width = 256;
    source.original_height = source.height = 256;
    Plan plan = {}; plan.source = &source;
    FontState state = {}; state.font = font; state.table = original; state.count = 1; state.plan = &plan;
    *(unsigned short*)(font + 0x34) = 1;
    *(unsigned char**)(font + 0x38) = original;
    const unsigned before = allocation_calls;
    append_font(state);
    append_font(state);
    check(allocation_calls == before && !state.retained, "zero additions never allocate or retain another table");
    check(state.appended && state.scaled && state.table == original && state.count == 1,
          "zero additions preserve the original table and complete preparation");
    check(!memcmp(original + 12, uv, sizeof(uv)), "zero additions preserve original UVs");
}

void test_low_codepoint_advance()
{
    unsigned char font[0x45C] = {}, appended[32] = {};
    unsigned char* original = (unsigned char*)allocate(32, 0);
    if (!original) { check(false, "advance fixture allocation"); return; }
    InitializeCriticalSection(&g_lock);
    const unsigned short fields[] = {0x0416, 24, 24, 0, 2, 29};
    memcpy(appended, fields, sizeof(fields));
    core::RuntimeFont source = {};
    source.name = "fixture";
    source.original_count = source.appended_count = 1;
    source.original_width = source.original_height = 256;
    source.width = source.height = 1024; source.records = appended;
    memset(g_fonts, 0, sizeof(g_fonts)); memset(g_plans, 0, sizeof(g_plans));
    memset(page, 0, sizeof(page));
    g_host = select_host(core::HOST_FLAVOR_DX9, 0xE8936C99); g_base = 0x400000;
    *(unsigned*)font = g_base + g_host->font_vtables[0];
    *(unsigned short*)(font + 0x34) = 1;
    *(unsigned char**)(font + 0x38) = original;
    *(unsigned*)(font + 0x3C + 4 * 4) = (unsigned)page;
    g_plans[0].source = &source; g_plans[0].expanded = 1;
    FontState& state = g_fonts[0];
    state.font = font; state.table = original; state.count = 1; state.plan = &g_plans[0];
    g_gate = 1; g_font_failed = 0;
    check(hook_advance(font, NULL, 0x0416) == 29 && state.appended,
          "ADVANCE prepares an appended character below U+3000");
    check(page[0x16] == 1 && *(unsigned short*)(font + 0x34) == 2,
          "low-codepoint preparation publishes its native mapping before lookup");
    release(state.retained); release(state.table);
    memset(g_fonts, 0, sizeof(g_fonts)); memset(g_plans, 0, sizeof(g_plans));
    g_host = NULL;
    DeleteCriticalSection(&g_lock);
}
}

namespace ac1 { namespace core {
HostFlavor host_flavor() { return HOST_FLAVOR_NONE; }
unsigned long host_crc32() { return 0; }
unsigned host_base() { return 0; }
const char* host_flavor_name() { return "fixture"; }
}}

int main()
{
    core::init_log("");
    core::RuntimePackage unopened;
    check(!runtime_bind(unopened) && !runtime_install(0) && !runtime_ready(), "unopened package never enables resources");
    check(select_host(core::HOST_FLAVOR_DX9, 0xE8936C99) != NULL, "Dx9 identity selects its adapter");
    check(select_host(core::HOST_FLAVOR_DX10, 0x3AF8F9D0) != NULL, "Dx10 identity selects its adapter");
    check(select_host(core::HOST_FLAVOR_DX9, 0x3AF8F9D0) == NULL, "cross-host CRC is rejected");
    g_alloc = allocate; g_free = release; g_array_ctor = construct;
    g_set_entry = (SetEntryFn)set_entry; g_advance = (AdvanceFn)advance;
    test_transaction(false, false);
    test_transaction(true, false);
    test_transaction(false, true);
    test_no_append();
    test_low_codepoint_advance();
    g_gate = 1; g_font_failed = 0;
    for (unsigned i = 0; i < 4; ++i) {
        check(!runtime_ready(), "partial CPU resource set keeps text disabled");
        g_plans[i].expanded = 1; g_plans[i].uploaded = 0;
    }
    check(runtime_ready(), "all CPU atlases permit text before lazy GPU upload");
    g_font_failed = 1;
    check(!runtime_ready(), "font failure closes the readiness gate");
    g_font_failed = 0; g_gate = -1;
    check(!runtime_ready(), "early-load failure latch cannot enable text");
    printf("==== 通过 %d / 失败 %d ====\n", passed, failed);
    return failed ? 1 : 0;
}
