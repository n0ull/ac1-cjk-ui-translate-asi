#pragma once

// modules/core —— 宿主守卫
//
// 本插件支持《刺客信条1》的 Dx9 与 Dx10 主程序（host.cpp 里的宿主表，
// 一行一门：exe 名 + 基准 CRC32）。判定：文件名必须在表里；基址/大小/CRC 由
// host_detail() 一并产出，**是否落日志由调用方决定**（本模块不写任何日志）。
// 不在表里的宿主：host_check() 返回 false，一个钩都不装。
//
// core 不依赖任何其它模块。

namespace ac1 {
namespace core {

// 宿主口味（决定挂点表取哪一列 RVA / 哪一栏签名）
enum HostFlavor {
    HOST_FLAVOR_DX9 = 0,
    HOST_FLAVOR_DX10 = 1,
    HOST_FLAVOR_NONE = 2 // 不是本插件支持的宿主
};

// 真正的守卫。false ⇒ 调用方到此为止（写一行日志就收工）。
bool host_check();

HostFlavor    host_flavor();
unsigned      host_base();      // 主模块基址（正常 0x400000）
unsigned long host_file_size(); // 主 exe 文件字节数
unsigned long host_crc32();     // 主 exe 文件 CRC32；0 = 算不出来
const char*   host_exe();       // 完整路径
const char*   host_name();      // 文件名（不含目录）
const char*   host_detail();    // 一行摘要：基址/大小/CRC/口味

// ---- 宿主表 ----
// 一门宿主 = 表里一行（exe 名 → 口味 + 基准 CRC）。加一门宿主只需加一行，
// 但 exe 名与基准 CRC 必须实测，不许照抄别的口味。

// 命中行的基准 CRC（未识别宿主 = 0）。与 host_crc32()（本机实测值）配对使用：
// 两者相等才说明这份宿主是本项目依据的版本。注意 host_check() 对 CRC 只记不卡
//（不一致照样通过——各挂点另有逐字节签名核对兜底）；没有签名核对的判据必须先自己
// 过这道闸（地址是版本绑定的）。
unsigned long host_crc_reference();
int           host_table_size();  // 表里登记了几门宿主
const char*   host_flavor_name(); // 当前口味的可读名（Dx9 / Dx10 / 未知）

} // namespace core
} // namespace ac1
