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

cmake_minimum_required(VERSION 3.10)
project(whpar CXX)

set(CMAKE_CXX_STANDARD 17)

# Maps your modular local code and internal third-party namespaces
include_directories(.)
include_directories(include)
include_directories(include/wirehair)
include_directories(src)

# Compile into your custom target
add_executable(whpar
    main.cpp
    create.cpp
    create.h
    repair.cpp
    repair.h
    parity.h
    src/gf256.cpp
    src/wirehair.cpp
    src/WirehairCodec.cpp
    src/WirehairTools.cpp
)

if(MSVC)
    target_compile_options(whpar PRIVATE /O2 /Oi /Ot /W3)
elseif(UNIX)
    target_compile_options(whpar PRIVATE -O3 -Wall)
    target_compile_definitions(whpar PRIVATE _POSIX_C_SOURCE=200809L _GNU_SOURCE)
endif()
