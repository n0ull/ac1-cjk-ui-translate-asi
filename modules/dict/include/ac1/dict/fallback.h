// modules/dict/include/ac1/dict/fallback.h —— 词典兜底规整（译文侧）
//
// 「兜底」= 译文里某些字符不能原样交给游戏：不是缺字，而是原版字体在那个码位上
// 放的不是这个字。典型：U+2026（…）——四套原字体里只有 title 自带该条目，而它指向
// 育碧的装饰图标，省略号画出来是个图标。
//
// 这些码位我们一律不接管（打包器同理：接管会把原字形顶掉）。所以唯一的做法是
// 改译文：把那些字符换成等价的安全写法。U+2026 → `...` 之所以对，是因为英文原文
// 本来就写 `...`。
//
// 集中在这里，是为了它可枚举、可测试、可审计：兜底每加一条都只动 kRules 一行
// + 一条单测。
//
// ===== 契约 =====
// · 就地改写 buf；可能变长（`…` 1 → `...` 3）。
// · 返回新长度；装不下返回 -1，且此时 buf 内容可能已被改动——调用方必须以返回值
//   为准，-1 之后不可继续使用 buf（要保留原文请自己先留副本）。
// · 单条规则内部不含 NUL；len < 0 表示按 NUL 自行算长度。
// · 计数：每命中一个字符 +1，供日志/单测核对（见 fallback_hits）。
#ifndef AC1_DICT_FALLBACK_H
#define AC1_DICT_FALLBACK_H

// 规则数上限（给「按规则逐条计数」的静态数组用）。加规则超过这个数会被
//   fallback.cpp 的 static_assert 挡在编译期（DictStats 的逐条计数装不下）。
enum {
    FALLBACK_MAX_RULES = 8
};

namespace ac1 {
namespace dict {

// 对一条译文跑全部兜底规整。cap = buf 的容量（码元数）。返回新长度；装不下返回 -1。
int fallback_apply(wchar_t* buf, int len, int cap);

// 各类兜底规则的累计命中次数（进程生命期累计）。下标与 kRules 一致。
int fallback_hits(int ruleIndex);

// 规则总数
int fallback_rule_count();

// 规则名（给日志用）。越界返回 "?"
const char* fallback_rule_name(int ruleIndex);

// 一行说明：规则数 + 每条命中次数。给日志用。
const char* fallback_status();

} // namespace dict
} // namespace ac1

#endif // AC1_DICT_FALLBACK_H
