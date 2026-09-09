# 建置 Braille Typewriter（Windows，MSYS2 ucrt64 的 g++，靜態連結，單一 exe）。
# 用法：powershell -ExecutionPolicy Bypass -File build.ps1 [-Test]
param([switch]$Test)
$ErrorActionPreference = 'Stop'
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
if (-not $env:TEMP -or $env:TEMP -like 'C:\WINDOWS*') { $env:TEMP = "$env:LOCALAPPDATA\Temp"; $env:TMP = $env:TEMP }
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force -Path build | Out-Null
$common = @('-std=c++17', '-O2', '-municode', '-I', 'src', '-static')
& g++ @common -mwindows src\braillekey.cpp src\brl_engine.cpp src\jaws_com.cpp -lole32 -loleaut32 -luuid -loleacc -limm32 -lwinmm -lshell32 -o build\BrailleTypewriter.exe
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
Write-Host ("build\BrailleTypewriter.exe  SHA-256 " + (Get-FileHash build\BrailleTypewriter.exe -Algorithm SHA256).Hash.Substring(0, 12))
if ($Test) {
    & g++ @common tools\brl_test.cpp src\brl_engine.cpp -o build\brl_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'test build failed' }
    & build\brl_test.exe | Select-Object -Last 3
}
