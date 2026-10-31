# 部署与恢复

所有安装检查和事务实现都在 `runtime_install.py`，固定管理 ASI 与 `AC1_Language.bin`两文件。
候选由`runtime_candidate.py build`生成；部署和回退消费本工具保存的计划与收据。

```powershell
python -X utf8 tools/modules/runtime_install.py plan --candidate '<candidate-directory>' --game '<game-directory>' --out '<new-plan>'
python -X utf8 tools/modules/runtime_install.py install --plan '<new-plan>' --out '<new-deployment>'
python -X utf8 tools/modules/runtime_install.py verify --receipt '<new-deployment>' --out '<new-verification>'
python -X utf8 tools/modules/runtime_install.py rollback --receipt '<new-deployment>' --out '<new-rollback>'
```

每个输出必须是新目录，且不能包含输入或位于游戏、候选、计划、源收据内。
计划、部署、核验和回退各自产生记录，不回写候选与先前收据。

## 计划与提交

`plan`只读核验候选、宿主、归档、loader 和项目冲突。允许完整覆盖升级、相同配对或首次安装；
部分安装、冲突数据和额外启用的项目ASI拒绝。计划绑定候选 manifest、已存在宿主摘要、
DataPC.forge、dinput8.dll 和两目标的原摘要/缺失状态。部署重新核对绑定，过期计划拒绝。

写操作需可信 Windows 进程视图：CIM 查询发现游戏运行或查询失败即停止。
随后持有全部已存在 Dx9/Dx10 宿主文件的独占句柄，采用 Open/ReadWrite/FileShare.None，
不创建、截断或写入宿主；锁后核验计划中的宿主身份，每步确认守卫仍在。
没有跳过守卫的 CLI 开关。受限进程视图中拒绝写操作。

替换前，复制原配对到 `backup/`并核验；在游戏卷内暂存新文件并核验。
检查快照、备份与封存的 `transaction.json`先于提交；固定包在前、ASI 在后。
已相同文件保持原位。每步核对目标、最后核对完整配对；两个文件不构成一次原子提交。
语言切换复用同一事务，相同 ASI 无需重写，回退恢复上一语言。

## 结果与中断恢复

| 状态 | 含义 |
| --- | --- |
| `DEPLOYED` | 目标配对一致，未判断实机功能 |
| `FAILED_RESTORED` | 安装失败，原配对/缺失状态已恢复并核验 |
| `RECOVERY_REQUIRED` | 恢复未完成，保留事务和备份 |
| `ROLLED_BACK` | 已恢复部署前状态 |

记录先写 `.sha256`再发布 JSON，JSON 是完成点。有事务无有效结果按中断处理，不能当作已恢复。
再次启动游戏前，用该部署目录执行 `rollback`；保留原目录及备份。
回退自身中断/失败时，以回退事件目录继续恢复，换一个新输出：

```powershell
python -X utf8 tools/modules/runtime_install.py rollback --receipt '<interrupted-rollback>' --out '<new-retry>'
```

恢复不依赖源候选仍在，但要求备份完整、目标仍为事务允许的原/新状态。
用户后续改动拒绝覆盖；首次安装回退恢复缺失。未发布事务时没有可消费的正式收据。
操作系统断电不承诺两文件原子性。

`verify`独立读取安装和部署记录，只有部署结果完整为 DEPLOYED、项目与受保护输入一致、
且无链接/项目冲突才返回 VERIFIED；其它为 ATTENTION。第三方启停和配置变化单独报告。
回退后，原部署的 verify 仍比较部署后的目标，不能用它认证回退。
工具不改第三方插件、字幕开关、配置、存档和日志。

退出码 0 为成功，1 为已保存的需关注/失败结果，2 为输入、守卫或发布拒绝。
失败需核对 transaction/result，不能只凭退出码判断已恢复。

## 验证

默认临时文件回归覆盖过期计划、文件/备份篡改、恢复失败及安装/回退中断。
以下守卫集成只接触临时夹具，在可信 Windows 进程视图显式运行：

```powershell
$env:AC1_TEST_WINDOWS_GUARD = '1'
python -X utf8 -m unittest discover -s tools/modules/tests -p test_runtime_transactions.py -v
python -X utf8 -m unittest discover -s tools/modules/tests -p test_language_deployment.py -v
```

日志和人工反馈关联见[运行归档](RUN_ARCHIVE.md)；磁盘核验不代替实机测试。
