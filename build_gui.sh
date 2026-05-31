#!/bin/sh
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

set -e

echo "=== Starting whpar GUI Automated Rebuild ==="

# 1. Remove old build directory
if [ -d "build" ]; then
    echo "Removing old build directory..."
    rm -rf build
fi

# 2. Create fresh build directory
echo "Creating fresh build directory..."
mkdir build
cd build

# 3. Configure with CMake, enable GUI
echo "Configuring with CMake (GUI enabled)..."
cmake .. -DCMAKE_BUILD_TYPE=Release -DWHPAR_GUI=ON

# 4. Compile GUI binary
echo "Compiling whpar-gui binary..."
cmake --build . --target whpar-gui -- -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "=== GUI Rebuild Completed Flawlessly! ==="
