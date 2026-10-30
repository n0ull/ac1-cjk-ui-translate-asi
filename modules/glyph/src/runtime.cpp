#include "ac1/glyph/runtime.h"
#include "ac1/core/runtime_package.h"
#include "ac1/core/host.h"
#include "ac1/core/hook.h"
#include "ac1/core/mem.h"
#include "ac1/core/log.h"
#include "ac1/core/trace.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>

namespace ac1 {
namespace glyph {
namespace {

const unsigned kFontLimit = 32, kRecordLimit = core::RuntimePackage::MAX_RECORDS;
const unsigned kTextureClass = 0xA2B7E917;
struct ResourceIdentity { const char* name; unsigned id, width, height, count; };
const ResourceIdentity kResources[] = {
    {"bold", 0x4719595F, 256, 512, 199}, {"techno", 0x47195991, 64, 1024, 198},
    {"textpc", 0x47195956, 256, 256, 199}, {"title", 0x87E599EC, 512, 1024, 261}
};
// Version-bound addresses and ABI evidence: docs/DX10_RUNTIME.md. Keep the
// allocator, object identities and hooks on the same verified host column.
struct EngineEntry { const char* name; unsigned rva; unsigned char bytes[13]; unsigned size; };
struct RuntimeHost {
    core::HostFlavor flavor;
    unsigned long crc;
    unsigned font_vtables[2], ctm_vtable, allocation_tag;
    EngineEntry helpers[6];
};
const RuntimeHost kHosts[] = {
    {core::HOST_FLAVOR_DX9, 0xE8936C99, {0x12A2A20, 0x12AB870}, 0x12C2A78, 0x161E680, {
        {"alloc", 0x387980, {0x6A,0xFF,0x68,0x98,0xAC,0x54,0x01,0x64}, 8},
        {"free", 0x387A20, {0x56,0x8B,0x74,0x24,0x08,0x85,0xF6}, 7},
        {"set_entry", 0x496720, {0x80,0x7C,0x24,0x0C,0,0x53,0x56,0x57}, 8},
        {"record_ctor", 0x4842B0, {0xD9,0xEE,0x33,0xC0,0xD9,0x51,0x0C,0x66}, 8},
        {"array_ctor", 0x20A0, {0x57,0x8B,0x7C,0x24,0x10,0x83,0xEF,0x01}, 8},
        {"reserve", 0x517140, {0x55,0x8B,0xE9,0x8B,0x45,0x08,0x56}, 7}
    }},
    {core::HOST_FLAVOR_DX10, 0x3AF8F9D0, {0x12D4998, 0x1316B2C}, 0x12AF048, 0x25A3710, {
        {"alloc", 0x4D5370, {0x6A,0xFF,0x68,0x38,0x01,0x56,0x01,0x64,0xA1,0,0,0,0}, 13},
        {"free", 0x4D5410, {0x56,0x8B,0x74,0x24,0x08,0x85,0xF6}, 7},
        {"set_entry", 0x7F8910, {0x80,0x7C,0x24,0x0C,0,0x53,0x56,0x57}, 8},
        {"record_ctor", 0x7EC550, {0xD9,0xEE,0x33,0xC0,0xD9,0x51,0x0C,0x66}, 8},
        {"array_ctor", 0x2090, {0x57,0x8B,0x7C,0x24,0x10,0x83,0xEF,0x01}, 8},
        {"reserve", 0x4F6D80, {0x55,0x8B,0xE9,0x8B,0x45,0x08,0x56}, 7}
    }}
};
const RuntimeHost* g_host = NULL;

const RuntimeHost* select_host(core::HostFlavor flavor, unsigned long crc)
{
    for (unsigned i = 0; i < sizeof(kHosts)/sizeof(kHosts[0]); ++i)
        if (kHosts[i].flavor == flavor && kHosts[i].crc == crc) return &kHosts[i];
    return NULL;
}
struct Plan {
    const core::RuntimeFont* source;
    unsigned char* original;
    volatile LONG ctm, expanded, uploaded, generation;
};
struct FontState {
    void* font;
    unsigned char* table;
    unsigned count;
    Plan* plan;
    void* retained;
    bool scaled, appended, attempted, invalid;
};
struct LoadContext {
    void* texture;
    unsigned id, hash;
    LONG generation;
};
Plan g_plans[4] = {};
FontState g_fonts[kFontLimit] = {};
CRITICAL_SECTION g_lock;
// Separate from font preparation: native callbacks must run outside this lock.
SRWLOCK g_resource_lock = SRWLOCK_INIT;
DWORD g_tls = TLS_OUT_OF_INDEXES;
unsigned g_base = 0;
bool g_bound = false;
// A target load seen during partial installation latches -1. Activation uses
// the same CAS, so an early load cannot race an "everything is ready" check.
volatile LONG g_gate = 0;
volatile LONG g_expanded = 0, g_uploaded = 0, g_prepared = 0, g_glyphs = 0;
volatile LONG g_rejected = 0, g_faults = 0, g_retained_bytes = 0;
volatile LONG g_font_failed = 0;
volatile LONG g_font_active = 0, g_font_peak = 0, g_font_full = 0;
int g_trace[9] = {-1,-1,-1,-1,-1,-1,-1,-1,-1};

// ABI/offset evidence: private atlas_title.cpp and manual-135734/170504/114835.
// TextureMap+4C is CTM; CTM+4/+8/+C is its engine-owned byte array.
// reserve: thiscall, four stack arguments, retn 10h; reset: retn 4;
// font-load: retn 0Ch; Advance/Width: retn 4; CTM release: plain retn.
typedef void(__fastcall* GlyphRecCtorFn)(void* rec, void* edx_unused);
typedef void* (__cdecl* AllocFn)(unsigned, unsigned);
typedef void (__cdecl* FreeFn)(void*);
typedef void (__thiscall* SetEntryFn)(void*, unsigned, unsigned, unsigned);
typedef void (__stdcall* ArrayCtorFn)(void*, unsigned, unsigned, GlyphRecCtorFn);
typedef unsigned (__thiscall* ReserveFn)(void*, unsigned, unsigned, unsigned, unsigned);
typedef int (__thiscall* FontLoadFn)(void*, unsigned, unsigned, unsigned);
typedef int (__thiscall* ResetFn)(void*, unsigned);
typedef int (__thiscall* AdvanceFn)(void*, unsigned);
typedef int (__thiscall* WidthFn)(void*, const unsigned short*);
typedef int (__thiscall* LoadFn)(void*, void*);
typedef int (__thiscall* PrepareFn)(void*, void*);
typedef int (__thiscall* DestroyFn)(void*);
typedef int (__thiscall* DrawFn)(void*, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
typedef void* (__thiscall* UploadFn)(void*, void*, void*);
AllocFn g_alloc = NULL;
FreeFn g_free = NULL;
SetEntryFn g_set_entry = NULL;
ArrayCtorFn g_array_ctor = NULL;
GlyphRecCtorFn g_record_ctor = NULL;
ReserveFn g_reserve = NULL;
FontLoadFn g_font_load = NULL;
ResetFn g_reset = NULL;
AdvanceFn g_advance = NULL;
WidthFn g_width = NULL;
LoadFn g_load = NULL;
PrepareFn g_prepare = NULL;
DestroyFn g_destroy = NULL;
DrawFn g_draw = NULL;
UploadFn g_upload = NULL;

bool read_memory(void* out, const void* source, unsigned bytes)
{
    if (!core::mem_readable(source, bytes)) return false;
    __try { memcpy(out, source, bytes); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); return false; }
}

int glyph_index(const Plan& plan, unsigned cp)
{
    unsigned first = 0, end = plan.source->appended_count;
    while (first < end) {
        unsigned middle = first + (end - first) / 2;
        unsigned found = *(const unsigned short*)(plan.source->records + middle * 32);
        if (found == cp) return (int)middle;
        if (found < cp) first = middle + 1; else end = middle;
    }
    return -1;
}

bool read_slot(void* font, unsigned cp, unsigned short* slot)
{
    unsigned page = 0;
    *slot = 0;
    if (!read_memory(&page, (unsigned char*)font + 0x3C + (cp >> 8) * 4, 4)) return false;
    return !page || read_memory(slot, (void*)(page + (cp & 255) * 2), 2);
}

Plan* identify_font(const unsigned char* table, unsigned count)
{
    bool possible = false;
    for (unsigned i = 0; i < 4; ++i) possible |= count == g_plans[i].source->original_count;
    if (!possible) return NULL;
    unsigned char canonical[kRecordLimit * 30];
    for (unsigned i = 0; i < count; ++i) memcpy(canonical + i * 30, table + i * 32, 30);
    unsigned char digest[32];
    if (!core::runtime_sha256(canonical, count * 30, digest)) return NULL;
    for (unsigned i = 0; i < 4; ++i) {
        Plan& plan = g_plans[i];
        if (count != plan.source->original_count || memcmp(digest, plan.source->original_records_sha256, 32)) continue;
        for (unsigned j = 0; j < count; ++j)
            if (glyph_index(plan, *(const unsigned short*)(table + j * 32)) >= 0) return NULL;
        return &plan;
    }
    return NULL;
}

void scale_original(unsigned char* table, const core::RuntimeFont& source)
{
    for (unsigned i = 0; i < source.original_count; ++i) {
        float* uv = (float*)(table + i * 32 + 12);
        uv[0] *= (float)source.original_width / source.width;
        uv[2] *= (float)source.original_width / source.width;
        uv[1] *= (float)source.original_height / source.height;
        uv[3] *= (float)source.original_height / source.height;
    }
}

void restore_map(void* font, const Plan& plan, const unsigned short* slots)
{
    for (unsigned i = 0; i < plan.source->appended_count; ++i) {
        unsigned cp = *(const unsigned short*)(plan.source->records + i * 32);
        g_set_entry((unsigned char*)font + 0x3C, cp, slots[i], 0);
    }
}

void append_font(FontState& state)
{
    if (state.attempted) return;
    state.attempted = true;
    const core::RuntimeFont& source = *state.plan->source;
    if (!source.appended_count) {
        state.scaled = state.appended = true;
        return;
    }
    unsigned short slots[kRecordLimit] = {};
    for (unsigned i = 0; i < source.appended_count; ++i) {
        unsigned cp = *(const unsigned short*)(source.records + i * 32);
        if (!read_slot(state.font, cp, slots + i)) { InterlockedExchange(&g_font_failed, 1); InterlockedIncrement(&g_rejected); return; }
    }
    const unsigned total = source.original_count + source.appended_count;
    unsigned char* next = (unsigned char*)g_alloc(total * 32, 0);
    if (!next) { InterlockedExchange(&g_font_failed, 1); InterlockedIncrement(&g_rejected); return; }
    unsigned char* previous = state.table;
    bool published = false, ok = false;
    __try {
        g_array_ctor(next, 32, total, g_record_ctor);
        memcpy(next, previous, source.original_count * 32);
        if (!state.scaled) scale_original(next, source);
        memcpy(next + source.original_count * 32, source.records, source.appended_count * 32);
        *(unsigned char**)((unsigned char*)state.font + 0x38) = next;
        *(unsigned short*)((unsigned char*)state.font + 0x34) = (unsigned short)total;
        published = true;
        ok = true;
        for (unsigned i = 0; i < source.appended_count; ++i) {
            unsigned cp = *(const unsigned short*)(source.records + i * 32);
            g_set_entry((unsigned char*)state.font + 0x3C, cp, source.original_count + i, 0);
            unsigned short observed = 0;
            ok = read_slot(state.font, cp, &observed) && observed == source.original_count + i &&
                 g_advance(state.font, cp) == *(const short*)(source.records + i * 32 + 10);
            if (!ok) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); }
    if (!ok) {
        InterlockedExchange(&g_font_failed, 1);
        if (published) {
            __try {
                restore_map(state.font, *state.plan, slots);
                *(unsigned char**)((unsigned char*)state.font + 0x38) = previous;
                *(unsigned short*)((unsigned char*)state.font + 0x34) = (unsigned short)source.original_count;
            } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); }
        }
        // Either table may have escaped during a failed publication. Release
        // its separate counterpart only at the validated native reset boundary.
        unsigned current = 0;
        if (read_memory(&current, (unsigned char*)state.font + 0x38, 4)) {
            if (current == (unsigned)previous || current == (unsigned)next) {
                state.retained = current == (unsigned)next ? previous : next;
                state.table = (unsigned char*)current;
                state.invalid = current != (unsigned)previous;
                InterlockedExchangeAdd(&g_retained_bytes, current == (unsigned)next ? source.original_count * 32 : total * 32);
            }
        }
        InterlockedIncrement(&g_rejected);
        return;
    }
    state.retained = previous;
    state.table = next; state.count = total; state.scaled = state.appended = true;
    InterlockedExchangeAdd(&g_retained_bytes, source.original_count * 32);
    InterlockedIncrement(&g_prepared);
    InterlockedExchangeAdd(&g_glyphs, source.appended_count);
    core::log_line("[RTP] font=%s original=%u appended=%u total=%u", source.name, source.original_count, source.appended_count, total);
}

void prepare_font_locked(void* font, bool allow_append)
{
    unsigned header[15] = {};
    if (!read_memory(header, font, sizeof(header))) return;
    if (!g_host) return;
    const bool known_font = header[0] == g_base + g_host->font_vtables[0] || header[0] == g_base + g_host->font_vtables[1];
    if (!known_font) return;
    unsigned count = header[13] & 65535;
    unsigned char* table = (unsigned char*)header[14];
    if (!count || count > kRecordLimit) return;
    FontState* state = NULL;
    for (unsigned i = 0; i < kFontLimit; ++i) {
        if (g_fonts[i].font != font) continue;
        state = &g_fonts[i];
        if (state->table != table || state->count != count) { InterlockedIncrement(&g_rejected); return; }
        if (state->invalid || state->appended) return;
        break;
    }
    if (!state) {
        if (!core::mem_writable(table, count * 32)) return;
        Plan* plan = identify_font(table, count);
        if (!plan) return;
        for (unsigned i = 0; i < kFontLimit; ++i) if (!g_fonts[i].font) { state = &g_fonts[i]; break; }
        if (!state) { InterlockedIncrement(&g_font_full); InterlockedIncrement(&g_rejected); return; }
        state->font = font; state->table = table; state->count = count; state->plan = plan;
        // Peak precedes occupancy publication so the monitor cannot observe active > peak.
        const LONG active = g_font_active + 1;
        if (active > g_font_peak) InterlockedExchange(&g_font_peak, active);
        InterlockedExchange(&g_font_active, active);
    }
    // The renderer can consume the expanded CPU atlas without visiting the
    // observed upload hook. Its original UVs must move in the same resource phase.
    if (!InterlockedCompareExchange(&state->plan->expanded, 0, 0)) return;
    if (!core::mem_writable(table, count * 32)) return;
    if (allow_append && !state->appended) append_font(*state);
    if (!state->scaled && !state->appended) {
        scale_original(state->table, *state->plan->source);
        state->scaled = true;
    }
}

void prepare_font(void* font, bool allow_append)
{
    if (InterlockedCompareExchange(&g_gate, 0, 0) != 1) return;
    EnterCriticalSection(&g_lock);
    __try { prepare_font_locked(font, allow_append); }
    __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); }
    LeaveCriticalSection(&g_lock);
}

