// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "machine.hpp"

#include "test_util.hpp"

#include <vector>

int main()
{
	Machine machine;
	std::vector<int16_t> captured;
	machine.set_audio_sink([&](const int16_t *samples, size_t count) {
		captured.assign(samples, samples + count);
	});
	for (uint32_t i = 0; i < 80; i++) {
		const int16_t sample = (int16_t)(i * 100);
		machine.write16(Machine::SYS_SRAM_BASE + 0xA0u + 2u * i, (uint16_t)sample);
	}
	CHECK_EQ(captured.size(), 80u);
	for (size_t i = 0; i < captured.size(); i++) CHECK_EQ(captured[i], (int16_t)(i * 100));
	return 0;
}
