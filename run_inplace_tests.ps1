$ErrorActionPreference = "Stop"

$d = Join-Path $env:TEMP "whpar_inplace_tests"
if (Test-Path "$d") { Remove-Item -Path "$d" -Recurse -Force }
New-Item -ItemType Directory "$d" | Out-Null

$whpar = "C:\Users\edslo\source\repos\whpar\build\Release\whpar.exe"

function New-RandomFile($path, $size) {
    $bytes = New-Object byte[] $size
    [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $dir = Split-Path -Parent $path
    if (!(Test-Path "$dir")) { New-Item -ItemType Directory "$dir" | Out-Null }
    [System.IO.File]::WriteAllBytes($path, $bytes)
}

function Hash-File($path) {
    return (Get-FileHash $path -Algorithm SHA256).Hash
}

$passed = 0
$failed = 0

function Run-Test($name, $scriptBlock) {
    Write-Host "`n=== $name ===" -ForegroundColor Cyan
    # d=$d whpar=$whpar
    try {
        & $scriptBlock
        Write-Host "PASS" -ForegroundColor Green
        $script:passed++
    } catch {
        Write-Host "FAILED: $_" -ForegroundColor Red
        $script:failed++
    }
}

# ===== TEST 1: Single file, in-place =====
Run-Test "Single file in-place" {
    $td = Join-Path $d "t1"
    $bak = Join-Path $d "t1bak"
    New-Item -ItemType Directory "$td" | Out-Null
    New-RandomFile (Join-Path $td "f.bin") 524288
    $oh = Hash-File (Join-Path $td "f.bin")
    & $whpar -c (Join-Path $td "f.bin") -o $bak -b 64 0.50 --force
    if ($LASTEXITCODE -ne 0) { throw "create failed" }

    $fs = [System.IO.File]::OpenWrite((Join-Path $td "f.bin"))
    $fs.Seek(70000, 0) | Out-Null
    $fs.Write((New-Object byte[] 180000), 0, 180000)
    $fs.Close()

    Push-Location "$td"
    & $whpar -r "$bak.p50.whpar" -o . --force
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "repair failed" }
    Pop-Location

    $rh = Hash-File (Join-Path $td "f.bin")
    if ($oh -ne $rh) { throw "hash mismatch: $oh vs $rh" }
}

# ===== TEST 2: Single file, separate output =====
Run-Test "Single file separate output" {
    $td = Join-Path $d "t2"
    $out = Join-Path $d "t2_out"
    $bak = Join-Path $d "t2bak"
    $null = New-Item -ItemType Directory "$td" -Force -ErrorAction Stop
    $null = New-Item -ItemType Directory "$out" -Force -ErrorAction Stop
    New-RandomFile (Join-Path $td "f.bin") 524288
    $oh = Hash-File (Join-Path $td "f.bin")
    & $whpar -c (Join-Path $td "f.bin") -o $bak -b 64 0.50 --force
    if ($LASTEXITCODE -ne 0) { throw "create failed" }
    if (!(Test-Path "$bak.p50.whpar")) { throw "parity file missing: $bak.p50.whpar" }

    $fs = [System.IO.File]::OpenWrite((Join-Path $td "f.bin"))
    $fs.Seek(70000, 0) | Out-Null
    $fs.Write((New-Object byte[] 180000), 0, 180000)
    $fs.Close()

    Write-Host "  out before repair: exists=$(Test-Path "$out") isdir=$([System.IO.Directory]::Exists("$out"))"
    Push-Location "$td"
    & $whpar -r "$bak.p50.whpar" -o "$out" --force
    Write-Host "  repair exit=$LASTEXITCODE"
    Pop-Location

    $rp = Join-Path $out "f.bin"
    Write-Host "  expecting file at $rp"
    if (!(Test-Path "$rp")) { 
        Write-Host "  path ${out}:"
        $item = Get-Item "${out}" -ErrorAction SilentlyContinue
        if ($item) { Write-Host "    mode=$($item.Mode) len=$($item.Length) type=$($item.GetType().Name)" }
        throw "file not created at $rp"
    }
    $rh = Hash-File $rp
    if ($oh -ne $rh) { throw "hash mismatch" }
}

