// Optional integration test: game-owned bytes stay outside the release tree.
// This module's private host seam is injected; all mutation logic is production.
#include "../src/runtime.cpp"
#include "ac1/dict/dict.h"
#include "ac1/text/text.h"
#include <math.h>

using namespace ac1;
using namespace ac1::glyph;
namespace {
unsigned failures = 0, checks = 0;
void* blocks[512] = {};
bool refuse_alloc = false, refuse_reserve = false, refuse_upload = false;
unsigned refuse_cp = 0, wrong_advance_cp = 0;
unsigned draw_arguments[8] = {}, load_arguments[3] = {};
Plan* change_upload_generation = NULL;
core::HostFlavor fixture_flavor = core::HOST_FLAVOR_NONE;
unsigned long fixture_crc = 0;
unsigned fixture_font_vtable = 0, fixture_ctm_vtable = 0, fixture_allocation_tag = 0;
unsigned fixture_callers[7] = {};
unsigned upload_arguments[3] = {};

void check(bool ok, const char* message)
{
    ++checks;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}
void* __cdecl fake_alloc(unsigned bytes, unsigned flags)
{
    if (flags) ++failures;
    if (refuse_alloc) return NULL;
    void* result = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes);
    for (unsigned i = 0; i < 512; ++i) if (!blocks[i]) { blocks[i] = result; return result; }
    ++failures;
    return NULL;
}
void __cdecl fake_free(void* pointer)
{
    if (!pointer) return;
    for (unsigned i = 0; i < 512; ++i) {
        if (blocks[i] != pointer) continue;
        blocks[i] = NULL;
        HeapFree(GetProcessHeap(), 0, pointer);
        return;
    }
    ++failures;
}
void __stdcall fake_array(void* data, unsigned stride, unsigned count, GlyphRecCtorFn)
{
    if (stride != 32 || count > 4096) ++failures;
    memset(data, 0, stride * count);
}
void __fastcall fake_set(void* map, void*, unsigned cp, unsigned slot, unsigned flags)
{
    if (flags) ++failures;
    if (cp == refuse_cp) return;
    unsigned* pages = (unsigned*)map;
    if (!pages[cp >> 8]) pages[cp >> 8] = (unsigned)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 512);
    ((unsigned short*)pages[cp >> 8])[cp & 255] = (unsigned short)slot;
}
int __fastcall fake_advance(void* font, void*, unsigned cp)
{
    if (cp == wrong_advance_cp) return -1;
    unsigned short slot = 0;
    if (!read_slot(font, cp, &slot)) return -2;
    unsigned table = ((unsigned*)font)[14];
    return *(short*)(table + slot * 32 + 10);
}
unsigned __fastcall fake_reserve(void* array, void*, unsigned bytes, unsigned stride, unsigned tag, unsigned flags)
{
    if (stride != 1 || tag != 1234 || flags) ++failures;
    if (refuse_reserve) return 0;
    unsigned* fields = (unsigned*)array;
    void* next = HeapReAlloc(GetProcessHeap(), 0, (void*)fields[0], bytes);
    if (!next) return 0;
    fields[0] = (unsigned)next;
    fields[2] = 0x40000000 | bytes;
    return 1;
}
void* __fastcall fake_upload(void* destination, void*, void* renderer, void* compiled)
{
    upload_arguments[0] = (unsigned)destination;
    upload_arguments[1] = (unsigned)renderer;
    upload_arguments[2] = (unsigned)compiled;
    if (change_upload_generation) InterlockedIncrement(&change_upload_generation->generation);
    return refuse_upload ? NULL : (void*)0x12345678;
}
int __fastcall fake_reset(void* font, void*, unsigned count)
{
    unsigned* fields = (unsigned*)font;
    fake_free((void*)fields[14]);
    fields[14] = count ? (unsigned)fake_alloc(count * 32, 0) : 0;
    fields[13] = count;
    for (unsigned i = 0; i < 256; ++i) {
        if (fields[15+i]) HeapFree(GetProcessHeap(), 0, (void*)fields[15+i]);
        fields[15+i] = 0;
    }
    return 0x76543210;
}
int __fastcall fake_load(void*, void*, void*) { return 91; }
int __fastcall fake_prepare(void*, void*, void*) { return 92; }
int __fastcall fake_font_load(void*, void*, unsigned a, unsigned b, unsigned c)
{
    load_arguments[0] = a; load_arguments[1] = b; load_arguments[2] = c;
    return 0x101;
}
int __fastcall fake_width(void* font, void*, const unsigned short* text)
{
    int result = 0;
    while (*text) result += fake_advance(font, NULL, *text++);
    return result;
}
int __fastcall fake_draw(void*, void*, unsigned a, unsigned b, unsigned c, unsigned d, unsigned e, unsigned f, unsigned g, unsigned h)
{
    unsigned arguments[] = {a,b,c,d,e,f,g,h};
    memcpy(draw_arguments, arguments, sizeof(arguments));
    return 0x2468;
}
int __fastcall fake_destroy(void* compiled, void*)
{
    unsigned* fields = (unsigned*)compiled;
    HeapFree(GetProcessHeap(), 0, (void*)fields[1]); fields[1] = 0;
    return 0x1357;
}

