// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "machine.hpp"

#include "test_util.hpp"

#include <cstdint>

namespace {

constexpr uint32_t GPIOA = 0x14020000;
constexpr uint32_t DDR = 0x04;
constexpr uint32_t DR = 0x00;
constexpr uint32_t EXT = 0x50;
constexpr uint32_t CS = 1u << 24;
constexpr uint32_t CLK = 1u << 7;
constexpr uint32_t DATA = 1u << 8;

void pins(Machine &m, uint32_t ddr, uint32_t dr)
{
	m.write32(GPIOA + DDR, ddr);
	m.write32(GPIOA + DR, dr);
}

void bit(Machine &m, bool value)
{
	pins(m, CS | CLK | DATA, value ? DATA : 0);
	pins(m, CS | CLK | DATA, (value ? DATA : 0) | CLK);
	pins(m, CS | CLK | DATA, value ? DATA : 0);
}

void write_reg(Machine &m, uint8_t reg, uint16_t value)
{
	pins(m, CS | CLK | DATA, CS | CLK | DATA);
	pins(m, CS | CLK | DATA, CLK | DATA);
	for (int i = 23; i >= 0; i--) bit(m, (((uint32_t)reg << 16 | value) >> i) & 1u);
	pins(m, CS | CLK | DATA, CS | CLK | DATA);
}

uint16_t read_reg(Machine &m, uint8_t reg)
{
	pins(m, CS | CLK | DATA, CS | CLK | DATA);
	pins(m, CS | CLK | DATA, CLK | DATA);
	for (int i = 7; i >= 0; i--) bit(m, (((uint32_t)reg | 0x80u) >> i) & 1u);
	pins(m, CS | CLK, 0);
	uint16_t value = 0;
	for (int i = 0; i < 16; i++) {
		pins(m, CS | CLK, 0);
		pins(m, CS | CLK, CLK);
		value = (uint16_t)((value << 1) | ((m.read32(GPIOA + EXT) & DATA) ? 1u : 0u));
	}
	pins(m, CS | CLK | DATA, CS | CLK | DATA);
	return value;
}

} // namespace

int main()
{
	Machine machine;
	write_reg(machine, 0x30, 0xC1FE);
	CHECK_EQ(read_reg(machine, 0x30), 0xC1FE);
	CHECK_EQ(read_reg(machine, 0x0C), 0x0200);
	CHECK((read_reg(machine, 0x67) & 0x1FFu) > 0);
	return 0;
}