Plan* resource_plan(unsigned id, unsigned hash)
{
    if (hash != kTextureClass) return NULL;
    for (unsigned i = 0; i < 4; ++i) if (g_plans[i].source->asset_id == id) return &g_plans[i];
    return NULL;
}

void expand_texture(void* texture)
{
    if (!g_host || InterlockedCompareExchange(&g_gate, 0, 0) != 1) return;
    LoadContext* context = (LoadContext*)TlsGetValue(g_tls);
    if (!context || context->texture != texture) return;
    Plan* plan = resource_plan(context->id, context->hash);
    if (!plan) return;
    if (context->generation != InterlockedCompareExchange(&plan->generation, 0, 0)) return;
    const core::RuntimeFont& source = *plan->source;
    unsigned parent[24] = {}, ctm[21] = {}, tag = 0;
    if (!read_memory(parent, texture, sizeof(parent)) || !read_memory(ctm, (void*)parent[19], sizeof(ctm))) return;
    const unsigned old_bytes = source.original_width * source.original_height;
    const unsigned new_bytes = source.width * source.height;
    const bool parent_ok = parent[2] == source.original_width && parent[3] == source.original_height && parent[5] == 4 && parent[8] == 1;
    const bool ctm_ok = ctm[0] == g_base + g_host->ctm_vtable && ctm[2] == old_bytes && ctm[7] == source.original_width &&
                        ctm[8] == source.original_height && ctm[10] == 1 && ctm[11] == 4;
    const bool owned = (ctm[3] & 0x40000000) && (ctm[3] & 0x1FFFFFFF) >= old_bytes;
    if (!parent_ok || !ctm_ok || !owned) { InterlockedIncrement(&g_rejected); return; }
    if (!core::mem_writable(texture, sizeof(parent)) || !core::mem_writable((void*)parent[19], sizeof(ctm)) ||
        !core::mem_readable((void*)ctm[1], old_bytes) || !read_memory(&tag, (void*)(g_base + g_host->allocation_tag), 4)) return;
    unsigned* compiled = (unsigned*)parent[19];
    __try {
        if (memcmp((void*)ctm[1], plan->original, old_bytes)) { InterlockedIncrement(&g_rejected); return; }
        if (new_bytes != old_bytes) g_reserve(compiled + 1, new_bytes, 1, tag, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return;
    }

    // LOAD invalidation and the entire write/rollback share one commit point.
    // reserve can reenter LOAD or wait for another thread, so it stays outside.
    bool changed = false, published = false;
    AcquireSRWLockExclusive(&g_resource_lock);
    __try {
        if (context->generation != plan->generation) return;
        __try {
            const bool reserved = compiled[1] && compiled[2] == old_bytes &&
                                  (compiled[3] & 0x1FFFFFFF) >= new_bytes;
            if (!reserved || !core::mem_writable((void*)compiled[1], new_bytes)) {
                InterlockedIncrement(&g_rejected);
                return;
            }
            changed = true;
            memcpy((void*)compiled[1], source.pixels, new_bytes);
            compiled[2] = new_bytes;
            compiled[7] = source.width;
            compiled[8] = source.height;
            ((unsigned*)texture)[2] = source.width;
            ((unsigned*)texture)[3] = source.height;
            if (memcmp((void*)compiled[1], source.pixels, new_bytes))
                RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
            InterlockedExchange(&plan->ctm, (LONG)compiled);
            InterlockedExchange(&plan->expanded, 1);
            InterlockedIncrement(&g_expanded);
            published = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (changed) {
                __try {
                    memcpy((void*)compiled[1], plan->original, old_bytes);
                    compiled[2] = old_bytes;
                    compiled[7] = source.original_width;
                    compiled[8] = source.original_height;
                    ((unsigned*)texture)[2] = source.original_width;
                    ((unsigned*)texture)[3] = source.original_height;
                } __except (EXCEPTION_EXECUTE_HANDLER) { }
            }
            InterlockedIncrement(&g_faults);
        }
    } __finally { ReleaseSRWLockExclusive(&g_resource_lock); }
    if (published)
        core::log_line("[RTP] atlas=%s bytes=%u dimensions=%ux%u", source.name, new_bytes, source.width, source.height);
}

int __fastcall hook_font_load(void* font, void*, unsigned a, unsigned b, unsigned c)
{
    core::hooktrace_enter(g_trace[2], (unsigned)font, a);
    int result = g_font_load(font, a, b, c);
    if (result & 255) prepare_font(font, true);
    core::hooktrace_leave(g_trace[2]);
    return result;
}
int __fastcall hook_advance(void* font, void*, unsigned cp)
{
    core::hooktrace_enter(g_trace[3], (unsigned)font, cp);
    for (unsigned i = 0; i < 4; ++i) {
        if (!g_plans[i].source || glyph_index(g_plans[i], cp) < 0) continue;
        prepare_font(font, true);
        break;
    }
    int result = g_advance(font, cp);
    core::hooktrace_leave(g_trace[3]);
    return result;
}
int __fastcall hook_width(void* font, void*, const unsigned short* text)
{
    core::hooktrace_enter(g_trace[4], (unsigned)font, (unsigned)text);
    prepare_font(font, true);
    int result = g_width(font, text);
    core::hooktrace_leave(g_trace[4]);
    return result;
}
int __fastcall hook_draw(void* font, void*, unsigned a, unsigned b, unsigned c, unsigned d, unsigned e, unsigned f, unsigned g, unsigned h)
{
    core::hooktrace_enter(g_trace[5], (unsigned)font, a);
    // Original UVs may be repaired before a draw. Appending here is too late:
    // native measurement may already hold the previous table or glyph indices.
    prepare_font(font, false);
    int result = g_draw(font, a, b, c, d, e, f, g, h);
    core::hooktrace_leave(g_trace[5]);
    return result;
}
int __fastcall hook_load(void* texture, void*, void* stream)
{
    core::hooktrace_enter(g_trace[8], (unsigned)texture, (unsigned)stream);
    LoadContext context = {texture, 0, 0, 0};
    unsigned cursor = 0, identity[2] = {};
    if (read_memory(&cursor, (unsigned char*)stream + 16, 4) && cursor >= 8 && read_memory(identity, (void*)(cursor - 8), 8)) {
        context.id = identity[0]; context.hash = identity[1];
    }
    Plan* plan = resource_plan(context.id, context.hash);
    if (plan) {
        InterlockedCompareExchange(&g_gate, -1, 0);
        AcquireSRWLockExclusive(&g_resource_lock);
        InterlockedExchange(&plan->uploaded, 0);
        InterlockedExchange(&plan->expanded, 0);
        InterlockedExchange(&plan->ctm, 0);
        context.generation = InterlockedIncrement(&plan->generation);
        ReleaseSRWLockExclusive(&g_resource_lock);
    }
    void* previous = TlsGetValue(g_tls);
    TlsSetValue(g_tls, &context);
    int result = 0;
    __try { result = g_load(texture, stream); }
    __finally { TlsSetValue(g_tls, previous); core::hooktrace_leave(g_trace[8]); }
    return result;
}
int __fastcall hook_prepare(void* texture, void*, void* argument)
{
    core::hooktrace_enter(g_trace[7], (unsigned)texture, (unsigned)argument);
    expand_texture(texture);
    int result = g_prepare(texture, argument);
    core::hooktrace_leave(g_trace[7]);
    return result;
}
void* __fastcall hook_upload(void* destination, void*, void* renderer, void* compiled)
{
    core::hooktrace_enter(g_trace[6], (unsigned)compiled, (unsigned)renderer);
    Plan* plan = NULL;
    AcquireSRWLockShared(&g_resource_lock);
    for (unsigned i = 0; i < 4; ++i)
        if (compiled && (unsigned)InterlockedCompareExchange(&g_plans[i].ctm, 0, 0) == (unsigned)compiled) plan = &g_plans[i];
    LONG generation = plan ? InterlockedCompareExchange(&plan->generation, 0, 0) : 0;
    ReleaseSRWLockShared(&g_resource_lock);
    bool valid = false;
    if (plan && InterlockedCompareExchange(&g_gate, 0, 0) == 1) {
        unsigned fields[21] = {};
        const core::RuntimeFont& source = *plan->source;
        valid = read_memory(fields, compiled, sizeof(fields)) && fields[2] == source.width * source.height &&
                fields[7] == source.width && fields[8] == source.height && fields[10] == 1 && fields[11] == 4;
        __try { valid = valid && memcmp((void*)fields[1], source.pixels, source.width * source.height) == 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { valid = false; InterlockedIncrement(&g_faults); }
    }
    void* result = g_upload(destination, renderer, compiled);
    AcquireSRWLockExclusive(&g_resource_lock);
    const bool current = plan && generation == plan->generation &&
                         (unsigned)plan->ctm == (unsigned)compiled;
    if (current) {
        if (!valid) {
            InterlockedExchange(&plan->uploaded, 0);
            InterlockedIncrement(&g_rejected);
        } else if (result) {
            InterlockedExchange(&plan->uploaded, 1);
            InterlockedIncrement(&g_uploaded);
        }
    }
    ReleaseSRWLockExclusive(&g_resource_lock);
    core::hooktrace_leave(g_trace[6]);
    return result;
}
int __fastcall hook_destroy(void* compiled, void*)
{
    core::hooktrace_enter(g_trace[1], (unsigned)compiled, 0);
    AcquireSRWLockExclusive(&g_resource_lock);
    for (unsigned i = 0; i < 4; ++i) InterlockedCompareExchange(&g_plans[i].ctm, 0, (LONG)compiled);
    ReleaseSRWLockExclusive(&g_resource_lock);
    // CPU CTM cleanup is distinct from resident GPU texture invalidation.
    int result = g_destroy(compiled);
    core::hooktrace_leave(g_trace[1]);
    return result;
}
int __fastcall hook_reset(void* font, void*, unsigned count)
{
    core::hooktrace_enter(g_trace[0], (unsigned)font, count);
    if (InterlockedCompareExchange(&g_gate, 0, 0) != 1) {
        int result = g_reset(font, count);
        core::hooktrace_leave(g_trace[0]);
        return result;
    }
    EnterCriticalSection(&g_lock);
    void* retained = NULL;
    unsigned before = 0, retained_bytes = 0;
    read_memory(&before, (unsigned char*)font + 0x38, 4);
    for (unsigned i = 0; i < kFontLimit; ++i) {
        FontState& state = g_fonts[i];
        if (state.font != font) continue;
        retained = state.retained;
        if (retained) retained_bytes = state.appended ? state.plan->source->original_count * 32 :
            (state.plan->source->original_count + state.plan->source->appended_count) * 32;
        memset(&state, 0, sizeof(state));
        InterlockedDecrement(&g_font_active);
    }
    int result = 0;
    __try {
        result = g_reset(font, count);
        unsigned after = 0;
        if (retained && read_memory(&after, (unsigned char*)font + 0x38, 4)) {
            if ((unsigned)retained == before) InterlockedExchangeAdd(&g_retained_bytes, -(LONG)retained_bytes);
            else if ((unsigned)retained != after) { g_free(retained); InterlockedExchangeAdd(&g_retained_bytes, -(LONG)retained_bytes); }
            else InterlockedIncrement(&g_faults);
        }
    } __finally { LeaveCriticalSection(&g_lock); core::hooktrace_leave(g_trace[0]); }
    return result;
}

// These are the same complete entry bytes and RVAs as the verified atlas
// adapter. Consumer hooks are installed before texture producer hooks.
const unsigned char kReset[] = {0x6A,0xFF,0x68,0x9B,0x59,0x55,0x01};
const unsigned char kDestroy[] = {0x6A,0xFF,0x68,0xD8,0xE3,0x56,0x01};
const unsigned char kFontLoad[] = {0x6A,0xFF,0x68,0x1B,0x58,0x55,0x01};
const unsigned char kAdvance[] = {0x8B,0x44,0x24,0x04,0x56};
const unsigned char kWidth[] = {0x53,0x56,0x8B,0x74,0x24,0x0C};
const unsigned char kDraw[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x6A,0xFF};
const unsigned char kUpload[] = {0x6A,0xFF,0x68,0x58,0xEF,0x56,0x01};
const unsigned char kPrepare[] = {0x64,0x8B,0x15,0x2C,0,0,0,0x56,0x8B,0xF1};
const unsigned char kLoad[] = {0x83,0xEC,0x08,0x53,0x56,0x8B,0x74,0x24,0x14};
const unsigned char kReset10[] = {0x6A,0xFF,0x68,0xCB,0xFF,0x56,0x01,0x64,0xA1,0,0,0,0};
const unsigned char kDestroy10[] = {0x6A,0xFF,0x68,0x98,0xC9,0x55,0x01,0x64,0xA1,0,0,0,0};
const unsigned char kFontLoad10[] = {0x6A,0xFF,0x68,0x4B,0xFE,0x56,0x01,0x64,0xA1,0,0,0,0};
const unsigned char kUpload10[] = {0x6A,0xFF,0x68,0x56,0xD7,0x55,0x01,0x64,0xA1,0,0,0,0};
#define RUNTIME_HOOK(name, rva9, rva10, bytes9, bytes10, detour, original) \
    {name, rva9, rva10, bytes9, sizeof(bytes9), bytes10, sizeof(bytes10), (void*)&detour, (void**)&original, -1}
core::HookSpec g_hooks[] = {
    RUNTIME_HOOK("RTP_FONT_RESET", 0x4842F0, 0x7EC590, kReset, kReset10, hook_reset, g_reset),
    RUNTIME_HOOK("RTP_CTM_RELEASE", 0x697DE0, 0x47A490, kDestroy, kDestroy10, hook_destroy, g_destroy),
    RUNTIME_HOOK("RTP_FONT_LOAD", 0x4840B0, 0x7EC350, kFontLoad, kFontLoad10, hook_font_load, g_font_load),
    RUNTIME_HOOK("RTP_ADVANCE", 0x481AF0, 0x7E9DB0, kAdvance, kAdvance, hook_advance, g_advance),
    RUNTIME_HOOK("RTP_WIDTH", 0x481B20, 0x7E9DE0, kWidth, kWidth, hook_width, g_width),
    RUNTIME_HOOK("RTP_DRAW", 0x399720, 0x4E81F0, kDraw, kDraw, hook_draw, g_draw),
    RUNTIME_HOOK("RTP_UPLOAD", 0x6ABB50, 0x48BE70, kUpload, kUpload10, hook_upload, g_upload),
    RUNTIME_HOOK("RTP_PREPARE", 0x664070, 0x444B30, kPrepare, kPrepare, hook_prepare, g_prepare),
    RUNTIME_HOOK("RTP_LOAD", 0x664260, 0x444BF0, kLoad, kLoad, hook_load, g_load)
};
#undef RUNTIME_HOOK

bool entry_matches(unsigned base, unsigned rva, const unsigned char* bytes, unsigned size, const char* name)
{
    unsigned char actual[16] = {};
    const bool readable = size <= sizeof(actual) && read_memory(actual, (void*)(base + rva), size);
    if (readable && memcmp(actual, bytes, size) == 0) return true;
    core::log_warn("[RTP] entry rejected: %s RVA=%08X", name, rva);
    return false;
}

bool preflight_entries(unsigned base, const RuntimeHost& host)
{
    for (unsigned i = 0; i < 6; ++i) {
        const EngineEntry& entry = host.helpers[i];
        if (!entry_matches(base, entry.rva, entry.bytes, entry.size, entry.name)) return false;
    }
    const bool dx10 = host.flavor == core::HOST_FLAVOR_DX10;
    for (unsigned i = 0; i < sizeof(g_hooks)/sizeof(g_hooks[0]); ++i) {
        const core::HookSpec& hook = g_hooks[i];
        const unsigned char* bytes = dx10 ? hook.expect10 : hook.expect;
        const unsigned size = dx10 ? hook.expect_len10 : hook.expect_len;
        if (!entry_matches(base, dx10 ? hook.rva10 : hook.rva9, bytes, size, hook.name)) return false;
    }
    return true;
}

} // namespace

bool runtime_bind(const core::RuntimePackage& package)
{
    if (g_bound) return false;
    Plan next[4] = {};
    bool ok = true;
    for (unsigned i = 0; i < 4; ++i) {
        const core::RuntimeFont* source = package.font(i);
        const ResourceIdentity& expected = kResources[i];
        const bool identity = source && strcmp(source->name, expected.name) == 0 && source->asset_id == expected.id && source->class_hash == kTextureClass;
        const bool dimensions = source && source->original_width == expected.width && source->original_height == expected.height && source->original_count == expected.count;
        if (!identity || !dimensions) { ok = false; break; }
        next[i].source = source;
        next[i].original = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, expected.width * expected.height);
        if (!next[i].original) { ok = false; break; }
        for (unsigned row = 0; row < expected.height / 4; ++row) {
            unsigned offset = ((source->height - expected.height) / 4 + row) * source->width * 4;
            memcpy(next[i].original + row * expected.width * 4, source->pixels + offset, expected.width * 4);
        }
    }
    if (!ok) {
        for (unsigned i = 0; i < 4; ++i) if (next[i].original) HeapFree(GetProcessHeap(), 0, next[i].original);
        return false;
    }
    memcpy(g_plans, next, sizeof(next));
    InitializeCriticalSection(&g_lock);
    g_bound = true;
    return true;
}

