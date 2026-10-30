# modules/dict 单测运行 → 失败数必须为 0
#
#   pwsh -NoProfile -File modules\dict\tests\run.ps1 [-Quiet]
param([switch]$Quiet)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe  = Join-Path $here 'out\dict_unit_test.exe'
if (-not (Test-Path $exe)) { throw "找不到 $exe（先跑 modules\dict\tests\build.ps1）" }

if ($Quiet) { & $exe | Out-Null }
else { & $exe }
$code = $LASTEXITCODE
Write-Host "=== dict_unit_test exit code: $code ==="
if ($code -ne 0) { throw "dict 单测失败（$code）" }
