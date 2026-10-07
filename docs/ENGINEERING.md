# 工程笔记（开发者向）

《刺客信条1》汉化 ASI 插件。构建产物 **`app\out\ac1_cjk.asi`**。
宿主：`AssassinsCreed_Dx9.exe`（CRC32 `E8936C99`）与 `AssassinsCreed_Dx10.exe`
（CRC32 `3AF8F9D0`），imagebase 均 `0x400000`；下文 Dx9 侧地址给 VA，
Dx10 侧只给 RVA（两 exe 的 RVA = VA − 0x400000）。

阅读顺序：**§1 原版 UI 数据流 → §2 我方钩位（按层） → §3 纹理方案两条路线**
（含已否决的运行时替换）；§4 起是模块/构建/部署/验收参考。

文中 RVA、偏移、调用约定全部有反汇编实证；实证过程与原始记录不随本发布树
分发（留在开发环境的逆向证据库），本文只留结论与逐处出注。标注 [推断] 的
是未钉死的项。

---

## 1. 原版 UI 数据流：字是怎么上屏的

未打补丁时，一段 UI 文本从磁盘到屏幕的完整链路（▲ = 我们的闸门，★ = 我们的
钩，详见 §2；层号沿用证据库的分层口径，L1 资源对象 / L3 文本对象是中间
子层、图中从略）：

```
L0 资源  DataPC.forge ──(LZO 容器)──► 2_-_Game Bootstrap Settings.data
           ├─ MGB 菜单/布局（菜单串 [u32 len][UTF-16LE][4B 尾] 记录流）
           ├─ 字符串表资产（StringTable / StringResource）
           └─ 字体资产：MFT 265–268（度量）+ tga32_Map 735–738（图集纹理）
         ▲ 启动闸：forge 尺寸/CRC 核验（整套补丁的总闸，§2.5）

L2 文本  流式记录 / MGB 属性值
           ──► ★漏斗 sub_8A1920(dst, src)（拷进临时 wstring）
           ──► assign(最终目标, dst, 0, -1)（按 _Mysize 真实长度搬运）
           ──► 控件内联 wstring(+0x14/+0x10) 或 StringResource+8

L4 字形  MGB 属性 FONTS/FONTFAMILY ──► 拼路径 "%sFonts%s%s.mft"
           ──► 字体 vtable 槽7 ★0x8840B0 加载 .mft
           ──► 字形记录表(font+0x38) + charmap(font+0x3C) + 页 vector(font+0x448)
         slot12 0x7993B0 解析第 0 页 ──► font+0x458 当前材质句柄（引用计数）

L4 排版  控件每帧取串 ──► 排版收口 0x8D4DB0 ──► ★DrawText 0x799720
           每码元 ──► ★charmap 0x8965D0 查表 ──► 字形记录（度量 + UV）
           ──► 四边形 20B 顶点 {x,y,z,u,v=1−v} ──► 全局绘制记录队列

L5 提交  帧末 0x786180 遍历记录：绑材质（rec+0x5C → 槽位索引 → 槽对象；
           槽缓存 miss 才在 sub_AA9980 惰性 CreateTexture = 设备槽 23）
           ──► SetTexture（设备槽 65）──► DrawPrimitive（设备槽 81）──► 屏幕
```

### 1.1 资源层：forge → 资产对象

- `DataPC*.forge` = LZO 压缩容器 + 索引。UI 资产集中在 `DataPC.forge` 的
  `2_-_Game Bootstrap Settings.data`（MGB、字符串表、四套字体资产全在这里）；
  主菜单/暂停菜单等在 `DataPC_Map_Menu.forge` 的 `1_-_Map_Menu.data`。
- **四套字体资产**（导出集是子集，「全游戏只有 4 套」不能断言）：

| 套 | tga32_Map（图集纹理） | MFT（度量/定义） | 原图集 | 字形数 | 用途（实机归纳） |
|---|---|---|---|---|---|
| techno | 735 | 265 | 64×1024 | 198 | HUD/数字；字体每轮加载但图集从未被请求 ⇒ 未用于绘制 |
| bold | 736 | 266 | 256×512 | 199 | 正文（主菜单实测用它） |
| title | 737 | 267 | 512×1024 | 261 | 标题 |
| textpc | 738 | 268 | 256×256 | 199 | 普通 UI 文本/设置页 |

- **735–738 只是 forge 打包序号，不参与运行时选择**。用哪套字体由 MGB 数据
  驱动：`sub_8C96D0` 读属性 `FONTS/FONTFAMILY/…` → 拼 `"%sFonts%s%s.mft"`
  （模板 @0x16AE298）→ 字体 vtable 槽7 加载。代码里没有任何硬编码字体名。
- `.mft` = magma 字体包（流标签 `"Magma Font"`）：记录每条 30 B（码位 u16 +
  宽高/偏移/前进宽 + 图集像素坐标）。
- `.TextureMap` 容器：+8 = 类型标签 `0xA2`，+9 = 宽 u32，+13 = 高 u32，
  +145 起 = 载荷（BC2 块流，16 B/4×4 块）。原版图集 **BC2/DXT3、RGB 纯白、
  字形蒙版在 alpha、单级 mip**。
- **资产定位 = hash 查表，身份在此被消费**：`font+0x458` → 材质链表节点
  +0xC = 资产 ID ⇒ `sub_C58B20` 在 hash 表里查它（0xC58CDD/0xC58CDF 取
  hash、0xC58CE2 比对，表 @0x198E5F8），**查到后传给下游的是表项值（槽位
  索引），不是 ID**。同一资产全局只加载一次的缓存架构，代价是资产身份在
  这一段不可恢复——这是 §3 运行时替换路线的死结一。
  实测 ID（两轮独立运行一致；AnvilToolkit File ID = 运行时值 + 3）：
  textpc `0x47195953` / bold `0x4719595C` / techno `0x4719598E` /
  title `0x87E599E9`。
- **纹理创建是惰性的**：不在加载时，而在首次绑定时（提交环槽对象缓存
  miss）才发生。`sub_AA9980` = 全程序唯一 `D3DXCheckTextureRequirements` +
  CreateTexture 收口（0xAA9A07 `mov eax,[ecx+5Ch]` / 0xAA9A0A `call eax`
  = 设备 vtable 槽 23）。请求的尺寸/格式来自字体纹理页描述符
  `src+0x28/+0x2C`，**不读 .TextureMap 头** ⇒ 数据级换尺寸走不通，
  同尺寸换内容走得通（Nexus 手柄图标 mod 先例）。

### 1.2 文本层：字符串出生 → wstring → 控件

- 文本来源**在 forge 里，不是裸文件**。两类：
  ① **MGB 菜单/布局数据**：菜单串以 `[u32 len][len×UTF-16LE][4B 尾]` 顺序
    记录流存在于块内（记录可落奇数偏移）；控件属性值也可能是
    `![CDATA[…]]` 引用（经 StringTable 取项，值不在 MGB 载荷缓冲里）。
  ② **字符串表资产**：`StringResource`（+8 内嵌 wstring，SSO 阈值 8；
    +0x24 ID），`StringTable` 只存对象指针。文本物化发生在 forge/.data
    对象池预填阶段（该 walker 的 VA 未定位 [未证实]）。
- **`LGCStringDict` 不是游戏的文件**（两个 exe 零引用）：它是 08 官方补丁
  `LG_AC.dll` 的数据（`_01` = 英文→繁中整句表 1,495 条；`_02` = Big5→GBK
  逐字转码表）。对本项目的角色 = **离线翻译素材来源**（繁→简/补译后产出
  `data/dict.txt` → 部署名 `AC1_Dict.txt`），运行期由我们自己的 dict 模块
  读取，与 LG 零运行期关系。