unsigned char* read_capture(const char* path, unsigned size)
{
    FILE* stream = NULL;
    if (fopen_s(&stream, path, "rb") || !stream) return NULL;
    unsigned char* data = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, size);
    bool ok = data && fread(data, size, 1, stream) == 1 && fgetc(stream) == EOF;
    fclose(stream);
    return ok ? data : NULL;
}
void make_font(unsigned* font, const unsigned char* records, unsigned count)
{
    memset(font, 0, 0x45C);
    font[0] = g_base + fixture_font_vtable; font[13] = count;
    font[14] = (unsigned)fake_alloc(count * 32, 0);
    memcpy((void*)font[14], records, count * 32);
    for (unsigned i = 0; i < count; ++i) fake_set(font + 15, NULL, *(unsigned short*)(records + i * 32), i, 0);
}

void test_resource(unsigned index, const unsigned char* original)
{
    Plan& plan = g_plans[index];
    const core::RuntimeFont& source = *plan.source;
    unsigned font[0x45C/4] = {};
    g_base = 0;
    make_font(font, original, source.original_count);
    unsigned previous = font[14];
    ((unsigned char*)previous)[6] ^= 1;
    check(hook_font_load(font, NULL, 111, 222, 333) == 0x101 && load_arguments[0] == 111 && load_arguments[2] == 333,
          "font load preserves native integer EAX and three arguments");
    check(font[14] == previous && font[13] == source.original_count, "changed original record identity cannot publish a font");
    ((unsigned char*)previous)[6] ^= 1;
    prepare_font(font, true);
    check(font[14] == previous && font[13] == source.original_count, "font stays original while texture upload is unavailable");

    unsigned parent[24] = {}, ctm[21] = {}, tag = 1234;
    unsigned old_bytes = source.original_width * source.original_height, new_bytes = source.width * source.height;
    parent[2] = source.original_width; parent[3] = source.original_height; parent[5] = 4; parent[8] = 1; parent[19] = (unsigned)ctm;
    g_base = (unsigned)&tag - fixture_allocation_tag;
    ctm[0] = g_base + fixture_ctm_vtable; ctm[1] = (unsigned)HeapAlloc(GetProcessHeap(), 0, old_bytes);
    ctm[2] = old_bytes; ctm[3] = 0x40000000 | old_bytes;
    ctm[7] = source.original_width; ctm[8] = source.original_height; ctm[10] = 1; ctm[11] = 4;
    memcpy((void*)ctm[1], plan.original, old_bytes);
    LoadContext context = {parent, source.asset_id, source.class_hash ^ 1, plan.generation};
    TlsSetValue(g_tls, &context);
    expand_texture(parent);
    check(!plan.ctm && ctm[2] == old_bytes, "unknown resource class keeps native texture");
    context.hash ^= 1;
    ctm[0] = g_base + (fixture_flavor == core::HOST_FLAVOR_DX10 ? 0x12C2A78 : 0x12AF048);
    expand_texture(parent);
    check(!plan.ctm && ctm[2] == old_bytes, "other host CTM vtable cannot publish pixels");
    ctm[0] = g_base + fixture_ctm_vtable;
    refuse_reserve = true;
    expand_texture(parent);
    check(!plan.ctm && ctm[2] == old_bytes && parent[2] == source.original_width, "reserve failure leaves original descriptors and bytes");
    refuse_reserve = false;
    check(hook_prepare(parent, NULL, NULL) == 92, "prepare forwards native arguments/result after expanding package pixels");
    check(ctm[2] == new_bytes && ctm[7] == source.width && ctm[8] == source.height && parent[2] == source.width &&
          memcmp((void*)ctm[1], source.pixels, new_bytes) == 0 && ctm[1] != (unsigned)source.pixels,
          "expanded payload/descriptors use an engine-owned allocation");
    check(!plan.uploaded, "CPU publication does not claim observed GPU upload completion");
    // The native Dx9 run expanded all four atlases without visiting our upload
    // hook. Original records must already address their protected atlas band.
    g_base = 0;
    font[0] = fixture_flavor == core::HOST_FLAVOR_DX10 ? 0x12A2A20 : 0x12D4998;
    prepare_font(font, true);
    check(font[14] == previous && !memcmp((void*)font[14], original, source.original_count * 32),
          "other host font vtable cannot publish records or rewrite original UVs");
    font[0] = fixture_font_vtable;
    hook_draw(font, NULL, 1, 2, 3, 4, 5, 6, 7, 8);
    bool original_sampling = true;
    for (unsigned i = 0; i < source.original_count; ++i) {
        const float* before_uv = (const float*)(original + i * 32 + 12);
        const float* after_uv = (const float*)(font[14] + i * 32 + 12);
        for (unsigned j = 0; j < 4; ++j) {
            double before_pixel = j % 2 ? (1.0 - before_uv[j]) * source.original_height : before_uv[j] * source.original_width;
            double after_pixel = j % 2 ? (1.0 - after_uv[j]) * source.height - (source.height - source.original_height) : after_uv[j] * source.width;
            original_sampling &= fabs(before_pixel - after_pixel) < 0.0001;
        }
    }
    check(original_sampling && font[13] == source.original_count && font[14] == previous,
          "native no-upload-hook sequence keeps original glyph texels instead of sampling an atlas-wide CJK grid");
    check(runtime_ready() == (index == 3), "text preparation follows four coherent CPU atlases without waiting for a draw-driven upload hook");
    unsigned cp = *(const unsigned short*)source.records;
    unsigned short text[2] = {(unsigned short)cp, 0};
    int pre_upload_width = index % 2 ? hook_width(font, NULL, text) : hook_advance(font, NULL, cp);
    check(pre_upload_width == *(const short*)(source.records + 10) && !plan.uploaded &&
          font[13] == source.original_count + source.appended_count && font[14] != previous,
          "native measurement prepares CJK records before the first observed GPU upload");
    refuse_upload = true;
    hook_upload(NULL, NULL, NULL, ctm);
    check(!plan.uploaded, "failed native upload does not publish readiness");
    refuse_upload = false;
    change_upload_generation = &plan;
    hook_upload(NULL, NULL, NULL, ctm);
    check(!plan.uploaded, "an upload from a stale resource generation cannot enable text");
    change_upload_generation = NULL;
    check(hook_upload((void*)0x1111, NULL, (void*)0x2222, ctm) == (void*)0x12345678 && plan.uploaded &&
          upload_arguments[0] == 0x1111 && upload_arguments[1] == 0x2222 && upload_arguments[2] == (unsigned)ctm,
          "successful upload preserves ECX destination, stack renderer/CTM and native EAX");
    g_base = 0;
    unsigned before_draw_table = font[14];
    check(hook_draw(font, NULL, 1, 2, 3, 4, 5, 6, 7, 8) == 0x2468 && draw_arguments[0] == 1 && draw_arguments[7] == 8 &&
          font[13] == source.original_count + source.appended_count && font[14] == before_draw_table,
          "Draw preserves all native arguments/EAX without replacing the prepared table");
    int measured = index % 2 ? hook_width(font, NULL, text) : hook_advance(font, NULL, cp);
    check(measured == *(const short*)(source.records + 10) &&
          font[13] == source.original_count + source.appended_count && font[14] != previous,
          "first CJK Advance/Width consumes an appended record before returning to native measurement");
    bool mappings = true, original_fields = true, texels = true;
    for (unsigned i = 0; i < source.appended_count; ++i) {
        unsigned codepoint = *(const unsigned short*)(source.records + i * 32);
        unsigned short slot = 0;
        mappings &= read_slot(font, codepoint, &slot) && slot == source.original_count + i &&
                    fake_advance(font, NULL, codepoint) == *(const short*)(source.records + i * 32 + 10);
    }
    for (unsigned i = 0; i < source.original_count; ++i) {
        const unsigned char* row = (const unsigned char*)font[14] + i * 32;
        original_fields &= !memcmp(row, original + i * 32, 12) && !memcmp(row + 28, original + i * 32 + 28, 4);
        const float* before_uv = (const float*)(original + i * 32 + 12);
        const float* after_uv = (const float*)(row + 12);
        for (unsigned j = 0; j < 4; ++j) {
            double before_pixel = j % 2 ? (1.0 - before_uv[j]) * source.original_height : before_uv[j] * source.original_width;
            double after_pixel = j % 2 ? (1.0 - after_uv[j]) * source.height - (source.height - source.original_height) : after_uv[j] * source.width;
            texels &= fabs(before_pixel - after_pixel) < 0.0001;
        }
    }
    check(mappings && original_fields, "every appended mapping/advance and every original non-UV field survives");
    check(texels, "every original UV still samples its captured texels in the protected bottom-left band");
    unsigned published = font[14];
    prepare_font(font, true);
    check(font[14] == published, "repeated preparation is idempotent");
    check(hook_reset(font, NULL, 0) == 0x76543210 && !font[14], "native reset releases both owned table allocations without double-free");
    check(hook_destroy(ctm, NULL) == 0x1357 && !plan.ctm && plan.uploaded,
          "CPU cleanup retires the CTM address without claiming resident GPU texture invalidation");
    TlsSetValue(g_tls, NULL);
}

