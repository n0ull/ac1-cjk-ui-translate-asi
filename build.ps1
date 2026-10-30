# 按依赖序调度模块构建，再装配运行时ASI；不混编各模块源码。
#   pwsh -NoProfile -File build.ps1 -RunTests
#   pwsh -NoProfile -File build.ps1 -CheckSwitches
# -Defines透传给dict/text/glyph/app及单测，原地重编共享库；使用后需重新构建默认模式。
# -OutDir/-AsiName只改变最终ASI输出，不能隔离模块库；隔离诊断使用-CheckSwitches。
param(
    [switch]$RunTests,
    [switch]$CheckSwitches,
    [string[]]$Defines = @(),
    [string]$OutDir = '',                 # 最终 .asi 的输出目录（空 = app\out）
    [string]$AsiName = 'ac1_runtime_package.asi'      # 最终 .asi 的文件名
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

# 同时接受PowerShell数组和其它shell传入的逗号串。
$Defines = @(
    foreach ($d in $Defines) {
        foreach ($part in ([string]$d -split ',')) {
            $t = $part.Trim().Trim('"').Trim("'")
            if ($t) { $t }
        }
    }
)
if ($Defines.Count -gt 0) {
    Write-Host "开关变体构建：-Defines $($Defines -join ' ')"
}

# 子脚本会切换目录，输出路径必须在调度前固定。
if ($OutDir) {
    if (-not [System.IO.Path]::IsPathRooted($OutDir)) { $OutDir = Join-Path $root $OutDir }
    $OutDir = [System.IO.Path]::GetFullPath($OutDir)
}

function Invoke-BuildStep([string]$title, [string]$script, [hashtable]$Parameters = @{}) {
    Write-Host ""
    Write-Host "──────────────────────────────────────────────────────────────"
    Write-Host "==> $title"
    if ($Parameters.Defines) { Write-Host "    （-Defines: $($Parameters.Defines -join ' ')）" }
    Write-Host "──────────────────────────────────────────────────────────────"
    & (Join-Path $root $script) @Parameters
    if ($LASTEXITCODE -ne 0) { throw "$title 失败（$LASTEXITCODE）" }
}

# ---- 0. 依赖规则检查（规则 1/2 的机器强制，不靠自觉）----
#   · 模块只能 include 自己 + 白名单里的其它模块的公开头（ac1/<层>/*.h）
#   · 任何 include 命中 <层>\src\ 一律违规
#   · app 只能 include 四个模块的公开头 + 系统头
function Assert-Includes {
    param([string]$Label, [string]$SrcDir, [string[]]$Allowed)

    $files = @(Get-ChildItem -Path $SrcDir -Filter *.cpp -Recurse -ErrorAction SilentlyContinue)
    foreach ($f in $files) {
        $lines = Get-Content -LiteralPath $f.FullName
        foreach ($line in $lines) {
            # (a) 禁止 include 任何模块的 src/ 下的东西
            if ($line -match '#\s*include\s*[<"]([^">]*[\\/]src[\\/][^">]*)[">]') {
                throw ("依赖违规：{0} include 了某个模块的 src/ 内部文件：{1}" -f $f.Name, $Matches[1])
            }
            # (b) 任何引用到 ac1/ 公开头的 include 都必须在白名单里。
            #     ★ 用 '.*ac1/' 而不是 '^ac1/'：写成 ../../core/include/ac1/core/log.h
            #       也能解析成功，但它同样是"绕过模块边界"，一样要拦。
            if ($line -match '#\s*include\s*"([^"]*ac1/[^"]+)"') {
                $full = $Matches[1]
                $h = $full.Substring($full.IndexOf('ac1/'))       # 归一成 ac1/<层>/<头>.h
                $ok = $false
                foreach ($a in $Allowed) { if ($h -like "$a/*") { $ok = $true; break } }
                if (-not $ok) {
                    throw ("依赖违规：{0} include 了未授权的模块公开头：{1}（归一为 {2}；允许：{3}）" -f `
                           $f.Name, $full, $h, ($Allowed -join ', '))
                }
                if ($full -ne $h) {
                    throw ("依赖违规：{0} 必须用 'ac1/<层>/<头>.h' 的形式 include，不能带相对前缀：{1}" -f $f.Name, $full)
                }
            }
        }
    }
    Write-Host ("  [OK] {0}：{1} 个源文件，include 全部合法" -f $Label, $files.Count)
}

Write-Host "==> 0/5 依赖规则检查"
Assert-Includes -Label 'core ' -SrcDir (Join-Path $root 'modules\core\src')   -Allowed @('ac1/core')
#   · dict：只用 core 的纯 Win32 原语（file.h / str.h），不碰 core 的 log.h
#     —— 日志仍然是 set_log(LogFn) 注入，那才是解耦的全部理由。
Assert-Includes -Label 'dict ' -SrcDir (Join-Path $root 'modules\dict\src')   -Allowed @('ac1/dict','ac1/core')
Assert-Includes -Label 'text ' -SrcDir (Join-Path $root 'modules\text\src')   -Allowed @('ac1/text','ac1/core','ac1/dict')
Assert-Includes -Label 'glyph' -SrcDir (Join-Path $root 'modules\glyph\src')  -Allowed @('ac1/glyph','ac1/core')
Assert-Includes -Label 'app  ' -SrcDir (Join-Path $root 'app\src')             -Allowed @('ac1/core','ac1/dict','ac1/text','ac1/glyph')

# core没有编译开关；其它模块及其单测使用同一份参数。
$moduleArgs = @{ Defines = $Defines }
Invoke-BuildStep "1/5 构建模块 core（依赖：无）"          'modules\core\build.ps1'
Invoke-BuildStep "2/5 构建模块 dict（依赖：core）"        'modules\dict\build.ps1' $moduleArgs
Invoke-BuildStep "3/5 构建模块 text（依赖：core + dict）" 'modules\text\build.ps1' $moduleArgs
Invoke-BuildStep "4/5 构建模块 glyph（依赖：core）"       'modules\glyph\build.ps1' $moduleArgs
# ---- 装配层 ----
$appArgs = @{ Defines = $Defines }
if ($OutDir) { $appArgs['OutDir'] = $OutDir }
$appArgs['AsiName'] = $AsiName
Invoke-BuildStep "5/5 装配 app => $AsiName" 'app\build.ps1' $appArgs

# ---- 2. 单测 ----
if ($RunTests) {
    Write-Host ""
    Write-Host "──────────────────────────────────────────────────────────────"
    Write-Host "==> 单测（四个模块各自独立运行）"
    Write-Host "──────────────────────────────────────────────────────────────"
    Invoke-BuildStep "单测 core"  'modules\core\tests\build.ps1'
    Invoke-BuildStep "单测 dict"  'modules\dict\tests\build.ps1' $moduleArgs
    Invoke-BuildStep "单测 text"  'modules\text\tests\build.ps1' $moduleArgs
    Invoke-BuildStep "单测 glyph" 'modules\glyph\tests\build.ps1' $moduleArgs
    foreach ($m in @('core','dict','text','glyph')) {
        Invoke-BuildStep "运行 $m 单测" "modules\$m\tests\run.ps1"
    }
    Invoke-BuildStep "单测 runtime loader startup" 'app\tests\build.ps1'
}

# 两个开关仅影响text；静态库不链接依赖，因此可在临时目录验证四组合而不重编其它模块。
if ($CheckSwitches) {
    Write-Host ""
    Write-Host "──────────────────────────────────────────────────────────────"
    Write-Host "==> 开关矩阵编译检查"
    Write-Host "──────────────────────────────────────────────────────────────"
    $tmp = [IO.Path]::GetFullPath((Join-Path $root ('tools/out/switchcheck-' + [guid]::NewGuid().ToString('N'))))
    $expectedParent = [IO.Path]::GetFullPath((Join-Path $root 'tools/out'))
    if ([IO.Path]::GetDirectoryName($tmp) -ne $expectedParent) { throw 'Unexpected switch-check directory' }
    New-Item -ItemType Directory $tmp | Out-Null
    $combos = @(
        @{ n = '默认（TEXT_REPLACE=1）'; d = @() },
        @{ n = 'TEXT_REPLACE=0';        d = @('-DTEXT_REPLACE=0') },
        @{ n = 'SAMPLE_N=8';            d = @('-DSAMPLE_N=8') },
        @{ n = '观测 SAMPLE_N=8';       d = @('-DTEXT_REPLACE=0','-DSAMPLE_N=8') }
    )
    $bad = 0
    foreach ($c in $combos) {
        & (Join-Path $root 'modules\text\build.ps1') -Defines $c.d -OutDir "$tmp\text" | Out-Null
        $e = $LASTEXITCODE
        if ($e -ne 0) { $bad++ }
        Write-Host ("  [{0,-32}] text={1}" -f $c.n, $e)
    }
    Remove-Item -LiteralPath $tmp -Recurse -Force
    Write-Host ("=== 开关矩阵: text {0}/{1} 组合编译通过 ===" -f ($combos.Count - $bad), $combos.Count)
    if ($bad) { throw "有开关组合编不过" }
}

# 链接器成功后仍核对产物时间，防止旧ASI被误报为本次结果。
$asi = Join-Path $(if ($OutDir) { $OutDir } else { Join-Path $root 'app\out' }) $AsiName
Write-Host ""
Write-Host "================================================================"
if (-not (Test-Path $asi)) { throw "产物缺失：$asi" }
$f = Get-Item $asi
$builtAfter = (Get-Process -Id $PID).StartTime.ToUniversalTime()
$obj = Join-Path (Split-Path -Parent $asi) 'dllmain.obj'
if (Test-Path $obj) {
    $objTime = (Get-Item $obj).LastWriteTimeUtc
    if ($objTime -gt $builtAfter) { $builtAfter = $objTime }
}
if ($f.LastWriteTimeUtc -lt $builtAfter) {
    throw ("产物过期：{0}`n产物时间：{1:u}`n最早有效时间：{2:u}（进程启动 / dllmain.obj 取晚者）" -f `
           $asi, $f.LastWriteTimeUtc, $builtAfter)
}
$sha = (Get-FileHash $asi -Algorithm SHA256).Hash
Write-Host ("完成：{0}  {1} 字节  sha256={2}" -f $f.FullName, $f.Length, $sha)
if ($Defines.Count -gt 0) {
    Write-Host ("本次为开关变体：-Defines {0}" -f ($Defines -join ' '))
    Write-Host "（变体会重编 modules\<层>\out\ 里的 .lib；要回到默认配置，再跑一次不带 -Defines 的 build.ps1）"
}
Write-Host "（sha256 每次编译都会变：PE 头时间戳。比对产物请用字节数 / dumpbin /headers）"
