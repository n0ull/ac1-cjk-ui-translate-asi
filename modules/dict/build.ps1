# modules/dict —— 独立构建 → out\dict.lib
#
#   pwsh -NoProfile -File modules\dict\build.ps1
#
# dict 依赖：无（只用 Win32：CP950 解码 + 文件读取 + 注入式日志出口）。
# 本脚本刻意不引用任何公共环境脚本 —— 每个模块都必须能被单独拎出去编，
# 所以 cl 环境这段在每个模块里都是自包含的。
param([string[]]$Defines = @(), [string]$OutDir = '')

$ErrorActionPreference = 'Stop'
$VC  = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
$SDK = 'C:\Program Files (x86)\Windows Kits\10'
$V   = '10.0.26100.0'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $OutDir) { $OutDir = Join-Path $here 'out' }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory $OutDir | Out-Null }

$env:PATH    = "$VC\bin\Hostx64\x86;$env:PATH"
$env:INCLUDE = "$VC\include;$SDK\Include\$V\ucrt;$SDK\Include\$V\shared;$SDK\Include\$V\um;$SDK\Include\$V\winrt"
$env:LIB     = "$VC\lib\x86;$SDK\Lib\$V\ucrt\x86;$SDK\Lib\$V\um\x86"

# 只编本模块自己的 src/。
# 本模块只用 core 的纯 Win32 原语（file.h 的整文件读取、str.h 的安全累加器），
#   因此把 core 的公开头加进 /I。注意这与「日志」无关：日志仍然走 set_log(LogFn)
#   注入，不 include core 的 log.h。依赖方向是 dict → core（单向、无环）。
$coreInc = Join-Path (Split-Path -Parent (Split-Path -Parent $here)) 'modules\core\include'
$sources = @('src\dict.cpp', 'src\fallback.cpp')
# /Zc:preprocessor：MSVC 的传统预处理器无法正确计算变参个数（实测 DLOG_NARG 恒返回 1），
#                  而 DLOG 的 static_assert 对账要靠它数实参。少了这个开关，检查形同虚设。
# /analyze + /W4  ：让 dlog_impl 的 _Printf_format_string_ 注解真正生效，
#                  抓实参类型不符（C6273）与实参过多（C6271）。
$flags = @('/nologo','/c','/O2','/MT','/EHsc','/W4','/utf-8','/GS','/Zc:preprocessor','/analyze','/D_CRT_SECURE_NO_WARNINGS')

# 格式串相关的诊断码：出现任何一个都让构建失败（不只是一条 warning）
$formatDiag = @('C4473','C4477','C6064','C6065','C6066','C6271','C6273','C28251')

Push-Location $here
try {
    Remove-Item -ErrorAction SilentlyContinue "$OutDir\*"
    $out = & cl.exe $flags @Defines "/I$here\include" "/I$coreInc" "/Fo$OutDir\" $sources 2>&1
    $out | ForEach-Object { Write-Host $_ }
    Write-Host "=== cl exit code: $LASTEXITCODE ==="
    if ($LASTEXITCODE -ne 0) { throw "dict: cl 失败" }

    # 第二道闸：cl 可能"编过了但报了格式串警告"，那种情况同样必须拦住
    $bad = @($out | Select-String -Pattern ($formatDiag -join '|'))
    if ($bad.Count -gt 0) {
        Write-Host ""
        Write-Host "=== 格式串检查失败（命中 $($bad.Count) 条）==="
        $bad | ForEach-Object { Write-Host $_.Line }
        throw "dict: 存在格式串/实参不匹配的诊断（$($formatDiag -join ', ')）"
    }
    Write-Host "=== 格式串检查：0 条命中（$($formatDiag -join ', ')）==="

    & lib.exe /nologo "/OUT:$OutDir\dict.lib" "$OutDir\*.obj"
    Write-Host "=== lib exit code: $LASTEXITCODE ==="
    if ($LASTEXITCODE -ne 0) { throw "dict: lib 失败" }

    $f = Get-Item "$OutDir\dict.lib"
    Write-Host ("产物: dict.lib  {0} 字节  源文件 {1} 个" -f $f.Length, $sources.Count)
} finally { Pop-Location }