bool tracking_is(long active, long peak, long full)
{
    char status[512] = {};
    runtime_status(status, sizeof(status));
    const char* start = strstr(status, "font_active=");
    long        actual_active = -1, actual_peak = -1, actual_full = -1;
    unsigned    limit = 0;
    return start &&
           sscanf_s(start, "font_active=%ld font_peak=%ld font_limit=%u font_full=%ld", &actual_active,
                    &actual_peak, &limit, &actual_full) == 4 &&
           actual_active == active && actual_peak == peak && limit == 32 && actual_full == full;
}

void test_tracking_capacity(const unsigned char* original)
{
    check(tracking_is(0, 1, 0),
          "tracking distinguishes current occupancy from prior sequential preparations");
    Plan&      plan = g_plans[0];
    const LONG expanded = plan.expanded;
    plan.expanded = 0;
    unsigned fonts[33][0x45C / 4] = {};
    for (unsigned i = 0; i < 33; ++i) {
        make_font(fonts[i], original, plan.source->original_count);
        if (i < 32) prepare_font(fonts[i], false);
    }
    check(tracking_is(32, 32, 0), "unprepared but tracked fonts count toward occupancy and peak");
    prepare_font(fonts[0], false);
    check(tracking_is(32, 32, 0), "repeated visits to one font do not count another instance");
    fonts[32][0] = 0;
    prepare_font(fonts[32], false);
    check(tracking_is(32, 32, 0), "unknown font identity does not count as tracking exhaustion");
    fonts[32][0] = fixture_font_vtable;
    const LONG rejects = g_rejected;
    prepare_font(fonts[32], false);
    check(tracking_is(32, 32, 1) && g_rejected == rejects + 1,
          "a 33rd identified font records capacity rejection without inventing a live slot");
    hook_reset(fonts[32], NULL, 0);
    check(tracking_is(32, 32, 1), "reset of an untracked font does not decrement occupancy");
    hook_reset(fonts[0], NULL, 0);
    check(tracking_is(31, 32, 1), "reset frees one tracking slot while preserving the peak");
    make_font(fonts[32], original, plan.source->original_count);
    prepare_font(fonts[32], false);
    check(tracking_is(32, 32, 1), "a new font reuses the released slot");
    for (unsigned i = 1; i < 33; ++i) hook_reset(fonts[i], NULL, 0);
    check(tracking_is(0, 32, 1), "reset retires every tracked instance without clearing history");
    hook_reset(fonts[0], NULL, 0);
    check(tracking_is(0, 32, 1), "repeated reset does not underflow occupancy");
    make_font(fonts[0], original, plan.source->original_count);
    prepare_font(fonts[0], false);
    check(tracking_is(1, 32, 1), "a reused object address starts a fresh tracked instance after reset");
    hook_reset(fonts[0], NULL, 0);
    plan.expanded = expanded;
}

