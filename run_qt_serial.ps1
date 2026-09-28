# run_qt_serial.ps1 — 一键启动 Qt GUI 并连接真实 STM32 串口数据
# 用法: .\run_qt_serial.ps1 [-Port COM4] [-Baud 115200]
param([string]$Port = 'COM4', [int]$Baud = 115200)
$ErrorActionPreference = 'Stop'
$root  = 'E:\Work\McuProject\DeviceMonitorPlatform'
$QtDir = 'G:\Software\Qt\6.10.0\mingw_64'
$Mingw = 'G:\Software\Qt\Tools\mingw1310_64\bin'
$exe   = Join-Path $root 'build_qt\qt_monitor.exe'

if (-not (Test-Path $exe)) { throw "未找到 $exe ，请先运行 build_qt.ps1" }

# COM 口独占：monitor_serial 若占用则先停掉（只杀自己工具链的进程）
$p = Get-Process monitor_serial -ErrorAction SilentlyContinue
if ($p) { Stop-Process -Id $p.Id -Force; Start-Sleep -Milliseconds 300; "已停止占用串口的 monitor_serial (pid=$($p.Id))" }

# Qt 运行时 DLL 通过 PATH 提供
$env:PATH = "$Mingw;$QtDir\bin;$env:PATH"
Start-Process -FilePath $exe -ArgumentList "--demo-serial $Port" -WorkingDirectory (Split-Path $exe)
"GUI 已启动: qt_monitor --demo-serial $Port @ $Baud"
"提示: 源下拉框选『串口 (STM32)』可手动切换；状态栏应显示 已连接 串口 $Port@$Baud 且 ok 计数增长。"
