# stop_demo.ps1 — 手动清扫 DMP 演示残留进程
#
# 用途: 若 run_demo.ps1 异常中断(Ctrl+C 强杀/崩溃)，后台模拟器/网关/控制台可能残留，
#       本脚本按进程名一次性回收。安全: 只匹配本项目这几个 exe 名, 不动其它进程。
#
# 用法:
#   .\stop_demo.ps1              # 停掉 simulator/gateway/monitor_console/qt_monitor
#   .\stop_demo.ps1 -WhatIf      # 先看会停哪些, 不实际杀

param([switch]$WhatIf)

$names = @(
    'device_simulator',
    'gateway_service',
    'monitor_console',
    'monitor_serial',
    'qt_monitor'
)

$killed = 0
foreach ($n in $names) {
    $procs = Get-Process -Name $n -ErrorAction SilentlyContinue
    foreach ($p in $procs) {
        if ($WhatIf) {
            Write-Host ("[会停] {0}  PID={1}" -f $n, $p.Id) -ForegroundColor Yellow
        }
        else {
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            Write-Host ("[已停] {0}  PID={1}" -f $n, $p.Id) -ForegroundColor Green
        }
        $killed++
    }
}

if ($killed -eq 0) { Write-Host "没有发现本项目残留进程，无需清理。" -ForegroundColor DarkGray }
else { Write-Host ("处理完成，共 {0} 个进程{1}。" -f $killed, $(if($WhatIf){'(预览, 未实际停止)'}else{'(已停止)'})) -ForegroundColor Cyan }
