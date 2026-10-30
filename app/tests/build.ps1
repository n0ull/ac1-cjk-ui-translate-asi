param()
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$outDir = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
. "$root/tools/modules/native-toolchain.ps1"
$includes = @('core','dict','glyph','text') | ForEach-Object { "/I$root/modules/$_/include" }
$exe = Join-Path $outDir 'runtime_startup_test.exe'
& cl.exe /nologo /O2 /MT /EHsc /W4 /WX /utf-8 $includes "/Fo$outDir/runtime_startup_test.obj" "/Fe$exe" (Join-Path $PSScriptRoot 'runtime_startup_test.cpp') kernel32.lib
if ($LASTEXITCODE -ne 0) { throw 'Runtime startup replay compilation failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Runtime startup replay failed' }
& $exe --reject-package
if ($LASTEXITCODE -ne 0) { throw 'Runtime rejected-package startup replay failed' }
& $exe --dx10
if ($LASTEXITCODE -ne 0) { throw 'Dx10 runtime startup replay failed' }
& $exe --dx10 --reject-package
if ($LASTEXITCODE -ne 0) { throw 'Dx10 rejected-package startup replay failed' }
& $exe --reject-dictionary
if ($LASTEXITCODE -ne 0) { throw 'Rejected-dictionary startup replay failed' }
& $exe --dx10 --reject-dictionary
if ($LASTEXITCODE -ne 0) { throw 'Dx10 rejected-dictionary startup replay failed' }
& $exe --partial-dictionary
if ($LASTEXITCODE -ne 0) { throw 'Partial-dictionary startup replay failed' }
& $exe --dx10 --partial-dictionary
if ($LASTEXITCODE -ne 0) { throw 'Dx10 partial-dictionary startup replay failed' }
