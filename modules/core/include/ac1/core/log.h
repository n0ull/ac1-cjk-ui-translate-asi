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
//         ERR（**渲染后的消息体**里含 "!!" 标记 ⇒ 自动归类；标记位置不限，
//              "!! …" 与 "[词典] !! …"、以及经 set_log 注入的预格式化消息体都算）。
//   ⚠ "!!" 是保留标记：正文里不要拿它当标点，否则那一行会被提级成 ERR。
//   分类 = 消息里的中文前缀（[启动]/[钩]/[字形]/[词典]/[FUNNEL]/[状态]/[迹]/
//   [崩溃] 等），grep 用中文前缀即可。
//
// 写文件全程走 Win32 API ⇒ DllMain 的 loader lock 下也安全；
// 格式化用 CRT 的 _vsnprintf（截断语义见 core/str.h 的工具链陷阱）⇒ 只允许在
// worker 线程启动后调用。
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

// 写一行 INFO（自动补日期/等级时间戳与 CRLF；消息体含 "!!" 标记则提级成 ERR）。
// 内部有临界区，多线程安全；打不开文件时静默丢弃并计数，绝不影响游戏。
//
// 这个函数同时是其它模块的日志出口：dict 通过 set_log 注入它来保持互不依赖。
// 提级判据看的是渲染后的消息体 ⇒ 注入方在 buf 里带 "!!" 同样成为 ERR 行。
void log_line(const char* fmt, ...);

// 写一行 WARN（降级运行：forge 闸门关、词典未就绪、补丁被拒/拉黑）。
void log_warn(const char* fmt, ...);

// 本 run 文本日志的完整路径（init_log("") 或打开失败时是空串）
const char* log_path();

// 日志被打不开/被丢弃的次数
unsigned long log_dropped();

} // namespace core
} // namespace ac1
