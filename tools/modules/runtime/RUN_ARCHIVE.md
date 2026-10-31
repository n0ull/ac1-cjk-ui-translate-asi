# 运行证据归档

`runtime_archive.py`只读封存显式指定的一次运行，关联候选与可选部署收据。
不启动或控制游戏，不更改安装、词典或既有收据，只需 Python 3.11+ 标准库。

```powershell
python -X utf8 tools/modules/runtime_archive.py --candidate '<candidate-directory>' --logs '<log-directory>' --run '<YYYYMMDD_HHMMSS>' --notranslate data/languages/zh/notranslate.policy --deployment '<deployment-directory>' --feedback '<feedback.json>' --out '<new-archive>'
```

`--notranslate`必须显式提供，可为空。`--deployment`和 `--feedback`可省略。
只读对应的 `run_<run>.log / strings_<run>.txt / crash_<run>.log`，不猜最新运行，不复制其它运行或 dump。
全部缺失拒绝，部分缺失保存未知状态。路径按调用者工作目录解析；链接/junction 不跟随。
输出必须是新目录，位于候选、日志、引用输入和已识别游戏根目录之外。

## 来源与人工反馈

正式部署引用核对事务封存、检查快照、结果及候选 manifest/宿主/交付摘要。
不同候选直接拒绝；缺结果保留 INTERRUPTED 并提示关注。不读取引用中的游戏目录或恢复备份。
省略引用时为NOT_PROVIDED / NOT_PROVEN。

人工反馈是 UTF-8 JSON，保留原文和声明的来源，仅选择对应运行：

```json
{
  "format_version": 1,
  "kind": "AC1_RUNTIME_USER_FEEDBACK",
  "observations": [
    {"run": "20260101_120000", "text": "运行正常。", "source": "该次运行的测试者反馈"}
  ]
}
```

提供的文件没有对应运行时拒绝；省略则保持未提供。整体反馈不推导逐页、图标或资源重建验收。

## 快照与日志

三份日志与引用作为一组复制，检查文件身份、大小、mtime、内容摘要后重新核对源文件。
变化时最多整组重试3次；持续变化则保存最后捕获字节和每次状态，标为 INCOMPLETE_SNAPSHOT，
停止日志结论、反馈关联和未译审计。读取失败或候选改变拒绝发布。STABLE 只说明读取窗口稳定。

ERR为错误等级，FUNNEL 使用同值计数，资源状态须完整包含
`font_active / font_peak / font_limit / font_full`。未识别行、缺字段、重复字段或损坏末行提示关注，
不按零或无错误处理。峰值/占用/上限不自洽也拒绝该状态行。

每轮只允许 hosts 列表内的一门宿主，同时核对语言、包内摘要、包/规则版本及 ASI SHA-256。
缺失、混合、重复或不匹配均提示关注；REPORTED_MATCHES 是启动自报告匹配，非内存认证。
保留全部非零资源/文本/容量错误样本，之后归零不会清除早期错误。
非空 crash、库存丢弃、非法编码或身份混合单独保存；库存单一运行头、PID 和时间窗口可信才进入文本审计。

日志无结束标记；日志跨度不是活跃游玩时长，上传计数不是 GPU 抓帧。
完整页面覆盖、第三方实际加载及设备重建需要单独测试。

## 输出

| 文件 | 内容 |
| --- | --- |
| `archive.json / archive.md` | 来源、快照、计数、错误、反馈和解释范围 |
| `logs/` | 本次捕获原字节 |
| `references/` | 不译政策、反馈及部署引用快照 |
| `text-audit/audit.json / audit.md` | 对照候选有效词典的待核实文本；输入不可信时不可用 |
| `SHA256SUMS` | 除自身外全部输出摘要 |

ARCHIVED 仅表示本次解析未列出问题；ARCHIVED_WITH_ATTENTION 保留需关注证据；
INCOMPLETE_SNAPSHOT 表示读取持续变化。native 始终为 NOT_CERTIFIED_BY_TOOL。
退出码依次为 0、1、1；输入或发布拒绝为 2。已有输出不覆盖，不完整归档也需换新目录重跑。
文本审计的 48 码元、前缀合并和首次刷盘限制见[构建与审计](README.md#runtime-text-audit)。
