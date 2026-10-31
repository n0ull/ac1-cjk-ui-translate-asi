# 安装检查

`runtime_install.py inspect`只读检查候选与游戏目录，发布检查报告。
只需 Python 3.11+ 标准库，不查询进程、不启动游戏、不改变安装。

```powershell
python -X utf8 tools/modules/runtime_install.py inspect --candidate '<candidate-directory>' --game '<game-directory>' --out '<new-inspection-directory>'
```

先完整核验候选，再检查其宿主列表。至少一门宿主须存在；已存在但 CRC 不符的宿主阻止部署。
交付配对固定为 `scripts/ac1_runtime_package.asi` 和 `scripts/AC1_Language.bin`。

| 状态 | 含义 |
| --- | --- |
| `MATCHES` | 两文件与指定候选一致 |
| `ABSENT` | 两文件都不存在 |
| `PARTIAL` | 仅一文件存在，拒绝计划 |
| `DIFFERENT` | 完整配对与候选不同，允许按实际摘要备份升级；不据此判损坏 |

按 Windows 大小写规则匹配路径，大小写歧义拒绝。输入根路径的链接/junction 拒绝；
扫描中的重解析入口列为未检查，不能认为检查完整。

## 项目冲突与第三方文件

扫描根目录 ASI、`scripts/`根文件及各级目录 ASI。`.asi`为启用文件名，
`.asi.off / .asi.disabled / .asi.bak`为停用文件名；不证明实际加载或兼容性。
按文件名前缀、摘要和运行时入口标记识别项目文件。
额外启用的项目ASI或冲突数据会阻止计划，报告列出具体路径；停用副本单列观察。

未知插件、根配置和字幕文件记录名字及摘要。嵌套非 ASI 素材、dump、日志、配置不读内容，
因此不是整个第三方目录的完整快照。游戏资源和 loader 只记录文件摘要：
宿主 CRC 不证明四字体资源身份，`dinput8.dll`文件名也不证明同步 InitializeASI 能力。

## 与前次检查比较

```powershell
python -X utf8 tools/modules/runtime_install.py inspect --candidate '<candidate-directory>' --game '<game-directory>' --baseline '<previous-inspection-directory>/inspection.json' --out '<new-inspection-directory>'
```

`--baseline`接受同一游戏目录的一次检查报告。第三方新增、缺失、修改与唯一同内容改名分开记录；
同摘要字幕插件的启停改名不影响项目配对结论。多份同摘要造成歧义时不猜测改名对应。
比较只读文件快照，保留输入报告。

输出 `inspection.json / inspection.md`；输出目录须不存在，且不能位于游戏、候选或基准报告目录内。
退出码 0 表示检查范围无问题，1 表示已生成需关注报告，2 表示输入、读取或发布拒绝。
逐文件快照不是整目录原子快照，也不能代替写入口的进程和独占句柄守卫。
后续操作见[部署与恢复](INSTALL_DEPLOY.md)。
