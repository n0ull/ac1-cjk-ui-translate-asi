# modules/dict —— 独立构建 → out\dict.lib
#
#   pwsh -NoProfile -File modules\dict\build.ps1
#
# dict 使用 Win32 UTF-8 解码与 core 公开输入上限；日志由调用方注入。
# 固定工具链共用；源清单、编译策略与产物仍由本模块负责。
param([string[]]$Defines = @(), [string]$OutDir = '')

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)
if (-not $OutDir) { $OutDir = Join-Path $here 'out' }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory $OutDir | Out-Null }

. "$root/tools/modules/native-toolchain.ps1"

# 只编本模块自己的 src/。
# core 的公开头提供输入上限，因此加入 /I；词典本身不读取文件。
# 普通/错误日志分别经 set_log(info,error) 注入。
#   注入，不 include core 的 log.h。依赖方向是 dict → core（单向、无环）。
$coreInc = Join-Path $root 'modules\core\include'
$sources = @('src\dict.cpp')
# CRT格式化在调用点展开；格式诊断直接作为编译错误，不解析编译日志。
$flags = @('/nologo','/c','/O2','/MT','/EHsc','/W4','/utf-8','/GS','/analyze','/D_CRT_SECURE_NO_WARNINGS',
    '/we4473','/we4474','/we4477','/we6064','/we6065','/we6066','/we6271','/we6273','/we28251')

Push-Location $here
try {
    Remove-Item -ErrorAction SilentlyContinue "$OutDir\*"
    & cl.exe $flags @Defines "/I$here\include" "/I$coreInc" "/Fo$OutDir\" $sources
    Write-Host "=== cl exit code: $LASTEXITCODE ==="
    if ($LASTEXITCODE -ne 0) { throw "dict: cl 失败" }

    & lib.exe /nologo "/OUT:$OutDir\dict.lib" "$OutDir\*.obj"
    Write-Host "=== lib exit code: $LASTEXITCODE ==="
    if ($LASTEXITCODE -ne 0) { throw "dict: lib 失败" }

    $f = Get-Item "$OutDir\dict.lib"
    Write-Host ("产物: dict.lib  {0} 字节  源文件 {1} 个" -f $f.Length, $sources.Count)
} finally { Pop-Location }
