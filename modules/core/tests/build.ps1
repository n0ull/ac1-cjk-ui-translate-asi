# modules/core 单测构建 → tests\out\core_unit_test.exe
#
#   pwsh -NoProfile -File modules\core\tests\build.ps1
#
# 只链接 core.lib，不链接任何其它模块的 lib，也不链接任何游戏 dll。
param([string[]]$Defines = @())

$ErrorActionPreference = 'Stop'

$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$mod   = Split-Path -Parent $here                 # modules\core
$out   = Join-Path $here 'out'
if (-not (Test-Path $out)) { New-Item -ItemType Directory $out | Out-Null }

$root = Split-Path -Parent (Split-Path -Parent $mod)
. "$root/tools/modules/native-toolchain.ps1"

$flags = @('/nologo','/O2','/MT','/EHsc','/W3','/utf-8','/D_CRT_SECURE_NO_WARNINGS')

Push-Location $here
try {
    Remove-Item -ErrorAction SilentlyContinue "$out\*" -Recurse -Force   # -Recurse：单测会落子目录（AC1_CJK 日志目录等）
    & cl.exe $flags @Defines "/I$mod\include" core_unit_test.cpp "/link" "$mod\out\core.lib" kernel32.lib "/OUT:$out\core_unit_test.exe"
    Write-Host "=== cl exit code: $LASTEXITCODE ==="
    if ($LASTEXITCODE -ne 0) { throw "core 单测: 编译/链接失败" }
    $f = Get-Item "$out\core_unit_test.exe"
    Write-Host ("产物: core_unit_test.exe  {0} 字节" -f $f.Length)
} finally { Pop-Location }
