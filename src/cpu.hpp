// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

#include "machine.hpp"

struct CpuFault : std::runtime_error {
	uint32_t pc;
	uint32_t insn;
	CpuFault(uint32_t pc_, uint32_t insn_, const std::string &what)
		: std::runtime_error(what), pc(pc_), insn(insn_)
	{
	}
};

// C-SKY ABIv2 interpreter as used by the CK803S in the HR_C7000.
//
// The instruction encoding is unusual: a 16-bit halfword is little-endian,
// but a 32-bit instruction is formed as (first_half << 16) | second_half.
// The top two bits of the first halfword select the 32-bit form.
class Cpu {
public:
	explicit Cpu(Machine &machine);

	void reset(uint32_t entry);
	void step();

	uint32_t pc() const { return pc_; }
	uint32_t psr() const { return psr_; }
	uint32_t reg(int n) const { return r_[n & 31]; }
	uint64_t cycles() const { return cycles_; }
	bool carry() const { return carry_; }

	bool irq_pending() const;
	void take_irq(int source);

	void dump(std::FILE *out) const;

private:
	Machine &bus_;
	uint32_t r_[32]{};
	uint32_t pc_ = 0;
	uint32_t psr_ = 0x80000100;
	uint32_t epsr_ = 0;
	uint32_t epc_ = 0;
	uint32_t vbr_ = 0;
	uint32_t ss_[5]{};
	bool carry_ = false;
	uint64_t cycles_ = 0;

	uint8_t rd8(uint32_t a) { return bus_.read8(a); }
	uint16_t rd16(uint32_t a) { return bus_.read16(a); }
	uint32_t rd32(uint32_t a) { return bus_.read32(a); }
	void wr8(uint32_t a, uint8_t v) { bus_.write8(a, v); }
	void wr16(uint32_t a, uint16_t v) { bus_.write16(a, v); }
	void wr32(uint32_t a, uint32_t v) { bus_.write32(a, v); }

	void exec16(uint32_t insn, uint32_t pc, uint32_t &next);
	void exec32(uint32_t insn, uint32_t pc, uint32_t &next);
	void unknown(uint32_t pc, uint32_t insn);
	void push16(uint32_t list);
	void pop16(uint32_t list, uint32_t &next);
	void push32(uint32_t list, uint32_t pc);
	void pop32(uint32_t list, uint32_t &next);
	void load_cr(uint32_t num, uint32_t &value);
	void store_cr(uint32_t num, uint32_t value);
	void set_psr(uint32_t value);
	uint32_t psr_value() const;
};
