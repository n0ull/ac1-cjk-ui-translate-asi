# modules/dict 单测构建 → tests\out\dict_unit_test.exe
#
#   pwsh -NoProfile -File modules\dict\tests\build.ps1
#
# 链接 dict.lib + core.lib（dict 用 core 的 file.h/str.h 原语），不链 text.lib / glyph.lib / 游戏 dll。
param([string[]]$Defines = @())

$ErrorActionPreference = 'Stop'
$VC  = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
$SDK = 'C:\Program Files (x86)\Windows Kits\10'
$V   = '10.0.26100.0'

$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$mod   = Split-Path -Parent $here                 # modules\dict
$out   = Join-Path $here 'out'
if (-not (Test-Path $out)) { New-Item -ItemType Directory $out | Out-Null }

$env:PATH    = "$VC\bin\Hostx64\x86;$env:PATH"
$env:INCLUDE = "$VC\include;$SDK\Include\$V\ucrt;$SDK\Include\$V\shared;$SDK\Include\$V\um;$SDK\Include\$V\winrt"
$env:LIB     = "$VC\lib\x86;$SDK\Lib\$V\ucrt\x86;$SDK\Lib\$V\um\x86"

$flags = @('/nologo','/O2','/MT','/EHsc','/W3','/utf-8','/D_CRT_SECURE_NO_WARNINGS')

Push-Location $here
try {
    Remove-Item -ErrorAction SilentlyContinue "$out\*" -Recurse -Force   # -Recurse：单测会落子目录（AC1_CJK 日志目录等）
    # 多链一个 core.lib：本模块用 core 的纯 Win32 原语（file.h / str.h）。
    #   仍然不链 core 的日志用法：dict 的日志出口是 set_log(LogFn) 注入，
    #   单测注入自己的 printf 出口并断言它被调用 —— 这条不变式没有被削弱。
    $coreLib = Join-Path (Split-Path -Parent $mod) 'core\out\core.lib'
    if (-not (Test-Path $coreLib)) { throw "dict 单测: 找不到 $coreLib（先跑 modules\core\build.ps1）" }
    & cl.exe $flags @Defines "/I$mod\include" "/I$(Join-Path (Split-Path -Parent $mod) 'core\include')" dict_unit_test.cpp "/link" "$mod\out\dict.lib" $coreLib kernel32.lib "/OUT:$out\dict_unit_test.exe"
    Write-Host "=== cl exit code: $LASTEXITCODE ==="
    if ($LASTEXITCODE -ne 0) { throw "dict 单测: 编译/链接失败" }
    $f = Get-Item "$out\dict_unit_test.exe"
    Write-Host ("产物: dict_unit_test.exe  {0} 字节" -f $f.Length)
} finally { Pop-Location }
