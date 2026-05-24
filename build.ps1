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

# Ensure the script stops executing if any command throws an error
$ErrorActionPreference = "Stop"

Write-Host "=== Starting whpar Automated Rebuild ===" -ForegroundColor Cyan

# 1. Safely remove the old build directory if it exists
if (Test-Path "build") {
    Write-Host "Removing old build directory..." -ForegroundColor Yellow
    Remove-Item -Path "build" -Recurse -Force
}

# 2. Create and change context into the fresh build directory
Write-Host "Creating fresh build directory..." -ForegroundColor Yellow
New-Item -Path "build" -ItemType Directory | Out-Null
Set-Location -Path "build"

# 3. CMake generator selection — detect VS versions in descending order
$vsGenerators = @(
    "Visual Studio 17 2022",
    "Visual Studio 16 2019",
    "Visual Studio 15 2017"
)
$generator = $null
foreach ($gen in $vsGenerators) {
    $output = cmake .. -G $gen 2>&1
    if ($LASTEXITCODE -eq 0) { Write-Host $output; $generator = $gen; break }
    # Clean partial build artifacts for next attempt
    Remove-Item -Path "*" -Recurse -Force -ErrorAction SilentlyContinue
}
if (-not $generator) {
    Write-Host "No Visual Studio generator found; using CMake default." -ForegroundColor Yellow
    cmake ..
}

# 4. Compile the target executable binary in high-performance Release mode
Write-Host "Compiling whpar binary in Release mode..." -ForegroundColor Yellow
cmake --build . --config Release

Write-Host "=== Rebuild Completed Flawlessly! ===" -ForegroundColor Green
# Return to repository root (one directory up) before exiting
Set-Location -Path ".."
