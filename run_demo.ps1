# run_demo.ps1 — 一键本地演示 DMP 数据链
#
# 作用: 在 Windows 桌面拉起完整链路 —— [模拟设备] ──TCP──> [Qt 上位机]，
#       可选再插入一层 [遥测网关] 供多客户端订阅。关闭 Qt 窗口即自动回收后台进程。
#
# 用法(在本项目根目录):
#   .\run_demo.ps1                     # 模拟器(:9000) + Qt 上位机(自动连 TCP)
#   .\run_demo.ps1 -Gateway            # 额外起网关(:9100<-:9000)，Qt 连网关看订阅流
#   .\run_demo.ps1 -SimPort 9010       # 换模拟器端口
#   .\run_demo.ps1 -SelfTest           # 不起 GUI，只跑 qt_monitor --verify 同步自检
#
# 依赖: 已用 Qt 自带工具链构建好 build_qt\qt_monitor.exe 与 build\*.exe。
param(
    [int]    $SimPort   = 9000,
    [int]    $GwPort    = 9100,
    [switch] $Gateway,                 # 是否额外启动遥测网关
    [switch] $SelfTest,                # 无 GUI，仅 --verify 自检
    [string] $QtDir  = 'G:\Software\Qt\6.10.0\mingw_64',
    [string] $Mingw  = 'G:\Software\Qt\Tools\mingw1310_64\bin'
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# ---- 定位可执行文件 ----
$sim = Join-Path $root 'build\device_simulator.exe'
$gw  = Join-Path $root 'build\gateway_service.exe'
$qt  = Join-Path $root 'build_qt\qt_monitor.exe'

foreach ($p in @($sim, $qt)) {
    if (-not (Test-Path $p)) {
        Write-Host "[缺失] $p`n       请先构建 (非Qt: cmake --build build；Qt: build_qt.ps1)。" -ForegroundColor Red
        exit 1
    }
}

# ---- 让 Qt GUI 程序能找到 DLL (Qt6Core.dll / libgcc 等) ----
$env:PATH = "$Mingw;$QtDir\bin;$env:PATH"

$children = @()
function Start-Daemon([string]$file, [string]$args_, [string]$label) {
    $proc = Start-Process -FilePath $file -ArgumentList $args_ -PassThru `
            -WindowStyle Normal
    Write-Host ("[{0}] 已启动  PID={1}" -f $label, $proc.Id) -ForegroundColor Green
    $script:children += $proc
    return $proc
}

try {
    # 1) 模拟设备 (TCP 广播帧流)
    Start-Daemon $sim "$SimPort" "SIMULATOR" | Out-Null
    Start-Sleep -Milliseconds 800      # 等 listen socket 就绪

    # 2) 可选: 遥测网关  上游=模拟器  下游监听=GwPort
    $qtTargetPort = $SimPort            # Qt 默认直连模拟器
    if ($Gateway) {
        if (-not (Test-Path $gw)) { Write-Host "[缺失] $gw，跳过网关" -ForegroundColor Yellow }
        else {
            Start-Daemon $gw "$GwPort 127.0.0.1 $SimPort" "GATEWAY" | Out-Null
            Start-Sleep -Milliseconds 600
            Write-Host ("     Qt 将连网关(:{0})看订阅流；网关上游=模拟器(:{1})" -f $GwPort, $SimPort) -ForegroundColor DarkGray
            # Qt 走 TCP 直连；这里连网关端口以演示服务化链路
            $qtTargetPort = $GwPort
        }
    }

    # 3) Qt 上位机
    if ($SelfTest) {
        Write-Host "`n=== qt_monitor --verify (同步自检) ===" -ForegroundColor Cyan
        & $qt --verify
        Write-Host "verify 退出码 = $LASTEXITCODE" -ForegroundColor Cyan
    }
    else {
        Write-Host ("`n[QT] 启动上位机，自动连 TCP 127.0.0.1:{0}" -f $qtTargetPort) -ForegroundColor Green
        Write-Host "     (GUI 里也可手动切 TCP/串口。关闭此窗口将回收后台进程。)`n" -ForegroundColor DarkGray
        # --demo-tcp <host> <port> <runms>; runms=0 => 连上后常驻交互, 不自动退出
        $qtProc = Start-Process -FilePath $qt -PassThru -WindowStyle Normal `
                    -ArgumentList @('--demo-tcp', '127.0.0.1', "$qtTargetPort", '0')
    }
}
catch {
    Write-Host "[错误] $($_.Exception.Message)" -ForegroundColor Red
}
finally {
    # 等待 Qt 进程结束 (SelfTest 模式下 & 已同步跑完，qtProc 可能为空)
    if ($qtProc -and -not $qtProc.HasExited) {
        Write-Host "等待 Qt 上位机退出..." -ForegroundColor DarkGray
        $qtProc.WaitForExit()
    }
    # 回收后台守护进程
    foreach ($c in $children) {
        if ($c -and -not $c.HasExited) {
            Stop-Process -Id $c.Id -Force -ErrorAction SilentlyContinue
            Write-Host ("回收 PID={0}" -f $c.Id) -ForegroundColor DarkGray
        }
    }
    Write-Host "`n全部进程已退出。" -ForegroundColor Green
}
