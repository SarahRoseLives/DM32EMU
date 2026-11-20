// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
//
// End-to-end virtual RF test against a real RadioFoundry image. This test is
// skipped unless the firmware, codec backup and two DMR codeplugs are provided.
// The two codeplugs must have different DMR IDs and a DMR channel as the home
// channel; otherwise the receiver treats the caller as its own echo.
#include "codec_backup.hpp"
#include "cpu.hpp"
#include "firmware.hpp"
#include "machine.hpp"
#include "virtual_rf.hpp"

#include "test_util.hpp"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

struct Config {
	std::string firmware;
	std::string codec;
	std::string data_a;
	std::string data_b;
};

bool getenv_str(const char *name, std::string &out)
{
	const char *value = std::getenv(name);
	if (value == nullptr || *value == '\0') return false;
	out = value;
	return true;
}

bool setup(Machine &machine, const Config &cfg, const std::string &data, uint32_t &entry)
{
	std::string err;
	std::vector<uint8_t> image;
	uint32_t offset = 0;
	if (!load_data_backup(data, image, offset, err) ||
	    !machine.install_data_flash(image, offset, err)) {
		std::fprintf(stderr, "data backup %s: %s\n", data.c_str(), err.c_str());
		return false;
	}
	FirmwareInfo info;
	if (!load_firmware(machine, cfg.firmware, info, err)) {
		std::fprintf(stderr, "firmware: %s\n", err.c_str());
		return false;
	}
	std::vector<uint8_t> slot;
	CodecBackupInfo codec_info;
	if (!build_codec_slot(cfg.codec, slot, codec_info, err) ||
	    !machine.install_codec_slot(slot, err)) {
		std::fprintf(stderr, "codec: %s\n", err.c_str());
		return false;
	}
	std::fprintf(stderr, "codec: %s\n", codec_info.profile.c_str());
	entry = info.entry;
	return true;
}

uint16_t crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xFFFF;
	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i] << 8;
		for (int bit = 0; bit < 8; bit++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
	}
	return crc;
}

std::vector<uint8_t> command_frame(uint8_t cmd, const std::vector<uint8_t> &payload, int seq)
{
	std::vector<uint8_t> frame = {0xA5, 0x5A, cmd, (uint8_t)seq, (uint8_t)payload.size(),
				      (uint8_t)(payload.size() >> 8)};
	frame.insert(frame.end(), payload.begin(), payload.end());
	const uint16_t crc = crc16(frame.data() + 2, frame.size() - 2);
	frame.push_back((uint8_t)crc);
	frame.push_back((uint8_t)(crc >> 8));
	return frame;
}

std::vector<uint8_t> view_frame(int section, int seq)
{
	return command_frame(0x09, {(uint8_t)section}, seq);
}

long value_of(const std::string &text, const char *key)
{
	const std::string needle(key);
	const size_t pos = text.find(needle);
	if (pos == std::string::npos) return -1;
	return std::strtol(text.c_str() + pos + needle.size(), nullptr, 10);
}

int skip()
{
	std::fprintf(stderr, "firmware integration test skipped: set DM32EMU_TEST_FIRMWARE, "
			     "DM32EMU_TEST_CODEC, DM32EMU_TEST_DATA_A and DM32EMU_TEST_DATA_B\n");
	return 77;
}

} // namespace

