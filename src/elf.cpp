// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "elf.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <iterator>

namespace {

uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

} // namespace

bool load_file(const std::string &path, std::vector<uint8_t> &out, std::string &err)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		err = "cannot open " + path;
		return false;
	}
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	if (!f.eof() && f.fail()) {
		err = "cannot read " + path;
		return false;
	}
	return true;
}

bool load_symbols(const std::string &path, std::vector<ElfSymbol> &out, std::string &err)
{
	std::vector<uint8_t> file;
	if (!load_file(path, file, err)) {
		return false;
	}
	if (file.size() < 52 || std::memcmp(file.data(), "\x7f" "ELF", 4) != 0 || file[4] != 1 || file[5] != 1) {
		err = "not an ELF32 little-endian file: " + path;
		return false;
	}
	const uint16_t shentsize = rd16(file.data() + 46);
	const uint16_t shnum = rd16(file.data() + 48);
	const uint32_t shoff = rd32(file.data() + 32);
	if (shentsize < 40 || shoff + (size_t)shnum * shentsize > file.size()) {
		err = "truncated section header table";
		return false;
	}
	auto sh = [&](int i) { return file.data() + shoff + (size_t)i * shentsize; };
	int symtab = -1;
	for (int i = 0; i < shnum; i++) {
		if (rd32(sh(i) + 4) == 2) { symtab = i; break; }
	}
	if (symtab < 0) {
		out.clear();
		return true;
	}
	const uint32_t symoff = rd32(sh(symtab) + 16);
	const uint32_t symsize = rd32(sh(symtab) + 20);
	const uint32_t link = rd32(sh(symtab) + 24);
	const uint32_t entsize = rd32(sh(symtab) + 36);
	if (link >= shnum || entsize < 16) {
		err = "bad symbol table";
		return false;
	}
	const uint32_t stroff = rd32(sh(link) + 16);
	const uint32_t strsize = rd32(sh(link) + 20);
	if (stroff + strsize > file.size() || symoff + symsize > file.size()) {
		err = "truncated symbol table";
		return false;
	}
	out.clear();
	for (uint32_t off = symoff; off + 16 <= symoff + symsize; off += entsize) {
		const uint8_t *sym = file.data() + off;
		const uint32_t nameoff = rd32(sym);
		const uint32_t value = rd32(sym + 4);
		const uint32_t size = rd32(sym + 8);
		const uint8_t info = sym[12];
		if ((info & 0xF) != 2 && (info & 0xF) != 0) continue; // functions and notypes
		if (nameoff >= strsize) continue;
		const char *name = (const char *)file.data() + stroff + nameoff;
		if (!*name) continue;
		out.push_back({value, size, name});
	}
	std::sort(out.begin(), out.end(), [](const ElfSymbol &a, const ElfSymbol &b) {
		if (a.addr != b.addr) return a.addr < b.addr;
		return a.name < b.name;
	});
	return true;
}

bool load_elf(const std::string &path, ElfImage &out, std::string &err)
{
	std::vector<uint8_t> file;
	if (!load_file(path, file, err)) {
		return false;
	}
	if (file.size() < 52 || std::memcmp(file.data(), "\x7f" "ELF", 4) != 0) {
		err = "not an ELF file: " + path;
		return false;
	}
	if (file[4] != 1 || file[5] != 1) {
		err = "only ELF32 little-endian is supported";
		return false;
	}
	if (rd16(file.data() + 18) != 0x00F3) {
		err = "not a C-SKY ELF file";
		return false;
	}

	const uint32_t phoff = rd32(file.data() + 28);
	const uint16_t phentsize = rd16(file.data() + 42);
	const uint16_t phnum = rd16(file.data() + 44);
	out.entry = rd32(file.data() + 24);
	out.segments.clear();

	for (uint16_t i = 0; i < phnum; i++) {
		const size_t off = (size_t)phoff + (size_t)i * phentsize;
		if (off + 32 > file.size()) {
			err = "truncated program header table";
			return false;
		}
		const uint8_t *ph = file.data() + off;
		if (rd32(ph) != 1) {
			continue;
		}
		ElfSegment seg;
		seg.vaddr = rd32(ph + 8);
		seg.paddr = rd32(ph + 12);
		seg.filesz = rd32(ph + 16);
		seg.memsz = rd32(ph + 20);
		const size_t fileoff = rd32(ph + 4);
		if (fileoff + seg.filesz > file.size()) {
			err = "truncated ELF segment";
			return false;
		}
		seg.data.assign(file.begin() + (ptrdiff_t)fileoff, file.begin() + (ptrdiff_t)(fileoff + seg.filesz));
		out.segments.push_back(std::move(seg));
	}
	return true;
}
