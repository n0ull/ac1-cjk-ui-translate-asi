#include "ac1/core/runtime_package.h"
#include "ac1/core/file.h"

#include <bcrypt.h>
#include <float.h>
#include <string.h>

namespace ac1 {
namespace core {
namespace {

unsigned u32(const unsigned char* p) { unsigned value; memcpy(&value, p, 4); return value; }
unsigned short u16(const unsigned char* p) { unsigned short value; memcpy(&value, p, 2); return value; }
short i16(const unsigned char* p) { short value; memcpy(&value, p, 2); return value; }
float f32(const unsigned char* p) { float value; memcpy(&value, p, 4); return value; }
unsigned long long u64(const unsigned char* p)
{
    unsigned long long value; memcpy(&value, p, 8); return value;
}

bool zero(const unsigned char* p, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) if (p[i]) return false;
    return true;
}

bool power_of_two(unsigned value)
{
    return value >= 4 && value <= 8192 && (value & (value - 1)) == 0;
}

bool language_valid(const unsigned char* label)
{
    if (label[0] < 'a' || label[0] > 'z') return false;
    bool separator = false;
    for (unsigned i = 0; i < 32; ++i) {
        const unsigned char c = label[i];
        if (!c) return !separator && zero(label + i, 32 - i);
        const bool letter = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        if (c == '-') {
            if (separator) return false;
            separator = true;
        } else {
            if (!letter && !digit) return false;
            separator = false;
        }
    }
    return false;
}

struct Segment { unsigned first, end; };

bool add_segment(Segment* segments, unsigned* count, unsigned long long offset,
                 unsigned long long bytes, unsigned file_bytes)
{
    const bool aligned = offset >= 128 && offset % 16 == 0;
    const bool in_bounds = offset <= file_bytes && bytes <= file_bytes - offset;
    if (!aligned || !in_bounds) return false;
    segments[*count].first = (unsigned)offset;
    segments[*count].end = (unsigned)(offset + bytes);
    ++*count;
    return true;
}

bool matches_hash(const unsigned char* data, unsigned bytes, const unsigned char* expected)
{
    unsigned char actual[32];
    return runtime_sha256(data, bytes, actual) && memcmp(actual, expected, 32) == 0;
}

bool glyph_records_valid(const RuntimeFont& font)
{
    unsigned previous = 0;
    for (unsigned i = 0; i < font.appended_count; ++i) {
        const unsigned char* record = font.records + i * 32;
        unsigned cp = u16(record), width = u16(record + 2), height = u16(record + 4);
        const bool scalar = cp >= 32 && !(cp >= 0xD800 && cp <= 0xDFFF) && cp != 127;
        if (!scalar || cp <= previous || !width || !height || i16(record + 10) <= 0) return false;
        if (u16(record + 28) || u16(record + 30)) return false;
        float uv[4];
        for (unsigned field = 0; field < 4; ++field) {
            uv[field] = f32(record + 12 + field * 4);
            if (!_finite(uv[field]) || uv[field] < 0 || uv[field] > 1) return false;
        }
        if (uv[0] >= uv[2] || uv[1] >= uv[3]) return false;
        if (cp != 0x3000) {
            float x0 = uv[0] * font.width, x1 = uv[2] * font.width;
            float y0 = (1 - uv[3]) * font.height, y1 = (1 - uv[1]) * font.height;
            const bool texel_aligned = x0 == (unsigned)x0 && x1 == (unsigned)x1 &&
                                       y0 == (unsigned)y0 && y1 == (unsigned)y1;
            const bool density_matches = x1 - x0 == width * font.density && y1 - y0 == height * font.density;
            const bool guarded = x0 >= 4 && x1 + 4 <= font.width && y0 >= 4 &&
                                 y1 + 4 <= font.height - font.original_height;
            if (!texel_aligned || !density_matches || !guarded) return false;
        }
        previous = cp;
    }
    return true;
}

bool original_band_valid(const RuntimeFont& font)
{
    const unsigned bytes = font.original_width * font.original_height;
    unsigned char* original = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!original) return false;
    const unsigned old_stride = font.original_width * 4, new_stride = font.width * 4;
    for (unsigned row = 0; row < font.original_height / 4; ++row) {
        const unsigned start = ((font.height - font.original_height) / 4 + row) * new_stride;
        memcpy(original + row * old_stride, font.pixels + start, old_stride);
    }
    bool ok = matches_hash(original, bytes, font.original_texture_sha256);
    HeapFree(GetProcessHeap(), 0, original);
    return ok;
}

} // namespace

