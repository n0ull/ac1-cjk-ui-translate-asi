#pragma once

// modules/core —— 文件读取与路径拼接
//
// 全程 Win32、零 CRT：同一份代码可能出现在 DllMain 的 loader lock 下，
// CRT 的文件 API 不保证那个环境安全。跨模块只传 POD。

#include <windows.h>

namespace ac1 {
namespace core {

// 整文件读进新分配的缓冲。
//   path   文件路径（ANSI）
//   out    成功时指向新缓冲（HeapAlloc），失败时置 NULL
//   outN   成功时写入字节数，失败时置 0
//   maxBytes 上限；<=0 时用 FILE_READ_MAX_DEFAULT。超过上限按失败处理
//           （不是截断——被悄悄截断的清单/词典比直接失败危险得多）。
// 返回 1 = 读成功（调用方负责 core::file_free），0 = 失败。
//   ★ 0 覆盖一整集形态：参数非法 / 打不开（不存在、无权限、被独占）/ 尺寸读不出 /
//     **0 字节** / 超过 maxBytes（不截断）/ 分配失败 / ReadFile 失败或读不满。
//     调用方**不得**据 0 断言「文件不存在」，也别读 GetLastError 区分——超限与分配
//     失败分支不设置错误码；要区分请自行 file_exists() 或探测尺寸。
bool file_read_all(const char* path, unsigned char** out, int* outN, int maxBytes);

// 释放 file_read_all 交出的缓冲。
void file_free(void* p);

// 文件是否存在（可读）。用于「词典在不在」「清单在不在」这类前置判定。
bool file_exists(const char* path);

// 目录 + 文件名 → 路径。dir 末尾有没有反斜杠都行；装不下返回 0（out 内容未定义，
// 调用方应按失败处理并写日志）。
bool path_join(char* out, int cap, const char* dir, const char* name);

enum {
    FILE_READ_MAX_DEFAULT = 16 * 1024 * 1024
}; // 16 MB：词典 / 字形清单够用

} // namespace core
} // namespace ac1
