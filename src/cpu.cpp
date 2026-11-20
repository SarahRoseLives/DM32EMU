// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The DM32EMU Authors
#include "cpu.hpp"
#include "machine.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t PSR_S = 0x80000000u;
constexpr uint32_t PSR_EE = 0x00000100u;
constexpr uint32_t PSR_IE = 0x00000040u;
constexpr uint32_t PSR_FE = 0x00000010u;
constexpr uint32_t PSR_AF = 0x00000002u;
constexpr uint32_t PSR_VEC = 0x00FF0000u;

inline int32_t sext(uint32_t v, int bits)
{
	const uint32_t sign = 1u << (bits - 1);
	return (int32_t)((v ^ sign) - sign);
}

inline uint32_t shl_imm(uint32_t v, uint32_t n) { return n >= 32 ? 0 : v << n; }
inline uint32_t shr_imm(uint32_t v, uint32_t n) { return n >= 32 ? 0 : v >> n; }
inline uint32_t sar_imm(uint32_t v, uint32_t n)
{
	n = n > 31 ? 31 : n;
	return (uint32_t)((int32_t)v >> n);
}
inline uint32_t rotl_imm(uint32_t v, uint32_t n)
{
	n &= 31;
	return n == 0 ? v : (v << n) | (v >> (32 - n));
}
inline uint32_t shl_reg(uint32_t v, uint32_t n) { n &= 0x3f; return n > 31 ? 0 : v << n; }
inline uint32_t shr_reg(uint32_t v, uint32_t n) { n &= 0x3f; return n > 31 ? 0 : v >> n; }
inline uint32_t sar_reg(uint32_t v, uint32_t n)
{
	n &= 0x3f;
	n = n > 31 ? 31 : n;
	return (uint32_t)((int32_t)v >> n);
}
inline uint32_t rotl_reg(uint32_t v, uint32_t n)
{
	n &= 0x3f;
	return n > 31 ? 0 : rotl_imm(v, n);
}

} // namespace

Cpu::Cpu(Machine &machine) : bus_(machine) { reset(Machine::APP_BASE); }

void Cpu::reset(uint32_t entry)
{
	std::memset(r_, 0, sizeof(r_));
	pc_ = entry & ~1u;
	psr_ = 0x80000100u; // supervisor, exceptions enabled, interrupts off
	epsr_ = 0;
	epc_ = 0;
	vbr_ = 0;
	carry_ = false;
	cycles_ = 0;
	bus_.set_vbr(vbr_);
}

bool Cpu::irq_pending() const
{
	if (!(psr_ & PSR_EE) || !(psr_ & PSR_IE)) {
		return false;
	}
	return bus_.irq_pending();
}

void Cpu::take_irq(int source)
{
	const uint32_t vector = (uint32_t)source + 32u;
	epc_ = pc_;
	epsr_ = psr_value();
	psr_ = (psr_ & ~(PSR_VEC | PSR_IE | PSR_EE | PSR_AF)) | PSR_S | (vector << 16);
	pc_ = bus_.read_vector(vector) & ~1u;
}

void Cpu::step()
{
	if (irq_pending()) {
		const int source = bus_.irq_source();
		if (source >= 0) {
			take_irq(source);
			return;
		}
	}

	const uint32_t pc = pc_;
	const uint16_t half = bus_.read16(pc);
	uint32_t insn;
	uint32_t next;

	if ((half & 0xC000u) != 0xC000u) {
		insn = half;
		next = pc + 2;
		exec16(insn, pc, next);
	} else {
		insn = ((uint32_t)half << 16) | bus_.read16(pc + 2);
		next = pc + 4;
		exec32(insn, pc, next);
	}
	pc_ = next & ~1u;
	cycles_++;
}

void Cpu::unknown(uint32_t pc, uint32_t insn)
{
	char buf[128];
	std::snprintf(buf, sizeof(buf), "illegal instruction 0x%08" PRIx32 " at 0x%08" PRIx32, insn, pc);
	throw CpuFault(pc, insn, buf);
}

void Cpu::dump(std::FILE *out) const
{
	std::fprintf(out, "pc=%08" PRIx32 " psr=%08" PRIx32 " epsr=%08" PRIx32 " epc=%08" PRIx32
			  " vbr=%08" PRIx32 " c=%d cycles=%" PRIu64 "\n",
			 pc_, psr_value(), epsr_, epc_, vbr_, carry_ ? 1 : 0, cycles_);
	for (int i = 0; i < 32; i += 4) {
		std::fprintf(out, "r%-2d=%08" PRIx32 " r%-2d=%08" PRIx32 " r%-2d=%08" PRIx32 " r%-2d=%08" PRIx32 "\n",
				 i, r_[i], i + 1, r_[i + 1], i + 2, r_[i + 2], i + 3, r_[i + 3]);
	}
}

