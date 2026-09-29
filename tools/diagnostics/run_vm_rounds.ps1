#!/usr/bin/env pwsh
<#
.SYNOPSIS
    从 Windows 一条链驱动远端: git bundle -> scp -> 同步构建 -> 跑指定取证脚本 -> 原始输出落文件。
.DESCRIPTION
    为什么入库: ADR-003 的 4 轮重复表、素材录 B-40 的三格读数, 都是用这样一条链跑出来的。
    脚本不入库, 文档里那些表就只是"我说了算"。
    这条链上最容易出事的地方是**同步失败但后面照跑** (素材录 B-30: bundle 缺前置提交 ->
    git fetch 非 0 退出 -> 脚本继续 -> 交出"旧代码编译通过"的假绿报告)。所以每一步都查
    $LASTEXITCODE, 非 0 立刻停。
    两个调用细节不是讲究, 是踩过的坑:
      1) vm_sync_build.sh 要先 scp 到 /tmp 再跑 —— 它是**这次提交才进仓库的**, fetch 之前远端
         仓库里根本没有它, 直接 `bash tools/diagnostics/vm_sync_build.sh` 会 file not found。
      2) bundle 落到远端 $HOME/dmp.bundle, 文件名被 vm_sync_build.sh 写死了, 两边必须一致。
    远端地址不写死: 只从环境变量取。
.EXAMPLE
    $env:DMP_VM_IP='192.168.10.131'; $env:DMP_VM_USER='dirk'
    pwsh -NoProfile -File tools/diagnostics/run_vm_rounds.ps1 -RoundScript vm_probeone.sh
.EXAMPLE
    # 跑仓库里已有的主验收脚本(不再 scp 一遍), 只跑状态机 + on_shutdown:
    pwsh -NoProfile -File tools/diagnostics/run_vm_rounds.ps1 -RepoScript tools/vm_lifecycle_compose.sh -RemoteEnv @('POISON=0')
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$VmHost = $env:DMP_VM_IP,
    [string]$VmUser = $env:DMP_VM_USER,
    # 本目录下的取证脚本: 先 scp 到远端 /tmp 再跑 (与 -RepoScript 二选一)
    [string]$RoundScript = '',
    # 仓库内已有的脚本: 直接在远端仓库根目录跑, 不做 scp (前提是它已随 bundle 同步过去)
    [string]$RepoScript  = '',
    # bundle 的前置提交; 留空 = 全量 bundle(包大一点, 但不会缺前置提交)
    [string]$Base = '',
    # 追加给远端脚本的环境变量赋值, 形如 @('POISON=0','LOG=/tmp/dmp_x')
    [string[]]$RemoteEnv = @(),
    [string]$OutFile = '',
    # 仓库根 = 本脚本目录的上上级 (tools/diagnostics -> 仓库根)
    [string]$RepoRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
)
# 为什么手写命令仍会害死人: 从外层 PowerShell 传 `-RemoteEnv @('POISON=0','LOG=/tmp/x')` 时,
# 数组会先被展开成两个独立参数, 第二个就**按位置**落到第一个形参 $VmHost 上 ——
# 于是 scp 目标变成 dirk@LOG=/tmp/x, 报错看起来像"网络/免密问题"。
# 所以开头那行 `[CmdletBinding(PositionalBinding = $false)]` 不是风格: 它让多出来的位置参数
# 直接报错, 而不是静悄悄绑到第一个形参上。再加一道形状校验兜底。

$ErrorActionPreference = 'Continue'
if (-not $VmUser) { $VmUser = 'dirk' }
# 为什么不用 $fail = { ... } + &fail: `&fail '...'` 会被解析成一条叫 fail 的命令, 报"术语不会被识别"
# 然后**继续往下跑** —— 一个不生效的守卫比没有守卫更险, 因为它看起来在拦。实测踩过。
function Die([string]$why) {
    Write-Host "ABORT: $why" -ForegroundColor Red
    exit 1
}
if (-not $VmHost) { Die 'DMP_VM_IP 未设置 —— 远端地址一律不写死, 也不猜测。' }
if ($VmHost -match '[=;\s]') { Die "VmHost 看着不像主机地址: '$VmHost' (八成是参数串了位)" }
if ($RoundScript -and $RepoScript) { Die '-RoundScript 与 -RepoScript 只能给一个。' }
if (-not $RoundScript -and -not $RepoScript) { Die '用 -RoundScript 指定本目录脚本, 或用 -RepoScript 指定仓库内脚本。' }
if (-not (Test-Path (Join-Path $RepoRoot '.git'))) { Die "RepoRoot 不像 git 仓库: $RepoRoot" }
$sync = Join-Path $PSScriptRoot 'vm_sync_build.sh'
if (-not (Test-Path $sync)) { Die "同目录缺 vm_sync_build.sh: $sync" }

