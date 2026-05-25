// whpar - High-Speed Fountain Parity CLI Tool
// Copyright (C) 2026 Edward Sloter
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <string>
#include <cstdint>

void RepairDataset(const std::string& damagedPath, const std::string& parityPath, const std::string& outputPath, bool force = false, bool debug = false, bool showTiming = false, uint32_t numJobs = 0, uint64_t maxMemBytes = 0);

void InfoCheck(const std::string& parityPath, bool debug = false, const std::string& sourceDir = "");

void ListManifest(const std::string& parityPath);
