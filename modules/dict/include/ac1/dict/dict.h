#pragma once
#include "ac1/dict/fallback.h" // DictStats 里有 main_fallback[] 逐条兜底计数

// modules/dict —— 词典层（ENG 宽串 → 中文宽串）
//
// dict 只依赖 core 的纯 Win32 原语（file.h 整文件读取、str.h 安全累加器），
// 不依赖其它模块。日志不 include core 的 log.h，而是出口注入：装配层负责
// `ac1::dict::set_log(&ac1::core::log_line)`；单测注入自己的出口；谁都不注入就静默。
//
// 部署形态（两种，init() 里二选一）：
//   · 权威形态：用户词典 AC1_Dict.txt —— UTF-8、`ENG<TAB>译文`、`#` 注释。
//     独占：绝不拿 LG 主词典（繁体中文）兜底，没列出的条目保留英文原文。
//   · 兼容形态：AC1_Dict.txt 缺失时 LG 主词典 LGCStringDict_01.txt
//     （CP950/Big5 + `\xNNNN` 转义，成对块标记 ENG_Start/_End 与 CHI_Start/_End）
//     叠加 AC1_CN_Dict_Supplement.txt（UTF-8，同键覆盖主词典）。
//
// 代码页：LG 主词典固定 CP950，用户/补充词典固定 CP_UTF8，绝不用 CP_ACP
//（CP_ACP 随机器 locale 变，换台电脑整份译文变乱码）。
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

// ---- 日志出口（不依赖 core 的关键）----
typedef void (*LogFn)(const char* fmt, ...);
void set_log(LogFn fn);
void set_warn_log(LogFn fn); // WARN 出口（降级行，如「未就绪」）；不接则回退走 set_log

// 计数快照（状态行/单测用）
struct DictStats {
    int keys, keypool_used, valpool_used;
    int main_pairs, main_noval, main_cjk, main_keylong, main_dup, main_valfull, main_keyfull;
    int sup_lines, sup_new, sup_over, sup_same;
    int sup_comment, sup_empty, sup_bad, sup_keybad, sup_valbad, sup_poolfull, sup_keyfull;
    int sup_keylong; // 键超长（解码 >8192 或被 canon 按 DICT_KEY_MAX 拒）：与「键非法」分开计
    int sup_vallong; // 译文超长（解码 >8192 码元）
    // ★ 某行「既超长又含非法字节」时，Win32 先判容量 ⇒ 计入上面两个「过长」桶（不可分）。
    int main_valtrunc; // 主词典 CHI 值超 8192 码元被截断的次数（计截断本身，最终不一定入库）
    int decode_qmark;  // CP950 解不出 → 落 '?' 的次数
    // 两条载入路径各自一组兜底计数。字段按 FALLBACK_MAX_RULES 定长 ⇒
    // 加规则只改 fallback.cpp，不动这个结构的长度。
    int main_fallback[FALLBACK_MAX_RULES]; // 逐条兜底规则的命中次数
    int main_fallback_full;                // 兜底展开后装不下 ⇒ 整条丢弃（宁可不翻译也不写坏）
    int sup_fallback[FALLBACK_MAX_RULES];
    int sup_fallback_full;
};

// 定位并载入两份词典（asi_dir 带尾反斜杠）。词典缺失时不安装文本翻译，
// 但日志会写清楚；调用方仍可继续装钩（只观测、不替换）。
bool init(const char* asi_dir);
bool ready();

const char*      main_path(); // 实际用到的路径（没找到是空串）
const char*      supp_path();
const char*      status(); // 一行用量摘要
const DictStats& stats();

// ---- 词典原语（离线单测直接打这些）----

// 规范化：连续空白折叠成单空格 + 掐头去尾；遇 0 视为串尾。返回新长度；超 cap 返回 -1。
// n < 0 表示按 NUL 结尾自行算长度。
// ★ out **不写终止符** —— 输出一律按返回值取用（cap 只限制写入的码元数）。
int canon(const wchar_t* in, int n, wchar_t* out, int cap);

// 精确匹配（canon 之后逐码元相等）的查表结果。
// 命中 = {译文指针, 长度}；未命中 p == NULL（len 恒为 0，可直接读）。
// 指针进值池，有效期 = 进程生命期。
struct DictVal {
    const wchar_t* p;
    int            len;
};

// 精确匹配（canon 之后逐码元相等）。返回查表结果；未命中 p == NULL。
// n 必须 <= DICT_KEY_MAX（表里装不下更长的键）——超出一律按未命中处理。
DictVal lookup(const wchar_t* key, int n);

// 载入主词典（CP950 成对块）。返回新建的键条数。
int load_main(const char* path);

// 载入补充词典（UTF-8，同键覆盖）。返回新+覆盖的条目数。
int load_supplement(const char* path);

// 单测用：把键表 / 两个池 / 计数 / 就绪标志全部清零（同一进程里验证多种部署形态）。
// 真机上只 init 一次，不要调它。
void reset();

} // namespace dict
} // namespace ac1