# ===== TEST 3: Folder with nested subdirs, in-place =====
Run-Test "Folder with nested subdirs in-place" {
    $td = Join-Path $d "t3"
    $bak = Join-Path $d "t3bak"
    New-Item -ItemType Directory (Join-Path $td "a/b") -Force | Out-Null
    # Use larger files so corruption doesn't wipe all blocks per track
    New-RandomFile (Join-Path $td "r.bin") 524288
    New-RandomFile (Join-Path $td "a/x.bin") 524288
    New-RandomFile (Join-Path $td "a/b/y.bin") 262144

    $hs = @{}
    Get-ChildItem -Path "$td" -Recurse -File | ForEach-Object {
        $r = [System.IO.Path]::GetRelativePath($td, $_.FullName)
        $hs[$r] = Hash-File $_.FullName
    }

    & $whpar -c (Join-Path $td "r.bin") (Join-Path $td "a") -o $bak -b 64 0.50 --force
    if ($LASTEXITCODE -ne 0) { throw "create failed" }

    # Corrupt only one block per file
    Get-ChildItem -Path "$td" -Recurse -File | ForEach-Object {
        $fs = [System.IO.File]::OpenWrite($_.FullName)
        $fs.Seek(70000, 0) | Out-Null
        $fs.Write((New-Object byte[] 64000), 0, 64000)
        $fs.Close()
    }

    Push-Location "$td"
    & $whpar -r "$bak.p50.whpar" -o . --force
    Write-Host "  repair exit=$LASTEXITCODE"
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "repair failed" }
    Pop-Location

    Get-ChildItem -Path "$td" -Recurse -File | ForEach-Object {
        $r = [System.IO.Path]::GetRelativePath($td, $_.FullName)
        $h = Hash-File $_.FullName
        if ($hs[$r] -ne $h) { throw "$r mismatch" }
    }
}

# ===== TEST 4: Multiple source folders, in-place =====
Run-Test "Multiple source folders in-place" {
    $td = Join-Path $d "t4"
    $bak = Join-Path $d "t4bak"
    New-Item -ItemType Directory (Join-Path $td "f1") -Force | Out-Null
    New-Item -ItemType Directory (Join-Path $td "f2/sub") -Force | Out-Null
    New-RandomFile (Join-Path $td "f1/a.bin") 524288
    New-RandomFile (Join-Path $td "f1/b.bin") 262144
    New-RandomFile (Join-Path $td "f2/c.bin") 524288
    New-RandomFile (Join-Path $td "f2/sub/d.bin") 262144

    $hs = @{}
    Get-ChildItem -Path "$td" -Recurse -File | ForEach-Object {
        $r = [System.IO.Path]::GetRelativePath($td, $_.FullName)
        $hs[$r] = Hash-File $_.FullName
    }

    & $whpar -c (Join-Path $td "f1") (Join-Path $td "f2") -o $bak -b 64 0.99 --force
    if ($LASTEXITCODE -ne 0) { throw "create failed" }

    # Corrupt one block per file
    Get-ChildItem -Path "$td" -Recurse -File | ForEach-Object {
        $fs = [System.IO.File]::OpenWrite($_.FullName)
        $fs.Seek(70000, 0) | Out-Null
        $fs.Write((New-Object byte[] 64000), 0, 64000)
        $fs.Close()
    }

    Push-Location "$td"
    & $whpar -r "$bak.p99.whpar" -o . --force
    Write-Host "  repair exit=$LASTEXITCODE"
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "repair failed" }
    Pop-Location

    Get-ChildItem -Path "$td" -Recurse -File | ForEach-Object {
        $r = [System.IO.Path]::GetRelativePath($td, $_.FullName)
        $h = Hash-File $_.FullName
        if ($hs[$r] -ne $h) { throw "$r mismatch" }
    }
}