int main()
{
	Config cfg;
	if (!getenv_str("DM32EMU_TEST_FIRMWARE", cfg.firmware) ||
	    !getenv_str("DM32EMU_TEST_CODEC", cfg.codec) ||
	    !getenv_str("DM32EMU_TEST_DATA_A", cfg.data_a) ||
	    !getenv_str("DM32EMU_TEST_DATA_B", cfg.data_b))
		return skip();

	Machine a;
	Machine b;
	uint32_t entry_a = 0, entry_b = 0;
	if (!setup(a, cfg, cfg.data_a, entry_a) || !setup(b, cfg, cfg.data_b, entry_b)) return 1;

	auto bus = std::make_shared<VirtualRfBus>();
	a.attach_virtual_rf(bus, 0);
	b.attach_virtual_rf(bus, 1);
	a.set_virtual_rf_enabled(true);
	b.set_virtual_rf_enabled(true);
	std::vector<uint8_t> b_uart;
	b.set_uart_tx_sink([&](uint8_t c) { b_uart.push_back(c); });

	Cpu cpu_a(a);
	Cpu cpu_b(b);
	cpu_a.reset(entry_a);
	cpu_b.reset(entry_b);

	uint32_t idle_lo = 0, idle_hi = 0;
	if (find_function_range(cfg.firmware, "prvIdleTask", idle_lo, idle_hi)) {
		cpu_a.set_idle_range(idle_lo, idle_hi);
		cpu_b.set_idle_range(idle_lo, idle_hi);
		std::fprintf(stderr, "idle range: 0x%08x-0x%08x\n", idle_lo, idle_hi);
	}

	uint64_t steps = 0;
	auto tick = [&] {
		if ((steps & 0x3FFu) == 0) {
			a.tick(cpu_a.cycles());
			b.tick(cpu_b.cycles());
		}
	};
	auto run = [&](uint64_t count) {
		for (uint64_t i = 0; i < count; i++, steps++) {
			tick();
			cpu_a.step();
			cpu_b.step();
		}
	};
	auto query_view = [&](int section, int seq) {
		const auto frame = view_frame(section, seq);
		for (uint8_t c : frame) b.feed_uart(0, c);
		const size_t before = b_uart.size();
		run(3000000);
		std::string text;
		for (size_t i = before; i < b_uart.size(); i++) {
			const uint8_t c = b_uart[i];
			text.push_back((c >= 32 && c < 127) || c == '\n' || c == '\r' || c == '\t' ? (char)c : '.');
		}
		return text;
	};

	for (; steps < 160000000ULL && (a.frame_count() < 3 || b.frame_count() < 3); steps++) {
		tick();
		cpu_a.step();
		cpu_b.step();
	}
	std::fprintf(stderr, "boot: %llu steps, frames %llu/%llu\n", (unsigned long long)steps,
		     (unsigned long long)a.frame_count(), (unsigned long long)b.frame_count());
	CHECK(a.frame_count() >= 3);
	CHECK(b.frame_count() >= 3);

	run(10000000);
	const std::string dmr_view = query_view(2, 0x20);
	const long channel_ts = value_of(dmr_view, "dmr.ts=");
	if (channel_ts != 1 && channel_ts != 2) {
		std::fprintf(stderr, "radio B is not on a DMR channel; dmr view:\n%s\n", dmr_view.c_str());
		return 1;
	}
	bus->set_timeslot((uint8_t)channel_ts);
	std::fprintf(stderr, "virtual RF timeslot: TS%ld\n", channel_ts);

	std::string err;
	CHECK(a.set_key("ptt", true, err));
	run(40000000);
	CHECK(a.set_key("ptt", false, err));
	run(10000000);

	std::fprintf(stderr, "rf: sent=%llu delivered=%llu dropped=%llu\n",
		     (unsigned long long)bus->sent(), (unsigned long long)bus->delivered(),
		     (unsigned long long)bus->dropped());
	CHECK(bus->sent() > 0);
	CHECK(bus->delivered() > 0);

	const std::string stats = query_view(4, 0x50);
	const long calls = value_of(stats, "call.rx_calls=");
	const long voice = value_of(stats, "call.rx_voice_bursts=");
	if (calls < 1) {
		std::fprintf(stderr, "no call at radio B; dmr_stats view:\n%s\n", stats.c_str());
		std::fprintf(stderr, "check that the two codeplugs have different DMR IDs and a DMR home channel\n");
		return 1;
	}
	CHECK(calls >= 1);
	CHECK(voice > 0);
	std::fprintf(stderr, "radio B: rx_calls=%ld, voice_bursts=%ld\n", calls, voice);

	return 0;
}
