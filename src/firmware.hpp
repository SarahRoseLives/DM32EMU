// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The DM32EMU Authors
#pragma once

#include <cstdint>
#include <string>

class Machine;

struct FirmwareInfo {
	uint32_t entry = 0;
	bool elf = false;
};

bool load_firmware(Machine &machine, const std::string &path, FirmwareInfo &info, std::string &err);
