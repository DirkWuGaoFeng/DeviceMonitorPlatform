# serial_check.ps1 — 真机链路 Windows 侧验收: 打开板子串口, 前台跑裸串口监控端
#
# 作用: 列出本机 COM 口 -> 确保 monitor_serial.exe 已构建(缺则用 MinGW g++ 现编) ->
#       前台运行它读真实 STM32 帧流, 终端滚动显示四通道值 + ok/crcErr/drop + 危急值告警。
#       看到 ok 递增、CRC 不误报, 即证明"固件帧内核 <-> 上位机解码"跨端字节级打通。
#
# 用法:
#   .\serial_check.ps1                 # 自动挑第一个 COM 口, 115200
#   .\serial_check.ps1 -Port COM4      # 指定口
#   .\serial_check.ps1 -Port COM4 -Csv history.csv   # 顺带导出历史
param(
    [string]$Port,
    [int]   $Baud = 115200,
    [string]$Csv,
    [string]$Gxx  = 'G:\Software\Qt\Tools\mingw1310_64\bin\g++.exe'
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# ---- 列出串口 ----
$ports = [System.IO.Ports.SerialPort]::GetPortNames()
Write-Host ("本机串口: {0}" -f ($(if($ports){$ports -join ', '}else{'未检测到 (板子未插/驱动未装?)'}))) -ForegroundColor Cyan
if (-not $Port) {
    if (-not $ports -or $ports.Count -eq 0) { Write-Host "[错误] 无可用串口。插好 USB 转串口并装 CH340/CP210x 驱动。" -ForegroundColor Red; exit 1 }
    $Port = $ports[0]
    Write-Host ("       自动选用 {0} (要换口用 -Port)" -f $Port) -ForegroundColor DarkGray
}

# ---- 确保 monitor_serial.exe 存在, 缺则现编 ----
$exe = Join-Path $root 'build\monitor_serial.exe'
if (-not (Test-Path $exe)) {
    Write-Host "[构建] monitor_serial.exe 不存在, 用 g++ 现编 ..." -ForegroundColor Yellow
    if (-not (Test-Path $Gxx)) { Write-Host "[错误] 找不到编译器 $Gxx (用 -Gxx 指定)" -ForegroundColor Red; exit 1 }
    New-Item -ItemType Directory -Force (Join-Path $root 'build') | Out-Null
    & $Gxx -std=c++17 -pthread -I "$root\include" "$root\src\monitor_serial.cpp" -o $exe
    if ($LASTEXITCODE -ne 0) { Write-Host "[错误] 编译失败" -ForegroundColor Red; exit 1 }
    Write-Host "[构建] OK -> $exe" -ForegroundColor Green
}

# ---- 前台运行 (Ctrl+C 退出) ----
$argv = @($Port, "$Baud")
if ($Csv) { $argv += @('--csv', $Csv) }
Write-Host ("`n== 运行: monitor_serial.exe {0}  (Ctrl+C 退出) ==`n" -f ($argv -join ' ')) -ForegroundColor Cyan
& $exe @argv