bool runtime_sha256(const void* data, unsigned bytes, unsigned char digest[32])
{
    HMODULE module = LoadLibraryExA("bcrypt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return false;
    auto open = (decltype(&BCryptOpenAlgorithmProvider))GetProcAddress(module, "BCryptOpenAlgorithmProvider");
    auto hash = (decltype(&BCryptHash))GetProcAddress(module, "BCryptHash");
    auto close = (decltype(&BCryptCloseAlgorithmProvider))GetProcAddress(module, "BCryptCloseAlgorithmProvider");
    BCRYPT_ALG_HANDLE algorithm = NULL;
    bool ok = open && hash && close;
    if (ok) ok = open(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0;
    if (ok) ok = hash(algorithm, NULL, 0, (PUCHAR)data, bytes, digest, 32) >= 0;
    if (algorithm) close(algorithm, 0);
    FreeLibrary(module);
    return ok;
}

RuntimePackage::RuntimePackage() : data_(NULL), dictionary_(NULL), dictionary_bytes_(0), error_("not opened")
{
    memset(fonts_, 0, sizeof(fonts_));
}

RuntimePackage::~RuntimePackage() { file_free(data_); }

bool RuntimePackage::reject(const char* message) { error_ = message; return false; }

const unsigned char* RuntimePackage::dictionary(unsigned* bytes) const
{
    if (bytes) *bytes = data_ ? dictionary_bytes_ : 0;
    return data_ ? dictionary_ : NULL;
}

const RuntimeFont* RuntimePackage::font(unsigned index) const
{
    return data_ && index < FONT_COUNT ? &fonts_[index] : NULL;
}

bool RuntimePackage::open(const char* path)
{
    if (data_) return reject("package already published");
    unsigned char* candidate = NULL;
    int bytes = 0;
    if (!file_read_all(path, &candidate, &bytes, MAX_BYTES)) return reject("package read failed");
    if (!validate(candidate, (unsigned)bytes)) {
        file_free(candidate);
        dictionary_ = NULL;
        dictionary_bytes_ = 0;
        memset(fonts_, 0, sizeof(fonts_));
        return false;
    }
    data_ = candidate;
    error_ = "ok";
    return true;
}

bool RuntimePackage::validate(unsigned char* data, unsigned bytes)
{
    if (bytes < 128 || memcmp(data, "AC1LANG\0", 8)) return reject("package magic/header");
    if (u32(data + 8) != 2 || u32(data + 12) != 128) return reject("package version/header");
    if (u64(data + 16) != bytes || u32(data + 24) || u32(data + 28) != FONT_COUNT)
        return reject("package size/flags/count");
    if (u32(data + 56) != 224 || u32(data + 60) != 2)
        return reject("package directory/rules");
    if (!language_valid(data + 96)) return reject("package language");
    unsigned char expected[32];
    memcpy(expected, data + 64, 32);
    memset(data + 64, 0, 32);
    const bool package_hash_ok = matches_hash(data, bytes, expected);
    memcpy(data + 64, expected, 32);
    if (!package_hash_ok) return reject("package hash or CNG unavailable");

    Segment segments[11] = {{0, 128}};
    unsigned segment_count = 1;
    const unsigned long long dict_at = u64(data + 32), dict_size = u64(data + 40), dir_at = u64(data + 48);
    if (!dict_size || dict_size > FILE_READ_MAX_DEFAULT) return reject("dictionary size");
    if (!add_segment(segments, &segment_count, dict_at, dict_size, bytes) ||
        !add_segment(segments, &segment_count, dir_at, FONT_COUNT * 224, bytes)) return reject("dictionary/directory range");
    static const char* names[] = {"bold", "techno", "textpc", "title"};
    for (unsigned index = 0; index < FONT_COUNT; ++index) {
        const unsigned char* entry = data + (unsigned)dir_at + index * 224;
        char name[16] = {};
        memcpy(name, names[index], strlen(names[index]));
        if (memcmp(name, entry, 16)) return reject("font name/order");
        RuntimeFont& font = fonts_[index];
        font.name = names[index];
        font.asset_id = u32(entry + 16); font.class_hash = u32(entry + 20);
        font.original_width = u32(entry + 24); font.original_height = u32(entry + 28);
        font.width = u32(entry + 32); font.height = u32(entry + 36);
        const bool dimensions_ok = power_of_two(font.original_width) && power_of_two(font.original_height) &&
                                   power_of_two(font.width) && power_of_two(font.height);
        if (!dimensions_ok || font.width < font.original_width || font.height < font.original_height)
            return reject("font dimensions");
        if (!font.asset_id || !font.class_hash || u32(entry + 40) != 4 || u32(entry + 44) != 1 || u32(entry + 60))
            return reject("font identity/format/reserved");
        for (unsigned previous = 0; previous < index; ++previous) {
            if (fonts_[previous].asset_id == font.asset_id && fonts_[previous].class_hash == font.class_hash)
                return reject("duplicate font identity");
        }
        font.original_count = u32(entry + 48); font.appended_count = u32(entry + 52); font.density = u32(entry + 56);
        const bool original_size = font.width == font.original_width && font.height == font.original_height;
        if ((!font.appended_count && !original_size) || (font.appended_count && font.height == font.original_height))
            return reject("font dimensions/count");
        if (!font.original_count || font.original_count > MAX_RECORDS ||
            font.appended_count > MAX_RECORDS - font.original_count || font.density != 2)
            return reject("font count/density");
        const unsigned long long rec_at = u64(entry + 64), rec_bytes = u64(entry + 72);
        const unsigned long long tex_at = u64(entry + 80), tex_bytes = u64(entry + 88);
        if (rec_bytes != font.appended_count * 32 || tex_bytes != font.width * font.height)
            return reject("font segment size");
        if (!add_segment(segments, &segment_count, rec_at, rec_bytes, bytes) ||
            !add_segment(segments, &segment_count, tex_at, tex_bytes, bytes)) return reject("font segment range");
        font.records = data + (unsigned)rec_at; font.pixels = data + (unsigned)tex_at;
        font.original_texture_sha256 = entry + 96; font.original_records_sha256 = entry + 128;
        if (!matches_hash(font.records, (unsigned)rec_bytes, entry + 160) ||
            !matches_hash(font.pixels, (unsigned)tex_bytes, entry + 192)) return reject("font segment hash");
        if (!glyph_records_valid(font)) return reject("font glyph record");
        if (!original_band_valid(font)) return reject("original texture band hash");
    }
    for (unsigned i = 1; i < segment_count; ++i) {
        Segment value = segments[i];
        unsigned j = i;
        while (j && segments[j - 1].first > value.first) { segments[j] = segments[j - 1]; --j; }
        segments[j] = value;
    }
    unsigned end = 0;
    for (unsigned i = 0; i < segment_count; ++i) {
        if (segments[i].first < end) return reject("overlapping segments");
        if (!zero(data + end, segments[i].first - end)) return reject("nonzero segment padding");
        end = segments[i].end;
    }
    if (end != bytes) return reject("unreferenced trailing data");
    dictionary_ = data + (unsigned)dict_at;
    dictionary_bytes_ = (unsigned)dict_size;
    return true;
}

} // namespace core
} // namespace ac1
