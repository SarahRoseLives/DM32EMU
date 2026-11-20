// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "codec_backup.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <zlib.h>

namespace {

constexpr uint32_t CODE_SIG0 = 0x152514D8u;
constexpr uint32_t CODE_SIG1 = 0x74C776CDu;
constexpr uint32_t TABLES_HEAD_CRC = 0x11475389u;
constexpr uint32_t SLOT_MAGIC = 0x43524444u;
constexpr uint32_t SLOT_VERSION = 2u;
constexpr uint32_t HEADER_LEN = 128u;
constexpr uint32_t SLOT_BASE = 0x030E8000u;
constexpr uint32_t SLOT_ROOM = 0x03100000u - SLOT_BASE;
constexpr uint32_t RUN_LO = 0x00043A00u;
constexpr uint32_t RUN_HI = 0x00058000u;
constexpr uint32_t TAB_LO = 0x18000E48u;
constexpr uint32_t TAB_HI = 0x18003E38u;

struct Profile {
	const char *name;
	uint32_t code_crc;
	uint32_t code_len;
	uint32_t code_run;
	uint32_t table_crc;
	uint32_t table_len;
	uint32_t table_run;
	uint32_t tag;
	uint32_t fec_init;
};

const Profile PROFILES[] = {
	{"DM32 046 (01.01.046/01.02.046)", 0xE43F09D9u, 0x14600u, 0x00016CDCu, 0x8A08ACF1u, 0x2F78u, 0x18000EA8u, 46, 0x1BF8u},
	{"DM32 049 (01.01.047/049)", 0x0AF70BEBu, 0x14600u, 0x00016CDCu, 0x8A08ACF1u, 0x2F78u, 0x18000EA8u, 49, 0x1BF8u},
};

uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
void put32(std::vector<uint8_t> &v, uint32_t x)
{
	v.push_back((uint8_t)x);
	v.push_back((uint8_t)(x >> 8));
	v.push_back((uint8_t)(x >> 16));
	v.push_back((uint8_t)(x >> 24));
}

bool read_file(const std::string &path, std::vector<uint8_t> &out, std::string &err)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		err = "cannot open " + path;
		return false;
	}
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	if (f.bad()) {
		err = "cannot read " + path;
		return false;
	}
	return true;
}

bool is_zip(const std::string &path)
{
	std::string ext = std::filesystem::path(path).extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return ext == ".zip";
}

uint32_t zip_crc(const std::vector<uint8_t> &data)
{
	return (uint32_t)::crc32(0, data.data(), (uInt)data.size());
}

