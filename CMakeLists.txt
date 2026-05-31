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
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /I \"${CMAKE_CURRENT_SOURCE_DIR}\"")
endif()

# Maps your modular local code and internal third-party namespaces
include_directories(.)
include_directories(include)
include_directories(include/wirehair)
include_directories(src)

# ── Core library (shared between CLI and GUI) ──
add_library(whpar_core STATIC
    create.cpp
    create.h
    repair.cpp
    repair.h
    parity.h
    portability.h
    src/gf256.cpp
    src/wirehair.cpp
    src/WirehairCodec.cpp
    src/WirehairTools.cpp
)

if(MSVC)
    target_compile_options(whpar_core PRIVATE /O2 /Oi /Ot /W3)
elseif(UNIX)
    target_compile_options(whpar_core PRIVATE -O3 -Wall -mssse3)
    target_compile_definitions(whpar_core PRIVATE _POSIX_C_SOURCE=200809L _GNU_SOURCE)
endif()

# ── CLI frontend ──
add_executable(whpar main.cpp resources.rc)
target_link_libraries(whpar PRIVATE whpar_core)

if(MSVC)
    target_compile_options(whpar PRIVATE /O2 /Oi /Ot /W3)
elseif(UNIX)
    target_compile_options(whpar PRIVATE -O3 -Wall -mssse3)
    target_compile_definitions(whpar PRIVATE _POSIX_C_SOURCE=200809L _GNU_SOURCE)
endif()

# ── GUI frontend (optional) ──
option(WHPAR_GUI "Build whpar with wxWidgets GUI" OFF)
option(WHPAR_USE_STATIC_WX "Link wxWidgets statically" OFF)

if(WHPAR_GUI)
    set(WXWIN "$ENV{WXWIN}" CACHE PATH "Path to wxWidgets root")
    if(NOT WXWIN)
        set(WXWIN "C:/wxWidgets")
    endif()

    set(wxWidgets_ROOT_DIR "${WXWIN}")

    if(WHPAR_USE_STATIC_WX)
        set(wxWidgets_USE_STATIC ON)
        set(wxWidgets_LIB_DIR "${WXWIN}/lib/vc_lib")
    else()
        set(wxWidgets_LIB_DIR "${WXWIN}/lib/vc14x_x64_dll")
    endif()

    set(wxWidgets_CONFIGURATION mswu)

    find_package(wxWidgets REQUIRED COMPONENTS core base adv html)

    include(${wxWidgets_USE_FILE})

    add_executable(whpar-gui guimain.cpp resources.rc)
    target_link_libraries(whpar-gui PRIVATE whpar_core ${wxWidgets_LIBRARIES})

    if(MSVC)
        target_compile_options(whpar-gui PRIVATE /O2 /Oi /Ot /W3)
        target_compile_definitions(whpar-gui PRIVATE _CRT_SECURE_NO_WARNINGS)
        set_target_properties(whpar-gui PROPERTIES WIN32_EXECUTABLE TRUE)
        if(WHPAR_USE_STATIC_WX)
            target_compile_options(whpar_core PRIVATE $<$<CONFIG:Release>:/MT> $<$<CONFIG:Debug>:/MTd>)
            target_compile_options(whpar-gui PRIVATE $<$<CONFIG:Release>:/MT> $<$<CONFIG:Debug>:/MTd>)
        endif()
    elseif(UNIX)
        target_compile_options(whpar-gui PRIVATE -O3 -Wall)
    endif()
endif()
