# modules/glyph 单测运行 → 失败数必须为 0
#
#   pwsh -NoProfile -File modules\glyph\tests\run.ps1
param([switch]$Quiet)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe  = Join-Path $here 'out\glyph_unit_test.exe'
if (-not (Test-Path $exe)) { throw "找不到 $exe（先跑 modules\glyph\tests\build.ps1）" }

if ($Quiet) { & $exe | Out-Null } else { & $exe }
$code = $LASTEXITCODE
Write-Host "=== glyph_unit_test exit code: $code ==="
if ($code -ne 0) { throw "glyph 单测失败（$code）" }
