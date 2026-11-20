// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct CodecBackupInfo {
	std::string profile;
	uint32_t tag = 0;
	uint32_t code_len = 0;
	uint32_t table_len = 0;
};

// Read a vendor program-memory.bin (or a RadioFoundry backup ZIP that holds
// one) and build the radio's version-2 codec slot. The backup is read-only.
bool build_codec_slot(const std::string &path, std::vector<uint8_t> &slot, CodecBackupInfo &info, std::string &err);

// Read a full data-memory.bin, a stock codeplug start, or an open-firmware
// codeplug image. Returns the bytes and the data-flash offset they belong at.
bool load_data_backup(const std::string &path, std::vector<uint8_t> &data, uint32_t &offset, std::string &err);
