// modules/glyph/src/forgecheck.cpp —— forge 补丁核验（启动期文件核验）
//
// 为什么存在：forge 路线下，我们的图集直接打进 DataPC.forge，运行时不再
//   换纹理。但字形补丁（插表 + charmap 页 + UV 缩放）只对「我们的图集在场」
//   成立。失同步场景——用户 Steam「验证文件完整性」还原了 forge、或把
//   scripts\ 拷到没打补丁的机器——若无闸门，UV 缩放会把原版文字搞花。
//   本文件给出整套补丁的总闸：sidecar 说补丁在、forge 尺寸对得上 ⇒ 开；
//   否则 ⇒ 关，原版游戏零影响。
//
// sidecar = <asiDir>AC1_CJK_Forge.txt（tools/write_forge_sidecar.py 产出）：
//   文本，# 为注释；数据行 `DataPC.forge <尺寸> <crc32-hex> <flags>`。
//   flags&1 ⇒ 除尺寸外再核 CRC32（多读 ~200MB，启动慢；默认只核尺寸——
//   三种已知状态的尺寸互不相同，见工具注释，尺寸 alone 已够用）。
//
// 只依赖 core 的日志与文件小读；不 include 任何别的模块。
#include "ac1/glyph/glyph.h"
#include "ac1/core/log.h"
#include "ac1/core/file.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace ac1 {
namespace glyph {

static int g_forgePatch = 0; // 闸门：0=关（安全默认），1=开

void set_forge_patch(int present) { g_forgePatch = present ? 1 : 0; }

int forge_patch_present() { return g_forgePatch; }

// ---- CRC32（zlib 多项式；仅 flags&1 时用）----
static unsigned crc32_update(unsigned crc, const unsigned char* buf, unsigned n)
{
    static unsigned tab[256];
    static int      tabReady = 0;
    if (!tabReady) {
        for (unsigned i = 0; i < 256; i++) {
            unsigned c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tab[i] = c;
        }
        tabReady = 1;
    }
    crc = ~crc;
    for (unsigned i = 0; i < n; i++) crc = tab[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// 流式算文件的 CRC32；失败返回 0 且 *ok=0（0 本身是合法 CRC，必须靠 ok 区分）。
static unsigned file_crc32(const char* path, int* ok)
{
    *ok = 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    unsigned      crc = 0;
    unsigned char buf[1 << 16];
    DWORD         got = 0;
    for (;;) {
        if (!ReadFile(h, buf, (DWORD)sizeof(buf), &got, NULL)) {
            CloseHandle(h);
            return 0;
        }
        if (got == 0) break;
        crc = crc32_update(crc, buf, got);
    }
    CloseHandle(h);
    *ok = 1;
    return crc;
}

int check_forge_patch(const char* asiDir)
{
    char sidecar[MAX_PATH], forge[MAX_PATH];
    // 两条路径都走带容量自检的 path_join：装不下就直接判「路径过长」（闸门关），不产出
    //   半截路径 —— 半截路径会让日志把排障引向「重跑 repack 工具」这种无效方向。
    if (!core::path_join(sidecar, (int)sizeof(sidecar), asiDir, "AC1_CJK_Forge.txt") ||
        !core::path_join(forge, (int)sizeof(forge), asiDir, "..\\DataPC.forge")) {
        core::log_warn("[闸门] forge 核验：路径过长，装不进 %d 字节 ⇒ **字形补丁停用**",
                       (int)sizeof(sidecar));
        set_forge_patch(0);
        return 0;
    }

    // ---- 读 sidecar（小文件；缺文件 = 未经安装步骤 ⇒ 闸门关，属正常状态）----
    unsigned char* text = NULL;
    int            textLen = 0;
    if (!core::file_read_all(sidecar, &text, &textLen, 64 * 1024)) {
        core::log_warn("[闸门] forge 核验：sidecar 缺失或不可读（%s）⇒ **字形补丁停用**"
                       "（原版不受影响；repack 后请运行 tools\\write_forge_sidecar.py）",
                       sidecar);
        set_forge_patch(0);
        return 0;
    }

    // 找第一条非注释数据行：DataPC.forge <size> <crc-hex> <flags>
    // （缓冲没有 NUL 保证：逐行拷进本地定长缓冲再 sscanf）
    unsigned long expectSize = 0, expectCrc = 0;
    int           flags = 0, found = 0;
    {
        const char* p = (const char*)text;
        const char* end = p + textLen;
        while (p < end && !found) {
            const char* nl = p;
            while (nl < end && *nl != '\n' && *nl != '\r') nl++;
            char     line[256];
            unsigned l = (unsigned)(nl - p);
            if (l >= sizeof(line)) l = (unsigned)sizeof(line) - 1;
            memcpy(line, p, l);
            line[l] = 0;
            p = (nl < end) ? nl + 1 : end;
            if (line[0] == '#' || line[0] == 0) continue;
            char               name[64] = "";
            unsigned long long sz = 0;
            unsigned int       crc = 0, fl = 0;
            if (sscanf(line, "%63s %llu %x %u", name, &sz, &crc, &fl) == 4 &&
                lstrcmpiA(name, "DataPC.forge") == 0) {
                expectSize = (unsigned long)sz;
                expectCrc = (unsigned long)crc;
                flags = (int)fl;
                found = 1;
            }
        }
    }
    core::file_free(text);

    if (!found) {
        core::log_warn("[闸门] forge 核验：sidecar 无数据行 ⇒ **字形补丁停用**");
        set_forge_patch(0);
        return 0;
    }

    // ---- 尺寸比对（主判据）----
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(forge, GetFileExInfoStandard, &fad)) {
        core::log_warn("[闸门] forge 核验：读不到 %s ⇒ **字形补丁停用**", forge);
        set_forge_patch(0);
        return 0;
    }
    const unsigned long long actualSize = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;

    if (actualSize != (unsigned long long)expectSize) {
        core::log_warn("[闸门] forge 核验：尺寸不符（sidecar=%lu 实际=%llu）⇒ **字形补丁停用**"
                       "（forge 已被还原/替换；Steam 验证完整性是最常见原因）",
                       expectSize, actualSize);
        set_forge_patch(0);
        return 0;
    }

    // ---- CRC32（可选强化，flags&1）----
    if (flags & 1) {
        int                 ok = 0;
        const unsigned long actualCrc = file_crc32(forge, &ok);
        if (!ok || actualCrc != expectCrc) {
            core::log_warn("[闸门] forge 核验：CRC32 不符（sidecar=%08lX 实际=%08lX ok=%d）"
                           "⇒ **字形补丁停用**",
                           expectCrc, actualCrc, ok);
            set_forge_patch(0);
            return 0;
        }
        core::log_line("[闸门] forge 核验 ✓：尺寸=%lu CRC32=%08lX（含 CRC 复核）⇒ **字形补丁启用**",
                       expectSize, expectCrc);
    }
    else {
        core::log_line("[闸门] forge 核验 ✓：尺寸=%lu（flags=0 免 CRC）⇒ **字形补丁启用**", expectSize);
    }
    set_forge_patch(1);
    return 1;
}

} // namespace glyph
} // namespace ac1
