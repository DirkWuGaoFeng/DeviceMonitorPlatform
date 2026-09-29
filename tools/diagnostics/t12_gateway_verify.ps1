# tools/diagnostics/t12_gateway_verify.ps1 — T1.2「Qt 经网关 (TCP RAW)」帧计数等式验收（Windows 本地实跑，不进 CI）
#
# 作用: 起 device_simulator(上游 TCP) -> 起 gateway_service(监听 GW_PORT, 上游连 simulator) ->
#       连续跑 N 轮 `qt_monitor --verify-gateway 127.0.0.1 <GW_PORT> <WIN_MS>`，
#       统计 PASS / 精确相等 / 最大|diff|。判据: 网关 STATS.ok 本轮增量 == Qt 本地解码条数, 且 crc_err==0。
#
# 依据(方案 §4 / T1.2 验收): RAW 透传逐字节无损 => 同一条上游帧流, 网关解一份、Qt 解一份, 增量应相等。
#   两条独立 TCP 连接对 5Hz 连续源做非原子采样, 用「双静默快照」(两次网关读数相等且期间 raw 零新字节)
#   把基线/末值锁定在同一批次间隙 -> 实测可做到逐位 diff=0。
#
# 用法: .\tools\diagnostics\t12_gateway_verify.ps1 [-Rounds 20] [-WinMs 3000] [-SimPort 9321] [-GwPort 9322]
#   前置: build_qt\ 下已有 device_simulator.exe / gateway_service.exe / qt_monitor.exe (见 build_qt.ps1)
#   纪律: 按 PID 收尾, 不 pkill; 结论由本轮数据现算。
param([int]$Rounds = 20, [int]$WinMs = 3000, [int]$SimPort = 9321, [int]$GwPort = 9322)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
$env:PATH = 'G:\Software\Qt\6.10.0\mingw_64\bin;G:\Software\Qt\Tools\mingw1310_64\bin;' + $env:PATH

foreach ($e in 'device_simulator.exe','gateway_service.exe','qt_monitor.exe') {
    if (-not (Test-Path "build_qt\$e")) { throw "未找到 build_qt\$e ，请先运行 build_qt.ps1" }
}

$sim = Start-Process -FilePath 'build_qt\device_simulator.exe' -ArgumentList "$SimPort" -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 500
$gw  = Start-Process -FilePath 'build_qt\gateway_service.exe' -ArgumentList "$GwPort 127.0.0.1 $SimPort" -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 1000
Write-Output "sim pid=$($sim.Id) gw pid=$($gw.Id) alive: sim=$(-not $sim.HasExited) gw=$(-not $gw.HasExited)"

$pass = 0; $exact = 0; $maxAbs = 0
for ($r = 1; $r -le $Rounds; $r++) {
    $out  = & 'build_qt\qt_monitor.exe' --verify-gateway 127.0.0.1 $GwPort $WinMs 2>&1
    $line = ($out | Select-String 'VERIFY_GW').Line
    if ($LASTEXITCODE -eq 0) { $pass++ }
    if ($line -match 'diff=(-?\d+)') {
        $d = [int]$Matches[1]
        if ($d -eq 0) { $exact++ }
        if ([Math]::Abs($d) -gt $maxAbs) { $maxAbs = [Math]::Abs($d) }
    }
    Write-Output ("round {0}/{1} exit={2} :: {3}" -f $r, $Rounds, $LASTEXITCODE, $line)
}
Write-Output "=== T1.2 VERIFY_GW: PASS $pass / $Rounds ; 精确相等 $exact / $Rounds ; 最大|diff| = $maxAbs (每批=4 帧) ==="

Stop-Process -Id $gw.Id  -Force -ErrorAction SilentlyContinue
Stop-Process -Id $sim.Id -Force -ErrorAction SilentlyContinue
Write-Output "stopped gw+sim"
if ($pass -eq $Rounds) { exit 0 } else { exit 1 }
