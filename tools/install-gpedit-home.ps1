# Windows Home / Home China: 로컬 그룹 정책 편집기(gpedit.msc)용 패키지 DISM 설치
# 관리자 PowerShell에서 실행:  우클릭 -> 관리자 권한으로 실행

$ErrorActionPreference = 'Stop'
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host '이 스크립트는 관리자 권한이 필요합니다.' -ForegroundColor Red
    Write-Host 'PowerShell을 "관리자 권한으로 실행"한 뒤 다시 실행하세요.' -ForegroundColor Yellow
    exit 1
}

$pkgs = Join-Path $env:Windir 'servicing\Packages'

function Get-LatestNeutralMum([string]$glob) {
    $files = Get-ChildItem -Path (Join-Path $pkgs $glob) -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '~amd64~~\d' }
    if (-not $files) { return $null }
    $files | Sort-Object {
        if ($_.Name -match '~~(\d+\.\d+\.\d+\.\d+)\.mum$') { [version]$Matches[1] } else { [version]'0.0.0.0' }
    } -Descending | Select-Object -First 1
}

function Add-PackageSafe([string]$path) {
    if (-not $path) { return }
    Write-Host "추가: $(Split-Path $path -Leaf)" -ForegroundColor Cyan
    & dism.exe /online /norestart /add-package:$path
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010) {
        Write-Host "DISM 종료 코드: $LASTEXITCODE (3010=재부팅 필요일 수 있음)" -ForegroundColor Yellow
    }
}

# 순서: Extensions -> Tools, 그다음 WOW64
$pairs = @(
    @{ Glob = 'Microsoft-Windows-GroupPolicy-ClientExtensions-Package~3*.mum'; Label = 'ClientExtensions' }
    @{ Glob = 'Microsoft-Windows-GroupPolicy-ClientTools-Package~3*.mum'; Label = 'ClientTools' }
    @{ Glob = 'Microsoft-Windows-GroupPolicy-ClientExtensions-WOW64-Package~3*.mum'; Label = 'ClientExtensions WOW64' }
    @{ Glob = 'Microsoft-Windows-GroupPolicy-ClientTools-WOW64-Package~3*.mum'; Label = 'ClientTools WOW64' }
)

foreach ($p in $pairs) {
    $m = Get-LatestNeutralMum $p.Glob
    if ($m) {
        Add-PackageSafe $m.FullName
    } else {
        Write-Host "건너뜀(파일 없음): $($p.Label)" -ForegroundColor DarkYellow
    }
}

Write-Host "`n정책 새로고침 시도..." -ForegroundColor Cyan
& gpupdate.exe /force 2>$null

$gp = Join-Path $env:Windir 'System32\gpedit.msc'
if (Test-Path $gp) {
    Write-Host "`n설치 확인: $gp 존재" -ForegroundColor Green
    Write-Host '실행: Win+R -> gpedit.msc 또는 관리자 CMD에서: mmc.exe /s gpedit.msc' -ForegroundColor Green
} else {
    Write-Host "`n아직 gpedit.msc가 없습니다. 재부팅 후 다시 확인하거나 Windows 업데이트 후 재시도하세요." -ForegroundColor Yellow
}

exit 0
