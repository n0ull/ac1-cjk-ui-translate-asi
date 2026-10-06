# modules/text —— 独立构建 → out\text.lib
#
#   pwsh -NoProfile -File modules\text\build.ps1
#
# text 依赖：core（读门/挂点/日志）+ dict（词库）。
#   · 只 include 它们的公开头（modules\<层>\include），不碰任何 src/。
#   · 建 .lib 时不需要链接它们（静态库不做链接），依赖在最终 link 时满足。
param([string[]]$Defines = @(), [string]$OutDir = '')

$ErrorActionPreference = 'Stop'
$VC  = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
$SDK = 'C:\Program Files (x86)\Windows Kits\10'
$V   = '10.0.26100.0'

$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$mods  = Split-Path -Parent $here                 # modules\
$coreInc = Join-Path $mods 'core\include'
$dictInc = Join-Path $mods 'dict\include'
foreach ($d in @($coreInc, $dictInc)) {
    if (-not (Test-Path $d)) { throw "text 需要依赖模块的公开头：$d 不存在（先构建 core / dict）" }
}
if (-not $OutDir) { $OutDir = Join-Path $here 'out' }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory $OutDir | Out-Null }

$env:PATH    = "$VC\bin\Hostx64\x86;$env:PATH"
$env:INCLUDE = "$VC\include;$SDK\Include\$V\ucrt;$SDK\Include\$V\shared;$SDK\Include\$V\um;$SDK\Include\$V\winrt"
$env:LIB     = "$VC\lib\x86;$SDK\Lib\$V\ucrt\x86;$SDK\Lib\$V\um\x86"

$sources = @('src\path_funnel.cpp', 'src\inventory.cpp')
$flags = @('/nologo','/c','/O2','/MT','/EHsc','/W3','/utf-8','/GS','/D_CRT_SECURE_NO_WARNINGS')

Push-Location $here
try {
    Remove-Item -ErrorAction SilentlyContinue "$OutDir\*"
    # /I 顺序：本模块的公开头在前，依赖模块的公开头在后。没有任何 ../<层>/src 路径。
    & cl.exe $flags @Defines "/I$here\include" "/I$coreInc" "/I$dictInc" "/Fo$OutDir\" $sources
    Write-Host "=== cl exit code: $LASTEXITCODE ==="
    if ($LASTEXITCODE -ne 0) { throw "text: cl 失败" }

    & lib.exe /nologo "/OUT:$OutDir\text.lib" "$OutDir\*.obj"
    Write-Host "=== lib exit code: $LASTEXITCODE ==="
    if ($LASTEXITCODE -ne 0) { throw "text: lib 失败" }

    $f = Get-Item "$OutDir\text.lib"
    Write-Host ("产物: text.lib  {0} 字节  源文件 {1} 个  依赖 core+dict（仅公开头）" -f $f.Length, $sources.Count)
} finally { Pop-Location }
