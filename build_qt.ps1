$ErrorActionPreference = 'Continue'
Set-Location 'e:\Work\McuProject\DeviceMonitorPlatform'
$log = 'build\qt_build.txt'
Remove-Item $log -ErrorAction SilentlyContinue

$QtDir   = 'G:\Software\Qt\6.10.0\mingw_64'
$CMake   = 'G:\Software\Qt\Tools\CMake_64\bin\cmake.exe'
$Ninja   = 'G:\Software\Qt\Tools\Ninja\ninja.exe'
$Mingw   = 'G:\Software\Qt\Tools\mingw1310_64\bin'
$env:PATH = "$Mingw;$QtDir\bin;$env:PATH"

# 探测工具链
"cmake=$(Test-Path $CMake) ninja=$(Test-Path $Ninja) qtdir=$(Test-Path $QtDir)" | Add-Content $log
"qtcharts=$(Test-Path "$QtDir\lib\cmake\Qt6Charts") qtsql=$(Test-Path "$QtDir\plugins\sqldrivers\qsqlite.dll")" | Add-Content $log

& $CMake -S . -B build_qt -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER="$Mingw\gcc.exe" `
    -DCMAKE_CXX_COMPILER="$Mingw\g++.exe" `
    -DCMAKE_MAKE_PROGRAM="$Ninja" `
    -DCMAKE_PREFIX_PATH="$QtDir" `
    -DDMP_BUILD_QT=ON *>> $log
"configure_exit=$LASTEXITCODE" | Add-Content $log

if ($LASTEXITCODE -eq 0) {
    & $CMake --build build_qt --target qt_monitor *>> $log
    "build_exit=$LASTEXITCODE" | Add-Content $log
    $exe = 'build_qt\qt_monitor.exe'
    "exe_exists=$(Test-Path $exe) size=$(if(Test-Path $exe){(Get-Item $exe).Length})" | Add-Content $log
}
"---- LAST 40 ----" | Add-Content $log
Get-Content $log -Tail 40 | Add-Content $log
"DONE" | Add-Content $log