unsigned* overlap_newer_texture = NULL;
unsigned overlap_stream[5] = {};
bool overlap_pending = false, overlap_fail = false, overlap_threaded = false;
bool overlap_invalidated = false, overlap_tls_restored = false;
enum OverlapPoint { DuringReserve, DuringUpload, DuringInvalidUpload };
OverlapPoint overlap_point = DuringReserve;

int __fastcall load_with_overlap(void* texture, void*, void*)
{
    if (texture == overlap_newer_texture) {
        overlap_invalidated = !runtime_ready();
        refuse_reserve = overlap_fail;
        hook_prepare(texture, NULL, NULL);
        refuse_reserve = false;
        if (!overlap_fail) hook_upload(NULL, NULL, NULL, (void*)((unsigned*)texture)[19]);
        return 93;
    }
    overlap_pending = overlap_point == DuringReserve;
    hook_prepare(texture, NULL, NULL);
    if (overlap_point != DuringReserve) {
        unsigned* compiled = (unsigned*)((unsigned*)texture)[19];
        if (overlap_point == DuringInvalidUpload) compiled[2] = 0;
        overlap_pending = true;
        check(hook_upload(NULL, NULL, NULL, compiled) == (void*)0x12345678,
              "overlapping upload forwards the native result");
    }
    return 91;
}

