#pragma once

// modules/core —— 安全字符串累加器
//
// 用于替代「先 _snprintf 打头、再拿返回值当偏移继续往后拼」的写法——两种 CRT 约定下
// 这个写法都会写坏缓冲，且方向相反：
//   · MSVC 的 _snprintf/_vsnprintf：截断时写满 count 字节、**不补 NUL**、返回负数；
//     不判负数就直接拿去当偏移，(size_t) 转换后是天文数字。
//   · ISO C 的 snprintf：返回「本来想写的长度」，截断时它 >= cap；拿它当偏移，
//     cap - o 在无符号运算下变成巨数，下一次调用直接越界写。
// 本累加器按实际写入量推进，并在两条约定下都自己补 NUL（见 src/str.cpp）。
//
// 设计约束：纯 POD，不分配、不抛异常；目标缓冲由调用方提供（core 不持有其生命周期）；
// 永远留一个 NUL；满了就静默丢弃，绝不越界。

#include <stddef.h>

namespace ac1 {
namespace core {

// 累加器。p/cap 由调用方给（cap 是含 NUL 的容量），len 由 str_init 置 0。
struct Str {
    char* p;   // 目标缓冲（可为空）
    int   cap; // 容量（含 NUL）；<= 0 视为不可用
    int   len; // 当前已写长度（不含 NUL）；只由本模块写
};

// 绑定缓冲。cap<=0 或 p==NULL 时进入「不可用」态，此后所有 str_addf 都是空操作。
void str_init(Str* s, char* p, int cap);

// 追加一段格式化文本，永不超过 cap。
//   · 目标已满 ⇒ 什么都不加（保留已有内容与 NUL）；
//   · 格式化失败(n<0) ⇒ 什么都不加；
//   · 会被截断 ⇒ 按实际写入量推进 len，不按 _snprintf 的返回值推进。
void str_addf(Str* s, const char* fmt, ...);

// 当前长度（不含 NUL）。不可用时为 0。
int str_len(const Str* s);

// 缓冲是否已经放不下更多内容（调用方决定还要不要继续拼）。
bool str_full(const Str* s);

} // namespace core
} // namespace ac1
