// modules/text/src/inventory.cpp —— 字符串全记录（inventory）
//
// 设计要点：
//   · 零动态分配：4096 条 × 48 码元的静态表；表满只累计丢弃数，不覆盖旧条目
//     （保持"首次出现顺序"，这对"按出现顺序读清单"的离线流程很重要）
//   · 去重是线性扫：本模块每轮实机只有几千次调用（实测 3231），
//     4096 上限下最坏 ~1.3e7 次短比较，代价可忽略；换哈希表的复杂度不值得。
//     命中的快路径无锁（绝大多数调用落在这里），只有「新增」才进锁（见 inv_remember）
//   · 刷盘由状态节拍驱动（每 ~10s），写出 [已写出, N) 区间
//     （新增走「占槽 + 写 + 发布」的原子三元组，发布是最后一步 ⇒ 下标 < N 的条目
//       必然完整，不会读到"半条"）
//   · 文件是追加的：每个进程一份 run 头，方便把多次实机拼在一个文件里比对

#include "inventory.h"

#include "ac1/core/log.h"
#include "ac1/dict/dict.h" // 只读 dict::ready()/status() 写 run 头（text → dict 是允许方向）
#include "ac1/core/ctr.h"
#include "ac1/core/trace.h"
#include "ac1/core/file.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>

