# modules/dict 单测构建 → tests\out\dict_unit_test.exe
#
#   pwsh -NoProfile -File modules\dict\tests\build.ps1
#
# 链接 dict.lib + core.lib（dict 用 core 的 file.h/str.h 原语），不链 text.lib / glyph.lib / 游戏 dll。
param([string[]]$Defines = @())

$ErrorActionPreference = 'Stop'

$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$mod   = Split-Path -Parent $here                 # modules\dict
$out   = Join-Path $here 'out'
if (-not (Test-Path $out)) { New-Item -ItemType Directory $out | Out-Null }

$receipt = Join-Path $out 'dictionary_probe.json'
if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt }

$root = Split-Path -Parent (Split-Path -Parent $mod)
$probeSources = @(
    'modules/dict/tests/dictionary_probe.cpp', 'modules/dict/src/dict.cpp',
    'modules/core/src/file.cpp', 'modules/core/src/str.cpp'
)
$probeInputs = $probeSources + @(
    'modules/dict/tests/build.ps1', 'tools/modules/native-toolchain.ps1',
    'modules/dict/include/ac1/dict/dict.h',
    'modules/core/include/ac1/core/file.h',
    'modules/core/include/ac1/core/str.h'
)
$probeHashes = [ordered]@{}
foreach ($name in $probeInputs) {
    $probeHashes[$name] = (Get-FileHash -LiteralPath (Join-Path $root $name) -Algorithm SHA256).Hash.ToLowerInvariant()
}
. "$root/tools/modules/native-toolchain.ps1"

$flags = @('/nologo','/O2','/MT','/EHsc','/W3','/utf-8','/D_CRT_SECURE_NO_WARNINGS')

Push-Location $here
try {
    Remove-Item -ErrorAction SilentlyContinue "$out\*" -Recurse -Force   # -Recurse：单测会落子目录（AC1_CJK 日志目录等）
    # 多链一个 core.lib：本模块用 core 的纯 Win32 原语（file.h / str.h）。
    #   仍然不链 core 的日志用法：dict 的普通/错误日志经 set_log(info,error) 注入，
    #   单测注入自己的 printf 出口并断言它被调用 —— 这条不变式没有被削弱。
    $coreLib = Join-Path (Split-Path -Parent $mod) 'core\out\core.lib'
    if (-not (Test-Path $coreLib)) { throw "dict 单测: 找不到 $coreLib（先跑 modules\core\build.ps1）" }
    & cl.exe $flags @Defines "/I$mod\include" "/I$(Join-Path (Split-Path -Parent $mod) 'core\include')" dict_unit_test.cpp "/link" "$mod\out\dict.lib" $coreLib kernel32.lib "/OUT:$out\dict_unit_test.exe"
    Write-Host "=== cl exit code: $LASTEXITCODE ==="
    if ($LASTEXITCODE -ne 0) { throw "dict 单测: 编译/链接失败" }
    $f = Get-Item "$out\dict_unit_test.exe"
    Write-Host ("产物: dict_unit_test.exe  {0} 字节" -f $f.Length)
    # Compile the observed implementation directly: an old dict.lib must not bless new sources.
    & cl.exe $flags @Defines /Zc:preprocessor "/I$mod\include" "/I$root\modules\core\include" "/Fo$out\" ($probeSources | ForEach-Object { Join-Path $root $_ }) "/link" kernel32.lib "/OUT:$out\dictionary_probe.exe"
    if ($LASTEXITCODE -ne 0) { throw "dict 行为探针: 编译/链接失败" }
    foreach ($name in $probeInputs) {
        $current = (Get-FileHash -LiteralPath (Join-Path $root $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($current -ne $probeHashes[$name]) { throw "dict 行为探针: 构建期间来源变化 $name" }
    }
    [ordered]@{
        format_version = 1; sources = $probeHashes
        exe_sha256 = (Get-FileHash -LiteralPath "$out\dictionary_probe.exe" -Algorithm SHA256).Hash.ToLowerInvariant()
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath "$out\dictionary_probe.json" -Encoding utf8
} finally { Pop-Location }