bool zip_member(const std::string &path, const std::string &member, std::vector<uint8_t> &out,
		size_t max_size, std::string &err)
{
	std::vector<uint8_t> zip;
	if (!read_file(path, zip, err)) return false;
	if (zip.size() < 22) {
		err = "short ZIP file";
		return false;
	}
	size_t eocd = std::string::npos;
	for (size_t i = zip.size() - 22; i + 22 <= zip.size(); i--) {
		if (le32(zip.data() + i) == 0x06054b50u) {
			eocd = i;
			break;
		}
		if (i == 0) break;
	}
	if (eocd == std::string::npos) {
		err = "ZIP end record not found";
		return false;
	}
	const uint16_t count = le16(zip.data() + eocd + 10);
	const uint32_t cd_off = le32(zip.data() + eocd + 16);
	size_t p = cd_off;
	for (uint16_t n = 0; n < count; n++) {
		if (p + 46 > zip.size() || le32(zip.data() + p) != 0x02014b50u) {
			err = "bad ZIP central directory";
			return false;
		}
		const uint16_t method = le16(zip.data() + p + 10);
		const uint32_t crc = le32(zip.data() + p + 16);
		const uint32_t csize = le32(zip.data() + p + 20);
		const uint32_t usize = le32(zip.data() + p + 24);
		const uint16_t nlen = le16(zip.data() + p + 28);
		const uint16_t xlen = le16(zip.data() + p + 30);
		const uint16_t clen = le16(zip.data() + p + 32);
		const uint32_t lho = le32(zip.data() + p + 42);
		const std::string name(reinterpret_cast<const char *>(zip.data() + p + 46), nlen);
		if (name == member) {
			if (usize > max_size) {
				err = "ZIP member " + member + " is too large";
				return false;
			}
			if (lho + 30 > zip.size() || le32(zip.data() + lho) != 0x04034b50u) {
				err = "bad ZIP local header";
				return false;
			}
			const uint16_t lnlen = le16(zip.data() + lho + 26);
			const uint16_t lxlen = le16(zip.data() + lho + 28);
			const size_t data = lho + 30 + lnlen + lxlen;
			if (data + csize > zip.size()) {
				err = "truncated ZIP member";
				return false;
			}
			if (method == 0) {
				if (csize != usize) {
					err = "bad stored ZIP member size";
					return false;
				}
				out.assign(zip.begin() + (ptrdiff_t)data, zip.begin() + (ptrdiff_t)(data + csize));
			} else if (method == 8) {
				out.resize(usize);
				z_stream zs{};
				if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
					err = "inflateInit2 failed";
					return false;
				}
				zs.next_in = zip.data() + data;
				zs.avail_in = csize;
				zs.next_out = out.data();
				zs.avail_out = usize;
				const int rc = inflate(&zs, Z_FINISH);
				inflateEnd(&zs);
				if (rc != Z_STREAM_END || zs.total_out != usize) {
					err = "cannot inflate ZIP member";
					return false;
				}
			} else {
				err = "unsupported ZIP compression method";
				return false;
			}
			if (zip_crc(out) != crc) {
				err = "ZIP member " + member + " CRC mismatch";
				return false;
			}
			return true;
		}
		p += 46 + nlen + xlen + clen;
	}
	err = "ZIP member " + member + " not found";
	return false;
}

bool get_program_memory(const std::string &path, std::vector<uint8_t> &pm, std::string &err)
{
	if (is_zip(path)) {
		if (!zip_member(path, "program-memory.bin", pm, 0x100000u, err)) return false;
	} else if (!read_file(path, pm, err)) {
		return false;
	}
	if (pm.size() != 0x100000u) {
		err = "expected a 1 MB program-memory.bin or a backup ZIP";
		return false;
	}
	return true;
}

uint32_t crc32_bytes(const uint8_t *p, uint32_t n, uint32_t c = 0)
{
	return (uint32_t)::crc32(c, p, n);
}

bool find_code(const std::vector<uint8_t> &pm, const Profile &prof, std::vector<uint8_t> &code)
{
	const uint8_t sig[8] = {0xD8, 0x14, 0x25, 0x15, 0xCD, 0x76, 0xC7, 0x74};
	for (size_t off = 0; off + prof.code_len <= pm.size(); off += 4) {
		if (std::memcmp(pm.data() + off, sig, sizeof(sig)) != 0) continue;
		if (crc32_bytes(pm.data() + off, prof.code_len) == prof.code_crc) {
			code.assign(pm.begin() + (ptrdiff_t)off, pm.begin() + (ptrdiff_t)(off + prof.code_len));
			return true;
		}
	}
	return false;
}

bool find_tables(const std::vector<uint8_t> &pm, const Profile &prof, std::vector<uint8_t> &tables)
{
	for (size_t off = 0; off + prof.table_len <= pm.size(); off += 4) {
		if (crc32_bytes(pm.data() + off, 64) != TABLES_HEAD_CRC) continue;
		if (crc32_bytes(pm.data() + off, prof.table_len) == prof.table_crc) {
			tables.assign(pm.begin() + (ptrdiff_t)off, pm.begin() + (ptrdiff_t)(off + prof.table_len));
			return true;
		}
	}
	return false;
}

} // namespace

