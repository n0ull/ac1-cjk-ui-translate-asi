# modules/core —— 独立构建 → out\core.lib
#
#   pwsh -NoProfile -File modules\core\build.ps1
#
# core 依赖：无（只用 Win32 + CRT + vendored MinHook）。本脚本刻意不引用任何公共环境脚本 ——
# 每个模块都必须能被单独拎出去编，所以 cl 环境这段在这里是自包含的。
#
# MinHook（BSD，third_party/minhook/）在本模块编译进 core.lib，全仓库唯一一份。
#   它是进程全局单例（g_hooks 一张表、MH_Initialize 全进程只成功一次）；
#   任何别的模块再编一份 = 两张互不可见的表，谁都看不见对方改过的字节。
param([string[]]$Defines = @(), [string]$OutDir = '')

$ErrorActionPreference = 'Stop'
$VC  = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
$SDK = 'C:\Program Files (x86)\Windows Kits\10'
$V   = '10.0.26100.0'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $OutDir) { $OutDir = Join-Path $here 'out' }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory $OutDir | Out-Null }

# MinHook（BSD）只在本模块编译进 core.lib —— 全仓库唯一一份。
#   来源：git submodule third_party/minhook → github.com/TsudaKageyu/minhook
#   （钉在 8af6b4a / v1.3.4-17）。不要改它，要改就改上游或换 pin。
#   只编 x86 需要的 4 个 .c（buffer/hook/trampoline/hde32）。
#   它是进程全局单例（g_hooks 一张表、MH_Initialize 全进程只成功一次）；
#   任何别的模块再编一份 = 两张互不可见的表，谁都看不见对方改过的字节。
$root    = Split-Path -Parent (Split-Path -Parent $here)   # 仓库根
$mh      = Join-Path $root 'third_party\minhook'
$mhInc   = Join-Path $mh   'include'
$mhSrc   = Join-Path $mh   'src'
$mhHde   = Join-Path $mhSrc 'hde'
$mhFiles = @("$mhSrc\buffer.c","$mhSrc\hook.c","$mhSrc\trampoline.c","$mhHde\hde32.c")
foreach ($p in @($mhFiles, $mhInc, "$mhSrc\buffer.h", "$mhSrc\trampoline.h", "$mhHde\hde32.h", "$mhHde\pstdint.h", "$mhHde\table32.h")) {
    if (-not (Test-Path $p)) {
        throw @"
MinHook 缺文件：$p

third_party\minhook 是 **git submodule**，全新 clone 后是空目录。
先在仓库根执行：
    git submodule update --init --recursive
"@
    }
}

$env:PATH    = "$VC\bin\Hostx64\x86;$env:PATH"
$env:INCLUDE = "$VC\include;$SDK\Include\$V\ucrt;$SDK\Include\$V\shared;$SDK\Include\$V\um;$SDK\Include\$V\winrt"
$env:LIB     = "$VC\lib\x86;$SDK\Lib\$V\ucrt\x86;$SDK\Lib\$V\um\x86"

# 只编本模块自己的 src/，公开头在 include/。不会碰其它模块的任何文件。
$sources = @('src\log.cpp', 'src\mem.cpp', 'src\hook.cpp', 'src\host.cpp', 'src\str.cpp', 'src\ctr.cpp', 'src\file.cpp', 'src\trace.cpp')
$flags = @('/nologo','/c','/O2','/MT','/EHsc','/W3','/utf-8','/GS','/D_CRT_SECURE_NO_WARNINGS')

Push-Location $here
try {
    Remove-Item -ErrorAction SilentlyContinue "$OutDir\*"
    # hook.cpp 要 include "MinHook.h" ⇒ 把 third_party 的 include 加进来。
    & cl.exe $flags @Defines "/I$here\include" "/I$mhInc" "/Fo$OutDir\" $sources
    Write-Host "=== cl exit code: $LASTEXITCODE ==="
    if ($LASTEXITCODE -ne 0) { throw "core: cl 失败" }

    # MinHook 的 4 个 .c 编进本模块 ⇒ 全仓库只有一份 MinHook 实现。
    # （MinHook 是 C，hook.cpp 是 C++，所以分两轮编，各自的 /Fo 不能撞。）
    $n = 1
    foreach ($m in $mhFiles) {
        $o = Join-Path $OutDir ("mh{0}.obj" -f $n); $n++
        & cl.exe '/nologo' '/c' '/O2' '/MT' '/GS' "/I$mhInc" "/I$mhSrc" "/I$mhHde" "/Fo$o" $m
        if ($LASTEXITCODE -ne 0) { throw "core: MinHook 编译失败（$m）" }
    }

    & lib.exe /nologo "/OUT:$OutDir\core.lib" "$OutDir\*.obj"
    Write-Host "=== lib exit code: $LASTEXITCODE ==="
    if ($LASTEXITCODE -ne 0) { throw "core: lib 失败" }

    $f = Get-Item "$OutDir\core.lib"
    Write-Host ("产物: core.lib  {0} 字节  本模块源文件 {1} 个 + MinHook 4 个 .c" -f $f.Length, $sources.Count)
} finally { Pop-Location }
