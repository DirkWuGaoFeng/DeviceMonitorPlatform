# deploy_firmware.ps1 — 一键把 DMP 固件接入 STM32 Keil 工程 (默认 12_usart_printf_hal)
#
# 做三件事(全部幂等, 改动前自动留 .dmp.bak):
#   1) 拷贝 firmware 的 4 个源文件到工程 Core\Src / Core\Inc
#   2) 用 CubeMX 的 USER CODE 标记往 main.c 精确插 3 行(#include / dmp_frame_init / dmp_task)
#   3) 往 uvprojx 的 Application/User/Core 组注入 2 个 .c 编译单元
# 之后你只需在 Keil 里编译(F7)+下载, 或直接用命令行 UV4 构建。
#
# 编码安全: main.c 含 GBK 中文注释, 全程用 Latin1(ISO-8859-1) 字节保真往返, 只插纯 ASCII 行, 绝不重编码。
#
# 用法(在你自己的 PowerShell 控制台, 非受限会话):
#   .\deploy_firmware.ps1                       # 默认工程 12_usart_printf_hal
#   .\deploy_firmware.ps1 -DryRun               # 只预览将做的改动, 不写盘
#   .\deploy_firmware.ps1 -Project 'E:\Work\STM32Project\09_usart_rolling_hal'
param(
    [string]$Project = 'E:\Work\STM32Project\12_usart_printf_hal',
    [string]$Group   = 'Application/User/Core',
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$fwRoot  = Join-Path $PSScriptRoot 'firmware'
$srcDir  = Join-Path $Project 'Core\Src'
$incDir  = Join-Path $Project 'Core\Inc'
$mainC   = Join-Path $srcDir 'main.c'

# ---- Latin1 字节保真读写 ----
$lat = [System.Text.Encoding]::GetEncoding('ISO-8859-1')
function ReadBText($p) { $lat.GetString([System.IO.File]::ReadAllBytes($p)) }
function WriteBText($p, $t) { [System.IO.File]::WriteAllBytes($p, $lat.GetBytes($t)) }
function BackupOnce($p) { $b = "$p.dmp.bak"; if (-not (Test-Path $b)) { Copy-Item $p $b; return $b } return $null }

if (-not (Test-Path $srcDir) -or -not (Test-Path $incDir)) {
    Write-Host "[错误] 找不到 $srcDir 或 $incDir (用 -Project 指定正确工程)" -ForegroundColor Red; exit 1
}
Write-Host ("目标工程: {0}   DryRun={1}" -f $Project, [bool]$DryRun) -ForegroundColor Cyan

# ============ 1) 拷贝固件源文件 ============
Write-Host "`n== [1/3] 拷贝固件源文件 ==" -ForegroundColor Yellow
$map = @{ 'dmp_frame_core.c' = $srcDir; 'dmp_frame.c' = $srcDir; 'dmp_frame_core.h' = $incDir; 'dmp_frame.h' = $incDir }
foreach ($f in $map.Keys) {
    $from = Join-Path $fwRoot $f
    $to   = Join-Path $map[$f] $f
    if (-not (Test-Path $from)) { Write-Host "  [缺] $from" -ForegroundColor Red; continue }
    if ((Test-Path $to) -and ((Get-FileHash $from).Hash -eq (Get-FileHash $to).Hash)) {
        Write-Host ("  [跳过] {0} (已一致)" -f $f) -ForegroundColor DarkGray
    } elseif ($DryRun) {
        Write-Host ("  [将复制] {0} -> {1}" -f $f, $map[$f]) -ForegroundColor Green
    } else {
        Copy-Item $from $to -Force; Write-Host ("  [复制] {0} -> {1}" -f $f, $map[$f]) -ForegroundColor Green
    }
}

# ============ 2) 打补丁 main.c ============
Write-Host "`n== [2/3] 打补丁 main.c (CubeMX USER CODE 标记) ==" -ForegroundColor Yellow
$t = ReadBText $mainC
$orig = $t
$acts = @()
if ($t -notmatch 'dmp_frame\.h') {
    $t = $t -replace '(?m)(/\* USER CODE BEGIN Includes \*/)', "`$1`r`n#include `"dmp_frame.h`""
    $acts += '加 #include "dmp_frame.h"'
}
if ($t -notmatch 'dmp_frame_init\s*\(') {
    $t = $t -replace '(?m)(/\* USER CODE BEGIN 2 \*/)', "`$1`r`n  dmp_frame_init();"
    $acts += '加 dmp_frame_init(); (BEGIN 2, 一次性)'
}
if ($t -notmatch 'dmp_task\s*\(') {
    $t = $t -replace '(?m)(/\* USER CODE BEGIN 3 \*/)', "`$1`r`n    dmp_task();"
    $acts += '加 dmp_task(); (BEGIN 3, 循环内)'
}
if ($t -ne $orig) {
    foreach ($a in $acts) { Write-Host "  [改] $a" -ForegroundColor Green }
    if ($DryRun) { Write-Host "  (DryRun: 未写盘)" -ForegroundColor DarkGray }
    else { $b = BackupOnce $mainC; WriteBText $mainC $t; Write-Host ("  已写回 main.c{0}" -f $(if($b){" (备份: $b)"}else{''})) -ForegroundColor Green }
} else {
    Write-Host "  [跳过] main.c 三处改动均已存在" -ForegroundColor DarkGray
}

# ============ 3) 注入 uvprojx 编译单元 ============
Write-Host "`n== [3/3] 注入 uvprojx 文件组 ($Group) ==" -ForegroundColor Yellow
$uv = Get-ChildItem (Join-Path $Project 'MDK-ARM') -Filter *.uvprojx -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $uv) {
    Write-Host "  [警告] 没找到 .uvprojx, 跳过 (稍后在 Keil 里手动 Add Existing Files)" -ForegroundColor Yellow
} else {
    $x = ReadBText $uv.FullName
    if ($x -match '<FileName>dmp_frame\.c</FileName>') {
        Write-Host "  [跳过] uvprojx 已含 dmp_frame.c" -ForegroundColor DarkGray
    } else {
        $blocks = @"
            <File>
              <FileName>dmp_frame_core.c</FileName>
              <FileType>1</FileType>
              <FilePath>../Core/Src/dmp_frame_core.c</FilePath>
            </File>
            <File>
              <FileName>dmp_frame.c</FileName>
              <FileType>1</FileType>
              <FilePath>../Core/Src/dmp_frame.c</FilePath>
            </File>
"@
        $pattern = '(<GroupName>' + [regex]::Escape($Group) + '</GroupName>\s*<Files>.*?)(</Files>)'
        $rx = [regex]::new($pattern, 'Singleline')
        if (-not $rx.IsMatch($x)) {
            Write-Host "  [警告] 组 '$Group' 结构没匹配上, 跳过 (手动 Add Existing Files)" -ForegroundColor Yellow
        } else {
            $x2 = $rx.Replace($x, ('$1' + $blocks + '`r`n          $2'), 1)
            if ($DryRun) { Write-Host ("  [将注入] {0} (+2 文件)" -f $uv.Name) -ForegroundColor Green }
            else {
                $b = BackupOnce $uv.FullName; WriteBText $uv.FullName $x2
                Write-Host ("  已注入 {0}: +dmp_frame_core.c +dmp_frame.c{1}" -f $uv.Name, $(if($b){" (备份: $b)"}else{''})) -ForegroundColor Green
            }
        }
    }
}

# ============ 下一步 ============
$uvHint = if ($uv) { $uv.FullName } else { '<工程>.uvprojx' }
Write-Host @"

== 完成. 接下来 ==
  1) 编译: 打开 $Group 组应能看到 dmp_frame*.c; Keil 按 F7 (0 error)
     或命令行: & 'D:\Software\KeilMDK\Keil_v5\UV4\UV4.exe' -b "$uvHint" -j0 -o build.log
  2) 下载刷录 (Alt+F7 / 工具栏 Download), 看到 Verify OK / Application running
  3) 验收: .\serial_check.ps1   ->  raw 与 ok 同时递增即真机链路打通
  回退: 恢复各自 *.dmp.bak 即可
"@ -ForegroundColor Cyan
