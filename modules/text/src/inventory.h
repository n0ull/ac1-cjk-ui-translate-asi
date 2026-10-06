#pragma once

// modules/text 内部：字符串全记录（inventory）
//
// 目的：先把游戏真正用到的串全记录下来，翻译决策等清单齐了再定。
//   · 每个唯一串只登记一次，同时累计出现次数、状态标签、首次调用者
//   · 由状态节拍（每 ~10s）批量刷到 <ASI目录>\AC1_CJK\strings_<run>.txt
//   · 固定大小、零动态分配（运行期不 malloc）；表满只计"丢弃"，不覆盖旧条目
//   · 可从任意线程调用：命中的快路径无锁，新增路径用一把小锁把「占槽 + 写 + 发布」
//     串起来 —— 「读游标 → 判边界 → 写 → 自增」在并发下会让两个线程覆盖写同一格，
//     丢一条唯一串并让游标指向一格从没写过的空洞（落盘成 [×0] 空行）。
//
// 本头是模块内部的（不进 include/，不跨模块）；只有本模块的 .cpp 引它。

namespace ac1 {
namespace text {

// 状态标签位（可叠加；同一条串可能先"命中"、后又被"替换"）
enum {
    INV_F_HIT = 1,      // 词典命中
    INV_F_REPLACED = 2, // 真正写回了译文（TEXT_REPLACE=1 且容量够）
    INV_F_MISS = 4,     // 词典未命中
    INV_F_NONASCII = 8, // 含 ≥0x80 码位（图标转义类，本就不该翻）
};

// 钩子里调：登记一条串（去重 + 计数 + 合并标签 + 记首次调用者）
void inv_remember(const wchar_t* t, int n, unsigned flags, void* ra);

// 刷盘入口（inventory_tick）的公开声明在 text.h（装配层要调它）；
// 本头只管"钩子线程侧"的 inv_remember + 标签位。

} // namespace text
} // namespace ac1
