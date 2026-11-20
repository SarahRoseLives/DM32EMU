// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "codec_backup.hpp"

#include "test_util.hpp"

#include <zlib.h>

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


void put16(std::vector<uint8_t> &out, uint16_t value)
{
	out.push_back((uint8_t)value);
	out.push_back((uint8_t)(value >> 8));
}

void put32(std::vector<uint8_t> &out, uint32_t value)
{
	out.push_back((uint8_t)value);
	out.push_back((uint8_t)(value >> 8));
	out.push_back((uint8_t)(value >> 16));
	out.push_back((uint8_t)(value >> 24));
}

void make_zip(const std::string &path, const std::string &name, const std::vector<uint8_t> &data, bool bad_crc)
{
	const uint32_t crc = (uint32_t)::crc32(0, data.data(), (uInt)data.size()) ^ (bad_crc ? 1u : 0u);
	std::vector<uint8_t> zip;
	put32(zip, 0x04034b50u);
	put16(zip, 20);
	put16(zip, 0);
	put16(zip, 0);
	put16(zip, 0);
	put16(zip, 0);
	put32(zip, crc);
	put32(zip, (uint32_t)data.size());
	put32(zip, (uint32_t)data.size());
	put16(zip, (uint16_t)name.size());
	put16(zip, 0);
	zip.insert(zip.end(), name.begin(), name.end());
	zip.insert(zip.end(), data.begin(), data.end());

	const uint32_t local_size = 30u + (uint32_t)name.size() + (uint32_t)data.size();
	std::vector<uint8_t> central;
	put32(central, 0x02014b50u);
	put16(central, 20);
	put16(central, 20);
	put16(central, 0);
	put16(central, 0);
	put16(central, 0);
	put16(central, 0);
	put32(central, crc);
	put32(central, (uint32_t)data.size());
	put32(central, (uint32_t)data.size());
	put16(central, (uint16_t)name.size());
	put16(central, 0);
	put16(central, 0);
	put16(central, 0);
	put16(central, 0);
	put32(central, 0);
	put32(central, 0);
	central.insert(central.end(), name.begin(), name.end());

	std::vector<uint8_t> tail;
	put32(tail, 0x06054b50u);
	put16(tail, 0);
	put16(tail, 0);
	put16(tail, 1);
	put16(tail, 1);
	put32(tail, (uint32_t)central.size());
	put32(tail, local_size);
	put16(tail, 0);

	zip.insert(zip.end(), central.begin(), central.end());
	zip.insert(zip.end(), tail.begin(), tail.end());

	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out.write((const char *)zip.data(), (std::streamsize)zip.size());
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

	TempFile zip("dm32emu-backup-" + id + ".zip");
	const std::vector<uint8_t> payload(256, 0x5A);
	make_zip(zip.path, "data-memory.bin", payload, false);
	CHECK(load_data_backup(zip.path, data, offset, err));
	CHECK_EQ(data.size(), payload.size());
	CHECK_EQ(data[0], 0x5A);
	CHECK_EQ(offset, 0);

	TempFile bad_zip("dm32emu-bad-" + id + ".zip");
	make_zip(bad_zip.path, "data-memory.bin", payload, true);
	CHECK(!load_data_backup(bad_zip.path, data, offset, err));
	CHECK(err.find("CRC") != std::string::npos);

	return 0;
}