uint32_t Cpu::psr_value() const { return (psr_ & ~1u) | (carry_ ? 1u : 0u); }

void Cpu::set_psr(uint32_t value)
{
	psr_ = value;
	carry_ = (value & 1u) != 0;
}

void Cpu::load_cr(uint32_t num, uint32_t &value)
{
	switch (num) {
	case 0: value = psr_value(); break;
	case 1: value = vbr_; break;
	case 2: value = epsr_; break;
	case 3: value = 0; break;
	case 4: value = epc_; break;
	case 5: value = 0; break;
	case 6: case 7: case 8: case 9: case 10: value = ss_[num - 6]; break;
	default: value = 0; break;
	}
}

void Cpu::store_cr(uint32_t num, uint32_t value)
{
	switch (num) {
	case 0: set_psr(value); break;
	case 1: vbr_ = value & ~0x3FFu; bus_.set_vbr(vbr_); break;
	case 2: epsr_ = value; break;
	case 4: epc_ = value; break;
	case 6: case 7: case 8: case 9: case 10: ss_[num - 6] = value; break;
	default: break;
	}
}

void Cpu::push16(uint32_t list)
{
	uint32_t sp = r_[14];
	if (list & 0x10u) {
		sp -= 4;
		wr32(sp, r_[15]);
	}
	for (uint32_t i = list & 0xFu; i > 0; i--) {
		sp -= 4;
		wr32(sp, r_[i + 3]);
	}
	r_[14] = sp;
}

void Cpu::pop16(uint32_t list, uint32_t &next)
{
	uint32_t sp = r_[14];
	for (uint32_t i = 0; i < (list & 0xFu); i++) {
		r_[4 + i] = rd32(sp);
		sp += 4;
	}
	if (list & 0x10u) {
		r_[15] = rd32(sp);
		sp += 4;
	}
	r_[14] = sp;
	next = r_[15] & ~1u;
}

void Cpu::push32(uint32_t list, uint32_t pc)
{
	(void)pc;
	uint32_t sp = r_[14];
	if (list & 0x100u) {
		sp -= 4;
		wr32(sp, r_[28]);
	}
	if ((list >> 5) & 0x7u) {
		for (uint32_t i = (list >> 5) & 0x7u; i > 0; i--) {
			sp -= 4;
			wr32(sp, r_[15 + i]);
		}
	}
	if (list & 0x10u) {
		sp -= 4;
		wr32(sp, r_[15]);
	}
	for (uint32_t i = list & 0xFu; i > 0; i--) {
		sp -= 4;
		wr32(sp, r_[i + 3]);
	}
	r_[14] = sp;
}

void Cpu::pop32(uint32_t list, uint32_t &next)
{
	uint32_t sp = r_[14];
	for (uint32_t i = 0; i < (list & 0xFu); i++) {
		r_[4 + i] = rd32(sp);
		sp += 4;
	}
	if (list & 0x10u) {
		r_[15] = rd32(sp);
		sp += 4;
	}
	if ((list >> 5) & 0x7u) {
		for (uint32_t i = 0; i < ((list >> 5) & 0x7u); i++) {
			r_[16 + i] = rd32(sp);
			sp += 4;
		}
	}
	if (list & 0x100u) {
		r_[28] = rd32(sp);
		sp += 4;
	}
	r_[14] = sp;
	next = r_[15] & ~1u;
}

