// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ElfSegment {
	uint32_t vaddr = 0;
	uint32_t paddr = 0;
	uint32_t filesz = 0;
	uint32_t memsz = 0;
	std::vector<uint8_t> data;
};

struct ElfImage {
	uint32_t entry = 0;
	std::vector<ElfSegment> segments;
};

struct ElfSymbol {
	uint32_t addr = 0;
	uint32_t size = 0;
	std::string name;
};

bool load_elf(const std::string &path, ElfImage &out, std::string &err);
bool load_symbols(const std::string &path, std::vector<ElfSymbol> &out, std::string &err);
bool load_file(const std::string &path, std::vector<uint8_t> &out, std::string &err);
