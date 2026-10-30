# modules/glyph —— 独立构建 → out\glyph.lib
#
#   pwsh -NoProfile -File modules\glyph\build.ps1
#
# Runtime resources use core for host checks, memory, hooks and logging.
param([string[]]$Defines = @(), [string]$OutDir = '')

$ErrorActionPreference = 'Stop'

$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$mods  = Split-Path -Parent $here                 # modules\
$coreInc = Join-Path $mods 'core\include'
if (-not (Test-Path $coreInc)) { throw "glyph 需要 core 的公开头：$coreInc 不存在（先构建 core）" }
if (-not $OutDir) { $OutDir = Join-Path $here 'out' }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory $OutDir | Out-Null }

$root = Split-Path -Parent $mods
. "$root/tools/modules/native-toolchain.ps1"

$sources = @('src\runtime.cpp')
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
    Write-Host ("产物: glyph.lib  {0} 字节  源文件 {1} 个（运行时资源与字形接入）" -f $f.Length, $sources.Count)
} finally { Pop-Location }
