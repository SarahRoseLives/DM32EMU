// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include <cstdint>

struct DmrBurst {
	uint8_t source = 0;
	uint8_t kind = 0;
	uint8_t voice_index = 0;
	uint8_t cc = 0;
	uint8_t ts = 1;
	uint8_t len = 0;
	uint8_t payload[27]{};
};
