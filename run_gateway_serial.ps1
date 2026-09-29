# run_gateway_serial.ps1 — 起真机遥测网关: STM32(串口) -> TCP :9100 (文本行 API + RAW 透传)
# 用法: .\run_gateway_serial.ps1 [-Port COM4] [-Listen 9100]
# 下游: WSL 侧 bash e2e_realdevice.sh <windows_ip>  (dmp_grpc_server 连它做全链路)
param([string]$Port = 'COM4', [int]$Listen = 9100)
$ErrorActionPreference = 'Stop'
$root = 'E:\Work\McuProject\DeviceMonitorPlatform'
$exe  = Join-Path $root 'build\gateway_service.exe'
if (-not (Test-Path $exe)) { throw "未找到 $exe ，请先编译 (见 docs/真机链路-STM32到上位机.md 附录)" }

# 释放 COM4: 停掉仍直连串口的工具 (monitor_serial)。
# T1.2 后 qt_monitor 可选「网关 (TCP RAW)」经本机 9100 取数、不再占 COM4, 故不在此互杀名单内。
foreach ($n in 'monitor_serial') {
    $p = Get-Process $n -ErrorAction SilentlyContinue
    if ($p) { Stop-Process -Id $p.Id -Force; "已停止 $n (pid=$($p.Id)) 释放 $Port" }
}
# 运行时 DLL 已随 build\gateway_service.exe 同目录部署; 若缺失则补 PATH
if (-not (Test-Path (Join-Path $root 'build\libstdc++-6.dll'))) {
    $env:PATH = 'G:\Software\Qt\Tools\mingw1310_64\bin;' + $env:PATH
}
"网关启动中: $Port -> TCP :$Listen (Ctrl+C 停止)"
& $exe $Listen --serial $Port
