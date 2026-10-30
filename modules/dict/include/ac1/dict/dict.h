#pragma once

// modules/dict —— 词典层（原文宽串 → 目标宽串）
//
// 词典只接受包内 UTF-8 字节，不读取文件或搜索目录。日志走出口注入：装配层负责
// `set_log(info, error)`分别注入两种等级的出口；NULL关闭对应等级。
//
// 行格式：`原文<TAB>译文`，无 TAB 的 `#` 行是注释，规范键必须唯一。
// 缺失的键保持未命中，由调用方保留原文。
//
// 键的规范化 canon()：空白折叠成单空格 + 掐头去尾。词典侧和运行期侧调同一个函数。
//
// 匹配：整数哈希（FNV-1a）+ 线性探测，槽 8192。只做 canon + 精确相等；
// 大小写/写法差异由词典数据补全，匹配期不加模糊兜底。
//
// 跨模块只暴露 POD，不传任何 STL 对象。

#define DICT_SLOTS 8192     // 哈希槽（2^13，线性探测）
#define DICT_MAX_KEYS 6144  // 键条数上限
#define DICT_KEY_MAX 4096   // 规范化后键长上限（实测长对白超 3,600 码元，300 会丢它们）
#define DICT_KEYPOOL 196608 // 键码元池（u16；实测键总量 ≈ 99K 码元）
#define DICT_VALPOOL 131072 // 译文码元池（u16；实测译文总量 ≈ 38K 码元）

namespace ac1 {
namespace dict {

enum { RULES_VERSION = 2 };

// ---- 日志出口（不依赖 core 的关键）----
typedef void (*LogFn)(const char* fmt, ...);
void set_log(LogFn info, LogFn error);

// 计数快照（状态行/单测用）
struct DictStats {
    int keys, keypool_used, valpool_used;
    int sup_lines, sup_new, sup_duplicate;
    int sup_comment, sup_empty, sup_bad, sup_keybad, sup_valbad, sup_poolfull, sup_keyfull;
    int sup_keylong; // 键超长（解码 >8192 或被 canon 按 DICT_KEY_MAX 拒）：与「键非法」分开计
    int sup_vallong; // 译文超长（解码 >8192 码元）
    // ★ 某行「既超长又含非法字节」时，Win32 先判容量 ⇒ 计入上面两个「过长」桶（不可分）。
    int sup_ellipsis; // 本次初始化中成功展开的 U+2026 数量。
    int sup_fallback_full;
    // Physical lines include comments/blanks. Input-level failures use line 0.
    int rejected_lines, first_rejected_line, failure_line;
    const char* reason; // Static literal: first failed row or input failure.
};

// Startup-only runtime package input: strict UTF-8 TSV, escapes and fallback.
// Resets previous entries before reading; copies values into the dictionary pools;
// the input may be released afterwards. Invalid rows, duplicate canonical keys,
// capacity failures or non-idempotent mappings clear entries to not-ready.
// Every target must remain unchanged if queried again. Diagnostics survive a
// rejected attempt; status() reports rules=2 and result=clean|rejected.
bool init_bytes(const unsigned char* data, int bytes);
bool ready();

const char*      status(); // 一行用量摘要
const DictStats& stats();

// ---- 词典原语（离线单测直接打这些）----

// 规范化：连续空白折叠成单空格 + 掐头去尾；遇 0 视为串尾。返回新长度；超 cap 返回 -1。
// n < 0 表示按 NUL 结尾自行算长度。
// ★ out **不写终止符** —— 输出一律按返回值取用（cap 只限制写入的码元数）。
int canon(const wchar_t* in, int n, wchar_t* out, int cap);

// 精确匹配（canon 之后逐码元相等）的查表结果。
// 命中 = {译文指针, 长度}；未命中 p == NULL（len 恒为 0，可直接读）。
// 指针进值池，有效期到下一次 init_bytes；运行期启动后不再初始化。
struct DictVal {
    const wchar_t* p;
    int            len;
};

// 精确匹配（canon 之后逐码元相等）。返回查表结果；未命中 p == NULL。
// n 必须 <= DICT_KEY_MAX（表里装不下更长的键）——超出一律按未命中处理。
DictVal lookup(const wchar_t* key, int n);

} // namespace dict
} // namespace ac1