bool build_codec_slot(const std::string &path, std::vector<uint8_t> &slot, CodecBackupInfo &info, std::string &err)
{
	std::vector<uint8_t> pm;
	if (!get_program_memory(path, pm, err)) return false;

	const Profile *prof = nullptr;
	std::vector<uint8_t> code, tables;
	for (const auto &p : PROFILES) {
		if (find_code(pm, p, code) && find_tables(pm, p, tables)) {
			prof = &p;
			break;
		}
	}
	if (!prof) {
		err = "no supported DM32 codec found in the backup (supported: 046, 047/049)";
		return false;
	}
	if (prof->code_len > RUN_HI - RUN_LO || prof->table_run < TAB_LO ||
	    prof->table_run + prof->table_len > TAB_HI) {
		err = "codec windows are outside the DM32 layout";
		return false;
	}

	const uint32_t rel[3] = {0x1AD4u, 0x1AD8u, 0x2E8Cu};
	const uint32_t code_off = HEADER_LEN;
	const uint32_t tab_off = (code_off + prof->code_len + 3u) & ~3u;
	const uint32_t rel_off = (tab_off + prof->table_len + 3u) & ~3u;
	const uint32_t crc = crc32_bytes(reinterpret_cast<const uint8_t *>(rel), sizeof(rel), crc32_bytes(tables.data(), (uint32_t)tables.size(),
								       crc32_bytes(code.data(), (uint32_t)code.size())));

	slot.clear();
	slot.reserve(rel_off + sizeof(rel));
	put32(slot, SLOT_MAGIC);
	put32(slot, SLOT_VERSION);
	put32(slot, code_off);
	put32(slot, (uint32_t)code.size());
	put32(slot, prof->code_run);
	put32(slot, tab_off);
	put32(slot, (uint32_t)tables.size());
	put32(slot, prof->table_run);
	const uint32_t entries[7] = {0x0u, 0x260u, 0xB4u, 0x84u, 0x314u, 0x2E4u, prof->fec_init};
	for (uint32_t e : entries) put32(slot, e);
	put32(slot, prof->tag);
	put32(slot, crc);
	put32(slot, rel_off);
	put32(slot, 3);
	while (slot.size() < HEADER_LEN) slot.push_back(0);
	slot.insert(slot.end(), code.begin(), code.end());
	slot.resize(tab_off, 0xFF);
	slot.insert(slot.end(), tables.begin(), tables.end());
	slot.resize(rel_off, 0xFF);
	for (uint32_t r : rel) put32(slot, r);
	if (slot.size() > SLOT_ROOM) {
		err = "codec slot does not fit in program flash";
		return false;
	}
	info.profile = prof->name;
	info.tag = prof->tag;
	info.code_len = (uint32_t)code.size();
	info.table_len = (uint32_t)tables.size();
	return true;
}

bool load_data_backup(const std::string &path, std::vector<uint8_t> &data, uint32_t &offset, std::string &err)
{
	data.clear();
	offset = 0;
	if (is_zip(path)) {
		if (zip_member(path, "data-memory.bin", data, 0x1000000u, err)) {
			offset = 0;
			return true;
		}
		if (err.find("not found") == std::string::npos) return false;
		err.clear();
		if (zip_member(path, "original-channels.bin", data, 0x1000000u, err)) {
			offset = 0;
			return true;
		}
		if (err.find("not found") == std::string::npos) return false;
		err = "backup ZIP holds no data-memory.bin or original-channels.bin";
		return false;
	}
	if (!read_file(path, data, err)) return false;
	if (data.size() == 0x1000000u) {
		offset = 0;
		return true;
	}
	if (data.size() == 0x120000u) {
		offset = 0x700000u;
		return true;
	}
	if (data.size() <= 0x1000000u) {
		offset = 0;
		return true;
	}
	err = "data backup is larger than 16 MB";
	return false;
}
