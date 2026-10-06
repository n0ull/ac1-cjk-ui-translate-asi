# modules/glyph —— 独立构建 → out\glyph.lib
#
#   pwsh -NoProfile -File modules\glyph\build.ps1
#
# glyph 依赖：core（MinHook 落钩 + 日志）。
#   钩 0x8840B0（字体加载）的后置钩子 + 兜底两条（查表/DrawText），
#   换字形表 + 补 charmap 页。本模块不碰 D3D/纹理（forge 路线：图集在安装期进 forge）。
param([string[]]$Defines = @(), [string]$OutDir = '')

$ErrorActionPreference = 'Stop'
$VC  = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
$SDK = 'C:\Program Files (x86)\Windows Kits\10'
$V   = '10.0.26100.0'

$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$mods  = Split-Path -Parent $here                 # modules\
$coreInc = Join-Path $mods 'core\include'
if (-not (Test-Path $coreInc)) { throw "glyph 需要 core 的公开头：$coreInc 不存在（先构建 core）" }
if (-not $OutDir) { $OutDir = Join-Path $here 'out' }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory $OutDir | Out-Null }

$env:PATH    = "$VC\bin\Hostx64\x86;$env:PATH"
$env:INCLUDE = "$VC\include;$SDK\Include\$V\ucrt;$SDK\Include\$V\shared;$SDK\Include\$V\um;$SDK\Include\$V\winrt"
$env:LIB     = "$VC\lib\x86;$SDK\Lib\$V\ucrt\x86;$SDK\Lib\$V\um\x86"

$sources = @('src\glyph.cpp', 'src\engine_api.cpp', 'src\font_track.cpp', 'src\apply_patch.cpp', 'src\detours.cpp', 'src\manifest.cpp', 'src\cpindex.cpp', 'src\forgecheck.cpp')
$flags = @('/nologo','/c','/O2','/MT','/EHsc','/W3','/utf-8','/GS','/D_CRT_SECURE_NO_WARNINGS')

Push-Location $here
try {
    Remove-Item -ErrorAction SilentlyContinue "$OutDir\*"
    & cl.exe $flags @Defines "/I$here\include" "/I$coreInc" "/Fo$OutDir\" $sources
    Write-Host "=== cl exit code: $LASTEXITCODE ==="
    if ($LASTEXITCODE -ne 0) { throw "glyph: cl 失败" }

    & lib.exe /nologo "/OUT:$OutDir\glyph.lib" "$OutDir\*.obj"
    Write-Host "=== lib exit code: $LASTEXITCODE ==="
    if ($LASTEXITCODE -ne 0) { throw "glyph: lib 失败" }

    $f = Get-Item "$OutDir\glyph.lib"
    Write-Host ("产物: glyph.lib  {0} 字节  源文件 {1} 个（字形层数据半：换表 + charmap 补页）" -f $f.Length, $sources.Count)
} finally { Pop-Location }
