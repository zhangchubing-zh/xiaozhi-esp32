# 批量格式化所有 .c/.h 文件
# 用法: 在项目根目录运行  .\others\format\format.ps1
# 可选参数: .\others\format\format.ps1 -DryRun    (只预览不修改)
#          .\others\format\format.ps1 -Path src   (只格式化某个子目录)

param(
    [switch]$DryRun,
    [string[]]$Path = @("src", "include", "tests")
)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path

# 1. 定位 clang-format.exe - 优先 PATH,其次 VSCode cpptools 扩展自带的
$cf = $null
$fromPath = Get-Command clang-format -ErrorAction SilentlyContinue
if ($fromPath) {
    $cf = $fromPath.Source
}
else {
    $extRoot = "$env:USERPROFILE\.vscode\extensions"
    $candidate = Get-ChildItem -Path $extRoot -Directory -Filter "ms-vscode.cpptools-*" -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match "win32-" } |
        Sort-Object Name -Descending |
        Select-Object -First 1
    if ($candidate) {
        $exe = Join-Path $candidate.FullName "LLVM\bin\clang-format.exe"
        if (Test-Path -LiteralPath $exe) { $cf = $exe }
    }
}

if (-not $cf -or -not (Test-Path -LiteralPath $cf)) {
    Write-Host "错误: 找不到 clang-format.exe" -ForegroundColor Red
    Write-Host "请确认已安装 VSCode 的 ms-vscode.cpptools 扩展,或把 clang-format 加入 PATH" -ForegroundColor Yellow
    exit 1
}

Write-Host "使用 clang-format: $cf" -ForegroundColor Cyan
& $cf --version
Write-Host ""

# 2. 找 .clang-format 配置文件 (脚本所在目录)
$configFile = Join-Path $PSScriptRoot ".clang-format"
if (-not (Test-Path -LiteralPath $configFile)) {
    Write-Host "错误: 未找到 .clang-format 配置文件 ($configFile)" -ForegroundColor Red
    exit 1
}
Write-Host "使用配置: $configFile" -ForegroundColor Cyan
Write-Host ""

# 3. 收集要格式化的文件
$files = @()
foreach ($p in $Path) {
    $full = if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $projectRoot $p }
    if (Test-Path -LiteralPath $full) {
        $files += Get-ChildItem -Path $full -Recurse -Include "*.c","*.h" -File -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty FullName
    }
}

if ($files.Count -eq 0) {
    Write-Host "未找到任何 .c/.h 文件" -ForegroundColor Yellow
    exit 0
}

Write-Host "找到 $($files.Count) 个文件:" -ForegroundColor Green
$files | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
Write-Host ""

# 4. 执行格式化
if ($DryRun) {
    Write-Host "[DryRun 模式] 仅预览,不修改文件" -ForegroundColor Yellow
    foreach ($f in $files) {
        Write-Host "===== $f =====" -ForegroundColor Cyan
        & $cf "-style=file:$configFile" $f
    }
}
else {
    Write-Host "开始格式化..." -ForegroundColor Green
    & $cf -i "-style=file:$configFile" $files
    if ($LASTEXITCODE -eq 0) {
        Write-Host "完成: $($files.Count) 个文件已格式化" -ForegroundColor Green
    }
    else {
        Write-Host "格式化出错,退出码: $LASTEXITCODE" -ForegroundColor Red
        exit $LASTEXITCODE
    }
}