Push-Location $RepoRoot
try {
    $bundle = Join-Path $env:TEMP 'dmp_vm_rounds.bundle'
    if ($Base) {
        Write-Host "[1/6] git bundle create ($Base..main)"
        git bundle create $bundle "$Base..main" 2>&1 | ForEach-Object { "$_" }
    } else {
        Write-Host '[1/6] git bundle create main (未给 -Base: 全量包没有前置提交要求)'
        git bundle create $bundle main 2>&1 | ForEach-Object { "$_" }
    }
    if ($LASTEXITCODE -ne 0) { Die "git bundle 退出码 $LASTEXITCODE" }
    if (-not (Test-Path $bundle)) { Die 'bundle 文件没生成' }

    Write-Host "[2/6] scp bundle + vm_sync_build.sh -> ${VmUser}@${VmHost}"
    scp $bundle "${VmUser}@${VmHost}:dmp.bundle"
    if ($LASTEXITCODE -ne 0) { Die 'scp bundle 失败(网络/免密/主机名?)' }
    scp $sync "${VmUser}@${VmHost}:/tmp/vm_sync_build.sh"
    if ($LASTEXITCODE -ne 0) { Die 'scp 同步脚本失败' }

    Write-Host '[3/6] 远端同步 + colcon build/test'
    # sed 去 CR: Windows 侧编辑过的 .sh 常带 CRLF, bash 会把 '\r' 当成命令的一部分。
    $head = ssh "${VmUser}@${VmHost}" "sed -i 's/\r$//' /tmp/vm_sync_build.sh; bash -n /tmp/vm_sync_build.sh && bash /tmp/vm_sync_build.sh" 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        Write-Host ($head -split "`n" | Select-Object -Last 20)
        Die "vm_sync_build.sh 退出码 $LASTEXITCODE —— 停在这里, 不要拿后面任何数字(这就是 B-30 要拦的情况)"
    }
    ($head -split "`n" | Where-Object { $_ -match 'VM_HEAD_BEFORE|^HEAD=|BUILD_RC|SYNC_FAIL|SCRIPT_DONE' }) | ForEach-Object { "  $_" }

    # 这个开关必须在使用它的分支**之前**初始化。它先前写在下面, 于是 else 分支里刚置的 $true
    # 被紧接着的一行覆盖成 $false —— 拼出来的远程命令少了那句 cd, 从 $HOME 跑仓库内脚本, rc=127。
    $runAtRepo = $false
    if ($RoundScript) {
        $src = Join-Path $PSScriptRoot $RoundScript
        if (-not (Test-Path $src)) { Die "本目录没有这个脚本: $RoundScript" }
        Write-Host "[4/6] scp $RoundScript -> /tmp"
        scp $src "${VmUser}@${VmHost}:/tmp/$RoundScript"
        if ($LASTEXITCODE -ne 0) { Die 'scp 轮次脚本失败' }
        $run = "sed -i 's/\r$//' /tmp/$RoundScript && bash -n /tmp/$RoundScript && bash /tmp/$RoundScript"
    } else {
        Write-Host "[4/6] 用仓库内脚本 $RepoScript (已随上一步同步, 不经 scp)"
        $run = "bash $RepoScript"
        $runAtRepo = $true
    }

    $out = $OutFile
    if (-not $out) {
        $tag = if ($RoundScript) { $RoundScript -replace '\.sh$', '' } else { Split-Path -Leaf ($RepoScript -replace '\.sh$', '') }
        $out = Join-Path $env:TEMP ("vmrounds_{0}_{1}.txt" -f $tag, (Get-Date -Format 'yyyyMMdd_HHmmss'))
    }
    Write-Host "[5/6] 跑, 原始输出落盘 -> $out"
    # 环境变量用 `export` 打在整条远程命令的最前面, 而不是贴在某个子命令前:
    # 写成 `POISON=0 cd ... && bash ...` 只会把变量给 cd, 脚本里读到的仍是默认值 ——
    # 干的是"我想只跑状态机", 实跑的是 12 分钟全量毒化那一档。
    $remoteCmd = $run
    if ($runAtRepo) { $remoteCmd = "cd ~/RosProject/dmp && $remoteCmd" }
    if ($RemoteEnv) {
        $envs = ($RemoteEnv | ForEach-Object { "export $_" }) -join '; '
        $remoteCmd = "$envs; $remoteCmd"
    }
    Write-Host "       remote: $remoteCmd"
    ssh -o ServerAliveInterval=30 "${VmUser}@${VmHost}" $remoteCmd 2>&1 | Tee-Object -FilePath $out | Out-Null
    $rc = $LASTEXITCODE

    Write-Host '[6/6] 结论行(由远端脚本现算, 本脚本不改写它)'
    Get-Content $out | Where-Object { $_ -match '^(PASS=|FAIL=|\[.\] |.*结论|.*FAIL )' } | Select-Object -Last 15
    Write-Host "  远端退出码 = $rc  (非 0 不等于结论作废: 输出末尾的 PASS=/FAIL= 才是判据)"
    Write-Host "RAW_LOG=$out"
} finally {
    Pop-Location
}