- **漏斗 `sub_8A1920`**：`__cdecl (void* dst, const wchar_t* src)`，语义 =
  把 src 宽串拷进临时 wstring dst。入口 7 B `6A FF 68 xx xx 55 01`
  （`push -1; push offset SEH`），尾 `83 C4 58 C3`（`add esp,58h; ret` ⇒
  调用者清栈）。24 条直接 xref = **7 条 UI 路径 + 17 条诊断**（唯一非 UI
  调用者 `sub_F8A0E0` 诊断消息格式化；词典不命中即无副作用）。
- 七条 UI 路径（全部先过漏斗、再 `assign` 进最终目标）：

| 路径 | 漏斗调用点 VA | 场景 | 最终目标 |
|---|---|---|---|
| 流式填充 STRFILL | `0x892E40` | 字符串表资产逐条出生；菜单整表遍历（COLWALK）也走这 | `StringResource+8` |
| LoadVisitor 槽 A | `0x8C3F90` | 控件树装载期读属性 `"STRING"` | `widget+0x14` |
| LoadVisitor 槽 B | `0x8C8850` | 属性 `"LABEL"` | `widget+0x10` |
| 属性式 CONTENT | `0x8C43A0` | 另一张 vtable，属性 `"CONTENT"`；实机 20 轮触发 0 = 死路径 | `目标+0x08` |
| LoadVisitor 槽 D | `0x8C5690` | 版本分派 | `widget+0x14` |
| setter(STRING) `sub_892C10` | `0x892C86` | 运行期动态改控件文本 | `widget+0x14`（内联 wstring） |
| setter(LABEL) `sub_895370` | `0x8953E7` | 运行期标签 | `widget+0x10` |

- 所有调用者形态统一：`sub_8A1920(&temp, src)` → `assign(最终目标, temp,
  0, -1)`（Dx9 `0x8759B0` / Dx10 `0xB94A70`）**按 `_Mysize` 真实长度搬运**。
- 旁路：`sub_892BD0` = 运行期 setter 直调 assign、**不经漏斗**（曾装第二
  文本钩，后摘除：覆盖面不抵成本）；`0x8D4DB0` = 排版收口（读串不写串）。
- **读者**：控件每帧经共享 slot7 重新取串（`控件+0x14` 自带 wstring，或
  `*(控件+0x44)+8` = StringResource+8）→ 排版收口 `0x8D4DB0`
  （`(wstring*, font, x, y, …, start, end)` → 字体 vtable 槽8 DrawText）。

### 1.3 字形层：字体加载 → charmap → 字形记录

- 字体对象 `PixmapFontScimitar`，**0x45C 字节**（全 .text 唯一 `push 0x45C`
  分配点）；类层次 `Font → GlyphFont → PixmapFont → PixmapFontScimitar`；
  vtable 闸门值 `0x16A2A20`（Scimitar）/ `0x16AB870`（PixmapFont）。关键字段：

| 偏移 | 类型 | 含义 |
|---|---|---|
| +0x34 | u16 | 字形数 |
| +0x38 | dword | 字形记录表基址（count×0x20 一整块） |
| +0x3C..+0x43B | dword[256] 内联 | **charmap 页指针数组** |
| +0x43C | u16 | 缺字符默认字形下标（PC 恒 0） |
| +0x448..+0x450 | ptr×3 | 页 vector（8 B/条 {资源键, 对象}；PC 上对象恒 NULL） |
| +0x454 | u8 | 页数 |
| +0x458 | dword | **当前材质句柄**（引用计数：+0x0C 就绪、+0x08 被绑对象） |

- 加载 = vtable 槽7 `0x8840B0`（`bool __thiscall`，3 栈参 `retn 0Ch`），
  内部 `0x8837F0`：`setGlyphs` 建字形表 → 逐条写字形记录（UV = 像素坐标
  ÷ 图集尺寸归一化）→ `0x896780` 由记录**建 charmap**（全程序唯一调用点）
  → setPageCount → 逐页读图集名入页 vector（纹理解析等到 slot12
  `0x7993B0` 才做，且只解析第 0 页 ⇒ **一个字体一张图集**）。
- **charmap 查表 `0x8965D0`**：ecx = font+0x3C，栈参码位 C，返回 AX =
  字形下标，`retn 4`。`C>>8` 取页、`C&0xFF` 取槽、缺页返回 0（= 占位
  图标）；页按需 `0x896600` 分配（512 B/页）。寻址空间 65536 = 完整 BMP，
  无 ANSI/DBCS 转换。7 个直接 call：DrawText×2、Advance、Width、引擎2×3
  （引擎2 在 PC 是死路）。
- **字形记录**（内存 0x20 B/条）：+00 码点 u16 / +02 宽 / +04 高 /
  +06 X 偏移 i16 / +08 Y 偏移 i16 / +0A 前进宽（写 u16 读 i16）/
  +0C..+0x18 四 f32 UV / +1C 页下标（PC 不读）。

### 1.4 呈现层：排版 → 绘制记录 → D3D → 屏幕

- `DrawText 0x799720`（PC 实际路径，vtable 槽8，只虚调）：逐码元查
  charmap → 读记录度量/UV → 每码元发射 1 个四边形（**20 B 顶点
  `{x,y,z,u,v}`，`v = 1−v`**）→ 追加进 0x70 B 绘制记录（+0x00 = 当前字形
  记录指针，+0x5C = 材质（从 font+0x458 拷入），+0x64 = 官方回调扩展点，
  +0x6C = 标志位）→ 全局绘制记录队列。
- 帧末提交 `0x786180` 遍历记录：绑材质/纹理（rec+0x5C → 槽位索引 → 槽
  对象 → 影子缓存比对 → `SetTexture` 设备槽 65）→ 状态槽（混合/过滤/
  寻址，全带影子缓存）→ 写 24 B GPU 顶点 → `SetVertexDeclaration` +
  `SetStreamSource` + **`DrawPrimitive`（设备槽 81）**。
  最终消费者 = D3D9 设备（Present/EndScene 级别未逐指令逆向）。
- **UI 与 3D 的关系**：文字、UI 面板/边框/线段共用这套记录 + 提交环；
  3D 场景是另一路径（索引缓冲 + DrawIndexedPrimitive）。但**纹理创建是
  同一收口** `sub_AA9980` ⇒ 在 CreateTexture 层没有任何字段能区分
  「这是字体图集」——这是 §3 的死结二。
- 游戏逻辑层（全部游戏自算，与译文长度直接相关）：
  - **居中/对齐**：绘制当帧对当前串逐码元实测行宽 ⇒ 译文变长自动正确。
  - **条宽**：控件构建期按当时活串实测（不是 MGB 烘焙常量）⇒ 跟随译文。
  - **打字机**：每步字数 = 命令流里烘焙的**英文**长度 ⇒ 译文变长节奏仍按
    英文走（要节奏正确须同步改命令流字数——未做）。
  - **scramble**：逐帧改写活 wstring 内容的动画 ⇒ 译文真实长度天然正确。
- **原版呈现效果**：英文/拉丁正常；内联图标码位指向字体里的按钮图标；
  **CJK 码位无字形 ⇒ 占位图标**（查不到页/槽 ⇒ 字形 0）。游戏数据里本来
  就有中文（自定义按键页「按钮」标签），原版同样画成占位图标。

---

## 2. 我方介入点：4 钩 + 1 闸，按层标注

| 钩 | Dx9 RVA（VA）/ Dx10 RVA | 层 · 数据边 | pre/post | 干什么 |
|---|---|---|---|---|
| 文本漏斗 `sub_8A1920` | `0x4A1920`（0x8A1920）/ `0x7C3290` | L2：`属性值/流 → 临时 wstring` 与 `→ assign` 之间 | post | 词典命中 ⇒ 把译文写进临时 wstring（§2.1） |
| 字体加载 `0x8840B0` | `0x4840B0`（0x8840B0）/ `0x7EC350` | L1→L4：.mft 装载完成边（表/charmap/页刚就绪） | post（仅 al≠0） | 快路径：一步到位打补丁（§2.2） |
| charmap 查表 `0x8965D0` | `0x4965D0`（0x8965D0）/ `0x7F87C0` | L4 排版输入：`码位 → 字形下标`（每排一字必过） | post，**只读** | 兜底·发现：反推字体 → 匹配套号 → 排队；码位统计（§2.3） |
| DrawText `0x799720` | `0x399720`（0x799720）/ `0x4E81F0` | L4 排版入口：`控件 wstring + 字体 → 绘制记录` | pre（必须在 orig 前） | 兜底·应用：把队列里的字体逐个打补丁（§2.3） |
| forge 核验（非钩） | —（文件层） | L0：`DataPC.forge` 磁盘文件 | 启动一次 | 整套字形补丁 + 文本替换的**总闸**（§2.5） |

