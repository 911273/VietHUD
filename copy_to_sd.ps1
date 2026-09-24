param(
    [Parameter(Position=0)]
    [string]$DriveLetter
)

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "   VietHUD SD Card Copy Tool (Speedmap & OSM Data)" -ForegroundColor Yellow
Write-Host "==========================================================" -ForegroundColor Cyan

if (-not $DriveLetter) {
    Write-Host "`nCac o dia dang co tren may tinh:" -ForegroundColor Green
    Get-Volume | Where-Object { $_.DriveLetter -ne $null } | Format-Table DriveLetter, FriendlyName, FileSystemType, DriveType, SizeRemaining, Size
    $DriveLetter = Read-Host "Nhap ky tu o the nho (vi du: E hoac E:)"
}

$DriveLetter = $DriveLetter.TrimEnd(":\") + ":"
if (-not (Test-Path "$DriveLetter\")) {
    Write-Host "`nLoi: Khong tim thay o dia $DriveLetter\" -ForegroundColor Red
    exit 1
}

$destDir = "$DriveLetter\speedmap"
if (-not (Test-Path $destDir)) {
    New-Item -ItemType Directory -Path $destDir -Force | Out-Null
}

$srcDir = "C:\Users\phamq\radar_car\data\speedmap"
Write-Host "`nDang copy du lieu tu $srcDir sang $destDir ..." -ForegroundColor Cyan

$files = @(
    "metadata.bin",
    "cameras.bin",
    "signs.bin",
    "index.bin",
    "tiles.bin",
    "names.bin",
    "seg_names.bin",
    "maptiles.bin",
    "maptiles_osm.bin",
    "maptiles_voyager.bin"
)

foreach ($f in $files) {
    $srcPath = Join-Path $srcDir $f
    if (Test-Path $srcPath) {
        $sizeMB = (Get-Item $srcPath).Length / 1MB
        Write-Host "  Copying $f ($([math]::Round($sizeMB, 2)) MB)..." -NoNewline
        Copy-Item -Path $srcPath -Destination "$destDir\$f" -Force
        Write-Host " [OK]" -ForegroundColor Green
    } else {
        Write-Host "  Bo qua $f (khong co trong source)" -ForegroundColor Yellow
    }
}

if (Test-Path "$srcDir\sounds") {
    Write-Host "  Copying thu muc sounds/ ..." -NoNewline
    Copy-Item -Path "$srcDir\sounds" -Destination $destDir -Recurse -Force
    Write-Host " [OK]" -ForegroundColor Green
}

Write-Host "`n==========================================================" -ForegroundColor Cyan
Write-Host "DA COPY THANH CONG TOAN BO DU LIEU VAO THE NHO ($DriveLetter\speedmap)!" -ForegroundColor Green
Write-Host "Ban co the rut the nho an toan va cam vao thiet bi VietHUD." -ForegroundColor Yellow
Write-Host "==========================================================" -ForegroundColor Cyan