void Cpu::exec16(uint32_t insn, uint32_t pc, uint32_t &next)
{
	const uint32_t op = (insn >> 10) & 0x3F;

	switch (op) {
	case 0x00: {
		const uint32_t off = insn & 0x3FFu;
		if (off == 0) {
			// bkpt; no debugger attached in this emulator.
			next = pc + 2;
			break;
		}
		// With the ELRW extension the 16-bit bsr encoding is a second
		// form of lrw (a negative literal offset). The CK803S has it.
		uint32_t imm = ((insn & 0x300u) >> 3) | (insn & 0x1Fu);
		imm = (~imm & 0x7Fu) | 0x80u;
		const uint32_t rz = (insn >> 5) & 0x7u;
		const uint32_t addr = (pc + (imm << 2)) & ~3u;
		r_[rz] = rd32(addr);
		break;
	}
	case 0x01: {
		const uint32_t off = insn & 0x3FFu;
		next = pc + (uint32_t)sext(off << 1, 11);
		break;
	}
	case 0x02:
	case 0x03: {
		const uint32_t off = insn & 0x3FFu;
		const bool take = (op == 0x02) ? carry_ : !carry_;
		if (take) {
			next = pc + (uint32_t)sext(off << 1, 11);
		}
		break;
	}
	case 0x04: {
		const uint32_t imm = ((insn & 0x300u) >> 3) | (insn & 0x1Fu);
		const uint32_t rz = (insn >> 5) & 0x7u;
		const uint32_t addr = (pc + (imm << 2)) & ~3u;
		r_[rz] = rd32(addr);
		break;
	}
	case 0x05: {
		const uint32_t imm = ((insn >> 3) & 0x60u) | (insn & 0x1Fu);
		const uint32_t sop = (insn >> 5) & 0x7u;
		if (sop == 0) {
			r_[14] += imm << 2;
		} else if (sop == 1) {
			r_[14] -= imm << 2;
		} else if (sop == 4) {
			pop16(imm & 0x1Fu, next);
		} else if (sop == 6) {
			push16(imm & 0x1Fu);
		} else {
			unknown(pc, insn);
		}
		break;
	}
	case 0x06:
	case 0x07:
	case 0x08:
	case 0x09:
	case 0x0A:
	case 0x0B:
	case 0x0C:
	case 0x0D: {
		const uint32_t imm = insn & 0xFFu;
		const uint32_t rz = (insn >> 8) & 0x7u;
		switch (op) {
		case 0x06:
		case 0x07: r_[rz] = r_[14] + (imm << 2); break;
		case 0x08:
		case 0x09: r_[rz] += imm + 1; break;
		case 0x0A:
		case 0x0B: r_[rz] -= imm + 1; break;
		default: r_[rz] = imm; break;
		}
		break;
	}
	case 0x0E:
	case 0x0F: {
		const uint32_t imm = insn & 0x1Fu;
		const uint32_t sop = (insn >> 5) & 0x7u;
		const uint32_t rx = (insn >> 8) & 0x7u;
		switch (sop) {
		case 0: carry_ = r_[rx] >= imm + 1; break;
		case 1: carry_ = (int32_t)r_[rx] < (int32_t)(imm + 1); break;
		case 2: carry_ = r_[rx] != imm; break;
		case 4: r_[rx] &= ~(1u << imm); break;
		case 5: r_[rx] |= (1u << imm); break;
		case 6: carry_ = ((r_[rx] >> imm) & 1u) != 0; break;
		default: unknown(pc, insn); break;
		}
		break;
	}
	case 0x10:
	case 0x11:
	case 0x12:
	case 0x13:
	case 0x14:
	case 0x15: {
		const uint32_t imm = insn & 0x1Fu;
		const uint32_t rz = (insn >> 5) & 0x7u;
		const uint32_t rx = (insn >> 8) & 0x7u;
		if (op == 0x10 || op == 0x11) {
			r_[rz] = shl_imm(r_[rx], imm);
		} else if (op == 0x12 || op == 0x13) {
			r_[rz] = shr_imm(r_[rx], imm);
		} else {
			r_[rz] = sar_imm(r_[rx], imm);
		}
		break;
	}
	case 0x16:
	case 0x17: {
		const uint32_t sop = insn & 0x3u;
		const uint32_t imm = (insn >> 2) & 0x7u;
		const uint32_t rz = (insn >> 5) & 0x7u;
		const uint32_t rx = (insn >> 8) & 0x7u;
		switch (sop) {
		case 0: r_[rz] = r_[rx] + r_[imm]; break;
		case 1: r_[rz] = r_[rx] - r_[imm]; break;
		case 2: r_[rz] = r_[rx] + imm + 1; break;
		case 3: r_[rz] = r_[rx] - (imm + 1); break;
		}
		break;
	}
	case 0x18:
	case 0x19:
	case 0x1A:
	case 0x1B:
	case 0x1C:
	case 0x1D:
	case 0x1E:
	case 0x1F: {
		const uint32_t sop = insn & 0x3u;
		const uint32_t rx = (insn >> 2) & 0xFu;
		const uint32_t rz = (insn >> 6) & 0xFu;
		switch (op) {
		case 0x18:
			switch (sop) {
			case 0: r_[rz] = r_[rz] + r_[rx]; break;
			case 1: {
				const uint64_t s = (uint64_t)r_[rz] + r_[rx] + (carry_ ? 1u : 0u);
				r_[rz] = (uint32_t)s;
				carry_ = (s >> 32) != 0;
				break;
			}
			case 2: r_[rz] = r_[rz] - r_[rx]; break;
			case 3: {
				const uint64_t sub = (uint64_t)r_[rx] + (carry_ ? 0u : 1u);
				const uint32_t old = r_[rz];
				r_[rz] = old - (uint32_t)sub;
				carry_ = (uint64_t)old >= sub;
				break;
			}
			}
			break;
		case 0x19:
			switch (sop) {
			case 0: carry_ = r_[rx] >= r_[rz]; break;
			case 1: carry_ = (int32_t)r_[rx] < (int32_t)r_[rz]; break;
			case 2: carry_ = r_[rx] != r_[rz]; break;
			case 3: r_[rz] = carry_ ? 0u : 1u; break;
			}
			break;
		case 0x1A:
			switch (sop) {
			case 0: r_[rz] &= r_[rx]; break;
			case 1: r_[rz] &= ~r_[rx]; break;
			case 2: carry_ = (r_[rx] & r_[rz]) != 0; break;
			case 3: {
				const uint32_t v = r_[rx];
				carry_ = (v & 0xFFu) && (v & 0xFF00u) && (v & 0xFF0000u) && (v & 0xFF000000u);
				break;
			}
			}
			break;
		case 0x1B:
			switch (sop) {
			case 0: r_[rz] |= r_[rx]; break;
			case 1: r_[rz] ^= r_[rx]; break;
			case 2: r_[rz] = ~(r_[rz] | r_[rx]); break;
			case 3: r_[rz] = r_[rx]; break;
			}
			break;
		case 0x1C:
			switch (sop) {
			case 0: r_[rz] = shl_reg(r_[rz], r_[rx]); break;
			case 1: r_[rz] = shr_reg(r_[rz], r_[rx]); break;
			case 2: r_[rz] = sar_reg(r_[rz], r_[rx]); break;
			case 3: r_[rz] = rotl_reg(r_[rz], r_[rx]); break;
			}
			break;
		case 0x1D:
			switch (sop) {
			case 0: r_[rz] = r_[rx] & 0xFFu; break;
			case 1: r_[rz] = r_[rx] & 0xFFFFu; break;
			case 2: r_[rz] = (uint32_t)(int32_t)(int8_t)r_[rx]; break;
			case 3: r_[rz] = (uint32_t)(int32_t)(int16_t)r_[rx]; break;
			}
			break;
		case 0x1E:
			switch (sop) {
			case 0: next = r_[rx] & ~1u; break;
			case 1: r_[15] = pc + 2; next = r_[rx] & ~1u; break;
			case 2: r_[rz] = ((r_[rx] & 0xFFu) << 24) | ((r_[rx] & 0xFF00u) << 8) |
					 ((r_[rx] & 0xFF0000u) >> 8) | ((r_[rx] & 0xFF000000u) >> 24); break;
			case 3: r_[rz] = (r_[rx] << 16) | (r_[rx] >> 16); break;
			}
			break;
		case 0x1F:
			switch (sop) {
			case 0: r_[rz] = r_[rz] * r_[rx]; break;
			case 1: r_[rz] = (uint32_t)((int32_t)(int16_t)r_[rz] * (int32_t)(int16_t)r_[rx]); break;
			}
			break;
		}
		break;
	}
	case 0x20:
	case 0x21:
	case 0x22:
	case 0x23:
	case 0x24:
	case 0x25: {
		const uint32_t imm = insn & 0x1Fu;
		const uint32_t rz = (insn >> 5) & 0x7u;
		const uint32_t rx = (insn >> 8) & 0x7u;
		const uint32_t addr = r_[rx] + (op == 0x22 || op == 0x23 ? imm << 1 : op >= 0x24 ? imm << 2 : imm);
		if (op <= 0x21) r_[rz] = rd8(addr);
		else if (op <= 0x23) r_[rz] = rd16(addr);
		else r_[rz] = rd32(addr);
		break;
	}
	case 0x26:
	case 0x27: {
		const uint32_t imm = ((insn & 0x700u) >> 3) | (insn & 0x1Fu);
		const uint32_t rz = (insn >> 5) & 0x7u;
		r_[rz] = rd32(r_[14] + (imm << 2));
		break;
	}
	case 0x28:
	case 0x29:
	case 0x2A:
	case 0x2B:
	case 0x2C:
	case 0x2D: {
		const uint32_t imm = insn & 0x1Fu;
		const uint32_t rz = (insn >> 5) & 0x7u;
		const uint32_t rx = (insn >> 8) & 0x7u;
		const uint32_t addr = r_[rx] + (op == 0x2A || op == 0x2B ? imm << 1 : op >= 0x2C ? imm << 2 : imm);
		if (op <= 0x29) wr8(addr, (uint8_t)r_[rz]);
		else if (op <= 0x2B) wr16(addr, (uint16_t)r_[rz]);
		else wr32(addr, r_[rz]);
		break;
	}
	case 0x2E:
	case 0x2F: {
		const uint32_t imm = ((insn & 0x700u) >> 3) | (insn & 0x1Fu);
		const uint32_t rz = (insn >> 5) & 0x7u;
		wr32(r_[14] + (imm << 2), r_[rz]);
		break;
	}
	default:
		unknown(pc, insn);
		break;
	}
}

