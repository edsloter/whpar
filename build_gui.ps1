# whpar - High-Speed Fountain Parity CLI Tool
# Copyright (C) 2026 Edward Sloter
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

param(
    [switch]$static,
    [switch]$debug,
    [switch]$help
)

if ($help) {
    Write-Host @"
Usage: .\build_gui.ps1 [options]

Options:
  -static    Link wxWidgets statically (no DLL copy needed)
  -debug     Build Debug configuration instead of Release
  -help      Show this help message
"@
    exit 0
}

$ErrorActionPreference = "Stop"

$config = if ($debug) { "Debug" } else { "Release" }
$extraCmakeArgs = @()

if ($static) {
    Write-Host "Static linking requested." -ForegroundColor Yellow
    $extraCmakeArgs += "-DWHPAR_USE_STATIC_WX=ON"
}

Write-Host "=== Starting whpar GUI Automated Rebuild ($config) ===" -ForegroundColor Cyan

# 1. Safely remove the old build directory if it exists
if (Test-Path "build") {
    Write-Host "Removing old build directory..." -ForegroundColor Yellow
    Remove-Item -Path "build" -Recurse -Force
}

# 2. Create and change context into the fresh build directory
Write-Host "Creating fresh build directory..." -ForegroundColor Yellow
New-Item -Path "build" -ItemType Directory | Out-Null
Set-Location -Path "build"

# 3. CMake generator selection
$vsGenerators = @(
    "Visual Studio 17 2022",
    "Visual Studio 16 2019",
    "Visual Studio 15 2017"
)
$cmakeArgs = @("..", "-DWHPAR_GUI=ON") + $extraCmakeArgs
$generator = $null
foreach ($gen in $vsGenerators) {
    $output = cmake .. -G $gen @cmakeArgs 2>&1
    if ($LASTEXITCODE -eq 0) { Write-Host $output; $generator = $gen; break }
    Remove-Item -Path "*" -Recurse -Force -ErrorAction SilentlyContinue
}
if (-not $generator) {
    Write-Host "No Visual Studio generator found; using CMake default." -ForegroundColor Yellow
    cmake .. @cmakeArgs
}

# 4. Compile the GUI executable
Write-Host "Compiling whpar-gui binary ($config)..." -ForegroundColor Yellow
cmake --build . --config $config --target whpar-gui

# 5. Copy wxWidgets DLLs to output directory (only for dynamic linking)
if (-not $static) {
    Write-Host "Copying wxWidgets DLLs..." -ForegroundColor Yellow
    $wxDllDir = "$env:WXWIN\lib\vc14x_x64_dll"
    if (-not $env:WXWIN) { $wxDllDir = "C:\wxWidgets\lib\vc14x_x64_dll" }
    if (Test-Path $wxDllDir) {
        Copy-Item "$wxDllDir\*.dll" "$config\" -Force
    }
}

Write-Host "=== GUI Rebuild Completed Flawlessly! ===" -ForegroundColor Green
Set-Location -Path ".."