DWORD WINAPI load_newer_texture(void*)
{
    return (DWORD)hook_load(overlap_newer_texture, NULL, overlap_stream);
}

void run_overlapping_load()
{
    overlap_pending = false;
    void* previous = TlsGetValue(g_tls);
    if (overlap_threaded) {
        HANDLE worker = CreateThread(NULL, 0, load_newer_texture, NULL, 0, NULL);
        // A lock held across an engine callback must fail the replay, not hang it.
        if (!worker || WaitForSingleObject(worker, 5000) != WAIT_OBJECT_0) ExitProcess(3);
        CloseHandle(worker);
    } else {
        load_newer_texture(NULL);
    }
    overlap_tls_restored = TlsGetValue(g_tls) == previous;
}

unsigned __fastcall reserve_during_load(void* array, void*, unsigned bytes, unsigned stride, unsigned tag, unsigned flags)
{
    if (overlap_pending && overlap_point == DuringReserve) run_overlapping_load();
    return fake_reserve(array, NULL, bytes, stride, tag, flags);
}

void* __fastcall upload_during_load(void* destination, void*, void* renderer, void* compiled)
{
    if (overlap_pending && overlap_point != DuringReserve) run_overlapping_load();
    return fake_upload(destination, NULL, renderer, compiled);
}

void test_load_overlap(bool newer_fails, bool different_resource, bool threaded, OverlapPoint point = DuringReserve)
{
    printf("LOAD overlap: newer_fails=%d different_resource=%d threaded=%d point=%d\n",
           newer_fails, different_resource, threaded, point);
    const unsigned newer_index = different_resource ? 1 : 0;
    Plan saved[4];
    memcpy(saved, g_plans, sizeof(saved));
    unsigned parents[2][24] = {}, compiled[2][21] = {}, tag = 1234;
    const unsigned previous_base = g_base;
    g_base = (unsigned)&tag - fixture_allocation_tag;
    for (unsigned i = 0; i < 2; ++i) {
        const Plan& plan = g_plans[i ? newer_index : 0];
        const core::RuntimeFont& source = *plan.source;
        const unsigned bytes = source.original_width * source.original_height;
        parents[i][2] = source.original_width; parents[i][3] = source.original_height;
        parents[i][5] = 4; parents[i][8] = 1; parents[i][19] = (unsigned)compiled[i];
        compiled[i][0] = g_base + fixture_ctm_vtable;
        compiled[i][1] = (unsigned)HeapAlloc(GetProcessHeap(), 0, bytes);
        compiled[i][2] = bytes; compiled[i][3] = 0x40000000 | bytes;
        compiled[i][7] = source.original_width; compiled[i][8] = source.original_height;
        compiled[i][10] = 1; compiled[i][11] = 4;
        memcpy((void*)compiled[i][1], plan.original, bytes);
    }
    unsigned older_identity[2] = {g_plans[0].source->asset_id, kTextureClass};
    unsigned newer_identity[2] = {g_plans[newer_index].source->asset_id, kTextureClass};
    unsigned older_stream[5] = {}; older_stream[4] = (unsigned)(older_identity + 2);
    overlap_stream[4] = (unsigned)(newer_identity + 2);
    overlap_newer_texture = parents[1]; overlap_fail = newer_fails; overlap_threaded = threaded;
    overlap_invalidated = overlap_tls_restored = false;
    overlap_point = point;
    const LONG expanded_before = g_expanded;
    LoadFn previous_load = g_load;
    ReserveFn previous_reserve = g_reserve;
    UploadFn previous_upload = g_upload;
    g_load = (LoadFn)&load_with_overlap;
    g_reserve = (ReserveFn)&reserve_during_load;
    g_upload = (UploadFn)&upload_during_load;
    void* marker = (void*)0x1234;
    TlsSetValue(g_tls, marker);
    check(hook_load(parents[0], NULL, older_stream) == 91 && TlsGetValue(g_tls) == marker && overlap_tls_restored,
          "overlapping LOAD forwards native EAX and restores nested or thread-local contexts");
    check(overlap_invalidated, "newer LOAD invalidates readiness before its native prepare");
    const Plan& newer = g_plans[newer_index];
    const LONG expected_generation = saved[newer_index].generation + (different_resource ? 1 : 2);
    check(newer.generation == expected_generation, "only matching resource LOAD advances the current generation");
    const unsigned expected_ctm = newer_fails ? 0 : (unsigned)compiled[1];
    check((unsigned)newer.ctm == expected_ctm && newer.expanded == !newer_fails && newer.uploaded == !newer_fails,
          "older CPU/upload completion cannot overwrite a newer success or reopen a newer failed plan");
    check(runtime_ready() == !newer_fails, "global readiness follows the newer generation outcome");
    const core::RuntimeFont& older_source = *g_plans[0].source;
    const unsigned older_bytes = older_source.original_width * older_source.original_height;
    const bool older_published = different_resource || point != DuringReserve;
    const unsigned expected_bytes = older_published ? older_source.width * older_source.height : older_bytes;
    const unsigned char* expected_pixels = older_published ? older_source.pixels : g_plans[0].original;
    check(compiled[0][2] == (point == DuringInvalidUpload ? 0 : expected_bytes) &&
          !memcmp((void*)compiled[0][1], expected_pixels, expected_bytes),
          "stale callbacks preserve the older allocation and independent resources can still publish");
    const LONG expected_expansions = (newer_fails ? 0 : 1) + (older_published ? 1 : 0);
    check(g_expanded == expanded_before + expected_expansions, "only current CPU publications advance the expansion counter");
    g_load = previous_load; g_reserve = previous_reserve; g_upload = previous_upload;
    hook_destroy(compiled[0], NULL);
    check((unsigned)newer.ctm == expected_ctm, "destroying the older CTM does not retire the newer association");
    hook_destroy(compiled[1], NULL);
    memcpy(g_plans, saved, sizeof(saved));
    g_base = previous_base;
    TlsSetValue(g_tls, NULL);
}