装配顺序（`app/src/dllmain.cpp`）：宿主守卫 → VEH → **forge 闸门** →
dict → glyph 引擎表绑定 → text::install → glyph::install。

### 2.1 文本漏斗（L2，post）

```
hook_funnel(dst, src):
  ra = _ReturnAddress()   // 反查是哪个调用点
  r  = 原函数(dst, src)   // 先放行：语义完全不变
  run_post(dst, ra)       // 再后处理
  return r
```

`run_post` 的闸门（任一不满足就 `skipped++`，不写任何字节）：① `wstr_view`
自洽（`_Mysize <= _Myres <= 0x400000`、缓冲整段可读）② 空串 ③ 已有码元
`>= 0x3000`（幂等闸：上一轮译文）④ 词典未就绪 ⑤ canon 后未命中。

命中且**替换生效**（唯一判定口 `path_funnel.cpp`：
`replace_effective() = TEXT_REPLACE && forge 闸门`）后：`_Myres >= 译文长`
⇒ `memcpy` 译文 + 尾 NUL、`_Mysize = 译文长`、`replaced++`；容量不足
`skippedGrow++` 一个字节不动。**不做等长填充、不动 `_Myres`** ⇒ 下游
assign 按真实长度搬运 ⇒ 零填充。

在 post 而非 pre 换内容的原因：dst 已是转义折叠后的物化形态，且下游
assign 按真实长度搬运，新译文不需要等长。

### 2.2 字体加载钩（L1→L4 交界，post）

快路径：字体加载返回成功 ⇒ 一步到位打补丁（补丁事务见 §2.4）。
**⚠ 不可靠**：字体加载可能发生在钩子装好之前（亚秒级竞争）⇒ 不能只依赖
它，这就是兜底两条存在的理由。

### 2.3 charmap 查表钩 + DrawText 钩（L4，发现与应用分离）

- 查表钩（post、只读）：先调 orig 拿原值（一个字不干预）→
  charmap−0x3C 反推字体对象 → 形状闸门 → 指纹（'@' UV 跨度）匹配套号 →
  排队；顺带记码位命中/漏字统计。引擎每排一个字都查表 ⇒ 与装钩早晚无关，
  字体一定会被"发现"。
- DrawText 钩（pre）：把队列里的字体逐个 `apply_patch`（同一事务函数），
  然后原样转发。revived 检测（引擎重置对象后表指针 ≠ 我们的块）也在这补。

**铁律：查表钩绝不能打补丁。** 绘制循环里引擎寄存器缓存着旧 font+0x38
（0x7997BF 处读），中途换表 ⇒ 新的字形下标在旧表上越界读。只有 DrawText
的前置钩可以换表——换表必须赶在引擎任何一次读 font+0x38 之前。

### 2.4 补丁事务与 EngineApi 约束

`apply_patch`（`modules/glyph/src/apply_patch.cpp`）：形状闸门 → 引擎 alloc
建新块 → record_array_ctor + 拷旧记录 → **原记录 UV 按 (orig_w/our_w,
orig_h/our_h) 缩放** → write_rec 追加 CJK 记录 → 发布 font+0x38/+0x34 →
逐码位 `set_charmap_entry` 补页 + 回读校验（失败回滚 + 拉黑）→ 登记。

两条硬约束（违反 ⇒ 引擎析构把野指针交给 magma 堆）：

1. 自建表必须用引擎 alloc（`0x787980`；Dx10 `0x8D5370`）分配——引擎析构走引擎 free
   （`0x787A20`；Dx10 `0x8D5410`），绝不能 CRT malloc / new。★ mod 侧从不释放引擎块
   （旧表刻意让渡，见 `apply_patch.cpp` 文件头），所以 `EngineApi` 里只有 alloc、没有
   free；free 地址在此备查。
2. charmap 页必须让引擎自己建：逐码位 `set_charmap_entry(…, flag=0)`
   （flag 非 0 会改写 font+0x43C 全局默认字形）。

引擎函数全部经 EngineApi 注入（bind_engine_at），单测无游戏宿主也能跑
（假 API）。双渲染器地址表见 §7。

### 2.5 启动期 forge 核验（L0 文件层，总闸）

`modules/glyph/src/forgecheck.cpp`：读 sidecar `scripts\AC1_CJK_Forge.txt`
（`DataPC.forge <尺寸> <crc32> <flags>`，`tools/write_forge_sidecar.py`
在 repack 后落），比对实际 `DataPC.forge` 尺寸（flags&1 时再核 CRC32）：

- **符合 ⇒ 整套字形补丁启用 + 文本替换放行**（glyph 三处落闸 +
  `replace_effective()`，两层同闸）；
- **不符（Steam 验证完整性还原 / 拷到干净机器）⇒ 一个补丁不打、文本不翻
  = 纯原版**（界面全英文，而非方块化的中文）。

尺寸 alone 可区分三态（实测）：原版 203,259,904 / ATK 重打包原版内容
203,030,528 / 补丁后随图集尺寸变（参考：BC2 时期 203,489,280，
BC3 四套 204,701,696）。

闸门开 ⇒ 图集必然是我们的 ⇒ **UV 缩放无条件化**（原记录 UV 一律按
(orig/our) 缩放）；闸门关 ⇒ 不缩放也不打补丁。运行时替换时代「图集到底
换没换上」的判别问题（§3.4④）在此被结构性消除。

---

## 3. 纹理方案：运行时替换（不动 forge）→ 安装期 repack（现行）

字体图集有两种换法：**运行时替换**（游戏文件不动，钩 D3D 在运行期把引擎
的图集纹理换成我们的）与**安装期 repack**（把我们的图集直接打进
`DataPC.forge`，引擎走原生路径自己加载）。项目先走前者（2026-09-27 ~
10-03），实机定案否决后改后者。本节留存前者成果与否决证据——它是
「不再返工」的凭据，也是理解 §1 数据流设计约束（身份消费、唯一收口）的
直接应用。

### 3.1 两案对比

| | 运行时替换（旧，不动 forge） | forge repack（现行） |
|---|---|---|
| 原理 | 钩设备槽 23 CreateTexture，识别出字体图集请求 ⇒ 不交原函数、交出预烘大图集 | 安装期 AnvilToolkit 把 BC3 图集 Replace 进 735–738 + repack |
| 身份判别 | 只能靠尺寸+次序/栈特征（多轮实机互不一致） | 不需要：图集在场是安装期事实，启动期 sidecar 核验 |
| 时序 | 竞赛：字体加载 vs 装钩 vs 纹理惰性创建 | 无（启动期一次核验） |
| 回写 | 引擎可能把原尺寸数据回写进我们的纹理（L483 假说） | 无（纹理是引擎自己加载的） |
| 观测 | `atlas_swapped()` 意图 ≠ 观测 | sidecar 不符 ⇒ 全停用，无中间态 |
| 运行期 D3D | 钩 d3d9.dll（MinHook） | 零（产物只 import KERNEL32.dll） |
| 部署 | 纯 asi | asi + 一次性 forge 修改 + sidecar；卸载 = Steam 验证完整性 |

### 3.2 运行时替换路线的形态（全部实机跑过，代码已拆除）

