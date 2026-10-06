// modules/core/src/ctr.cpp
//
// 原子计数组 + 占槽游标的实现。刻意不持有名字表 —— 名字归调用方，
// 否则 core 要认识每个模块的业务语义，框架层会反向依赖业务层。

#include "ac1/core/ctr.h"

namespace ac1 {
namespace core {

static int ctr_slots(const Counters* c)
{
    if (!c) return 0;
    if (c->n <= 0 || c->n > COUNTER_SLOTS_MAX) return COUNTER_SLOTS_MAX;
    return c->n;
}

void ctr_reset(Counters* c, int n)
{
    if (!c) return;
    for (int i = 0; i < COUNTER_SLOTS_MAX; i++) c->v[i] = 0;
    c->next = 0;
    c->n = (n > 0 && n <= COUNTER_SLOTS_MAX) ? n : COUNTER_SLOTS_MAX;
}

void ctr_inc(Counters* c, int slot)
{
    if (!c || slot < 0 || slot >= ctr_slots(c)) return;
    InterlockedIncrement(&c->v[slot]);
}

void ctr_add(Counters* c, int slot, long delta)
{
    if (!c || slot < 0 || slot >= ctr_slots(c) || delta == 0) return;
    InterlockedExchangeAdd(&c->v[slot], (LONG)delta);
}

long ctr_get(const Counters* c, int slot)
{
    if (!c || slot < 0 || slot >= ctr_slots(c)) return 0;
    return (long)InterlockedCompareExchange((volatile LONG*)&c->v[slot], 0, 0);
}

int ctr_claim_n(Counters* c, int cap)
{
    if (!c) return -1;
    // 上界由表的容量决定，不是计数数组的长度。
    //   占槽游标(next)与 v[] 是两回事：next 只是一个 LONG，表比计数数组大完全正常
    //   （text 的「未命中 ASCII Top」有 TOP_N=128 格，计数器却只用十几个槽）。
    const int bound = (cap > 0) ? cap : ctr_slots(c);
    // bound ≥ 1 恒成立：cap>0 时它是 cap；否则是 ctr_slots(c)，而 c 非空时它要么是
    //   COUNTER_SLOTS_MAX，要么是 0<n≤MAX 的 c->n（见 ctr_slots）。
    // 自增必须提到最前面。InterlockedIncrement 是全屏障 ⇒ 每次拿到的序号全局唯一。
    //   「读计数 → 判边界 → 写第 n 格 → 最后自增」这种写法，两个线程会拿到同一个 n、
    //   都通过边界检查、都写第 n 格（丢一条），并把游标顶过表容量。
    LONG s = InterlockedIncrement(&c->next) - 1; // -1：next 从 0 起，序号从 0 起
    if (s >= bound) return -1;
    return (int)s;
}

int ctr_claim(Counters* c) { return ctr_claim_n(c, 0); }

void ctr_snapshot(const Counters* c, long* out, int outMax)
{
    if (!out || outMax <= 0) return;
    const int slots = ctr_slots(c);
    const int n = (slots < outMax) ? slots : outMax;
    for (int i = 0; i < n; i++) out[i] = ctr_get(c, i);
    for (int i = n; i < outMax; i++) out[i] = 0;
}

int ctr_count(const Counters* c) { return ctr_slots(c); }

} // namespace core
} // namespace ac1
