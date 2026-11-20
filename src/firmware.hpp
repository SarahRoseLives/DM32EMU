// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include <cstdint>
#include <string>

class Machine;

struct FirmwareInfo {
	uint32_t entry = 0;
	bool elf = false;
};

bool load_firmware(Machine &machine, const std::string &path, FirmwareInfo &info, std::string &err);
bool find_function_range(const std::string &firmware, const std::string &name, uint32_t &lo, uint32_t &hi);
