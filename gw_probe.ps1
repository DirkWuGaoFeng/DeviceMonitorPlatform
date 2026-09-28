# gw_probe.ps1 — 验证遥测网关文本行协议 (对 gateway_service :9100 发命令)
#
# 前提: 先跑 .\run_demo.ps1 -Gateway  (它会把网关起在 :9100, 上游连模拟器 :9000)
# 作用: 用 .NET TcpClient 连网关, 依次发 HELP / STATS / ALARMS / (可选)SUBSCRIBE,
#       把网关回到的文本行打印出来 —— 演示"二进制设备帧 -> 文本服务 API"的服务化链路。
#
# 用法:
#   .\gw_probe.ps1                      # 发 HELP/STATS/ALARMS 后退出
#   .\gw_probe.ps1 -Subscribe -Seconds 5   # 订阅实时 SAMPLE 推送 5 秒
#   .\gw_probe.ps1 -Port 9100 -Cmd "STATS"  # 只发单条命令

param(
    [int]    $Port      = 9100,
    [string] $Host_     = '127.0.0.1',
    [switch] $Subscribe,
    [int]    $Seconds   = 5,
    [string] $Cmd                      # 若指定, 只发这一条命令
)
$ErrorActionPreference = 'Stop'

function Send-Line($stream, $line) {
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($line + "`n")
    $stream.Write($bytes, 0, $bytes.Length)
}
function Read-Once($stream, $waitMs) {
    Start-Sleep -Milliseconds $waitMs
    $sb = New-Object System.Text.StringBuilder
    while ($stream.DataAvailable) {
        $buf = New-Object byte[] 4096
        $n = $stream.Read($buf, 0, $buf.Length)
        [void]$sb.Append([System.Text.Encoding]::ASCII.GetString($buf, 0, $n))
    }
    return $sb.ToString()
}

Write-Host ("连接网关 {0}:{1} ..." -f $Host_, $Port) -ForegroundColor Cyan
$client = New-Object System.Net.Sockets.TcpClient
$client.Connect($Host_, $Port)
$stream = $client.GetStream()
$stream.ReadTimeout  = 2000
$stream.WriteTimeout = 2000

try {
    if ($Cmd) {
        Send-Line $stream $Cmd
        Write-Host ("--> {0}" -f $Cmd) -ForegroundColor Green
        Write-Host (Read-Once $stream 400)
    }
    else {
        foreach ($c in @('HELP', 'STATS', 'ALARMS 5')) {
            Send-Line $stream $c
            Write-Host ("--> {0}" -f $c) -ForegroundColor Green
            Write-Host (Read-Once $stream 300)
        }
        if ($Subscribe) {
            Send-Line $stream 'SUBSCRIBE'
            Write-Host ("--> SUBSCRIBE (监听 {0}s 实时 SAMPLE 推送...)" -f $Seconds) -ForegroundColor Yellow
            $deadline = (Get-Date).AddSeconds($Seconds)
            while ((Get-Date) -lt $deadline) {
                $txt = Read-Once $stream 300
                if ($txt) { Write-Host $txt -NoNewline }
            }
        }
    }
}
finally {
    $stream.Close(); $client.Close()
    Write-Host "`n已断开网关连接。" -ForegroundColor DarkGray
}