| 形态 | 做法 | 实机结果 |
|---|---|---|
| LockRect 烧录（原型） | 找到字体图集纹理直接 LockRect 写字形像素 | 成功（256×512 pool=1 写入「中」上屏）；但 MANAGED 内容会被引擎改写（§3.4③） |
| B1 纹理解析返回劫持 | 钩 `0xA1B220`，返回值里认出图集则换 | 否定：探针实测它从没返回过字体图集（返回的是 0x20B 纹理槽对象，不在字体绑定路径） |
| B5/B6 SetTexture 补丁 | `0xAAB56E` 打 6 B 补丁 + 手写 32 B stub，把 edi（裸纹理）换成自建纹理 | 单点成功（4 个「下」字端正、拉丁照常）；但槽 65 取址点全 .text 123 处，单点覆盖不了 |
| F1 CreateTexture 交出 | 设备槽 23 识别字体图集请求 ⇒ 交出自建纹理 | **唯一跑通的形态**：中文与拉丁同屏正常；后演进为 backend_dx9 |
| backend_dx9（模块） | MinHook 钩 d3d9.dll 的 CreateTexture/EndScene（地址从临时 dummy 设备 vtable 取，装完即释放，不与游戏抢时序）；引擎请求 (原尺寸, DXT3) 时交出预烘图集（title 1024×8160 / bold 1024×1696 / textpc 1024×1184 / techno 1024×1216） | 实机「槽=2 帧=33763」；引擎包装/材质/影子缓存从创建起就持有我们的纹理，不需任何代码补丁。败在身份判别（§3.3） |

UV 缩放与图集替换必须成对：换了大图集 ⇒ 原记录 UV 按 (orig/our) 缩；
不缩放 ⇒ 拉丁文横跨整图采到 CJK 区（满屏乱码）；没换还缩放 ⇒ 满屏方框
（两条都实机踩过）。

### 3.3 身份判别判据演进（textpc 256×256 是唯一的硬骨头，其余三套尺寸唯一）

bold 256×512 / title 512×1024 / techno 64×1024 尺寸唯一 ⇒ 每次请求都换。
textpc 256×256 有同尺寸多张，判据五次更迭、全部证伪或不稳定：

1. **调用点+参数**：两张 256×256 DXT3 请求调用点完全相同（0xAA9A0C =
   唯一收口内返回地址）、pool/usage/mip 全同 ⇒ 不可分。
2. **内容比对**：运行期图集在 D3DPOOL_DEFAULT，LockRect 读不出 ⇒ 不可分。
3. **mip**：历史样本里 mip 出现过 1 与 9；实机一轮两张请求 mip 全 1
   ⇒ 要么全中要么全不中，不可分。
4. **次序（ORDINAL）**：=1 劫错非字体纹理；=2 当轮正常；改成只算 Levels==1
   又毁掉一块真 UI 纹理。**四次实机互不一致 ⇒ 这个维度上没有可用信号**。
5. **栈特征**：扫 CreateTexture 入口之上的栈，找落在 `0x786180..0x7862A0`
   （绘制记录提交环）的返回地址。两轮独立验证 9 个 256×256 候选只有它
   命中，2026-10-03 13:37 实机命中（漏字=0、textpc=✓换1）；**同日 17:09
   失灵**（栈上无材质路径特征 ⇒ 一张都没换）。两轮互不一致 ⇒ 定案否决。
6. **资产 ID（终点）**：见 §3.5——身份在 hash 查表后不存在于任何环节的
   参数里。

失败模式对比：栈特征「认不出就一张都不换」（只损失 textpc 的 CJK），
优于次序「猜错就毁掉一块真图形」——但两者都达不到「每轮都对」。

### 3.4 三个结构性难题（运行时路线固有）

① **身份判别**：如上——字段级判据不存在，行为级判据不稳定。
② **时序竞赛**：装钩必须赶在游戏建设备之前（dummy 设备还会占位挡真
   设备）；字体加载可先于纹理创建（纹理是惰性创建的）；「尺寸观测器」
   方案同样有「字体加载先于纹理创建」竞赛。
③ **回写踩字（L483 假说）**：替换纹理是 MANAGED 时，引擎可能把它那份
   原尺寸数据回写进来，正好踩掉顶部 CJK 栅格——与「回菜单后中字没了但
   补丁仍在」的观测吻合。看门狗（72 点抽样、被改写就自愈）实机两次抓到
   「bold 被改写 1/72」⇒ MANAGED 替换纹理内容确实会被改写（直接观测）；
   但「中字消失」的最终成因未钉死 [推断]。
④ **atlas_swapped() 意图 ≠ 观测**：后端「打算换/认为换了」≠ 引擎真绑着
   我们的纹理；实机踩过「中文对了、拉丁坏了」（图集已换但清单 flags 没
   跟上 ⇒ UV 没缩放）。forge 路线后由 sidecar 结构性闭合（§2.5）。

### 3.5 否决判据（2026-10-03 定案）

**判据一：资产 ID 到不了 CreateTexture（身份不可恢复）。**
完整链：font+0x458 → 材质链表节点+0xC = 资产 ID ⇒ `sub_C58B20` hash 查表
⇒ 传给下游的是**表项值** ⇒ 提交环 `0x786180` → 槽0x78 `0xA247C0` →
`0xA1B220`(idx, 模板)（槽位索引缓存）→ miss → … → `sub_AA9980` →
CreateTexture。**每一个环节的参数里都没有那个 ID**：档案解析用 ID
（用完即弃），材质解析→纹理只传槽位索引，纹理壳→CreateTexture 只有
this+尺寸/格式。架构解读：hash 取资产保证同一资产全局只加载一次（缓存），
代价是身份在这段不可恢复。
⇒ 栈特征不是「替代品」，是这个架构下**唯一存在**的运行期判据——而它
两轮互不一致（判据二）。

**判据二：行为判据多轮实机互不一致。** 次序四次不一致、栈特征
13:37/17:09 两轮不一致（§3.3）。

**结论**：身份判别、时序竞赛、回写踩字三个运行时难题在 forge 路线下
**全部不存在**（不需要身份、没有时序、纹理是引擎自己的），于是改走
安装期路线。运行期侧还做过一轮减法：钩子 8→4（backend_dx9 整模块删除 +
第二文本钩 sub_892BD0 摘除），链接行去 d3d9.lib，产物只 import
KERNEL32.dll。

路线格言（当时定案原话）：「引擎把纹理身份扔掉了，所以别再找身份，去找
行为。」——四轮实机证明行为也不稳定，于是转安装期。

### 3.6 forge 路线实证与格式定案

- **exp1（管线验证）**：把 738_TextureMap 的 BC2 载荷做块行旋转 128 px
  （格式/尺寸/头部原样）→ ATK repack → 游戏内 textpc 小字变方块/透明、
  其它字体正常、长度正常（MFT 度量没动）⇒ **纹理资产
  repack→加载→渲染管线实锤**。
- **exp2（格式接受度）**：A8R8G8B8 / 1024×1184 / Levels=1 的 textpc 大
  图集 → ATK Replace（不勾 mip）→ repack → 游戏内小字变汉字碎片（引擎
  拿原 UV 采进 CJK 区）、其它正常、游戏稳定 ⇒ **非原版尺寸、Levels=1、
  格式替换全被引擎接受**；ATK 写回的条目头 FileID 保持 0x47195956
  （= 运行时 ID +3，互洽）。
- **格式定案 BC3/DXT5**：未压缩 fmt=0 图集在 Dx10 **点击即崩**
  （0xC0000005；Dx10 格式码→DXGI_FORMAT 映射表没有 fmt=0 项 [推断，未
  IDA 验证]），Dx9 可跑——别被 Dx9 骗；BC2 的 4 bit alpha 把抗锯齿边缘
  量化出毛刺；BC3 双渲染器全通。出货 = BC3（`tools/rgba_to_dds.py`
  默认）。已知观感项：BC3 下字形略模糊（打磨候选：stroke_width /
  alpha 锐化 / 渲染 px 上调）。
- **全链路实机**：BC3 四套常驻 forge（204,701,696 B），Dx9 + Dx10 双
  渲染器中文上屏零崩溃；`替换=2306 容量不足=0 异常=0 漏字=0种` 量级。

