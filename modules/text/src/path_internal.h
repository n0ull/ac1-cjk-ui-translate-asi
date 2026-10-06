#pragma once

// modules/text/src/path_internal.h —— 同模块内部的接缝（不进 include/，不跨模块）
//
// 用途：run_post_counted 是漏斗 post 阶段的唯一实现，钩子与单测调的都是它；
// 它是单测直接驱动 post 阶段的缝。

#include <windows.h>

namespace ac1 {
namespace text {

// 观测 dst（引擎物化后的临时 wstring）并按词典决定要不要替换。
// 任一闸门不过就一个字节都不写。
void run_post_counted(void* dst, void* ra);

} // namespace text
} // namespace ac1
