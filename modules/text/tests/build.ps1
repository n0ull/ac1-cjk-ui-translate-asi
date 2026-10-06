# modules/text 单测构建 → tests\out\text_unit_test.exe
#
#   pwsh -NoProfile -File modules\text\tests\build.ps1
#
# 链接 core.lib + dict.lib + text.lib（不链接 glyph.lib，不链接任何游戏 dll）。
param([string[]]$Defines = @())

$ErrorActionPreference = 'Stop'
$VC  = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
$SDK = 'C:\Program Files (x86)\Windows Kits\10'
$V   = '10.0.26100.0'

$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$mod   = Split-Path -Parent $here                 # modules\text
$mods  = Split-Path -Parent $mod                  # modules
$out   = Join-Path $here 'out'
if (-not (Test-Path $out)) { New-Item -ItemType Directory $out | Out-Null }

foreach ($lib in @('core','dict','text')) {
    $l = Join-Path $mods "$lib\out\$lib.lib"
    if (-not (Test-Path $l)) { throw "缺少 $l（先构建 $lib 模块）" }
}

$env:PATH    = "$VC\bin\Hostx64\x86;$env:PATH"
$env:INCLUDE = "$VC\include;$SDK\Include\$V\ucrt;$SDK\Include\$V\shared;$SDK\Include\$V\um;$SDK\Include\$V\winrt"
$env:LIB     = "$VC\lib\x86;$SDK\Lib\$V\ucrt\x86;$SDK\Lib\$V\um\x86"

$flags = @('/nologo','/O2','/MT','/EHsc','/W3','/utf-8','/D_CRT_SECURE_NO_WARNINGS')

Push-Location $here
try {
    Remove-Item -ErrorAction SilentlyContinue "$out\*" -Recurse -Force   # -Recurse：单测会落子目录（AC1_CJK 日志目录等）
    & cl.exe $flags @Defines `
        "/I$mod\include" "/I$(Join-Path $mods 'core\include')" "/I$(Join-Path $mods 'dict\include')" `
        text_unit_test.cpp `
        /link "$(Join-Path $mods 'core\out\core.lib')" "$(Join-Path $mods 'dict\out\dict.lib')" `
              "$(Join-Path $mods 'text\out\text.lib')" kernel32.lib `
        "/OUT:$out\text_unit_test.exe"
    Write-Host "=== cl exit code: $LASTEXITCODE ==="
    if ($LASTEXITCODE -ne 0) { throw "text 单测: 编译/链接失败" }
    $f = Get-Item "$out\text_unit_test.exe"
    Write-Host ("产物: text_unit_test.exe  {0} 字节" -f $f.Length)
} finally { Pop-Location }
