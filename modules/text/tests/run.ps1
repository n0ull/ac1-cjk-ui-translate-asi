# modules/text 单测运行 → 失败数必须为 0
#
#   pwsh -NoProfile -File modules\text\tests\run.ps1
param([switch]$Quiet)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe  = Join-Path $here 'out\text_unit_test.exe'
if (-not (Test-Path $exe)) { throw "找不到 $exe（先跑 modules\text\tests\build.ps1）" }

if ($Quiet) { & $exe | Out-Null } else { & $exe }
$code = $LASTEXITCODE
Write-Host "=== text_unit_test exit code: $code ==="
if ($code -ne 0) { throw "text 单测失败（$code）" }

$ioExe = Join-Path $here 'out\inventory_io_test.exe'
if (-not (Test-Path $ioExe)) { throw "找不到 $ioExe（先跑 modules\text\tests\build.ps1）" }
foreach ($case in @('header-fail', 'entry-fail', 'header-partial', 'entry-partial', 'short-write',
                    'zero-write', 'repeat-fail', 'registration', 'open-fail')) {
    if ($Quiet) { & $ioExe $case | Out-Null } else { & $ioExe $case }
    if ($LASTEXITCODE -ne 0) { throw "inventory I/O 单测失败：$case（$LASTEXITCODE）" }
}
