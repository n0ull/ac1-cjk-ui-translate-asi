# modules/glyph 单测构建 → tests\out\glyph_unit_test.exe
#
#   pwsh -NoProfile -File modules\glyph\tests\build.ps1
#
# Private resource seam compiles the production runtime implementation with synthetic inputs.
param([string[]]$Defines = @())

$ErrorActionPreference = 'Stop'

$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$mod   = Split-Path -Parent $here                 # modules\glyph
$mods  = Split-Path -Parent $mod                  # modules
$out   = Join-Path $here 'out'
if (-not (Test-Path $out)) { New-Item -ItemType Directory $out | Out-Null }

foreach ($lib in @('core')) {
    $l = Join-Path $mods "$lib\out\$lib.lib"
    if (-not (Test-Path $l)) { throw "缺少 $l（先构建 $lib 模块）" }
}

$root = Split-Path -Parent (Split-Path -Parent $mod)
. "$root/tools/modules/native-toolchain.ps1"

$flags = @('/nologo','/O2','/MT','/EHsc','/W3','/utf-8','/D_CRT_SECURE_NO_WARNINGS')

Push-Location $here
try {
    & cl.exe $flags @Defines `
        "/I$mod\include" "/I$(Join-Path $mods 'core\include')" `
        glyph_unit_test.cpp `
        /link "$(Join-Path $mods 'core\out\core.lib')" kernel32.lib `
        "/OUT:$out\glyph_unit_test.exe"
    Write-Host "=== cl exit code: $LASTEXITCODE ==="
    if ($LASTEXITCODE -ne 0) { throw "glyph 单测: 编译/链接失败" }
    $f = Get-Item "$out\glyph_unit_test.exe"
    Write-Host ("产物: glyph_unit_test.exe  {0} 字节" -f $f.Length)
} finally { Pop-Location }
