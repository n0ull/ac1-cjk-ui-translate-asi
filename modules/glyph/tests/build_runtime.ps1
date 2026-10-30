param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$GameDir,
    [Parameter(Mandatory)][string]$BoldRecords,
    [Parameter(Mandatory)][string]$TechnoRecords,
    [Parameter(Mandatory)][string]$TextRecords,
    [Parameter(Mandatory)][string]$TitleRecords,
    [Parameter(Mandatory)][string]$OutDir
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$out = [IO.Path]::GetFullPath($OutDir)
$inputs = @($Package, $BoldRecords, $TechnoRecords, $TextRecords, $TitleRecords) | ForEach-Object {
    (Get-Item -LiteralPath $_ -ErrorAction Stop).FullName
}
$hosts = @('dx9','dx10') | ForEach-Object {
    (Get-Item -LiteralPath (Join-Path $GameDir "AssassinsCreed_$_.exe") -ErrorAction Stop).FullName
}
if (Test-Path -LiteralPath $out) { throw "Refusing to overwrite replay directory: $out" }
New-Item -ItemType Directory -Path $out | Out-Null
. "$root/tools/modules/native-toolchain.ps1"
$includes = @('core','dict','text','glyph') | ForEach-Object { "/I$root/modules/$_/include" }
$libraries = @('text','dict','core') | ForEach-Object { Join-Path $root "modules/$_/out/$_.lib" }
$exe = Join-Path $out 'runtime_unit_test.exe'
& cl.exe /nologo /O2 /MT /EHsc /W4 /WX /utf-8 /GS $includes "/Fo$out/runtime_unit_test.obj" "/Fe$exe" (Join-Path $PSScriptRoot 'runtime_unit_test.cpp') $libraries kernel32.lib
if ($LASTEXITCODE -ne 0) { throw 'Runtime resource replay compilation failed' }
for ($index = 0; $index -lt 2; ++$index) {
    $hostName = @('dx9','dx10')[$index]
    & $exe $hostName $hosts[$index] @inputs (Join-Path $out "$hostName.json") *> (Join-Path $out "$hostName.log")
    $code = $LASTEXITCODE
    Get-Content -LiteralPath (Join-Path $out "$hostName.log") -Tail 5
    if ($code -ne 0) { throw "$hostName runtime resource replay failed ($code)" }
}