void Cpu::exec32(uint32_t insn, uint32_t pc, uint32_t &next)
{
	const uint32_t op = (insn >> 26) & 0xFu;

	switch (op) {
	case 0x0: { // special
		const uint32_t ry = (insn >> 21) & 0x1Fu;
		const uint32_t rx = (insn >> 16) & 0x1Fu;
		const uint32_t sop = (insn >> 10) & 0x3Fu;
		const uint32_t rz = insn & 0x1Fu;
		switch (sop) {
		case 0x01: // sync
		case 0x07: // idly
		case 0x15: // we
		case 0x16: // se
			break;
		case 0x10: // rte
			set_psr(epsr_);
			next = epc_ & ~1u;
			break;
		case 0x11: // rfi
			set_psr(epsr_);
			next = epc_ & ~1u;
			break;
		case 0x12: // stop
		case 0x13: // wait
		case 0x14: // doze
			break;
		case 0x18: { // mfcr
			uint32_t v = 0;
			if (ry == 0) {
				load_cr(rx, v);
			}
			r_[rz] = v;
			break;
		}
		case 0x19: // mtcr
			if (ry == 0) {
				store_cr(rz, r_[rx]);
			}
			break;
		case 0x1C: // psrclr
			if (ry & 0x1u) psr_ &= ~PSR_AF;
			if (ry & 0x2u) psr_ &= ~PSR_FE;
			if (ry & 0x4u) psr_ &= ~PSR_IE;
			if (ry & 0x8u) psr_ &= ~PSR_EE;
			break;
		case 0x1D: // psrset
			if (ry & 0x1u) psr_ |= PSR_AF;
			if (ry & 0x2u) psr_ |= PSR_FE;
			if (ry & 0x4u) psr_ |= PSR_IE;
			if (ry & 0x8u) psr_ |= PSR_EE;
			break;
		default:
			unknown(pc, insn);
			break;
		}
		break;
	}
	case 0x1: { // register arithmetic
		const uint32_t ry = (insn >> 21) & 0x1Fu;
		const uint32_t rx = (insn >> 16) & 0x1Fu;
		const uint32_t sop = (insn >> 10) & 0x3Fu;
		const uint32_t pcode = (insn >> 5) & 0x1Fu;
		const uint32_t rz = insn & 0x1Fu;
		switch (sop) {
		case 0x00:
			if (pcode == 0x1) r_[rz] = r_[rx] + r_[ry];
			else if (pcode == 0x2) {
				const uint64_t s = (uint64_t)r_[rx] + r_[ry] + (carry_ ? 1u : 0u);
				r_[rz] = (uint32_t)s;
				carry_ = (s >> 32) != 0;
			} else if (pcode == 0x4) r_[rz] = r_[rx] - r_[ry];
			else if (pcode == 0x8) {
				const uint64_t sub = (uint64_t)r_[ry] + (carry_ ? 0u : 1u);
				const uint32_t old = r_[rx];
				r_[rz] = old - (uint32_t)sub;
				carry_ = (uint64_t)old >= sub;
			} else if (pcode == 0x10) {
				if (r_[rx] == 0x80000000u || (int32_t)r_[rx] >= 0) r_[rz] = r_[rx];
				else r_[rz] = (uint32_t)-(int32_t)r_[rx];
			} else unknown(pc, insn);
			break;
		case 0x01:
			if (pcode == 0x1) carry_ = r_[rx] >= r_[ry];
			else if (pcode == 0x2) carry_ = (int32_t)r_[rx] < (int32_t)r_[ry];
			else if (pcode == 0x4) carry_ = r_[rx] != r_[ry];
			else if (pcode == 0x8) r_[rz] = carry_ ? 1u : 0u;
			else if (pcode == 0x10) r_[rz] = carry_ ? 0u : 1u;
			else unknown(pc, insn);
			break;
		case 0x02:
			if (pcode == 0x1) r_[rz] = r_[rx] + (r_[ry] << 1);
			else if (pcode == 0x2) r_[rz] = r_[rx] + (r_[ry] << 2);
			else if (pcode == 0x4) r_[rz] = r_[rx] + (r_[ry] << 3);
			else unknown(pc, insn);
			break;
		case 0x03:
			if (pcode == 0x1) { if (!carry_) r_[ry] = r_[rx] + rz; }
			else if (pcode == 0x2) { if (carry_) r_[ry] = r_[rx] + rz; }
			else if (pcode == 0x4) { if (!carry_) r_[ry] = r_[rx] - rz; }
			else if (pcode == 0x8) { if (carry_) r_[ry] = r_[rx] - rz; }
			else unknown(pc, insn);
			break;
		case 0x04:
			if (pcode == 0x1) { r_[rz] = r_[rx] - ry; carry_ = (int32_t)r_[rz] > 0; }
			else if (pcode == 0x2) { r_[rz] = r_[rx] - ry; carry_ = (int32_t)r_[rz] < 0; }
			else if (pcode == 0x4) { r_[rz] = r_[rx] - ry; carry_ = r_[rz] != 0; }
			else unknown(pc, insn);
			break;
		case 0x08:
			if (pcode == 0x1) r_[rz] = r_[rx] & r_[ry];
			else if (pcode == 0x2) r_[rz] = r_[rx] & ~r_[ry];
			else if (pcode == 0x4) carry_ = (r_[rx] & r_[ry]) != 0;
			else if (pcode == 0x8) {
				const uint32_t v = r_[rx];
				carry_ = (v & 0xFFu) && (v & 0xFF00u) && (v & 0xFF0000u) && (v & 0xFF000000u);
			} else unknown(pc, insn);
			break;
		case 0x09:
			if (pcode == 0x1) r_[rz] = r_[rx] | r_[ry];
			else if (pcode == 0x2) r_[rz] = r_[rx] ^ r_[ry];
			else if (pcode == 0x4) r_[rz] = ~(r_[rx] | r_[ry]);
			else unknown(pc, insn);
			break;
		case 0x0A:
			if (pcode == 0x1) r_[rz] = r_[rx] & ~(1u << ry);
			else if (pcode == 0x2) r_[rz] = r_[rx] | (1u << ry);
			else if (pcode == 0x4) carry_ = ((r_[rx] >> ry) & 1u) != 0;
			else unknown(pc, insn);
			break;
		case 0x0B:
			if (pcode == 0x1) { if (!carry_) r_[ry] = 0; }
			else if (pcode == 0x2) { if (carry_) r_[ry] = 0; }
			else unknown(pc, insn);
			break;
		case 0x10:
			if (pcode == 0x1) r_[rz] = shl_reg(r_[rx], r_[ry]);
			else if (pcode == 0x2) r_[rz] = shr_reg(r_[rx], r_[ry]);
			else if (pcode == 0x4) r_[rz] = sar_reg(r_[rx], r_[ry]);
			else if (pcode == 0x8) r_[rz] = rotl_reg(r_[rx], r_[ry]);
			else unknown(pc, insn);
			break;
		case 0x12:
			if (pcode == 0x1) r_[rz] = shl_imm(r_[rx], ry);
			else if (pcode == 0x2) r_[rz] = shr_imm(r_[rx], ry);
			else if (pcode == 0x4) r_[rz] = sar_imm(r_[rx], ry);
			else if (pcode == 0x8) r_[rz] = rotl_imm(r_[rx], ry);
			else unknown(pc, insn);
			break;
		case 0x13: {
			const uint32_t n = ry + 1;
			if (pcode == 0x1) { // lslc
				const uint32_t old = r_[rx];
				carry_ = (old & 1u) != 0;
				r_[rz] = shl_imm(old, n);
			} else if (pcode == 0x2) { // lsrc
				const uint32_t old = r_[rx];
				carry_ = n <= 32 ? ((old >> (n - 1)) & 1u) != 0 : false;
				r_[rz] = shr_imm(old, n);
			} else if (pcode == 0x4) { // asrc
				const uint32_t old = r_[rx];
				carry_ = n <= 32 ? ((old >> (n - 1)) & 1u) != 0 : false;
				r_[rz] = sar_imm(old, n);
			} else unknown(pc, insn);
			break;
		}
		case 0x14:
			if (pcode == 0x1) {
				const uint32_t n = ry + 1;
				r_[rz] = n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1u);
			} else if (pcode == 0x2) {
				r_[rz] = (r_[rx] & 0x20u) ? 0u : (1u << (r_[rx] & 0x1Fu));
			} else unknown(pc, insn);
			break;
		case 0x15: { // zext
			const uint32_t lsb = ry;
			const uint32_t msb = pcode;
			if (lsb == 0 && msb == 31) r_[rz] = r_[rx];
			else if (msb >= lsb && msb < 32) {
				const uint32_t width = msb - lsb + 1;
				const uint32_t mask = width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1u);
				r_[rz] = (r_[rx] >> lsb) & mask;
			} else r_[rz] = 0;
			break;
		}
		case 0x16: { // sext
			const uint32_t lsb = ry;
			const uint32_t msb = pcode;
			if (lsb == 0 && msb == 31) r_[rz] = r_[rx];
			else if (msb >= lsb && msb < 32) {
				const uint32_t width = msb - lsb + 1;
				const uint32_t mask = width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1u);
				const uint32_t v = (r_[rx] >> lsb) & mask;
				r_[rz] = (uint32_t)sext(v, (int)width);
			} else r_[rz] = 0;
			break;
		}
		case 0x17: { // ins
			const uint32_t lsb = rz;
			if (pcode == 31) r_[ry] = r_[rx];
			else {
				const uint32_t width = pcode + 1;
				const uint32_t mask = width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1u);
				r_[ry] = (r_[ry] & ~(mask << lsb)) | ((r_[rx] & mask) << lsb);
			}
			break;
		}
		case 0x18:
			if (pcode == 0x4) {
				const uint32_t v = r_[rx];
				r_[rz] = ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
					 ((v & 0xFF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
			} else if (pcode == 0x8) {
				r_[rz] = (r_[rx] << 16) | (r_[rx] >> 16);
			} else if (pcode == 0x10) {
				uint32_t v = r_[rx];
				v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
				v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
				v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
				v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
				r_[rz] = (v >> 16) | (v << 16);
			} else unknown(pc, insn);
			break;
		case 0x1C:
			if (pcode == 0x1) { r_[rz] = r_[rx] >> 24; carry_ = r_[rz] != 0; }
			else if (pcode == 0x2) { r_[rz] = (r_[rx] >> 16) & 0xFFu; carry_ = r_[rz] != 0; }
			else if (pcode == 0x4) { r_[rz] = (r_[rx] >> 8) & 0xFFu; carry_ = r_[rz] != 0; }
			else if (pcode == 0x8) { r_[rz] = r_[rx] & 0xFFu; carry_ = r_[rz] != 0; }
			else unknown(pc, insn);
			break;
		case 0x1F:
			if (pcode == 0x1) r_[rz] = r_[rx] ? (uint32_t)__builtin_ctz(r_[rx]) : 32u;
			else if (pcode == 0x2) {
				uint32_t v = r_[rx];
				r_[rz] = v ? (uint32_t)__builtin_ctz(v) : 32u;
			} else unknown(pc, insn);
			break;
		case 0x20:
			if (pcode == 0x1) {
				if (r_[ry] == 0) throw CpuFault(pc, insn, "divide by zero");
				r_[rz] = r_[rx] / r_[ry];
			} else if (pcode == 0x2) {
				if (r_[ry] == 0) throw CpuFault(pc, insn, "divide by zero");
				const int64_t a = (int32_t)r_[rx], b = (int32_t)r_[ry];
				r_[rz] = (uint32_t)(a / b);
			} else unknown(pc, insn);
			break;
		case 0x21:
			if (pcode == 0x1) r_[rz] = r_[rx] * r_[ry];
			else unknown(pc, insn);
			break;
		case 0x24:
			if (pcode == 0x1) {
				r_[rz] = (uint32_t)((int32_t)(int16_t)r_[rx] * (int32_t)(int16_t)r_[ry]);
			} else unknown(pc, insn);
			break;
		default:
			unknown(pc, insn);
			break;
		}
		break;
	}
	case 0x4: { // ldr
		const uint32_t ry = (insn >> 21) & 0x1Fu;
		const uint32_t rx = (insn >> 16) & 0x1Fu;
		const uint32_t sop = (insn >> 10) & 0x3Fu;
		const uint32_t pcode = (insn >> 5) & 0x1Fu;
		const uint32_t rz = insn & 0x1Fu;
		const uint32_t addr = r_[rx] + (r_[ry] << __builtin_ctz(pcode ? pcode : 1));
		switch (sop) {
		case 0x0: r_[rz] = rd8(addr); break;
		case 0x1: r_[rz] = rd16(addr); break;
		case 0x2: r_[rz] = rd32(addr); break;
		case 0x4: r_[rz] = (uint32_t)(int32_t)(int8_t)rd8(addr); break;
		case 0x5: r_[rz] = (uint32_t)(int32_t)(int16_t)rd16(addr); break;
		case 0x7: { // ldm
			uint32_t a = r_[rx];
			for (uint32_t i = 0; i <= rz; i++) {
				r_[(ry + i) & 31u] = rd32(a);
				a += 4;
			}
			break;
		}
		default: unknown(pc, insn); break;
		}
		break;
	}
	case 0x5: { // str
		const uint32_t ry = (insn >> 21) & 0x1Fu;
		const uint32_t rx = (insn >> 16) & 0x1Fu;
		const uint32_t sop = (insn >> 10) & 0x3Fu;
		const uint32_t pcode = (insn >> 5) & 0x1Fu;
		const uint32_t rz = insn & 0x1Fu;
		const uint32_t addr = r_[rx] + (r_[ry] << __builtin_ctz(pcode ? pcode : 1));
		switch (sop) {
		case 0x0: wr8(addr, (uint8_t)r_[rz]); break;
		case 0x1: wr16(addr, (uint16_t)r_[rz]); break;
		case 0x2: wr32(addr, r_[rz]); break;
		case 0x7: { // stm
			uint32_t a = r_[rx];
			for (uint32_t i = 0; i <= rz; i++) {
				wr32(a, r_[(ry + i) & 31u]);
				a += 4;
			}
			break;
		}
		default: unknown(pc, insn); break;
		}
		break;
	}
	case 0x6: { // ldi
		const uint32_t sop = (insn >> 12) & 0xFu;
		const uint32_t rz = (insn >> 21) & 0x1Fu;
		const uint32_t rx = (insn >> 16) & 0x1Fu;
		const uint32_t imm = insn & 0xFFFu;
		switch (sop) {
		case 0x0: r_[rz] = rd8(r_[rx] + imm); break;
		case 0x1: r_[rz] = rd16(r_[rx] + (imm << 1)); break;
		case 0x2: r_[rz] = rd32(r_[rx] + (imm << 2)); break;
		case 0x4: r_[rz] = (uint32_t)(int32_t)(int8_t)rd8(r_[rx] + imm); break;
		case 0x5: r_[rz] = (uint32_t)(int32_t)(int16_t)rd16(r_[rx] + (imm << 1)); break;
		default: unknown(pc, insn); break;
		}
		break;
	}
	case 0x7: { // sti
		const uint32_t sop = (insn >> 12) & 0xFu;
		const uint32_t rz = (insn >> 21) & 0x1Fu;
		const uint32_t rx = (insn >> 16) & 0x1Fu;
		const uint32_t imm = insn & 0xFFFu;
		switch (sop) {
		case 0x0: wr8(r_[rx] + imm, (uint8_t)r_[rz]); break;
		case 0x1: wr16(r_[rx] + (imm << 1), (uint16_t)r_[rz]); break;
		case 0x2: wr32(r_[rx] + (imm << 2), r_[rz]); break;
		default: unknown(pc, insn); break;
		}
		break;
	}
	case 0x8: { // bsr
		const uint32_t imm = insn & 0x3FFFFFFu;
		r_[15] = pc + 4;
		next = pc + (uint32_t)sext(imm << 1, 27);
		break;
	}
	case 0x9: { // immediate two-operand
		const uint32_t rz = (insn >> 21) & 0x1Fu;
		const uint32_t rx = (insn >> 16) & 0x1Fu;
		const uint32_t sop = (insn >> 12) & 0xFu;
		const uint32_t imm = insn & 0xFFFu;
		switch (sop) {
		case 0x0: r_[rz] = r_[rx] + imm + 1; break;
		case 0x1: r_[rz] = r_[rx] - (imm + 1); break;
		case 0x2: r_[rz] = r_[rx] & imm; break;
		case 0x3: r_[rz] = r_[rx] & ~imm; break;
		case 0x4: r_[rz] = r_[rx] ^ imm; break;
		default: unknown(pc, insn); break;
		}
		break;
	}
	case 0xA: { // immediate one-operand
		const uint32_t sop = (insn >> 21) & 0x1Fu;
		const uint32_t rx = (insn >> 16) & 0x1Fu;
		const uint32_t imm = insn & 0xFFFFu;
		const uint32_t target = pc + (uint32_t)sext(imm << 1, 17);
		switch (sop) {
		case 0x0: next = target; break;
		case 0x2: if (!carry_) next = target; break;
		case 0x3: if (carry_) next = target; break;
		case 0x6: next = r_[rx] & ~1u; break;
		case 0x7: r_[15] = pc + 4; next = r_[rx] & ~1u; break;
		case 0x8: if (r_[rx] == 0) next = target; break;
		case 0x9: if (r_[rx] != 0) next = target; break;
		case 0xA: if ((int32_t)r_[rx] > 0) next = target; break;
		case 0xB: if ((int32_t)r_[rx] <= 0) next = target; break;
		case 0xC: if ((int32_t)r_[rx] < 0) next = target; break;
		case 0xD: if ((int32_t)r_[rx] >= 0) next = target; break;
		case 0x10: r_[rx] = imm; break;
		case 0x11: r_[rx] = imm << 16; break;
		case 0x14: {
			const uint32_t addr = (pc + (imm << 2)) & ~3u;
			r_[rx] = rd32(addr);
			break;
		}
		case 0x16: {
			const uint32_t addr = (pc + (imm << 2)) & ~3u;
			next = rd32(addr) & ~1u;
			break;
		}
		case 0x17: {
			const uint32_t addr = (pc + (imm << 2)) & ~3u;
			r_[15] = pc + 4;
			next = rd32(addr) & ~1u;
			break;
		}
		case 0x18: carry_ = r_[rx] >= imm + 1; break;
		case 0x19: carry_ = (int32_t)r_[rx] < (int32_t)(imm + 1); break;
		case 0x1A: carry_ = r_[rx] != imm; break;
		case 0x1E: pop32(imm & 0x1FFu, next); break;
		case 0x1F: push32(imm & 0x1FFu, pc); break;
		default: unknown(pc, insn); break;
		}
		break;
	}
	case 0xB: { // ori
		const uint32_t rz = (insn >> 21) & 0x1Fu;
		const uint32_t rx = (insn >> 16) & 0x1Fu;
		r_[rz] = r_[rx] | (insn & 0xFFFFu);
		break;
	}
	default:
		unknown(pc, insn);
		break;
	}
}
