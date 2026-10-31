param([string]$OutDir = '')
$ErrorActionPreference = 'Stop'
$production = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $OutDir) { $OutDir = Join-Path $production 'build/runtime-oracle' }
if (-not [IO.Path]::IsPathRooted($OutDir)) { $OutDir = Join-Path (Get-Location).Path $OutDir }
$outPath = [IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Path $outPath -Force | Out-Null
$receipt = Join-Path $outPath 'dictionary_oracle.json'
# A failed attempt must not leave the previous build's success receipt beside its output.
if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt }
$sourceNames = @(
    'tools/modules/runtime/dictionary_oracle.cpp', 'modules/dict/src/dict.cpp',
    'modules/core/src/file.cpp', 'modules/core/src/str.cpp'
)
# Include the actual headers and build recipe, so changing production semantics
# or compiler options makes the Python consumers reject a stale executable.
$provenanceNames = $sourceNames + @(
    'tools/modules/build_runtime_oracle.ps1', 'tools/modules/native-toolchain.ps1', 'modules/dict/include/ac1/dict/dict.h',
    'modules/core/include/ac1/core/file.h',
    'modules/core/include/ac1/core/str.h'
)
$fingerprints = [ordered]@{}
foreach ($name in $provenanceNames) {
    $fingerprints[$name] = (Get-FileHash -LiteralPath (Join-Path $production $name) -Algorithm SHA256).Hash.ToLowerInvariant()
}
. "$PSScriptRoot/native-toolchain.ps1"
$sources = @($sourceNames | ForEach-Object { Join-Path $production $_ })
$executable = Join-Path $outPath 'dictionary_oracle.exe'
& cl.exe /nologo /O2 /MT /EHsc /W4 /WX /utf-8 /Zc:preprocessor /D_CRT_SECURE_NO_WARNINGS "/I$production\modules\dict\include" "/I$production\modules\core\include" "/Fo$outPath\\" "/Fe$executable" $sources kernel32.lib
if ($LASTEXITCODE -ne 0) { throw 'Production dictionary oracle compilation failed' }
foreach ($name in $provenanceNames) {
    $current = (Get-FileHash -LiteralPath (Join-Path $production $name) -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($current -ne $fingerprints[$name]) { throw "Oracle source changed during build: $name" }
}
[ordered]@{
    format_version = 1; target = 'x86'; msvc = '14.44.35207'; sdk = $sdkVersion
    flags = '/O2 /MT /EHsc /W4 /WX /utf-8 /Zc:preprocessor /D_CRT_SECURE_NO_WARNINGS'
    exe_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
    sources = $fingerprints
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $receipt -Encoding utf8
Write-Output "Built current production dictionary oracle: $executable"
