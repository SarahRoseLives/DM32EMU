// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "firmware.hpp"
#include "elf.hpp"
#include "machine.hpp"

#include <filesystem>

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

bool find_function_range(const std::string &firmware, const std::string &name, uint32_t &lo, uint32_t &hi)
{
	std::string path = firmware;
	if (path.size() < 4 || path.substr(path.size() - 4) != ".elf") {
		if (path.size() >= 4 && path.substr(path.size() - 4) == ".bin") path.resize(path.size() - 4);
		path += ".elf";
		if (!std::filesystem::exists(path)) return false;
	}
	std::vector<ElfSymbol> syms;
	std::string err;
	if (!load_symbols(path, syms, err)) return false;
	for (const auto &s : syms) {
		if (s.name == name && s.size != 0) {
			lo = s.addr;
			hi = s.addr + s.size;
			return true;
		}
	}
	return false;
}
