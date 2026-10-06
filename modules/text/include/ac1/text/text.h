#pragma once

// modules/text —— 文本层（单点漏斗）
//
// 一条文本路径 = sub_8A1920 漏斗（src/path_funnel.cpp）：全部 UI 文本的共享入口。
// 装钩、计数、出状态行。本层不认识游戏，只认识 wstring。
//
// 依赖：core（读门/挂点/日志/计数）+ dict（词库）。不依赖 glyph、不依赖 app。
//
// ---- 编译开关（本模块的两个）----
//   TEXT_REPLACE 默认 0 = 只观测，一个字节都不写（安全默认）。1 = 真替换。
//   SAMPLE_N     默认 32 = 命中/未命中各最多记多少条样本。
// 开关故意放在公开头里：单测要按同一组开关写断言，所以测试与实现必须看得到同一个宏。

#if !defined(TEXT_REPLACE)
#define TEXT_REPLACE 0
#endif
#if !defined(SAMPLE_N)
#define SAMPLE_N 32
#endif

namespace ac1 {
namespace text {

// 漏斗的运行期计数快照（纯 POD，跨模块安全）。
// 「跳过」按原因拆开：真机排查时"跳过 1502"没法行动，而"空串多少 / 含 CJK 多少 /
// 词典未就绪多少"能直接指出是哪一道闸门在拦。
struct TextCounters {
    int hits;         // 观测到的调用次数
    int replaced;     // 真正改写了 dst 的次数（TEXT_REPLACE=1 时才可能 >0）
    int miss;         // 词典未命中（= missAscii + missNonAscii）
    int missAscii;    //   其中：纯 ASCII 串 ⇒ 词典缺条目（可行动信号）
    int missNonAscii; //   其中：含 ≥0x80 码位（图标转义类，本就不该翻）
    int skippedGrow;  // 命中了但译文装不下（_Myres 容量不够）
    int fault;        // 被 __except 兜住的异常次数

    int skipEmpty;        // 跳过：空串
    int skipCjk;          // 跳过：串里已有 CJK（幂等，上一轮译文）
    int skipDictNotReady; // 跳过：词典未就绪
    int skipBadView;      // 跳过：wstring 结构不可信（读门/自洽检查没过）
    int skipOther;        // 跳过：其它（canon 失败 / 缓冲不可写）

    int sampleHit;  // 已记的命中样本数
    int sampleMiss; // 已记的未命中样本数
    int topCount;   // 「未命中 ASCII Top」当前攒了几条
};

// 「跳过」合计（状态行与单测用；分项见上面各字段）
inline int skipped(const TextCounters& k)
{ return k.skipEmpty + k.skipCjk + k.skipDictNotReady + k.skipBadView + k.skipOther; }

// ---- 生命周期（装配层）----

// 装漏斗钩。签名不符 / 宿主不适用 ⇒ 返回 false（绝不硬装）。
bool install();

// 计数快照（每次调用现场拷贝；状态行与单测都从这里取）
TextCounters counters();

// 状态行：主行（写进每 10 秒那条 [状态]）
const char* status_line();
// 附属明细行：追加在主行后面那条（当前 = 「未命中 ASCII Top」）
const char* detail_line();

// forge 闸门：装配层启动时注入 forge 核验结果。关 ⇒ 本层只观测不替换
//（与 TEXT_REPLACE=0 同形），闸门关 = 纯原版界面。
void set_forge_patch(int present);

// ---- 字符串全记录（inventory）----
// 把游戏真正用到的串全记录下来，翻译决策等清单齐了再定。由状态节拍驱动刷盘，
// 输出到 AC1_CJK\strings_<run>.txt（每 run 一份）。
// 实现见 src/inventory.cpp；登记入口 inv_remember() 只给本模块的钩子用，不在此公开。
void inventory_tick(const char* asi_dir);

// ------------------------------------------------------------------
// 自测缝（self-test seam）
//
// 漏斗的真正逻辑在 inline 钩返回之后（post 阶段），而 inline 钩在没有游戏的
// 离线环境里装不上。为了让这部分逻辑能被单测覆盖，这里把 post 阶段单独暴露出来：
// 它做的事与钩子里调的完全同一个函数，不是复制品。
//
// 用法（modules/text/tests）：造一个假 wstring ⇒ 灌进假词典 ⇒ 调它 ⇒ 查计数器。
// 真机运行时钩子调的也是它，所以测过的就是跑的。
// ------------------------------------------------------------------
void funnel_run_post(void* dst, unsigned ra);

} // namespace text
} // namespace ac1
