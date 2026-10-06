// modules/core/src/host.cpp
#include "ac1/core/host.h"

#include "ac1/core/log.h"

#include <windows.h>
#include <string.h>
#include <stdio.h>

namespace ac1 {
namespace core {
namespace {

// ---- 宿主表：exe 名 → (口味, 基准 CRC) ----------------------------------
// 加一门宿主 = 加一行，不必改判定逻辑。
//
//   加行之前必须把那一行的 exe 名与基准 CRC 实测出来，别照抄别的口味。
//     基准 CRC 是本项目该宿主所有 RVA 的依据；填错会打「不一致 ⚠」，
//     而真正的兜底是每条挂点安装前的逐字节签名核对（不是这个 CRC）。
struct HostDef {
    const char*   exe; // 主模块文件名（大小写不敏感）
    HostFlavor    flavor;
    unsigned long crc; // 该宿主版本的基准文件 CRC32
};
static const HostDef kHosts[] = {
    { "AssassinsCreed_Dx9.exe", HOST_FLAVOR_DX9, 0xE8936C99ul },
    // Dx10 宿主（exe 24,183,432 B，CRC32=3AF8F9D0）。各 HookSpec 的 rva10
    // 已按 Dx10 实测地址填齐（漏斗 + 字形三条）。
    { "AssassinsCreed_Dx10.exe", HOST_FLAVOR_DX10, 0x3AF8F9D0ul },
};
static const int kHostCount = (int)(sizeof(kHosts) / sizeof(kHosts[0]));

const char* flavor_name(HostFlavor f)
{
    switch (f) {
    case HOST_FLAVOR_DX9: return "Dx9";
    case HOST_FLAVOR_DX10: return "Dx10";
    default: return "未知";
    }
}

char          g_exe[MAX_PATH] = "";
char          g_name[64] = "";
unsigned      g_base = 0;
unsigned long g_size = 0;
unsigned long g_crc = 0;
unsigned long g_crcRef = 0; // 来自宿主表那一行
HostFlavor    g_flavor = HOST_FLAVOR_NONE;
char          g_detail[320] = "";
int           g_checked = 0;

unsigned long crc32_file(const char* path, unsigned long* outSize)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER li;
    unsigned long total = 0;
    if (GetFileSizeEx(h, &li) && li.QuadPart > 0 && li.QuadPart <= (LONGLONG)(64 * 1024 * 1024))
        total = (unsigned long)li.QuadPart;
    if (outSize) *outSize = total;
    if (total == 0) {
        CloseHandle(h);
        return 0;
    }

    static unsigned char buf[65536];
    unsigned long        crc = 0xFFFFFFFFul;
    unsigned long        left = total;
    while (left) {
        DWORD want = (left < sizeof(buf)) ? (DWORD)left : (DWORD)sizeof(buf);
        DWORD got = 0;
        if (!ReadFile(h, buf, want, &got, NULL) || got == 0) {
            CloseHandle(h);
            return 0;
        }
        for (DWORD i = 0; i < got; i++) {
            crc ^= buf[i];
            for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320ul & (unsigned long)(-(long)(crc & 1)));
        }
        left -= got;
    }
    CloseHandle(h);
    return (crc ^ 0xFFFFFFFFul);
}

} // namespace

bool host_check()
{
    if (g_checked) return (g_flavor != HOST_FLAVOR_NONE);

    HMODULE hm = GetModuleHandleA(NULL);
    g_base = (unsigned)(uintptr_t)hm;
    // 返回值 == nSize 是「被截断」，不是成功（与 app 的 find_self 同一判据）。截断出来的
    //   路径不在宿主表里，靠名字比对落空虽然也是 fail-safe，但那是巧合不是判据。
    const DWORD n = GetModuleFileNameA(hm, g_exe, MAX_PATH);
    if (!n || n >= MAX_PATH) g_exe[0] = 0;
    const char* slash = strrchr(g_exe, '\\');
    lstrcpynA(g_name, slash ? slash + 1 : g_exe, sizeof(g_name));

    // ---- 按表查口味与基准 CRC ----
    g_flavor = HOST_FLAVOR_NONE;
    g_crcRef = 0;
    for (int i = 0; i < kHostCount; i++) {
        if (_stricmp(g_name, kHosts[i].exe) != 0) continue;
        g_flavor = kHosts[i].flavor;
        g_crcRef = kHosts[i].crc;
        break;
    }

    g_size = 0;
    g_crc = crc32_file(g_exe, &g_size);

    // 输出形状一个字没动（README §9 的验收判据逐字比对这一行：
    //   `宿主 ✓ ... CRC32=E8936C99 一致 ✓ ... 口味=Dx9`）。变的只有数据来源。
    _snprintf(g_detail, sizeof(g_detail) - 1,
              "exe=%s 基址=0x%08X 大小=%lu 字节 CRC32=%08lX（基准 %08lX %s） 口味=%s", g_name, g_base, g_size,
              g_crc, g_crcRef,
              // 表里没有这一行 ⇒ 没有基准可言，如实说「未知」而不是拿别的口味的基准去比
              (g_crcRef && g_crc == g_crcRef) ? "一致 ✓"
              : (g_crcRef && g_crc)           ? "不一致 ⚠"
                                              : "未知",
              flavor_name(g_flavor));
    g_detail[sizeof(g_detail) - 1] = 0;

    g_checked = 1;
    // 只有表里有的宿主才算数。返回 false 时装配层直接结束、什么都不装。
    return (g_flavor != HOST_FLAVOR_NONE);
}

HostFlavor    host_flavor() { return g_flavor; }
unsigned      host_base() { return g_base; }
unsigned long host_file_size() { return g_size; }
unsigned long host_crc32() { return g_crc; }
const char*   host_exe() { return g_exe; }
const char*   host_name() { return g_name; }
const char*   host_detail() { return g_detail; }

// 当前宿主的基准 CRC（来自宿主表那一行；未识别宿主为 0）。
// 与 host_crc32()（本机实测值）配对使用：两者相等才说明这份宿主是本项目依据的版本。
unsigned long host_crc_reference() { return g_crcRef; }

// 表里登记了几门宿主 / 当前命中了哪一行（单测与诊断用）。
int         host_table_size() { return kHostCount; }
const char* host_flavor_name() { return flavor_name(g_flavor); }

} // namespace core
} // namespace ac1
