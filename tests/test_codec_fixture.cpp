// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "codec_backup.hpp"
#include "machine.hpp"

#include "test_util.hpp"

#include <cstdlib>
#include <string>
#include <vector>

int main()
{
	const char *path = std::getenv("DM32EMU_TEST_CODEC");
	if (path == nullptr || *path == '\0') {
		std::fprintf(stderr, "codec fixture test skipped: set DM32EMU_TEST_CODEC\n");
		return 77;
	}

	std::vector<uint8_t> slot;
	CodecBackupInfo info;
	std::string err;
	CHECK(build_codec_slot(path, slot, info, err));
	CHECK(info.tag == 46 || info.tag == 49);
	CHECK(info.code_len == 0x14600);
	CHECK(!slot.empty());

	Machine machine;
	CHECK(machine.install_codec_slot(slot, err));
	CHECK_EQ(machine.read32(Machine::FLASH_BASE + 0xE8000), 0x43524444u);

	// Installing again replaces the existing slot.
	CHECK(machine.install_codec_slot(slot, err));
	CHECK_EQ(machine.read32(Machine::FLASH_BASE + 0xE8000), 0x43524444u);

	std::vector<uint8_t> bad(128, 0);
	CHECK(!machine.install_codec_slot(bad, err));
	CHECK(!err.empty());

	const char *data_path = std::getenv("DM32EMU_TEST_DATA");
	if (data_path != nullptr && *data_path != '\0') {
		std::vector<uint8_t> data;
		uint32_t offset = 0;
		CHECK(load_data_backup(data_path, data, offset, err));
		CHECK(offset == 0 || offset == 0x700000);
		CHECK(machine.install_data_flash(data, offset, err));
	}

	return 0;
}