# ===== TEST 5: Mixed files + folders, in-place =====
Run-Test "Mixed files + folders in-place" {
    $td = Join-Path $d "t5"
    $bak = Join-Path $d "t5bak"
    New-Item -ItemType Directory (Join-Path $td "fold") -Force | Out-Null
    New-RandomFile (Join-Path $td "a.dat") 524288
    New-RandomFile (Join-Path $td "b.dat") 262144
    New-RandomFile (Join-Path $td "fold/c.dat") 262144

    $hs = @{}
    Get-ChildItem -Path "$td" -Recurse -File | ForEach-Object {
        $r = [System.IO.Path]::GetRelativePath($td, $_.FullName)
        $hs[$r] = Hash-File $_.FullName
    }

    & $whpar -c (Join-Path $td "a.dat") (Join-Path $td "b.dat") (Join-Path $td "fold") -o $bak -b 64 0.99 --force
    if ($LASTEXITCODE -ne 0) { throw "create failed" }

    # Corrupt one block per file
    Get-ChildItem -Path "$td" -Recurse -File | ForEach-Object {
        $fs = [System.IO.File]::OpenWrite($_.FullName)
        $fs.Seek(70000, 0) | Out-Null
        $fs.Write((New-Object byte[] 64000), 0, 64000)
        $fs.Close()
    }

    Push-Location "$td"
    & $whpar -r "$bak.p99.whpar" -o . --force
    Write-Host "  repair exit=$LASTEXITCODE"
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "repair failed" }
    Pop-Location

    Get-ChildItem -Path "$td" -Recurse -File | ForEach-Object {
        $r = [System.IO.Path]::GetRelativePath($td, $_.FullName)
        $h = Hash-File $_.FullName
        if ($hs[$r] -ne $h) { throw "$r mismatch" }
    }
}

# ===== TEST 6: Multi-file fresh restore to separate dir =====
Run-Test "Multi-file fresh restore" {
    $td = Join-Path $d "t6"
    $out = Join-Path $d "t6_restored"
    $bak = Join-Path $d "t6bak"
    New-Item -ItemType Directory (Join-Path $td "src") -Force | Out-Null
    New-RandomFile (Join-Path $td "src/x.bin") 200000
    New-RandomFile (Join-Path $td "src/y.bin") 300000

    $hs = @{}
    Get-ChildItem (Join-Path $td "src") -File | ForEach-Object {
        $hs[$_.Name] = Hash-File $_.FullName
    }

    & $whpar -c (Join-Path $td "src") -o $bak -b 64 0.50 --force
    if ($LASTEXITCODE -ne 0) { throw "create failed" }

    Get-ChildItem (Join-Path $td "src") -File | ForEach-Object {
        $fs = [System.IO.File]::OpenWrite($_.FullName)
        $fs.Seek(10000, 0) | Out-Null
        $fs.Write((New-Object byte[] 40000), 0, 40000)
        $fs.Close()
    }

    Push-Location (Join-Path $td "src")
    & $whpar -r "$bak.p50.whpar" -o "$out" --force
    if ($LASTEXITCODE -ne 0) { Pop-Location; throw "repair failed" }
    Pop-Location

    Get-ChildItem "$out" -File | ForEach-Object {
        $h = Hash-File $_.FullName
        if ($hs[$_.Name] -ne $h) { throw "$($_.Name) mismatch" }
    }
}

Write-Host "`n======================================" -ForegroundColor Yellow
Write-Host "RESULTS: $passed passed, $failed failed"
if ($failed -gt 0) { exit 1 } else { Write-Host "ALL TESTS PASSED" -ForegroundColor Green ; exit 0 }
