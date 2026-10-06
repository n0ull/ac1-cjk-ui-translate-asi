// modules/core/src/str.cpp
//
// 安全字符串累加器的实现。全部逻辑就是「按实际写入量推进长度」这一条，且**必须只有
// 一份**：副本与本实现漂移时，两边的截断行为会不一致，调用方按哪一份的边界判断都不可靠。

#include "ac1/core/str.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace ac1 {
namespace core {

void str_init(Str* s, char* p, int cap)
{
    if (!s) return;
    s->p = p;
    s->cap = (p && cap > 0) ? cap : 0;
    s->len = 0;
    if (p && cap > 0) p[0] = 0;
}

void str_addf(Str* s, const char* fmt, ...)
{
    if (!s || !s->p || s->cap <= 0) return;
    if (s->len < 0) s->len = 0; // len 只由本模块写且恒非负；这是防调用方直接改坏字段的兜底
    // 满了：只保证结尾有 NUL，绝不再写一个字节。
    if (s->len >= s->cap - 1) {
        s->p[s->cap - 1] = 0;
        return;
    }

    char* dst = s->p + s->len;
    int   room = s->cap - s->len; // 含 NUL 的可用字节数，恒 >= 2

    va_list ap;
    va_start(ap, fmt);
    // 工具链陷阱：MSVC 的 _vsnprintf 在被截断时返回负数，
    //   不是 ISO C 那种「本想写的长度」。同一条语句在两种约定下含义完全相反：
    //     · ISO C：n >= room 表示截断（拿 n 当偏移 ⇒ 无符号下溢 ⇒ 越界写）
    //     · MSVC ：n <  0   表示截断（若照 ISO 约定去比较，会把整段内容丢掉）
    //   两种都必须处理，否则换编译器/换约定就静默写坏缓冲。
    int n = _vsnprintf(dst, (size_t)room, fmt, ap);
    va_end(ap);

    if (n < 0) {
        // MSVC 约定：负数 = 被截断（也可能是编码错误）。
        // 此刻缓冲可能已被填满且没有 NUL，绝不能直接 strlen。
        // 先自己补一个 NUL，strlen 就一定被 room-1 兜住，不会读出界。
        dst[room - 1] = 0;
        int w = (int)strlen(dst);
        n = (w > room - 1) ? (room - 1) : w;
    }
    else if (n >= room) {
        // ISO C 约定：n 是本想写的长度，被截断时 >= room。
        // 只承认真正落盘的 room-1 个字节。
        n = room - 1;
    }

    s->len += n;
    s->p[s->len] = 0;
}

int  str_len(const Str* s) { return (s && s->cap > 0 && s->len > 0) ? s->len : 0; }
bool str_full(const Str* s) { return (!s || s->cap <= 0 || s->len >= s->cap - 1); }

} // namespace core
} // namespace ac1
