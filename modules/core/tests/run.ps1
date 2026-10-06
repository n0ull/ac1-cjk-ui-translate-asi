# modules/core 单测运行 → 失败数必须为 0
#
#   pwsh -NoProfile -File modules\core\tests\run.ps1
param([switch]$Quiet)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe  = Join-Path $here 'out\core_unit_test.exe'
if (-not (Test-Path $exe)) { throw "找不到 $exe（先跑 modules\core\tests\build.ps1）" }

if (-not $Quiet) { & $exe } else { & $exe | Out-Null }
$code = $LASTEXITCODE
Write-Host "=== core_unit_test exit code: $code ==="
if ($code -ne 0) { throw "core 单测失败（$code）" }