若未来重估运行时路线，候选拦截点只有这几层（除 L0 外全部「未实现」或
「不稳定」）：

| 层 | 拦截点 | 判词 |
|---|---|---|
| L0 数据层 | forge 内 735–738 条目，同尺寸换内容 | 可行 ⇒ 现行 forge 路线就是它的加强版（连尺寸一起换，安装期做）；数据级换尺寸走不通（§1.1） |
| L1 字体对象 | `font+0x458` 指针配对（借材质链表节点+0xC 的资产 ID——运行期唯一还拿得到身份的地方） | 要把材质/纹理包装一并接手；写句柄本身必崩（引用计数句柄）；未实现 |
| L4 记录构建 | 绘制记录 +0x5C 写入处 / 官方回调 +0x64 | 官方扩展点、语义稳定；UI 面板路径把 +0x64 置 0 ⇒ 需逐条补写；未实现 |
| L5 提交环 | `0xA1B220` 返回值换纹理槽对象 | 需构造假槽对象（12 B 包装 + AddRef）；24 个调用点含 3D，过滤成本高；未实现 |
| D3D 绑定 | 设备槽 65 SetTexture | 已证伪：实机 9 拍 0 绑定 + 123 处取址点（§3.2） |
| D3D 创建 | 设备槽 23 CreateTexture | 唯一实机跑通（§3.2），败在身份判别（§3.3） |
| 引擎2 接管 | 替换 RenderScimitar 单例（空桩 vtable） | 引擎2 在 PC 是死路；未做 |

---

## 4. 模块与边界

| 模块 | 产物 | 依赖 | 职责 |
|---|---|---|---|
| `modules/core` | `core.lib` | 无（仅 Win32） | 框架层：读门（VirtualQuery+SEH）、MSVC8 `wstring` 视图、挂点表 + 逐字节签名核对、宿主守卫、日志、公共原语（§5）、钩执行迹/崩溃转储（§6） |
| `modules/dict` | `dict.lib` | core（仅 file/str 原语；日志注入） | 词典解析（成对块 + 补充词典）、`canon` 规范化、FNV-1a + 线性探测精确匹配 |
| `modules/text` | `text.lib` | core + dict | 单点漏斗（§2.1）、状态行、字符串全记录（§11） |
| `modules/glyph` | `glyph.lib` | core | 字形层：三条挂点（§2.2/§2.3）+ 码位统计 + forge 核验（§2.5）+ UV 缩放 |
| `app` | `ac1_cjk.asi` | core + dict + text + glyph | 装配层：只有 `dllmain.cpp` 一个 .cpp |

```
core ──┬──────────→ glyph ─┐
       └─→ dict ──→ text ──┴──→ app ──→ ac1_cjk.asi
```

依赖单向无环；模块间只经 `include/ac1/<层>/` 公开头交互（**机器强制**：
顶层 `build.ps1` 第 0 步 `Assert-Includes` 逐个源文件检查——include 命中
别人 `src/`、引用 `ac1/` 越白名单、或写成 `../../` 相对前缀绕过，任一 ⇒
构建直接失败）；接口全部是 POD，不跨模块传 STL 对象；命名空间
`ac1::<层>`。`dict` 的日志不 include `core/log.h`，走出口注入
（`set_log(LogFn)`，app 接 `core::log_line`，单测接 printf，不接即静默）。

运行期与离线工具零依赖：`.asi` 里没有一个 `.py`、不碰 Pillow、不读字体
文件；唯一的第三方 C 代码是 `third_party/minhook/`（BSD；**只编进
core.lib 一份**——MinHook 是进程全局单例，编两份会出现两张互不可见的
钩子表）。

| | 内容 | 进 `.asi`？ | 进游戏 `scripts\`？ |
|---|---|---|---|
| 插件 | 4 模块 + `app/` | ✅ | `ac1_cjk.asi` |
| 离线工具 | `tools/*.py`（烤字形/转 DDS/词典模板/分类/sidecar） | ❌ | ❌ |
| 数据 | `data/dict.txt`（那一份词典）等 | ❌ | `AC1_Dict.txt` |
| 生成物（gitignore） | `tools/out/`、`app/out/`、`modules/*/out/` | — | `AC1_CJK_Glyphs.bin` + `AC1_CJK_Forge.txt` |

> **⚠ 字体不在版本控制里**。重跑打包器需自备

**设计约定：数据问题用数据解决。** 匹配期只做 canon（空白折叠）+ 精确
相等；大小写/写法差异一律由词典数据补全（主词典 + 补充词典），不在匹配
器里加模糊/前缀兜底。缺口清单由 `tools\classify_inventory.py` 从
「未命中 ASCII Top」生成。

---

## 5. core 的公共原语

| 头 | 提供 | 要点 |
|---|---|---|
| `ac1/core/str.h` | `Str` 安全字符串累加器 | 替代「拿 `_snprintf` 返回值当写入偏移」。MSVC `_vsnprintf` 截断时返回负数、不是 ISO 的「本想写的长度」——两套约定都必须处理 |
| `ac1/core/ctr.h` | `Counters` 原子计数组 + `ctr_claim`/`ctr_claim_n` | 占表槽时把自增提到最前面：「读→查边界→写→最后自增」允许两线程拿到同一下标 |
| `ac1/core/file.h` | `file_read_all`/`file_free`/`file_exists`/`path_join` | 整文件读取上限是参数（运行期用 16 MB 默认值 / sidecar 64 KB；32 MB 是离线打包器的内存预算，不进运行期） |
| `ac1/core/hook.h` | `hook_install_one`（RVA + 逐字节签名核对 + 失败回滚 + 钩执行迹登记） | MinHook 生命周期归 core 独占（只装不卸）；签名不符 ⇒ 打日志、绝不硬装 |

---

## 6. 钩执行迹与崩溃转储（`ac1/core/trace.h`）

用途只有一个：崩溃时判断是哪个模块、哪个 hook 弄崩的。常驻状态行每
10 秒一轮（崩溃时可能已陈旧）；而崩的就是某个钩本身时，那个钩永远来不
及给自己写日志——要的信息必须在它进入时就埋下。

机制：每个钩进入时把「我是谁 + 两个上下文值」压进属于本线程的固定栈，
正常返回弹出。进程发生未处理异常时，VEH 在异常分发时读栈，残留的正是
「崩的那一刻由内到外的调用链」，写进 `scripts\AC1_CJK\crash_<时间戳>.log`。
栈空时明确写出「未记录到任何钩 ⇒ 崩在游戏自身」。

**次序契约**：栈内记录按**进入次序**存（先进入的在下标 0 = 最外层，最后进入
的才最靠近崩点），由 `veh_dump` 与 `hooktrace_snapshot` 统一**逆序**对外
⇒ 报告里的 `#0` 恒为最内层（崩点本身），编号越大越靠外；`nMax` 不够时
留下的是最内层那几条。看崩溃报告先看 `#0`。

每个钩的策略按实测量级分档（「每一次命中都记一行」不可行：查表钩一轮约
27,904 次，实机曾到 1.8M）：

| 钩 | 一轮量级 | 进栈 | 明细行（代码设定值） |
|---|---|---|---|
| 字体加载 `0x8840B0` | 几次 | ✅ | 每次，上限 200 |
| 漏斗 | ~5,096 | ✅ | 每 2000 次，上限 200 |
| 查表 `0x8965D0` | ~27,904（曾 1.8M） | ✅ | 每 5000 次，上限 100 |
| DrawText `0x799720` | 渲染热路径 | ✅ | 不打 |

`logBudget` 数的是**已打行数**（CAS 抢名额，多线程不超发）；曾错写成数
进入次数 ⇒ 热钩在第一个采样点前就耗尽预算、一行打不出（实机复现后修复，
有回归断言）。

