// modules/core/src/file.cpp
// 文件读取与路径拼接。整文件读取按调用方给的上限判定成功/失败：超上限算失败而
//   不是截断（被悄悄截断的词典/清单会解析出半份数据而没人察觉）。
// 路径拼接带容量自检：装不下返回 0 并把输出置空，不产出半截路径。

#include "ac1/core/file.h"

namespace ac1 {
namespace core {

bool file_read_all(const char* path, unsigned char** out, int* outN, int maxBytes)
{
    if (out) *out = NULL;
    if (outN) *outN = 0;
    if (!path || !path[0] || !out || !outN) return false;

    const long long cap = (maxBytes > 0) ? (long long)maxBytes : (long long)FILE_READ_MAX_DEFAULT;

    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0 || li.QuadPart > cap) {
        // 超上限按失败处理，不截断。理由：调用方读的是自己生成的数据
        //   （词典 / 字形清单 / 图集），被悄悄截断的文件会解析出半份数据而没人察觉。
        CloseHandle(h);
        return false;
    }
    const int n = (int)li.QuadPart; // 已确认 0 < n <= cap <= INT_MAX 量级

    unsigned char* d = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n);
    if (!d) {
        CloseHandle(h);
        return false;
    }

    DWORD got = 0;
    if (!ReadFile(h, d, (DWORD)n, &got, NULL) || (int)got != n) {
        HeapFree(GetProcessHeap(), 0, d);
        CloseHandle(h);
        return false;
    }
    CloseHandle(h);

    *out = d;
    *outN = n;
    return true;
}

void file_free(void* p)
{
    if (p) HeapFree(GetProcessHeap(), 0, p);
}

bool file_exists(const char* path)
{
    if (!path || !path[0]) return false;
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

bool path_join(char* out, int cap, const char* dir, const char* name)
{
    if (!out || cap <= 0) return false;
    out[0] = 0;
    if (!dir || !name) return false;

    // 容量自检放在前面：目录本身装不下就别开始拼
    //   （否则长目录会安静地产生一个不存在的路径，日志里与「文件缺失」无法区分）。
    const size_t dl = lstrlenA(dir);
    const size_t nl = lstrlenA(name);
    const size_t need = dl + nl + 2; // 可能补一个 '\\' + NUL
    if ((int)need > cap) return false;

    lstrcpynA(out, dir, cap); // lstrcpynA 的 n 含 NUL
    size_t o = lstrlenA(out);
    if (o && out[o - 1] != '\\' && out[o - 1] != '/') {
        if (o + 2 >= (size_t)cap) {
            out[0] = 0; // 失败即置空，不留半截路径（这条路上 out 已被写入 dir）
            return false;
        }
        out[o++] = '\\';
        out[o] = 0;
    }
    lstrcpynA(out + o, name, (int)((size_t)cap - o));
    return true;
}

} // namespace core
} // namespace ac1
