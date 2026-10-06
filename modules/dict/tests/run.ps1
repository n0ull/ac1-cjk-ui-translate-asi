# modules/dict 单测运行 → 失败数必须为 0
#
#   pwsh -NoProfile -File modules\dict\tests\run.ps1 [-RealDict <主词典路径>]
param([switch]$Quiet, [string]$RealDict = '')

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe  = Join-Path $here 'out\dict_unit_test.exe'
if (-not (Test-Path $exe)) { throw "找不到 $exe（先跑 modules\dict\tests\build.ps1）" }

if ($Quiet) { & $exe | Out-Null }
elseif ($RealDict) {
    # 解析成绝对路径：单测 exe 的工作目录不固定，相对路径不可靠
    $abs = (Resolve-Path $RealDict).Path
    Write-Host "（真实词典: $abs）"
    & $exe $abs
}
else { & $exe }
$code = $LASTEXITCODE
Write-Host "=== dict_unit_test exit code: $code ==="
if ($code -ne 0) { throw "dict 单测失败（$code）" }