bool runtime_install(unsigned base)
{
    if (!g_bound) return false;
    g_host = select_host(core::host_flavor(), core::host_crc32());
    if (!g_host) {
        core::log_warn("[RTP] unsupported resource host or CRC32");
        return false;
    }
    g_base = base ? base : core::host_base();
    if (!preflight_entries(g_base, *g_host)) return false;
    const EngineEntry* entries = g_host->helpers;
    g_alloc = (AllocFn)(g_base + entries[0].rva); g_free = (FreeFn)(g_base + entries[1].rva);
    g_set_entry = (SetEntryFn)(g_base + entries[2].rva); g_record_ctor = (GlyphRecCtorFn)(g_base + entries[3].rva);
    g_array_ctor = (ArrayCtorFn)(g_base + entries[4].rva); g_reserve = (ReserveFn)(g_base + entries[5].rva);
    core::log_line("[RTP] Dx9/Dx10 resource adapters; selected=%s", core::host_flavor_name());
    g_tls = TlsAlloc();
    if (g_tls == TLS_OUT_OF_INDEXES) return false;
    for (unsigned i = 0; i < sizeof(g_hooks)/sizeof(g_hooks[0]); ++i) {
        if (!core::hook_install_one(g_hooks[i], g_base)) return false;
        g_trace[i] = g_hooks[i].traceSlot;
        core::hooktrace_set_policy(g_hooks[i].traceSlot, 0, 0);
    }
    if (InterlockedCompareExchange(&g_gate, 1, 0) != 0) {
        core::log_warn("[RTP] target resource loaded during installation; mutations remain disabled");
        return false;
    }
    return true;
}

