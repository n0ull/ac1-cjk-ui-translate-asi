# 项目与第三方来源

## 项目代码

本项目代码、脚本及原创文档采用 [MIT](LICENSE)。工程与工具整理使用了 OpenAI Codex 辅助。

## MinHook

运行时链接 [TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook)，源仓库以子模块固定在提交
`8af6b4a`所对应的版本。完整许可和 HDE 声明见 `third_party/minhook/LICENSE.txt`。
使用 Git 获取源码需初始化子模块；源码导出可包含同一提交的文件和许可。

## 字体

源字体由使用者提供。中文和日文配方分别校验指定 Noto Sans SC / Noto Sans JP 文件的摘要。
字体不包含在源码仓库或安装目录；生成候选会附带所用字体的元数据和内嵌许可说明。
具体字体来源和许可须随原始文件保留，不能用本项目 MIT 许可替代。

## 游戏资源

游戏 EXE、forge、原 MFT/纹理和完整中文词典不随公开源码提供。
本地语言包保留使用者提供的原字体记录与纹理；这类生成包与源码交付是不同材料。
仓库中的少量界面文字用于说明数据格式与测试路径。Assassin's Creed 及游戏资源属于其原权利人；
本项目与 Ubisoft 无隶属关系。
