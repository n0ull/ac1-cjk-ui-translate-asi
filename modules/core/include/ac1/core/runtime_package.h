#pragma once

namespace ac1 {
namespace core {

// These views borrow the package's private allocation. Open before installing
// consumers; keep the owner alive until every hook and queued user has stopped.
struct RuntimeFont {
    const char* name;
    unsigned asset_id, class_hash;
    unsigned original_width, original_height, width, height;
    unsigned original_count, appended_count, density;
    const unsigned char* records;
    const unsigned char* pixels;
    const unsigned char* original_records_sha256;
    const unsigned char* original_texture_sha256;
};

class RuntimePackage {
public:
    RuntimePackage();
    ~RuntimePackage();

    // Startup-only. Failure exposes no partial views. Reopening an already
    // published package is refused, preserving existing borrowed addresses.
    bool open(const char* path);
    const char* error() const { return error_; }
    const char* language_id() const { return data_ ? (const char*)data_ + 96 : ""; }
    const unsigned char* digest() const { return data_ ? data_ + 64 : 0; }
    const unsigned char* dictionary(unsigned* bytes) const;
    const RuntimeFont* font(unsigned index) const;

    enum { FONT_COUNT = 4, MAX_BYTES = 64 * 1024 * 1024, MAX_RECORDS = 4096 };

private:
    RuntimePackage(const RuntimePackage&);
    RuntimePackage& operator=(const RuntimePackage&);
    bool validate(unsigned char* data, unsigned bytes);
    bool reject(const char* message);
    unsigned char* data_;
    const unsigned char* dictionary_;
    unsigned dictionary_bytes_;
    RuntimeFont fonts_[FONT_COUNT];
    const char* error_;
};

// Windows SDK CNG, resolved from System32. No custom cryptographic algorithm
// or extra PE import; unavailable CNG rejects package/resource preparation.
bool runtime_sha256(const void* data, unsigned bytes, unsigned char digest[32]);

} // namespace core
} // namespace ac1
