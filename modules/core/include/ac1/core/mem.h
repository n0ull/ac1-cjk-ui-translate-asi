#pragma once

// modules/core —— 读门 + wstring 视图
//
// 所有对游戏内存的读取都必须走这里：先用 VirtualQuery 判"这段区间是不是已提交、
// 保护位允不允许读/写"，再用 __try/__except 兜住 VirtualQuery 判不准的情况
// （游戏运行中堆/栈会随时变动，单靠 VirtualQuery 是不够的）。异常只计数、绝不崩。
//
// 保护位判据是**保守**的：PAGE_NOACCESS 与 PAGE_GUARD 一律判否 —— guard page
// 技术上"读得动"（首次访问会把页解锁），但那一次访问会抛 STATUS_GUARD_PAGE_VIOLATION
// （0x80000001），对本模块的调用者而言等同于"这段内存不可信"。
//
// 跨模块只暴露 POD（int / bool / 指针 / 定长结构），不传任何 STL 对象。

#include <windows.h>
#include <cstddef>

namespace ac1 {
namespace core {

enum MemClass {
    MEMCLS_IMAGE = 0,   // MEM_IMAGE   —— exe/dll 映像
    MEMCLS_PRIVATE = 1, // MEM_PRIVATE —— 堆
    MEMCLS_MAPPED = 2,  // MEM_MAPPED  —— 映射文件
    MEMCLS_UNKNOWN = 3  // 查询失败
};

int mem_class(const void* p);

// [p, p+n) 是否整段可读 / 可写（跨 region 也逐段校验）
bool mem_readable(const void* p, size_t n);
bool mem_writable(const void* p, size_t n);

// 读门/视图里被 __except 兜住的异常总次数（状态行用；正常应恒为 0）
unsigned long mem_faults();

// MSVC8 std::wstring 视图：+0x04 _Bx（_Myres<8 时缓冲内联在 +0x04）/ +0x14 _Mysize / +0x18 _Myres
struct WStrView {
    const wchar_t* buf; // 数据指针
    unsigned       len; // _Mysize（码元数）
    unsigned       res; // _Myres（容量，码元数；buf[res] 是合法的 NUL 位）
    bool           ok;  // 自洽检查 + 读门全过
};

// wstring 对象在内存里的尺寸（SSO 与堆两种形态都是这么大）
#define AC1_WSTR_SIZE 0x1C
// 各字段偏移（与 MSVC8 std::wstring 实测布局一致）
#define AC1_WSTR_OFF_BX 0x04
#define AC1_WSTR_OFF_SIZE 0x14
#define AC1_WSTR_OFF_RES 0x18
// SSO 阈值：_Myres < 8 ⇒ 缓冲内联在 +0x04
#define AC1_WSTR_SSO_RES 8

// 取视图。任何一步不可信都返回 ok=false，且不抛异常。
// 容量上限 0x400000（=4M 码元）是防"读到垃圾 _Mysize/_Myres"的粗筛。
WStrView wstr_view(void* obj);

} // namespace core
} // namespace ac1