void test_failure(const unsigned char* original, unsigned kind)
{
    unsigned                 font[0x45C / 4] = {};
    const core::RuntimeFont& source = *g_plans[0].source;
    make_font(font, original, source.original_count);
    unsigned previous = font[14];
    unsigned cp = *(const unsigned short*)source.records;
    refuse_alloc = kind == 0; refuse_cp = kind == 1 ? cp : 0; wrong_advance_cp = kind == 2 ? cp : 0;
    prepare_font(font, true);
    check(font[14] == previous && font[13] == source.original_count && !runtime_ready(),
          "allocation/mapping/advance failure restores table/count and closes further translation");
    refuse_alloc = false; refuse_cp = wrong_advance_cp = 0;
    unsigned short slot = 999;
    read_slot(font, cp, &slot);
    check(slot == 0, "failed publication restores the prior charmap entry");
    hook_reset(font, NULL, 0);
    g_font_failed = 0;
}

void test_text(const core::RuntimePackage& package)
{
    unsigned bytes = 0;
    const unsigned char* dictionary = package.dictionary(&bytes);
    check(dict::init_bytes(dictionary, (int)bytes) && dict::stats().keys == 1466, "candidate consumes the exact reviewed package dictionary");
    text::set_runtime_resources(&runtime_ready);
    unsigned object[7] = {};
    memcpy((unsigned char*)object + 4, L"OPTIONS", 16); object[5] = 7; object[6] = 7;
    text::funnel_run_post(object, fixture_callers[1]);
    check(object[5] == 2 && !memcmp((unsigned char*)object + 4, L"選項", 6), "real main-menu dictionary value commits before downstream layout");
    memset(object, 0, sizeof(object));
    memcpy((unsigned char*)object + 4, L"OPTIONS", 16); object[5] = 7; object[6] = 7;
    g_plans[0].expanded = 0;
    text::funnel_run_post(object, fixture_callers[1]);
    check(object[5] == 7 && !memcmp((unsigned char*)object + 4, L"OPTIONS", 16), "unready resources keep the original materialized text");
    g_plans[0].expanded = 1;
    text::funnel_run_post(object, 0xB8A144);
    check(object[5] == 7, "diagnostic caller is excluded even when its text matches the dictionary");
    wchar_t buffer[32] = L">Resolution";
    memset(object, 0, sizeof(object)); object[1] = (unsigned)buffer; object[5] = 11; object[6] = 31;
    text::funnel_run_post(object, fixture_callers[1]);
    check(object[5] == 4 && wcscmp(buffer, L">解析度") == 0 && object[6] == 31,
          "real graphics label uses the exact prefixed dictionary key and preserves capacity");
    const unsigned char growing[] = "Go\t\\x9078\\x9805\\x5716\\x5F62\\x9078\\x9805\\x5716\\x5F62\n";
    dict::init_bytes(growing, sizeof(growing) - 1);
    memset(object, 0, sizeof(object)); memcpy((unsigned char*)object + 4, L"Go", 6); object[5] = 2; object[6] = 7;
    int before = text::counters().skippedGrow;
    text::funnel_run_post(object, fixture_callers[1]);
    check(object[5] == 2 && !memcmp((unsigned char*)object + 4, L"Go", 6) && text::counters().skippedGrow == before + 1,
          "insufficient native string capacity preserves original text and increments the explicit counter");
    dict::init_bytes(dictionary, (int)bytes);
    // A relocated image and all seven independent callsites must select the
    // current host, while the other host's return address stays excluded.
    core::hook_set_base(0x400000);
    for (unsigned i = 0; i < 7; ++i) {
        memset(object, 0, sizeof(object));
        memcpy((unsigned char*)object + 4, L"OPTIONS", 16); object[5] = 7; object[6] = 7;
        text::funnel_run_post(object, 0x400000 + fixture_callers[i]);
        check(object[5] == 2, "current host callsite materializes before layout at a nonzero image base");
    }
    memset(object, 0, sizeof(object));
    memcpy((unsigned char*)object + 4, L"OPTIONS", 16); object[5] = 7; object[6] = 7;
    text::funnel_run_post(object, 0x400000 + (fixture_flavor == core::HOST_FLAVOR_DX10 ? 0x492EC7 : 0x7F5137));
    check(object[5] == 7, "other host callsite cannot broaden the replacement whitelist");
    core::hook_set_base(0);
    text::set_runtime_resources(NULL);
}