热路径成本：每钩进出各一次 `GetCurrentThreadId()`（x86 读 TEB，非系统
调用）+ ≤8 槽线性探测 + 写 12 字节（`HookRec`）+ 改一次 depth。无 I/O、
无分配、无格式化、无锁。刻意不用 `__declspec(thread)`：x86 静态 TLS 依赖
加载器建 TLS 目录，而本插件由 ASI Loader 装入、加载方式未确认；固定数组
绕开这一点。

VEH 永远返回 `EXCEPTION_CONTINUE_SEARCH`——绝不吞异常、绝不改变崩溃
语义。只对 `ACCESS_VIOLATION` / `STACK_OVERFLOW` / `ILLEGAL_INSTRUCTION` /
`0xC0000409` / `0xC0000374` 动作，且转储走独立文件、只用预先打开的句柄
`WriteFile`（崩溃瞬间 `core::log_line` 可能正持有日志锁 ⇒ 死锁）。

---

## 7. 宿主表与挂点总表

`core/host.cpp`：exe 名 → (口味, 基准 CRC32) 一张表。**加一门宿主 = 加
一行**。

| exe | 口味 | 基准 CRC32 |
|---|---|---|
| `AssassinsCreed_Dx9.exe` | Dx9 | `E8936C99` |
| `AssassinsCreed_Dx10.exe` | Dx10 | `3AF8F9D0` |

四条挂点 + EngineApi（双渲染器，Dx10 全部 capstone 离线定案 + 实机复核；
Dx10 无 PixmapFontScimitar；字体对象布局跨渲染器不变）：

| 挂点 | Dx9 RVA | Dx10 RVA |
|---|---|---|
| 漏斗 | `0x4A1920` | `0x7C3290` |
| 字体加载 | `0x4840B0` | `0x7EC350` |
| charmap 查表 | `0x4965D0` | `0x7F87C0` |
| DrawText | `0x399720` | `0x4E81F0` |
| EngineApi: alloc | `0x787980` | `0x8D5370` |
| EngineApi: set_charmap_entry | `0x896720` | `0xBF8910` |
| EngineApi: record_ctor / record_array_ctor | `0x8842B0` / `0x40104B` | `0xBEC550` / `0x4010AF` |

签名字节数（装钩日志可见）：Dx9 = 7/7/8/7，Dx10 = 13/7/8/13（漏斗与字体
加载为 13 字节分栏签名）。

三条纪律：

- exe 名与基准 CRC 必须实测，不许照抄别的口味——填错会让 `host_check()`
  在错的宿主上返回 true，比返回 false 危险得多；
- 未识别的宿主，摘要里那格说「未知」，不拿别的口味的基准去比、也不虚报
  「不一致」；
- 配套还要填各 `HookSpec` 的 `rva10`/`expect10` 与 EngineApi 的 Dx10 列。
---

## 8. 构建 / 单测 / 开关

```powershell
pwsh -NoProfile -File build.ps1                  # 依赖检查 → 4 个模块 → 装配
pwsh -NoProfile -File build.ps1 -RunTests        # 顺带构建并运行四个单测
pwsh -NoProfile -File build.ps1 -CheckSwitches   # 额外把每个开关组合编一遍（只重编 text）
pwsh -NoProfile -File modules\core\tests\run.ps1 # 单模块单测（各模块同形）
pwsh -NoProfile -File modules\dict\tests\run.ps1 -RealDict <LGCStringDict_01.txt 路径>
```

任何模块的 `build.ps1` 都接受 `-Defines`（三种写法：pwsh 数组 / 逗号串 /
单个）与 `-OutDir`。

⚠ **单测链接的是 `modules/<层>/out/*.lib`（当前配置）**：刚用 `-Defines`
编过变体时那几个 `.lib` 已是变体版本，跑单测会拿变体断言默认期望而失败。
跑单测前先 `build.ps1` 回默认（`-RunTests` 是全量重建，自动复位）。

开关变体（`-Defines` 原样透传给 dict / text / glyph / app；core 无开关）：

```powershell
# 真替换档（默认关闭的那个开关），另存为 ac1_cjk_tr1.asi，不覆盖默认产物
pwsh -NoProfile -File build.ps1 -Defines "-DTEXT_REPLACE=1" -AsiName ac1_cjk_tr1.asi
```

| 开关 | 位置 | 默认 | 作用 |
|---|---|---|---|
| `TEXT_REPLACE` | `modules/text/include/ac1/text/text.h` | **0** | **文本**替换开关（与纹理无关）：0 = 只观测一个字节不写；1 = 真替换（还需 forge 闸门开，§2.1） |
| `SAMPLE_N` | 同上 | **32** | 命中/未命中样本日志各自的上限条数 |
| `AC1_NO_FALLBACK` | `modules/glyph/include/ac1/glyph/glyph.h` | **0** | 1 = 不装兜底两条（查表/DrawText），只装字体加载快路径。代价：无兜底 + 无码位统计 + 无指纹样本 |
| `AC1_DRAW_DIAG` | `modules/glyph/src/detours.cpp` | **0** | 1 = 编译进 DrawText 串坐标诊断行（400 条额度）。抓布局问题时 `-D` 打开 |

开关放在公开头里（不是 .cpp），因为单测要按同一组开关写断言（例外：
`AC1_DRAW_DIAG` 只被 detours.cpp 消费、无单测断言，故留在 .cpp）。

> ⚠ 默认 `TEXT_REPLACE=0` 时整个替换代码块被 `#if` 掉，默认构建从没编过
> 它；改完开关务必跑一次 `build.ps1 -CheckSwitches`。矩阵只重编 text
> （开关全在 text.h + path_funnel.cpp，dict 不引用任何宏 ⇒ 任何 -D 组合下
> 产物逐字节相同）。

### `dict` 模块两个必须保留的编译设置

| 设置 | 为什么必须保留 |
|---|---|
| **`/Zc:preprocessor`** | `DLOG` 用经典 `DLOG_NARG` 技巧数实参个数，MSVC 传统预处理器下它恒返回 1 ⇒ `static_assert` 全线误报；少了这个开关，格式串检查退化成没有 |
| **`/analyze` + 致命诊断码表**（`C4473/C4477/C6064/C6065/C6066/C6271/C6273/C28251`） | 构建脚本捕获 cl 输出，出现这些码即抛错。SAL 只覆盖类型不符/实参过多；实参不足由 `static_assert(count_conv(fmt) == DLOG_NARG(...)-1)` 抓 |

---

## 9. 部署

**一次性（forge 图集安装）**：

1. 烤字形：`python tools\pack_glyphs.py` ⇒ `tools\out\glyphs\atlas_*.rgba ×4`
2. 逐套转 DDS：`python tools\rgba_to_dds.py --all` ⇒ `atlas_*.dds ×4`
   （默认 **BC3/DXT5**、Levels=1；fmt=0 未压缩图集在 Dx10 点击即崩，§3.6）
3. **AnvilToolkit** 对四个 `tga32_Map`（735–738，在 `DataPC.forge` 的
   `2_-_Game Bootstrap Settings.data` 里）逐一 Replace 喂 DDS（不勾 mip），
   然后 **repack `.data` → repack `.forge`**（顺序不能反；改动的文件先
   落盘；别双击解包，会覆盖改动）
4. `python tools\write_forge_sidecar.py --game "<游戏目录>"` ⇒
   `scripts\AC1_CJK_Forge.txt`（没有它字形补丁一律不装）

**每次构建/更新插件**：

