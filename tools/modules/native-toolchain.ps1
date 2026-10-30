# Fixed x86 environment. Each target owns its sources, compiler flags and outputs.
$vc = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
$sdk = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVersion = '10.0.26100.0'
$env:PATH = "$vc\bin\Hostx64\x86;$env:PATH"
$env:INCLUDE = "$vc\include;$sdk\Include\$sdkVersion\ucrt;$sdk\Include\$sdkVersion\shared;$sdk\Include\$sdkVersion\um;$sdk\Include\$sdkVersion\winrt"
$env:LIB = "$vc\lib\x86;$sdk\Lib\$sdkVersion\ucrt\x86;$sdk\Lib\$sdkVersion\um\x86"
