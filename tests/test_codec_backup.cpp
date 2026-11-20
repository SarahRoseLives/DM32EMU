// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "codec_backup.hpp"

#include "test_util.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>


namespace {

struct TempFile {
	std::string path;
	explicit TempFile(const std::string &name)
	{
		path = (std::filesystem::temp_directory_path() / name).string();
	}
	~TempFile()
	{
		std::error_code ec;
		std::filesystem::remove(path, ec);
	}
};

void make_file(const std::string &path, size_t size, uint8_t value = 0)
{
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (size == 0) return;
	const size_t head = size < 65536 ? size : 65536;
	std::vector<char> buf(head, (char)value);
	f.write(buf.data(), (std::streamsize)buf.size());
	if (size > head) {
		f.seekp((std::streamoff)size - 1);
		f.put((char)value);
	}
}

} // namespace

int main()
{
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::string id = std::to_string((long long)stamp);
	std::vector<uint8_t> data;
	uint32_t offset = 0;
	std::string err;

	TempFile codeplug("dm32emu-codeplug-" + id + ".bin");
	make_file(codeplug.path, 0x120000, 0x5A);
	CHECK(load_data_backup(codeplug.path, data, offset, err));
	CHECK_EQ(data.size(), 0x120000);
	CHECK_EQ(offset, 0x700000);
	CHECK_EQ(data[0xE8], 0x5A);

	TempFile full("dm32emu-full-" + id + ".bin");
	make_file(full.path, 0x1000000, 0x11);
	CHECK(load_data_backup(full.path, data, offset, err));
	CHECK_EQ(data.size(), 0x1000000);
	CHECK_EQ(offset, 0);

	TempFile huge("dm32emu-huge-" + id + ".bin");
	make_file(huge.path, 0x1000001, 0x22);
	CHECK(!load_data_backup(huge.path, data, offset, err));
	CHECK(!err.empty());

	TempFile missing("dm32emu-missing-" + id + ".bin");
	std::error_code ec;
	std::filesystem::remove(missing.path, ec);
	CHECK(!load_data_backup(missing.path, data, offset, err));

	TempFile short_pm("dm32emu-short-pm-" + id + ".bin");
	make_file(short_pm.path, 1024, 0);
	std::vector<uint8_t> slot;
	CodecBackupInfo info;
	CHECK(!build_codec_slot(short_pm.path, slot, info, err));
	CHECK(!err.empty());

	return 0;
}