namespace ac1 {
namespace text {
namespace {

const int INV_MAX = 4096; // 唯一串上限
const int INV_LEN = 48;   // 每条最多记多少码元（超出截断，仍算同一个"前缀串"）

struct InvEntry {
    wchar_t        t[INV_LEN];
    unsigned short n;      // 实际登记的码元数（≤ INV_LEN）
    unsigned short cnt;    // 出现次数（饱和到 0xFFFF）
    unsigned char  flags;  // INV_F_*
    unsigned int   caller; // 首次见到的调用者返回地址（IDA 可反查）
};

InvEntry g_inv[INV_MAX];
// 只有 g_invDrop 是真正的计数 ⇒ 进 core::Counters。g_invN / g_invDone 故意留在外面：
//   · g_invN 是「已发布条目数」游标，必须在条目写完之后才自增 —— 读侧全靠这条
//     不变量（tick 与去重扫描都只碰下标 < g_invN 的格）。而 ctr_claim 的语义恰恰
//     相反（自增在前、序号即下标）：拿它当槽位来源，读侧就必须容忍「已认领但还没
//     写完」的格子，而它又给不出「自增在最后」。两个要求互斥 ⇒ 新增路径改用
//     g_invLock 把「占槽 + 写 + 发布」三者串起来，游标语义保持不变。
//   · g_invDone 不是计数，是落盘写指针，由 tick 单线程推进。
enum {
    C_DROP = 0,
    INV_COUNTER_SLOTS
};
core::Counters g_c;
struct InvCtrInit {
    InvCtrInit() { core::ctr_reset(&g_c, INV_COUNTER_SLOTS); }
} g_invCtrInit;
volatile LONG g_invN = 0;    // 已发布（写完的）唯一串数；自增必须是 inv_remember 的最后一步
volatile LONG g_invDone = 0; // 已经写进文件的条数
char          g_dir[MAX_PATH] = "";
int           g_hdrDone = 0;

static int inv_drop() { return (int)core::ctr_get(&g_c, C_DROP); }

void to_utf8(char* out, size_t cap, const wchar_t* s, int n)
{
    out[0] = 0;
    if (cap < 2) return;
    int w = WideCharToMultiByte(CP_UTF8, 0, s, n, out, (int)cap - 1, NULL, NULL);
    out[(w > 0) ? w : 0] = 0;
}

const char* flag_tag(unsigned char f)
{
    if (f & INV_F_REPLACED) return "替换   ";
    if (f & INV_F_HIT) return "命中   ";
    if (f & INV_F_MISS) return ((f & INV_F_NONASCII) ? "未命中·图标" : "未命中 ");
    return "未知   ";
}

void write_entry(HANDLE h, const InvEntry* e)
{
    char u8[INV_LEN * 4 + 8];
    to_utf8(u8, sizeof(u8), e->t, (int)e->n);
    char line[INV_LEN * 4 + 96];
    int n = _snprintf(line, sizeof(line), "[×%u] %s caller=%08X %s\r\n", (unsigned)e->cnt, flag_tag(e->flags),
                      e->caller, u8);
    if (n <= 0) return;
    DWORD wrote = 0;
    WriteFile(h, line, (DWORD)n, &wrote, NULL);
}

// ---- 新增槽的写锁 ----
// 只保护「占槽 + 写条目 + 发布 g_invN」这一段三元组。没有它，两个线程会在上面那次
//   去重扫描里都找不到对方要写的串（那时谁都没写），双双读到同一个 cnt、双双写
//   g_inv[cnt]：一条唯一串被覆盖丢失，而 g_invN 被顶到 cnt+2 ⇒ 下标 cnt+1 那格
//   永远没写过，tick 会把全零当条目落盘（`[×0]  caller=00000000`），
//   也会读到「文本来自一个串、n/flags/caller 来自另一个」的半条。
//   （core/src/trace.cpp 的 g_regLock 是同一形状：登记表的「扫重 → 填格 → 发布」
//     三元组也要串行化，理由相同。）
// 0 = 空闲、1 = 持有。临界区里只有一次 ≤4096 条的短比较扫描 + 一次 ≤96 字节拷贝；
//   命中的快路径根本不进锁，而新串每 run 最多 INV_MAX 次 ⇒ 热路径无锁。
volatile LONG g_invLock = 0;

void inv_lock()
{
    while (InterlockedCompareExchange(&g_invLock, 1, 0) != 0) YieldProcessor();
}
void inv_unlock() { InterlockedExchange(&g_invLock, 0); }

// 扫已发布区间 [0, g_invN) 找同串。命中 ⇒ 就地合并标签/计数/首次调用者并返回 1。
//   这三个字段的读改写不是原子的：并发命中同一条时可能丢一次计数或一个标签位 ——
//   只影响统计精度（出现次数 / 状态标签），不影响条目的存在性与完整性，
//   落盘侧也不依赖它们的一致性。必须原子的是「新增」（占槽 + 写 + 发布），见下。
int inv_find_update(const wchar_t* t, int store, unsigned flags, void* ra)
{
    const int cnt = (int)g_invN;
    for (int i = 0; i < cnt; i++) {
        InvEntry* e = &g_inv[i];
        if (e->n != (unsigned short)store) continue;
        if (memcmp(e->t, t, (size_t)store * sizeof(wchar_t)) == 0) {
            e->flags |= (unsigned char)flags;
            if (e->cnt != 0xFFFF) e->cnt++;
            if (!e->caller) e->caller = (unsigned int)(uintptr_t)ra;
            return 1;
        }
    }
    return 0;
}

} // namespace

void inv_remember(const wchar_t* t, int n, unsigned flags, void* ra)
{
    if (!t || n <= 0) return;
    const int store = (n > INV_LEN) ? INV_LEN : n;

    // 快路径：已登记过（无锁）。实机绝大多数调用走这里 —— 唯一串只有几千条。
    if (inv_find_update(t, store, flags, ra)) return;

    inv_lock();
    // 重扫：上面那次扫描到这里之间，可能有别的线程刚把这个串插进去
    if (!inv_find_update(t, store, flags, ra)) {
        const int cnt = (int)g_invN;
        if (cnt >= INV_MAX) {
            core::ctr_inc(&g_c, C_DROP); // 表满：只计丢弃，不覆盖旧条目（保持首次出现顺序）
        }
        else {
            InvEntry* e = &g_inv[cnt];
            memcpy(e->t, t, (size_t)store * sizeof(wchar_t));
            e->n = (unsigned short)store;
            e->cnt = 1;
            e->flags = (unsigned char)flags;
            e->caller = (unsigned int)(uintptr_t)ra;
            InterlockedIncrement(&g_invN); // 发布必须是最后一步（全屏障）⇒ 读侧看不到半条
        }
    }
    inv_unlock();
}

void inventory_tick(const char* asi_dir)
{
    if (!asi_dir || !asi_dir[0]) return;
    if (!g_hdrDone && !g_dir[0]) lstrcpynA(g_dir, asi_dir, MAX_PATH);

    int n = (int)g_invN;
    // 钳制是廉价的兜底：新增路径已经在 g_invLock 里判过 cnt >= INV_MAX，
    //   g_invN 不可能超过 INV_MAX。留着它是因为读侧一旦越界，损坏的是
    //   「喂给离线分类器的磁盘文件」而没有任何别的检查会拦下来（与 detail() 的
    //   钳制对称）。
    if (n > INV_MAX) n = INV_MAX;
    int done = (int)g_invDone;
    if (done >= n && g_hdrDone) return; // 没有新条目

    char path[MAX_PATH + 48];
    // 产出统一进 AC1_CJK\，文件名带 run stamp，与 run_<stamp>.log 连成同一轮；
    //   每 run 一个干净的全记录。路径拼接走 core::path_join（两步、带容量自检，
    //   与 log.cpp / trace.cpp 的写法一致）。
    char sub[MAX_PATH + 16];
    if (!core::path_join(sub, (int)sizeof(sub), g_dir, "AC1_CJK")) {
        core::log_line("!! [INV] 目录过长，拼不出 AC1_CJK\\ 子目录 ⇒ 本轮不落盘");
        return;
    }
    CreateDirectoryA(sub, NULL);
    char name[64];
    _snprintf(name, sizeof(name) - 1, "strings_%s.txt", core::log_run_stamp());
    name[sizeof(name) - 1] = 0;
    if (!core::path_join(path, (int)sizeof(path), sub, name)) {
        core::log_line("!! [INV] 目录过长，拼不出 %s（>%d 字节）⇒ 本轮不落盘", name, (int)sizeof(path));
        return;
    }

    HANDLE h =
        CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        core::log_line("!! [INV] 打不开 %s（err=%lu）⇒ 本轮不落盘", path, GetLastError());
        return;
    }

    if (!g_hdrDone) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        const char* ds = dict::ready() ? dict::status() : "未就绪";
        if (!ds || !ds[0]) ds = "-";
        char hdr[320];
        int k = _snprintf(hdr, sizeof(hdr), "# === run %04d-%02d-%02d %02d:%02d:%02d pid=%lu 词典=%s ===\r\n",
                          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                          (unsigned long)GetCurrentProcessId(), ds);
        if (k > 0) {
            DWORD w = 0;
            WriteFile(h, hdr, (DWORD)k, &w, NULL);
        }
        g_hdrDone = 1;
    }

    // 全部写出：新增路径在 g_invLock 里「先写条目、最后才自增 g_invN」（InterlockedIncrement
    //   是全屏障），所以任何下标 < n 的条目都已初始化完整 —— 既不会读到半条，也不会有
    //   两线程写同一格留下的空洞条目，无需跳过最新一条。
    for (int i = done; i < n; i++) write_entry(h, &g_inv[i]);
    CloseHandle(h);

    g_invDone = n;
    core::log_line("[INV] 字符串全记录：唯一 %d（丢弃 %d）｜ 已写出 %d 条 → %s", n, inv_drop(), n, path);
}

} // namespace text
} // namespace ac1
