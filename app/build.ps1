# app —— 装配层 → out\ac1_cjk.asi
#
#   pwsh -NoProfile -File app\build.ps1
#
# 这里只编译 app\src\dllmain.cpp 这一个源文件，其余全部来自四个 .lib：
#     modules\core\out\core.lib
#     modules\dict\out\dict.lib
#     modules\text\out\text.lib
#     modules\glyph\out\glyph.lib
#   没有任何一个模块的 .cpp 被直接编进来。运行时零 D3D 调用 ⇒ 只链系统库。
#
# /MT = 静态 CRT ⇒ 不需要 VC 运行时 redist；
# 产物只依赖 KERNEL32.dll（dumpbin /imports 实测，无其它导入项，依赖清单见 AGENTS.md）。
param([string[]]$Defines = @(), [string]$OutDir = '', [string]$AsiName = 'ac1_cjk.asi')

$ErrorActionPreference = 'Stop'
$VC  = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
$SDK = 'C:\Program Files (x86)\Windows Kits\10'
$V   = '10.0.26100.0'

$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$root  = Split-Path -Parent $here                 # 仓库根
$mods  = Join-Path $root 'modules'
if (-not $OutDir) { $OutDir = Join-Path $here 'out' }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory $OutDir | Out-Null }

# 只用各模块的公开头做 include 路径（绝不用 <层>\src）
$incs = @()
foreach ($m in @('core','dict','text','glyph')) {
    $i = Join-Path $mods "$m\include"
    if (-not (Test-Path $i)) { throw "缺少 $i（先构建 $m 模块）" }
    $incs += $i
}
$libs = @()
foreach ($m in @('core','dict','text','glyph')) {
    $l = Join-Path $mods "$m\out\$m.lib"
    if (-not (Test-Path $l)) { throw "缺少 $l（先构建 $m 模块）" }
    $libs += $l
}

$env:PATH    = "$VC\bin\Hostx64\x86;$env:PATH"
$env:INCLUDE = "$VC\include;$SDK\Include\$V\ucrt;$SDK\Include\$V\shared;$SDK\Include\$V\um;$SDK\Include\$V\winrt"
$env:LIB     = "$VC\lib\x86;$SDK\Lib\$V\ucrt\x86;$SDK\Lib\$V\um\x86"

# 本脚本只编译 app\src\dllmain.cpp 这一个编译单元，其中的清单路径拼接走
# core::path_join（带容量检查），不触碰 _snprintf 等弃用 CRT API
$flags = @('/nologo','/LD','/O2','/MT','/EHsc','/W3','/utf-8','/GS')

Push-Location $here
try {
    # 只删本次要写的这两个文件，不通配：out\ 里可能同时躺着其它 -AsiName 变体的产物
    Remove-Item -ErrorAction SilentlyContinue "$OutDir\dllmain.obj", "$OutDir\$AsiName"
    # 唯一被编译的源文件
    & cl.exe $flags @Defines ($incs | ForEach-Object { "/I$_" }) /c src\dllmain.cpp "/Fo$OutDir\dllmain.obj"
    Write-Host "=== cl exit code: $LASTEXITCODE ==="
    if ($LASTEXITCODE -ne 0) { throw "app: cl 失败" }

    & link.exe /nologo /DLL "/OUT:$OutDir\$AsiName" "$OutDir\dllmain.obj" @libs kernel32.lib
    Write-Host "=== link exit code: $LASTEXITCODE ==="
    if ($LASTEXITCODE -ne 0) { throw "app: link 失败" }

    $f = Get-Item "$OutDir\$AsiName"
    Write-Host ("产物: {0}  {1} 字节" -f $f.Name, $f.Length)
    Write-Host ("链接输入: 1 个 .obj（本目录 dllmain.obj）+ {0} 个模块 .lib + kernel32.lib" -f $libs.Count)
} finally { Pop-Location }
