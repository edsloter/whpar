# Ensure the script stops executing if any command throws an error
$ErrorActionPreference = "Stop"

Write-Host "=== Starting whpar End-to-End Recovery Test Pipeline ===" -ForegroundColor Cyan

# Use script directory so paths resolve correctly regardless of current working dir
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition

# Define local path configurations (explicit in script directory)
$dataFile = Join-Path $scriptDir "test_data.bin"
$parityFile = Join-Path $scriptDir "recovery_archive.whpar"
$recoveredFile = Join-Path $scriptDir "restored_data.bin"
$binPath = Join-Path $scriptDir "build\Release\whpar.exe"

# Helper function to quickly compute a clean SHA256 string for any file
function Get-FileSHA256 ($path) {
    if (-not (Test-Path $path)) {
        Write-Error "CRITICAL: Cannot compute hash because file does not exist: $path"
    }
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    # Try opening with shared read/write access and retry a few times if the file is briefly locked
    $maxAttempts = 8
    $attempt = 0
    while ($true) {
        try {
            $stream = [System.IO.File]::Open($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
            break
        } catch {
            $attempt++
            if ($attempt -ge $maxAttempts) { throw }
            Start-Sleep -Milliseconds 250
        }
    }
    try {
        $hashBytes = $sha256.ComputeHash($stream)
    } finally {
        $stream.Close()
    }
    return [System.BitConverter]::ToString($hashBytes).Replace("-", "").ToLower()
}

# 1. CLEAN: Remove any lingering old .bin and .whpar files in the script directory
Write-Host "Cleaning up old .bin and .whpar files..." -ForegroundColor Yellow
Get-ChildItem -Path $scriptDir -Filter "*.bin" -File -ErrorAction SilentlyContinue | ForEach-Object { Remove-Item $_.FullName -Force }
Get-ChildItem -Path $scriptDir -Filter "*.whpar" -File -ErrorAction SilentlyContinue | ForEach-Object { Remove-Item $_.FullName -Force }

# 2. GENERATE 100 MB OF SECURE RANDOM DATA
Write-Host "Generating 100 MB test file..." -ForegroundColor Yellow
$fileSize = 100 * 1024 * 1024
$binaryData = New-Object byte[] $fileSize
[System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($binaryData)
[System.IO.File]::WriteAllBytes($dataFile, $binaryData)
$binaryData = $null
[System.GC]::Collect()

# 3. CAPTURE ORIGINAL HASH
Write-Host "Computing original file hash..." -ForegroundColor Yellow
$originalHash = Get-FileSHA256 $dataFile

# 4. GENERATE PARITY ARCHIVE (10% overhead)
Write-Host "Creating parity files via whpar encoder..." -ForegroundColor Yellow
if (-not (Test-Path $binPath)) {
    Write-Error "CRITICAL: whpar.exe not found at: $binPath -- build must be run first."
}
& $binPath -c $dataFile $parityFile 0.10

# 5. INJECT LOCALIZED CORRUPTION (Overwriting 5 MB with zeros)
Write-Host "Injecting 5 MB of zero-sector corruption at the 45 MB offset..." -ForegroundColor Yellow
$stream = [System.IO.File]::OpenWrite($dataFile)
$stream.Seek((45 * 1024 * 1024), [System.IO.SeekOrigin]::Begin) | Out-Null
$zeroBuffer = New-Object byte[] (5 * 1024 * 1024)
$stream.Write($zeroBuffer, 0, $zeroBuffer.Length)
$stream.Close()

# 6. CAPTURE DAMAGED HASH
Write-Host "Computing damaged file hash..." -ForegroundColor Yellow
$damagedHash = Get-FileSHA256 $dataFile

# 7. ATTEMPT REPAIR (archive-only: rely on archive manifest to determine restored paths)
Write-Host "Running whpar decoder to isolate and repair damage (archive-only syntax, forced)..." -ForegroundColor Yellow
& $binPath -r $parityFile -f

# 8. CAPTURE RESTORED HASH
Write-Host "Computing restored file hash..." -Foreground Yellow
# The decoder will use the embedded manifest to restore files into the current directory.
# Prefer the original data filename in the script dir if it was restored, otherwise fall back to the explicit recovered file path.
$restoredPath = $null
if (Test-Path $dataFile) { $restoredPath = $dataFile }
elseif (Test-Path $recoveredFile) { $restoredPath = $recoveredFile }
else { Write-Error "CRITICAL: Restored file not produced in expected locations." }
$recoveredHash = Get-FileSHA256 $restoredPath

# OUTPUT THE RESULTS BLOCK
Write-Host "" 
Write-Host "==================================================================" -ForegroundColor Green
Write-Host "original hash : $originalHash" -ForegroundColor Green
Write-Host "corrupted hash: $damagedHash" -ForegroundColor Red
Write-Host "restored hash : $recoveredHash" -ForegroundColor Green
Write-Host "==================================================================" -ForegroundColor Green

if ($originalHash -eq $recoveredHash) {
    Write-Host "VERIFICATION SUCCESS: Restored file matches the original perfectly!" -ForegroundColor Green
} else {
    Write-Host "VERIFICATION FAILURE: Hashes mismatch. Repair was unsuccessful." -ForegroundColor Red
}