bool runtime_ready()
{
    if (InterlockedCompareExchange(&g_gate, 0, 0) != 1 || InterlockedCompareExchange(&g_font_failed, 0, 0)) return false;
    // Font preparation and text materialization precede the native draw that
    // may trigger lazy upload. An upload observation cannot gate its own input.
    bool ready = true;
    AcquireSRWLockShared(&g_resource_lock);
    for (unsigned i = 0; i < 4; ++i) if (!InterlockedCompareExchange(&g_plans[i].expanded, 0, 0)) ready = false;
    ReleaseSRWLockShared(&g_resource_lock);
    return ready;
}

void runtime_status(char* output, int capacity)
{
    if (!output || capacity <= 0) return;
    unsigned atlas_mask = 0, upload_mask = 0;
    AcquireSRWLockShared(&g_resource_lock);
    for (unsigned i = 0; i < 4; ++i) {
        if (InterlockedCompareExchange(&g_plans[i].expanded, 0, 0)) atlas_mask |= 1u << i;
        if (InterlockedCompareExchange(&g_plans[i].uploaded, 0, 0)) upload_mask |= 1u << i;
    }
    ReleaseSRWLockShared(&g_resource_lock);
    const LONG active = InterlockedCompareExchange(&g_font_active, 0, 0);
    const LONG peak = InterlockedCompareExchange(&g_font_peak, 0, 0);
    _snprintf_s(output, (size_t)capacity, _TRUNCATE,
        "RTP gate=%ld atlas_mask=%u uploaded_mask=%u expanded=%ld uploads=%ld fonts=%ld glyphs=%ld retained=%ld rejects=%ld faults=%ld font_failed=%ld font_active=%ld font_peak=%ld font_limit=%u font_full=%ld",
        g_gate, atlas_mask, upload_mask, g_expanded, g_uploaded, g_prepared, g_glyphs, g_retained_bytes, g_rejected, g_faults, g_font_failed,
        active, peak, kFontLimit, InterlockedCompareExchange(&g_font_full, 0, 0));
}

} // namespace glyph
} // namespace ac1
