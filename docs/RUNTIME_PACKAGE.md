# 运行时资源包契约 v2

运行期只读取 ASI 相邻的 `AC1_Language.bin`。包包含一个目标语言的 UTF-8 词典及 bold、techno、textpc、title 四字体计划。
调用关系见[运行机制](ENGINEERING.md)，生成命令见[构建说明](../tools/modules/runtime/README.md)。

## 数据与预算

词典使用生产规则2，按规范键精确查询并要求重复查询稳定。必需字集为有效译文与配置中的显式附加字集，
每字体扣除原有码位；原字符和图标不覆盖。语言标识来自包头，不用于选择另一份运行时实现。

包最多64MiB，每字体原加新记录最多4096、尺寸最多8192、BC2单mip、密度2、page0。
零追加保持原尺寸和原记录。追加计划把原图像块放在新图左下，原UV按尺寸比例适配；
新记录共享字体公共基线，UV、advance、墨迹和透明保护边由独立回读核对。

所有字体和词典必须共同核验通过，才向模块公开视图。摘要用于完整性与配对，不是来源签名。

## 二进制布局

小端、无隐式对齐填充。头128B，字体目录项224B；数据段起点16B对齐，间隔填0。未知版本、非零保留字段、越界/相交段、计数或长度不一致均拒绝整个包。

### 包头：128B

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | char[8] | magic=`AC1LANG\0` |
| 8 | u32 | version=2 |
| 12 | u32 | header_bytes=128 |
| 16 | u64 | package_bytes |
| 24 | u32 | flags=0，保留 |
| 28 | u32 | font_count |
| 32 | u64 | dictionary_offset |
| 40 | u64 | dictionary_bytes |
| 48 | u64 | font_directory_offset |
| 56 | u32 | font_entry_bytes=224 |
| 60 | u32 | dictionary_rules_version=2 |
| 64 | byte[32] | package_sha256 |
| 96 | char[32] | language_id；`[a-z][a-z0-9]*(?:-[a-z0-9]+)*`、最多31B、NUL结束并补0 |

`package_sha256`计算整个文件，计算时将偏移64..95的32B视为0；包内全部内容和对齐填充都参与摘要。运行时先校验结构与总摘要，再向下游公开任何视图。运行时文件大小还须满足实现配置的上限，不能因字段为u64就允许任意分配。

### 字体目录项：224B

| 偏移 | 类型 | 字段 |
|---:|---|---|
| 0 | char[16] | name，`bold/textpc/title/techno`，NUL结尾并补0 |
| 16 | u32 | texture_asset_id |
| 20 | u32 | texture_class_hash |
| 24 / 28 | u32 / u32 | original_width / original_height |
| 32 / 36 | u32 / u32 | target_width / target_height |
| 40 / 44 | u32 / u32 | native_format / mip_count |
| 48 / 52 | u32 / u32 | original_glyph_count / appended_glyph_count |
| 56 / 60 | u32 / u32 | raster_density / reserved=0 |
| 64 / 72 | u64 / u64 | appended_records_offset / bytes |
| 80 / 88 | u64 / u64 | texture_payload_offset / bytes |
| 96 | byte[32] | 原BC2载荷SHA-256 |
| 128 | byte[32] | 原记录规范化30B串的SHA-256 |
| 160 | byte[32] | 追加32B记录串SHA-256 |
| 192 | byte[32] | 新BC2载荷SHA-256 |

目录按name字节序排列。重复名字或重复资源身份拒绝。每套追加记录段严格等于`appended_glyph_count×32`，载荷严格等于尺寸/BC2/mip规则要求；不能让错误的记录与另一张正确载荷混配。

头部和目录结构分别用`<8sIIQIIQQQII32s32s`与`<16s12I4Q32s32s32s32s`表达。
公开视图及所有权见[读取接口](RUNTIME_READER.md)。

## 追加记录与原资源身份

追加记录32字节，对应 `<HHHhhh4fHH`：码位、逻辑宽高、偏移与advance、四个UV值、page及保留字段。
码位按升序唯一，BMP内，page/保留为0，度量和有限UV须符合原图保护带与烘焙密度。
原MFT记录的规范化指纹使用原始30字节串；追加序列和新纹理各自独立摘要。

运行时适配器还核对实际资源ID/class、尺寸、记录数、原记录指纹及新旧码位冲突。
包读取器不认证特定游戏EXE；宿主和入口签名由装配层/适配器核验。

## 提交与生命周期

core持有只读包，dict复制有效映射，glyph按引擎分配与构造路径准备图集/表，text在原生测量前物化译文。
CPU图集、描述符和代次一起发布；GPU上传另行观测。原有字符采样、reset所有权与资源重建不能只靠包内摘要证明。
接口见[读取器](RUNTIME_READER.md)，运行机制见[工程说明](ENGINEERING.md)，语言与容量限制见[支持范围](fonts.md#support-limits)。
