// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "machine.hpp"
#include "test_util.hpp"

#include <cstring>
#include <string>

namespace {
constexpr uint32_t GPIOB = 0x14100000;
constexpr uint32_t UART1 = 0x14040000;
}

int main()
{
	Machine machine;
	Machine::GpsFix fix;
	fix.valid = true;
	fix.latitude = 51.5074;
	fix.longitude = -0.1278;
	fix.altitude_m = 35;
	machine.set_gps_fix(fix);

	// GPS power is GPIO 0x2C, active low.
	machine.write32(GPIOB + 0x04, 1u << 12);
	machine.write32(GPIOB + 0x00, 0);
	machine.tick(168000000u); // 1 s of CPU time

	std::string sentences;
	for (int i = 0; i < 512; i++) {
		const uint32_t lsr = machine.read32(UART1 + 0x14);
		if ((lsr & 1u) == 0) break;
		sentences.push_back((char)(machine.read32(UART1 + 0x00) & 0xFFu));
	}
	CHECK(sentences.find("$GPRMC,") != std::string::npos);
	CHECK(sentences.find("$GPGGA,") != std::string::npos);
	CHECK(sentences.find("51") != std::string::npos);
	CHECK(sentences.find("W") != std::string::npos);
	return 0;
}