1. `pwsh -NoProfile -File build.ps1` ⇒ `app\out\ac1_cjk.asi`
2. 把 `ac1_cjk.asi`、`AC1_CJK_Glyphs.bin`、`AC1_Dict.txt` 放进游戏的
   `scripts\`（ASI Loader 自动加载）

**卸载**：Steam「验证游戏文件完整性」还原 forge ⇒ sidecar 尺寸不再匹配
⇒ 启动即判「forge 补丁未检出」⇒ 字形补丁与文本替换同时停用（纯原版；
观测计数照记）。完全移除再删 `scripts\ac1_cjk.asi`。

不随包部署 `atlas_*.rgba`；`AC1_CJK_Glyphs.bin` 仍部署（字形记录/charmap/
UV 数据运行时要读）。

操作坑位（实测定案）：ATK 不勾 mip；repack 顺序 `.data`→`.forge`；改动
文件先落盘且时间戳早于 repack；别双击 `.forge/.data`（= 解包覆盖树）；
ATK 开着会锁 `Extracted\`。Steam 拉起的是 Dx10，直启
`AssassinsCreed_Dx9.exe` 才是 Dx9。

---

## 10. 日志体系

一条命名规则：插件的全部产出进 ASI 同级的 **`AC1_CJK\`** 目录，文件名带
run 启动时间戳——每 run 天然一个新文件，旧文件永不被触碰，排序即时间序。

```
scripts\AC1_CJK\
├── run_YYYYMMDD_HHMMSS.log        本 run 文本日志（人读 + grep）
├── strings_YYYYMMDD_HHMMSS.txt    字符串全记录（喂 classify_inventory.py）
└── crash_YYYYMMDD_HHMMSS.log      崩溃转储（只在崩溃时写，§6）
```

行格式 `[YYYY-MM-DD HH:MM:SS][LVL][原前缀] 消息`；等级三级 `INFO` /
`WARN`（降级运行：forge 闸门关、词典未就绪）/ `ERR`（要行动的事故）。

`ERR` 的判据是**渲染后的消息体里含 `!!` 标记**（标记位置不限）：直接调
`log_line` 的 `"[词典] !! …"` 与经 `set_log` 注入的预格式化消息体都算，
`log_warn` 恒为 `WARN`。判据必须落在消息体上——曾经只看格式串的
`fmt[0..1]`，而注入方走的是 `g_log("%s", buf)`、标记在 `buf` 里 ⇒ 全树
23 处事故行只有 4 处真能打 `ERR`（实机 267 行全 `INFO`）。`!!` 因此是
**保留标记**，正文里不要拿它当标点。

分类 = 消息里的中文前缀（`[启动]/[宿主]/[钩]/[词典]/[FUNNEL]/[文本]/
[INV]/[字形]/[度量]/[闸门]/[状态]/[迹]/[崩溃]`），grep 用中文前缀即可。
启动日志里有一行 `[启动] 崩溃转储已就绪（<路径>）`，看不到说明 VEH 没
装上。

词典定位（不硬编码绝对路径；候选目录顺序：ASI 同目录 → 上级 `scripts\` →
上级 `LG_Data\` → 宿主根，逐个候选都打进日志）：

1. `AC1_Dict.txt` 在 ⇒ **只读它**（UTF-8；同键后到覆盖先到）：查得到就
   替换，查不到留英文原文；日志打「不读 LG 主词典」。
2. 不在 ⇒ 旧路径（兼容既有部署）：LG 主词典 `LGCStringDict_01.txt`
   （CP950；同键丢弃）+ `AC1_CN_Dict_Supplement.txt`（UTF-8，叠加覆盖）。
3. 全部失败 ⇒ 日志写明「词典未就绪」，钩子仍装、只观测不替换。

**代码页（关键）**：`AC1_Dict.txt` 与补充词典固定 CP_UTF8，LG 主词典固定
CP950（Big5）；绝不用 `CP_ACP`（随机器 locale 变，换台电脑整份译文变
乱码）。启动自检 `A4A4 → U+4E2D`；解不出的字节落 `'?'` 并计数。

---

## 11. 字符串全记录（inventory）

`AC1_CJK\strings_<run>.txt`（每 run 一份）：每个唯一串一行——
`[×次数] 状态 caller=XXXXXXXX 文本`，状态 ∈ `命中 / 替换 / 未命中 /
未命中·图标`，`caller` 是首次见到它的调用点返回地址（IDA 可反查）。
每 ~10 秒批量刷盘；表 4096 条，表满只计「丢弃」。用途：翻译决策的输入
清单。

离线分类（只用标准库；不参与构建）：

```powershell
python tools\classify_inventory.py --game "<游戏目录>"
python tools\classify_inventory.py --self-check   # 合成词典 + 全记录自检
```

产物在 `tools\out\`：分桶报告 / `dict_additions.suggested.txt`（建议
条目）/ 待译缺口。分桶口径与「不译清单」（`data\dict_notranslate.txt`）
见 [`data/README.md`](../data/README.md)；五个工具的参数/输入/输出/默认值
总表见 [`tools/README.md`](../tools/README.md)。

---

## 12. 验收

**第一轮（`TEXT_REPLACE=0`，默认）——只观测，不写游戏内存：**

1. 启动游戏，进主菜单 → 设置 → 进游戏 → 触发各种界面
2. 看 `scripts\AC1_CJK\run_*.log`（本 run 那份），应满足：
   - `宿主 ✓ ... CRC32=<基准> 一致 ✓ ... 口味=Dx9`（或 Dx10）
   - `[钩] sub_8A1920 漏斗 已装 ✓ ... 签名[逐字节符合]`（Dx9 四钩 =
     7/7/8/7 字节；Dx10 = 13/7/8/13）
   - `[词典] 就绪 ✓：键 N/6144 键池 .../196608 译文池 .../131072`
   - `[FUNNEL] 配置：TEXT_REPLACE=0（只观测、不写任何字节）`
   - `[启动] 模块就位：词典=就绪 文本=漏斗已装 字形=3（GLYPH 闸门=开
     钩子=3/3 字体=… 字形+… 页+… 集=… 指纹=…/8 排队=…）`
   - `[字形] forge 核验 ✓：尺寸=…（flags=0 免 CRC）⇒ 字形补丁启用`
     （若「sidecar 缺失或不可读/尺寸不符 ⇒ 字形补丁停用」⇒ 见 §9：repack +
     `write_forge_sidecar.py`；此时字形钩照装但一个补丁都不打）
   - 每 10 秒一行 `[状态] FUNNEL 命中=… 未命中=… 异常=0`
3. 判据：`命中` 持续增长、`异常=0`、`读门异常=0`、游戏完全正常
4. 拿样本行的 `ret` 反查——落在 7 个 UI 调用点里算正常；落在诊断格式化
   段 = 非 UI 文本，词典不命中即无副作用
5. 顺带收字形指纹（喂打包器）：日志开头最多 8 行
   `[字形] 指纹样本 #k 字体 <p> vtable=<8位hex> 字形=<n> '@'跨度 u=%.6f
   v=%.6f`——这是 `AC1_CJK_Glyphs.bin` 里 `fp_u_span/fp_v_span` 的唯一
   来源（exe 里枚举不出字体资源名，静态得不到）。`%.6f` 的位数是契约。
6. 字形层要看到补丁落地的证据（同一 run）：`由查表发现（加载钩没赶上）
   ⇒ 已排队…` 之后紧跟 `[字形][DrawText前] 字体 … 字形 224→227（+3）…`。
   若 `字体=0 指纹=0/8` ⇒ 这一轮字体加载发生在装钩之前；界面走一遍后
   应由查表/DrawText 两条兜住（`排队=` 短暂 >0、`Draw应用=` 增长）。
7. 码位 Top：每 ~10 秒、且只在「种类数变过」时打两行（命中码位 Top /
   漏字 Top），条目 `U+%04X ×次数 @%08X`，只统计 `C >= 0x3000`。

**第二轮（`TEXT_REPLACE=1`）**：改宏 → `build.ps1 -CheckSwitches` →
部署 → 跑同样操作。界面出现中文（forge 图集 + 闸门开 ⇒ 字形正常；闸门
未开则文本变中文但字形为方块/缺字——先查 forge 核验那行），`替换=`
增长、`容量不足=` 可接受、`异常=0`。回滚：改回 0 重编部署。

**如果没装上钩**：日志出现 `!! <挂点名> 签名不符（不硬装）：… 期望[…]
实际[…]`。这是保护行为，不要绕过——通常意味着宿主版本不是表里那份，
RVA 全都不作数。

双渲染器均按上表验收（Dx10 挂点地址不同、形状相同）。

---

## 13. 未验证 / 已知边界

**未验证**

- `core` 的 `wstr_view` 单测覆盖 SSO/堆/垃圾字段，但那是我们自造的
  wstring，不是引擎真对象。
- `glyph` 的 `font_loaded_post()` / `hook_lookup()` / `hook_drawtext()`
  只在合成字体对象 + 假 EngineApi + 假 orig 上跑过；引擎真对象没进过
  单测（真机行为由 §12 的验收行覆盖）。
- `revived`（引擎把已补丁的字体重置）判定依赖 font+0x38 指针比对；若
  引擎是就地改回同一块地址，这条路径不会触发（补丁也不会丢）。
- `veh_dump` 的判空兜底（`d ? d->name : "?"`）无法正向验证：转储链本身已被
  单测 [11] 用真 AV + `hooktrace_dump_now` 钉住，要走到那个分支还得崩溃现场
  把 `rec[]`/`g_nslots` 写坏（正常崩溃也走不到）⇒ 只有负向验证（crash 0 字节 +
  WARN/ERR=0 + 游戏不崩）。签名不符的诊断格式化器已提升为公开纯函数
  `hook_hexdump`（`ac1/core/hook.h`），由单测 [3b] 正向覆盖。
- `AC1_NO_FALLBACK=1` 与 `AC1_DRAW_DIAG=1` 已进 `-CheckSwitches` 矩阵（各编一遍 +
  两者同时，`-OutDir` 编到临时目录，不污染 `modules/*/out/`）；真机判据
  `GLYPH 钩子=1/3`（NO_FALLBACK）。

**已知边界**

- 诊断格式化串也过漏斗；设计上「不命中即无副作用」，但若某条诊断串
  恰好在词典里会被一起翻掉——真机日志可核对。
- 多行对话框按整键翻译时，第二行译文可能泄漏进第一行（逐行键对未
  实现）。
- 打字机节奏按烘焙的英文长度走，译文变长时节奏不变（§1.4）。
- 容量不足（`skippedGrow`）的真实占比未量化，看真机统计。
- `SAMPLE_N` 样本只覆盖运行期最初若干条，长期覆盖看词典命中率。
- 崩溃转储在 STACK_OVERFLOW 路径上用 ~2KB 栈缓冲组装；栈耗尽场景下
  VEH 仍在故障线程的残栈上运行，理论上可能二次故障（表现为崩了但没
  转储；VEH 恒 CONTINUE_SEARCH，不改变崩溃语义）。未修：无法低风险
  实测。
- apply_patch 并发窗口：加载线程与渲染线程可同时穿过幂等闸，对同一
  字体重复登记（多一轮补丁 + 一张 ~7KB 旧表泄漏）。渲染始终正确，
  revive 路径自稳定；修复需对补丁事务做串行化设计，列为发布后项。
- `modules/text/src/inventory.cpp` 的路径拼接已按 `log.cpp`/`trace.cpp` 的范例
  加固（`sub` 缓冲为 `g_dir` 上限 +8 留余量、文件名段走带容量自检的 `path_join`）；
  残留仅是第 122 行对超长 `asi_dir` 的静默 clamp —— 装配层的 `find_self` 已
  fail-closed 到 ≤259，当前调用链不可达。
- `cp_index_hit_total` 的累加器是 32 位 long：需全部码位饱和命中
  （≈3.49e9 次）才溢出，实机不可达；溢出仅使状态行次数变负，不影响
  任何判定。
- 词典行长上限（运行期）：键列解码 ≤8192 码元、canon 后 ≤4096（超长整行丢弃，
  日志把它与「不是合法 UTF-8」分开计）；用户/补充词典译文列 ≤8192 码元（超长
  整行丢弃）；主词典 CHI 值 >8192 会被截断后入库，按 `main_valtrunc` 计数。
  LG 实测最长键列 ≈3.6 KB（余量 ~10%）、最长译文列 ≈1.7 KB。
- `file_read_all` 的 0 只表示「没读到」：不存在 / 0 字节 / 超 `maxBytes` / 分配失败 /
  读失败共用同一个返回值（契约见 `core/file.h`），各消费者的日志文案已按这一口径写。
- `inventory` 的新增路径用一把只护「占槽 + 写 + 发布」的小锁（`g_invLock`）串行化：
  条目的存在性与完整性由此保证（下标 < 游标者必已写完）。**命中的快路径仍无锁**，
  其 `flags |= / cnt++ / caller` 三个读改写是**非原子**的 —— 并发命中同一条时可能
  丢一个标签位或一次计数，只影响统计精度（出现次数、状态标签优先级），不影响落盘
  条目的存在性、完整性或顺序；落盘侧也不读这两个字段做判定。
- `mem` 的保护位判据是保守的：`PAGE_NOACCESS` 与 `PAGE_GUARD` 一律判否（见
  `core/mem.h` 文件头）。guard page 理论上可读，但首访会抛
  `STATUS_GUARD_PAGE_VIOLATION` ⇒ 对调用者等同「不可信」。代价是若某段真实内存
  恰被置了 guard，读门会拒读它（当前无此调用场景）。

---

## 附录 A. 目录结构

```
<仓库根>
├── build.ps1                     顶层：依赖检查 + 按依赖序调度 + 装配（不混编源码）
├── AGENTS.md                     代理/维护者速查（构建命令、依赖政策、编码约定）
├── docs/ENGINEERING.md           本文件
├── modules/
│   ├── core/    → out\core.lib
│   │   ├── include\ac1\core\{hook,host,log,mem,str,ctr,file,trace}.h
│   │   ├── src\{hook,host,log,mem,str,ctr,file,trace}.cpp
│   │   └── tests\{core_unit_test.cpp, build.ps1, run.ps1}
│   ├── dict/    → out\dict.lib
│   │   ├── include\ac1\dict\{dict,fallback}.h
│   │   ├── src\{dict,fallback}.cpp
│   │   └── tests\{dict_unit_test.cpp, build.ps1, run.ps1}
│   ├── text/    → out\text.lib
│   │   ├── include\ac1\text\text.h
│   │   ├── src\{path_funnel,inventory}.cpp + {path_internal,inventory}.h（同模块内部）
│   │   └── tests\{text_unit_test.cpp, build.ps1, run.ps1}
│   └── glyph/   → out\glyph.lib
│       ├── include\ac1\glyph\glyph.h
│       ├── src\{glyph,engine_api,font_track,apply_patch,detours,manifest,cpindex,forgecheck}.cpp
│       │   + glyphsets.h / glyph_internal.h（同模块内部）
│       └── tests\{glyph_unit_test.cpp, build.ps1, run.ps1}
├── app/
│   ├── src\dllmain.cpp          装配层（唯一被编译的 .cpp）
│   └── build.ps1                → out\ac1_cjk.asi（1 个 .obj + 4 个 .lib）
├── third_party\minhook\         MinHook 源（只编进 core.lib 一份；submodule 钉 8af6b4a）
├── data\                        词典数据源（构建/部署都不依赖本目录）
│   ├── dict.txt                 那一份词典（部署名 AC1_Dict.txt）
│   ├── dict.{ja,ko}.txt         空译文模板（换语言用）
│   ├── dict_notranslate.txt     不译清单（离线分类用，运行期不读）
│   ├── LANGUAGES.md             换语言三步
│   └── README.md
└── tools\
    ├── pack_glyphs.py           离线图集打包器：AC1_CJK_Glyphs.bin + 每套 atlas_*.rgba
    ├── rgba_to_dds.py           atlas_*.rgba → DDS（默认 BC3/DXT5，Levels=1）
    ├── write_forge_sidecar.py   repack 后落 forge 核验 sidecar（--check 只核验）
    ├── make_dict_template.py    从主词典导出翻译模板（换语言用）
    └── classify_inventory.py    「字符串全记录 → 翻译决策输入」分类器（--self-check 自检）
```

全新 clone 需 `git submodule update --init`（minhook 钉 8af6b4a）。
