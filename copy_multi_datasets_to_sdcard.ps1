param(
    [string]$DriveLetter = ""
)

Write-Host "================================================================================" -ForegroundColor Cyan
Write-Host "   VIETHUD - SAO CHEP 3 BO DU LIEU (GOFA, WYN & VIETMAP) VAO THE NHO DE CHON TREN FW    " -ForegroundColor Yellow
Write-Host "================================================================================" -ForegroundColor Cyan

$ScriptDir = $PSScriptRoot
if (-not $ScriptDir) { $ScriptDir = "C:\Users\phamq\radar_car" }

if (-not $DriveLetter) {
    $drives = Get-Volume | Where-Object { $_.DriveType -eq 'Removable' -and $_.DriveLetter }
    if ($drives) {
        $DriveLetter = "$($drives[0].DriveLetter):"
        Write-Host "[*] Tu dong phat hien the nho tai o: $DriveLetter" -ForegroundColor Green
    } else {
        $DriveLetter = Read-Host "Nhap ky tu o the nho (vi du E: hoac F:)"
    }
}

if (-not $DriveLetter.EndsWith("\")) {
    $DriveLetter = "$DriveLetter\"
}

if (-not (Test-Path $DriveLetter)) {
    Write-Host "[!] Khong tim thay o $DriveLetter. Vui long kiem tra lai the nho!" -ForegroundColor Red
    exit 1
}

$GofaSrc = Join-Path $ScriptDir "speedmap_gofa\speedmap"
if (-not (Test-Path $GofaSrc)) { $GofaSrc = Join-Path $ScriptDir "VietHUD_SDCard_GOFA\speedmap" }
if (-not (Test-Path $GofaSrc)) { $GofaSrc = Join-Path $ScriptDir "speedmap" }

$WynSrc = Join-Path $ScriptDir "VietHUD_SDCard_WYN_PURE\speedmap"

$GofaDest = Join-Path $DriveLetter "speedmap_gofa"
$WynDest = Join-Path $DriveLetter "speedmap_wyn"
$DefDest = Join-Path $DriveLetter "speedmap"

Write-Host "`n1. Dang sao chep bo du lieu GOFA -> $GofaDest..." -ForegroundColor Magenta
if (-not (Test-Path $GofaDest)) { New-Item -ItemType Directory -Force -Path $GofaDest | Out-Null }
robocopy "$GofaSrc" "$GofaDest" /E /R:1 /W:1 /NP /NDL
Write-Host "[OK] Da sao chep xong bo GOFA!" -ForegroundColor Green

Write-Host "`n2. Dang sao chep bo du lieu WYN -> $WynDest..." -ForegroundColor Magenta
if (-not (Test-Path $WynDest)) { New-Item -ItemType Directory -Force -Path $WynDest | Out-Null }
robocopy "$WynSrc" "$WynDest" /E /R:1 /W:1 /NP /NDL
Write-Host "[OK] Da sao chep xong bo WYN!" -ForegroundColor Green


$VietmapSrc = Join-Path $ScriptDir "speedmap_vietmap\speedmap"
$VietmapDest = Join-Path $DriveLetter "speedmap_vietmap"
if (Test-Path $VietmapSrc) {
    Write-Host "`n3. Dang sao chep bo du lieu VIETMAP -> $VietmapDest..." -ForegroundColor Magenta
    if (-not (Test-Path $VietmapDest)) { New-Item -ItemType Directory -Force -Path $VietmapDest | Out-Null }
    robocopy "$VietmapSrc" "$VietmapDest" /E /R:1 /W:1 /NP /NDL
    Write-Host "[OK] Da sao chep xong bo VIETMAP!" -ForegroundColor Green
}

Write-Host "`n4. Tao thu muc mac dinh speedmap (tro vao GOFA)..." -ForegroundColor Magenta
if (-not (Test-Path $DefDest)) { New-Item -ItemType Directory -Force -Path $DefDest | Out-Null }
robocopy "$GofaSrc" "$DefDest" /E /R:1 /W:1 /NP /NDL
Write-Host "[OK] Da thiet lap xong thu muc mac dinh!" -ForegroundColor Green

Write-Host "`n================================================================================" -ForegroundColor Cyan
Write-Host " [THANH CONG] The nho da co day du 3 bo du lieu doc lap:
   - /speedmap_vietmap: Du lieu VIETMAP (23,402 bien bao + 2,009 camera VietMap Live & OSM)" -ForegroundColor Green
Write-Host "   - /speedmap_gofa : Du lieu GOFA (744MB OSM + 200km GOFA + 6 giong doc GOFA)" -ForegroundColor White
Write-Host "   - /speedmap_wyn  : Du lieu WYN goc (123MB + bien bao & giong goc WYN)" -ForegroundColor White
Write-Host "   - /speedmap      : Mac dinh (GOFA)" -ForegroundColor White
Write-Host " Tren man hinh VietHUD:" -ForegroundColor Yellow
Write-Host "   -> Tab Map   : Co Droplist 'Map Data Source' de chon GOFA hoac WYN" -ForegroundColor Yellow
Write-Host "   -> Tab Audio : Co Droplist 'Voice Pack' de chon 1 trong 6 giong GOFA hoac WYN" -ForegroundColor Yellow
Write-Host "================================================================================" -ForegroundColor Cyan
