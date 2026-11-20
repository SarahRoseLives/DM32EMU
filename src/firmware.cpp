// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The DM32EMU Authors
#include "firmware.hpp"
#include "elf.hpp"
#include "machine.hpp"

bool load_firmware(Machine &machine, const std::string &path, FirmwareInfo &info, std::string &err)
{
	const bool is_elf = path.size() >= 4 && path.substr(path.size() - 4) == ".elf";
	info.elf = is_elf;
	if (is_elf) {
		ElfImage image;
		if (!load_elf(path, image, err)) return false;
		if (!machine.load_elf_image(image, err)) return false;
		info.entry = image.entry;
	} else {
		std::vector<uint8_t> image;
		if (!load_file(path, image, err)) return false;
		if (!machine.load_bin(image, Machine::APP_BASE, err)) return false;
		info.entry = Machine::APP_BASE;
	}
	return true;
}
