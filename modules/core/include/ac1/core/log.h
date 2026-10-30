#pragma once

// modules/core —— 日志体系（等级 / 分类 / 单次 run 文件）
//
// 文件布局：全部产出进 AC1_CJK\，文件名带 run 启动时间戳——
//   <asi>\AC1_CJK\run_YYYYMMDD_HHMMSS.log        本 run 文本日志
//   <asi>\AC1_CJK\strings_YYYYMMDD_HHMMSS.txt    字符串全记录（inventory.cpp 写）
//   <asi>\AC1_CJK\crash_YYYYMMDD_HHMMSS.log      崩溃转储（trace.cpp 的 VEH 写）
// 每 run 天然一个新文件，旧文件永不被触碰。
//
// 行格式：`[YYYY-MM-DD HH:MM:SS][LVL][原前缀] 消息`
//   等级：INFO（默认）/ WARN（降级运行，显式调 log_warn）/
//         ERR（错误，显式调 log_error）。等级由调用方选择，正文不参与判断。
//   分类 = 消息里的中文前缀（[启动]/[钩]/[字形]/[词典]/[FUNNEL]/[状态]/[迹]/
//   [崩溃] 等），grep 用中文前缀即可。
//
// 写文件全程走 Win32 API ⇒ DllMain 的 loader lock 下也安全；
// 格式化用 CRT 的 _vsnprintf（截断语义见 core/str.h 的工具链陷阱）⇒ 只允许在
// 加载器锁之外调用（worker 或 InitializeASI）。
//
// core 不依赖任何其它模块（只用 Win32 + CRT）。

namespace ac1 {
namespace core {

// dir：ASI 所在目录，带尾反斜杠。传空串 = 彻底关闭日志（离线单测用，不落文件）。
// 非空时：建 AC1_CJK\ 子目录，按当前时间生成 run stamp，打开 run_<stamp>.log
// （同秒重启撞名则追加——仍是同一 run 的语义）。
void init_log(const char* dir);
void close_log();

// 本 run 的启动时间戳（"YYYYMMDD_HHMMSS"；init_log("") 时为空串）。
// inventory（strings 文件）与 trace（crash 文件）用它给产出文件命名。
const char* log_run_stamp();

// 写一行 INFO（自动补日期/等级时间戳与 CRLF）。
// 内部有临界区，多线程安全；打不开文件时静默丢弃并计数，绝不影响游戏。
//
// dict 分别注入 log_line/log_error，保持格式化消息与等级选择分离。
void log_line(const char* fmt, ...);

// 写一行 WARN（降级运行：资源未就绪、词典未就绪、补丁被拒/拉黑）。
void log_warn(const char* fmt, ...);

// 写一行 ERR（启动、装钩、资源或文本处理失败）。
void log_error(const char* fmt, ...);

// 本 run 文本日志的完整路径（init_log("") 或打开失败时是空串）
const char* log_path();

// 日志被打不开/被丢弃的次数
unsigned long log_dropped();

} // namespace core
} // namespace ac1