void test_host_image(const char* path)
{
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) { check(false, "host EXE opens read-only"); return; }
    // SEC_IMAGE_NO_EXECUTE maps the real PE sections without invoking any game
    // entry or imports. A private copy permits deliberate signature corruption.
    HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, NULL);
    unsigned char* image = mapping ? (unsigned char*)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : NULL;
    check(image != NULL, "real host EXE maps as a non-executable image");
    if (image) {
        check(preflight_entries((unsigned)image, *g_host), "all fifteen production entry guards match the real host EXE");
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)image;
        const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)(image + dos->e_lfanew);
        unsigned size = nt->OptionalHeader.SizeOfImage;
        unsigned char* copy = (unsigned char*)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        check(copy != NULL, "private host image buffer allocates");
        if (copy) {
            // Some PE gaps are inaccessible; copy only the headers and mapped
            // section ranges rather than reading SizeOfImage in one memcpy.
            memcpy(copy, image, nt->OptionalHeader.SizeOfHeaders);
            const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
            for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
                memcpy(copy + sections[i].VirtualAddress, image + sections[i].VirtualAddress, sections[i].SizeOfRawData);
            for (unsigned i = 0; i < 15; ++i) {
                const unsigned rva = i < 6 ? g_host->helpers[i].rva : core::hook_rva(g_hooks[i - 6]);
                copy[rva] ^= 1;
                check(!preflight_entries((unsigned)copy, *g_host), "one changed helper/hook signature rejects the entire preflight");
                copy[rva] ^= 1;
            }
            check(preflight_entries((unsigned)copy, *g_host), "restored real bytes pass preflight after every rejection");
            VirtualFree(copy, 0, MEM_RELEASE);
        }
        UnmapViewOfFile(image);
    }
    if (mapping) CloseHandle(mapping);
    CloseHandle(file);
}
} // namespace

namespace ac1 { namespace core {
HostFlavor host_flavor() { return fixture_flavor; }
unsigned long host_crc32() { return fixture_crc; }
unsigned host_base() { return 0; }
const char* host_flavor_name() { return fixture_flavor == HOST_FLAVOR_DX10 ? "Dx10" : "Dx9"; }
}}

