// modules/core/src/mem.cpp
#include "ac1/core/mem.h"

namespace ac1 {
namespace core {
namespace {

volatile LONG g_faults = 0;

bool protect_ok(DWORD protect, bool want_write)
{
    // PAGE_GUARD(0x100) 必须先单独拦：它在 0xFF 之外，会被下面的 `protect & 0xFF`
    //   掩掉 ⇒ PAGE_READWRITE|PAGE_GUARD 落进 PAGE_READWRITE 分支被判成可读写。
    //   guard page 的首次访问触发 STATUS_GUARD_PAGE_VIOLATION（0x80000001）：
    //   读侧凭空多一次异常、写侧直接终止进程，两者都违反本模块「任何一步不可信
    //   都返回 false」的契约（mem.h 文件头）。
    //   PAGE_NOCACHE(0x200) / PAGE_WRITECOMBINE(0x400) 同样被掩掉，但它们只是缓存
    //   提示、不改变可访问性 ⇒ 仍按基类型判定。
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    switch (protect & 0xFF) {
    case PAGE_READONLY: return !want_write;
    case PAGE_READWRITE: return true;
    case PAGE_WRITECOPY: return true;
    case PAGE_EXECUTE: return !want_write;
    case PAGE_EXECUTE_READ: return !want_write;
    case PAGE_EXECUTE_READWRITE: return true;
    case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false; // 未知保护位组合
    }
}

// 逐 region 走一遍 ⇒ 覆盖"区间跨越多个 region"的情形
bool mem_access_ok(const void* p, size_t n, bool want_write)
{
    if (!p) return false;
    if (n == 0) n = 1;
    const unsigned char* b = (const unsigned char*)p;
    const unsigned char* e = b + n;
    // b+n 必须落在地址空间内。回绕时 e<b ⇒ 下面 while 一次都不跑就直接
    //   return true —— 读门会失效放行。
    //   真实可达路径是 wstr_view：垃圾 _Bx 落在 [0xFF7FFFFE, 0xFFFFFFFF]、
    //   且 _Mysize 接近上限 0x400000 时，(len+1)*2 会把 b+n 顶过 4G。
    if (n > (size_t)(~(size_t)0 - (size_t)b)) return false;
    while (b < e) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((const void*)b, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (!protect_ok(mbi.Protect, want_write)) return false;
        const unsigned char* regionEnd = (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
        if (regionEnd <= b) return false; // 防御：RegionSize 为 0 时死循环
        b = (regionEnd > e) ? e : regionEnd;
    }
    return true;
}

// 读 wstring 三个字段：全部塞在 __try 里。
// 这个函数里不放任何需要展开的对象（/EHsc 下带对象析构的函数里不允许 __try，C2712）。
struct RawWs {
    const wchar_t* b;
    unsigned       len;
    unsigned       res;
};

bool read_raw_ws(void* obj, RawWs* out)
{
    __try {
        const unsigned char* o = (const unsigned char*)obj;
        out->res = *(const unsigned*)(o + AC1_WSTR_OFF_RES);
        out->len = *(const unsigned*)(o + AC1_WSTR_OFF_SIZE);
        const unsigned char* bx = o + AC1_WSTR_OFF_BX;
        out->b = (out->res < AC1_WSTR_SSO_RES) ? (const wchar_t*)bx : *(const wchar_t* const*)bx;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faults);
        return false;
    }
}

} // namespace

int mem_class(const void* p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p) return MEMCLS_UNKNOWN;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return MEMCLS_UNKNOWN;
    switch (mbi.Type) {
    case MEM_IMAGE: return MEMCLS_IMAGE;
    case MEM_PRIVATE: return MEMCLS_PRIVATE;
    case MEM_MAPPED: return MEMCLS_MAPPED;
    default: return MEMCLS_UNKNOWN;
    }
}

bool mem_readable(const void* p, size_t n) { return mem_access_ok(p, n, false); }
bool mem_writable(const void* p, size_t n) { return mem_access_ok(p, n, true); }

unsigned long mem_faults() { return (unsigned long)g_faults; }

WStrView wstr_view(void* obj)
{
    WStrView v;
    v.buf = NULL;
    v.len = 0;
    v.res = 0;
    v.ok = false;
    if (!obj) return v;
    if (mem_class(obj) == MEMCLS_UNKNOWN) return v;

    RawWs raw;
    if (!read_raw_ws(obj, &raw)) return v;
    if (!raw.b) return v;
    if (raw.res > 0x400000u) return v; // 明显是垃圾字段
    if (raw.len > raw.res) return v;   // 自洽检查：_Mysize 不得超 _Myres
    if (raw.res >= AC1_WSTR_SSO_RES) { // 堆缓冲：分配器按 16 起步，_Myres 至少 15
        if (raw.res < 15) return v;
    }
    if (!mem_readable(raw.b, ((size_t)raw.len + 1) * sizeof(wchar_t))) return v;

    v.buf = raw.b;
    v.len = raw.len;
    v.res = raw.res;
    v.ok = true;
    return v;
}

} // namespace core
} // namespace ac1
