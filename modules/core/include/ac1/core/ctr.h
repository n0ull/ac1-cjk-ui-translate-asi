#pragma once

// modules/core —— 原子计数组 + 占槽游标
//
// 各模块的计数器统一走这里。设计约束：纯 POD、固定容量、零分配。
// 槽位数编译期定死（COUNTER_SLOTS_MAX），模块自己决定用掉前多少格。
// 名字表不进 core——否则框架层要认识业务语义。
//
// 占槽（ctr_claim）把「自增」提到最前面，拿到的序号天然唯一——
// 不做这步的话，「读计数器 → 边界检查 → 写第 n 格 → 最后自增」会让两个线程
// 拿到同一下标、同写一格、并把计数器顶过数组上界。

#include <windows.h>

namespace ac1 {
namespace core {

enum {
    COUNTER_SLOTS_MAX = 32
};

struct Counters {
    volatile LONG v[COUNTER_SLOTS_MAX]; // 前 n 格是计数
    volatile LONG next;                 // 占槽游标（只增）；不占用 v 的任何一格
    int           n;                    // 实际使用槽数（<= COUNTER_SLOTS_MAX）
};

// 全部清零。n<=0 或 n>COUNTER_SLOTS_MAX 时按 COUNTER_SLOTS_MAX 处理（上限保护）。
void ctr_reset(Counters* c, int n);

// 给某个槽 +1。槽号越界时静默丢弃，绝不越界写。
void ctr_inc(Counters* c, int slot);

// 给某个槽加一个增量（可以是负数）。
void ctr_add(Counters* c, int slot, long delta);

// 读一个槽。越界返回 0。
long ctr_get(const Counters* c, int slot);

// 占一个表槽：返回本次独占的序号（0..n-1），满了返回 -1。
// 拿到序号之后再写 g_table[seq]，就不会出现「两线程写同一格」。
// next 只增不减 ⇒ 复用同一个 Counters 前必须 ctr_reset。
//   典型用法：int s = ctr_claim(&g_reg); if (s >= 0) g_reg[s].font = font;
//
// ctr_claim_n 是显式上界版（cap<=0 时等价于 ctr_claim）——占槽游标与计数数组
// 是两回事，「表比计数数组大」是正常用法（如 text 的 128 格 Top 表配十几个计数槽）。
int ctr_claim_n(Counters* c, int cap);
int ctr_claim(Counters* c);

// 把前 outMax 个槽读进 out（超出 n 的部分补 0）。
void ctr_snapshot(const Counters* c, long* out, int outMax);

// 本实例已用槽数。仅单测使用：状态行读的是各槽的 ctr_get。
int ctr_count(const Counters* c);

} // namespace core
} // namespace ac1