int main(int argc, char** argv)
{
    if (argc != 8 && argc != 9) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    core::RuntimePackage package;
    check(package.open(argv[3]) && runtime_bind(package), "formal resource adapter binds a single validated package");
    if (failures) return 1;
    check(!runtime_ready() && !runtime_install(0), "unsupported test host cannot install game hooks or enable mutation");
    const bool dx10 = strcmp(argv[1], "dx10") == 0;
    if (!dx10 && strcmp(argv[1], "dx9") != 0) return 2;
    fixture_flavor = dx10 ? core::HOST_FLAVOR_DX10 : core::HOST_FLAVOR_DX9;
    check(!runtime_install(1) && g_tls == TLS_OUT_OF_INDEXES, "known host with wrong CRC rejects before memory reads or hook allocation");
    fixture_crc = dx10 ? 0x3AF8F9D0 : 0xE8936C99;
    check(!select_host(fixture_flavor, dx10 ? 0xE8936C99 : 0x3AF8F9D0), "other host CRC cannot select a resource adapter");
    g_host = select_host(fixture_flavor, fixture_crc);
    if (!g_host) return 2;
    test_host_image(argv[2]);
    fixture_font_vtable = dx10 ? 0x12D4998 : 0x12A2A20;
    fixture_ctm_vtable = dx10 ? 0x12AF048 : 0x12C2A78;
    fixture_allocation_tag = dx10 ? 0x25A3710 : 0x161E680;
    const unsigned callers9[] = {0x492C8B,0x492EC7,0x4953EC,0x4C400E,0x4C4461,0x4C5B7C,0x4C88E3};
    const unsigned callers10[] = {0x7F4EFB,0x7F5137,0x7F75DC,0x8178BE,0x817D11,0x81942C,0x81C193};
    memcpy(fixture_callers, dx10 ? callers10 : callers9, sizeof(fixture_callers));
    g_tls = TlsAlloc(); g_gate = 1;
    g_alloc = &fake_alloc; g_free = &fake_free; g_set_entry = (SetEntryFn)&fake_set;
    g_array_ctor = &fake_array; g_reserve = (ReserveFn)&fake_reserve;
    g_advance = (AdvanceFn)&fake_advance; g_upload = (UploadFn)&fake_upload;
    g_reset = (ResetFn)&fake_reset; g_load = (LoadFn)&fake_load; g_prepare = (PrepareFn)&fake_prepare;
    g_font_load = (FontLoadFn)&fake_font_load; g_width = (WidthFn)&fake_width;
    g_draw = (DrawFn)&fake_draw; g_destroy = (DestroyFn)&fake_destroy;
    unsigned char* captures[4] = {};
    for (unsigned i = 0; i < 4; ++i) {
        captures[i] = read_capture(argv[i+4], g_plans[i].source->original_count * 32);
        if (!captures[i]) return 2;
        test_resource(i, captures[i]);
    }
    check(g_glyphs == 5820 && g_prepared == 4 && runtime_ready(), "all four formal plans resolve 5820 package records and enable text together");
    for (unsigned i = 0; i < 4; ++i) g_plans[i].uploaded = 0;
    check(runtime_ready(), "upload observation cannot deadlock materialization and the first translated draw");
    test_tracking_capacity(captures[0]);
    test_failure(captures[0], 0); test_failure(captures[0], 1); test_failure(captures[0], 2);
    check(!g_retained_bytes, "all successful and failed table generations release their retained allocations");
    test_text(package);
    test_load_overlap(true, false, true);
    test_load_overlap(false, false, true);
    test_load_overlap(false, true, true);
    test_load_overlap(true, false, false);
    test_load_overlap(false, false, false);
    test_load_overlap(false, false, true, DuringUpload);
    test_load_overlap(false, false, false, DuringInvalidUpload);
    unsigned identity[2] = {g_plans[0].source->asset_id, kTextureClass};
    unsigned stream[5] = {}; stream[4] = (unsigned)(identity + 2);
    void* marker = (void*)0x1234; TlsSetValue(g_tls, marker);
    g_gate = 0;
    check(hook_load((void*)0x5678, NULL, stream) == 91 && g_gate == -1 && !g_plans[0].expanded &&
          !g_plans[0].uploaded && TlsGetValue(g_tls) == marker,
          "early target load latches installation failure and restores the previous TLS context");
    check(InterlockedCompareExchange(&g_gate, 1, 0) == -1 && !runtime_ready(), "activation cannot overwrite the early-load latch");
    unsigned live = 0;
    for (unsigned i = 0; i < 512; ++i) live += blocks[i] != NULL;
    check(!live, "engine allocation accounting has no leak or double-free in the replay");
    for (unsigned i = 0; i < 4; ++i) HeapFree(GetProcessHeap(), 0, captures[i]);
    TlsFree(g_tls);
    if (argc == 9) {
        FILE* file = NULL;
        if (fopen_s(&file, argv[8], "w") || !file) return 2;
        fprintf(file, "{\"checks\":%u,\"failures\":%u,\"hooks\":[", checks, failures);
        for (unsigned i = 0; i < sizeof(g_hooks)/sizeof(g_hooks[0]); ++i) {
            const core::HookSpec& hook = g_hooks[i];
            fprintf(file, "%s{\"name\":\"%s\",\"rva\":%u,\"entry\":\"", i ? "," : "", hook.name, core::hook_rva(hook));
            const unsigned char* bytes = dx10 ? hook.expect10 : hook.expect;
            const int size = dx10 ? hook.expect_len10 : hook.expect_len;
            for (int j = 0; j < size; ++j) fprintf(file, "%02x", bytes[j]);
            fputs("\"}", file);
        }
        fputs("]}\n", file);
        fclose(file);
    }
    printf("%s formal adapter checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
