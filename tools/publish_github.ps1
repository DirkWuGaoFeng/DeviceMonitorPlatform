<#
.SYNOPSIS
    把本地已提交好的 DeviceMonitorPlatform 推到 GitHub, 并自动点亮 README 里的 CI 徽章。

.DESCRIPTION
    前置条件 (两个, 都需要你本人操作, 脚本无法代劳):
      1. 在 GitHub 网页新建一个 **空的 public 仓库**, 名字默认 DeviceMonitorPlatform,
         不要勾选 "Add a README / .gitignore / license" (否则首次 push 会被 non-fast-forward 拒掉)。
      2. GitHub 凭据可用。当前机器上凭据管理器里存的那条 PAT 已失效 (API 返回 401 Bad credentials),
         所以第一次 push 时 Git Credential Manager 会弹浏览器让你登录授权 —— 走完即可,
         之后凭据会被缓存, 后续 push 不再询问。
         (不要把 token 粘贴到聊天里。)

.PARAMETER Owner
    你的 GitHub 用户名 (大小写不敏感, 用于拼 URL 和替换徽章)。

.PARAMETER Repo
    仓库名, 需与网页上建的一致。

.PARAMETER Branch
    推送分支, 默认 main。

.EXAMPLE
    .\tools\publish_github.ps1 -Owner dirkwu
    # 仓库名不是默认值时:
    .\tools\publish_github.ps1 -Owner dirkwu -Repo DMP

.NOTES
    脚本幂等: 重复执行只会做 set-url + push + 徽章替换 (无变化则不再产生提交)。
    含中文, 故需以 UTF-8 BOM 保存以兼容 Windows PowerShell 5.1。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Owner,
    [string]$Repo = 'DeviceMonitorPlatform',
    [string]$Branch = 'main'
)

$ErrorActionPreference = 'Stop'
# 脚本在 tools/ 下, 仓库根在其上一级
Set-Location (Join-Path $PSScriptRoot '..')
"仓库根: $((Get-Location).Path)"

$url = "https://github.com/$Owner/$Repo.git"
"-- 目标远端: $url"

# ---------- 1. 配置远端 ----------
$hasOrigin = & git remote get-url origin 2>$null
if ($LASTEXITCODE -eq 0 -and $hasOrigin) {
    if ($hasOrigin.Trim() -ne $url) {
        "-- origin 已存在且指向 $hasOrigin -> 改为 $url"
        & git remote set-url origin $url
    } else {
        "-- origin 已正确指向 $url"
    }
} else {
    "-- 添加 origin -> $url"
    & git remote add origin $url
}
if ($LASTEXITCODE -ne 0) { throw "配置远端失败" }

# ---------- 2. 推送 ----------
"-- 推送 $Branch (首次会弹浏览器授权)"
& git push -u origin $Branch
if ($LASTEXITCODE -ne 0) {
    @"
推送失败。最常见三种原因:
  A. 授权未完成/凭据仍无效 -> 先手动跑一次 'git push -u origin $Branch' 在**你自己的终端**里,
     按提示走完浏览器登录; 或到 控制面板 -> 凭据管理器 -> Windows 凭据
     删除 git:https://github.com 那条过期 PAT 让它重新询问。
  B. 仓库还没建 / 名字或所有者拼错 -> 到 https://github.com/new 建空仓库 (public, 无 README)。
  C. 远端非空(建库时勾了 README) -> 先跑 'git pull origin $Branch --rebase' 再推。
"@ | Write-Host -ForegroundColor Yellow
    throw "git push 返回 $LASTEXITCODE"
}
"-- 推送成功"

# ---------- 3. 替换徽章占位符 ----------
$readme = Join-Path (Get-Location) 'README.md'
if (Test-Path $readme) {
    $txt = Get-Content -Raw -Encoding UTF8 $readme
    if ($txt -match '__OWNER__') {
        "-- 把徽章占位符 __OWNER__ 替换为 $Owner"
        $new = $txt -replace '__OWNER__', $Owner
        # 保持 UTF-8 无 BOM, 且换行与仓库内既有风格一致 (md 用 LF)
        $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText($readme, $new, $utf8NoBom)
        & git add README.md
        & git commit -m "ci: point status badge at $Owner/$Repo"
        if ($LASTEXITCODE -eq 0) {
            & git push origin $Branch
            if ($LASTEXITCODE -ne 0) { throw "徽章提交推送失败" }
            "-- 徽章替换已推送"
        }
    } else {
        "-- README 中无 __OWNER__ 占位符, 跳过"
    }
}

# ---------- 4. 后续链接 ----------
@"

下一步自检 (浏览器打开):
  仓库页面   : https://github.com/$Owner/$Repo
  Actions    : https://github.com/$Owner/$Repo/actions/workflows/ci.yml
               首次触发约 2~5 分钟; Linux/Windows 两条门禁腿都绿, README 顶部徽章才会亮。
  若 Actions 一条腿红: 点进 job 日志, 把报错前 30 行贴给我, 我按日志定位而不是猜。
"@ | Write-Host -ForegroundColor Green
