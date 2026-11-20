// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "hr_c7000_modem.hpp"

#include <algorithm>

namespace {

constexpr uint32_t WORK_MODE_DMR = 0x6Eu;
constexpr uint32_t WORK_MODE_PHY = 0x22u;
constexpr uint32_t WORK_MODE_PHY_CONT = 0x02u;
constexpr uint32_t WORK_MODE_FM = 0x80u;
constexpr uint64_t APB_HZ = 84000000ull;
constexpr uint64_t SLOT_APB = APB_HZ * 30ull / 1000ull;
constexpr uint64_t DEFRAME_APB = 168000ull;
constexpr uint64_t FM_HALF_APB = APB_HZ * 32ull / 1000ull;

uint8_t gf_mul8(uint8_t a, uint8_t b)
{
	uint8_t p = 0;
	while (b) {
		if (b & 1) p ^= a;
		a = (uint8_t)((a << 1) ^ ((a & 0x80u) ? 0x1Du : 0));
		b >>= 1;
	}
	return p;
}

void rs129_parity(const uint8_t data[9], uint8_t parity[3])
{
	uint8_t r2 = 0, r1 = 0, r0 = 0;
	for (int i = 0; i < 9; i++) {
		const uint8_t fb = data[i] ^ r2;
		r2 = r1 ^ gf_mul8(fb, 0x0E);
		r1 = r0 ^ gf_mul8(fb, 0x38);
		r0 = gf_mul8(fb, 0x40);
	}
	parity[0] = r2;
	parity[1] = r1;
	parity[2] = r0;
}

bool dmr_mode(uint32_t mode) { return mode == WORK_MODE_DMR; }
bool phy_mode(uint32_t mode) { return mode == WORK_MODE_PHY || mode == WORK_MODE_PHY_CONT; }

} // namespace

HrC7000Modem::HrC7000Modem(std::vector<uint8_t> &ram, IrqFn irq) : ram_(ram), irq_(std::move(irq)) {}

HrC7000Modem::~HrC7000Modem() = default;

void HrC7000Modem::reset()
{
	regs_ = {};
	work_mode_ = 0;
	local_cc_ = 0;
	send_type_ = 0;
	rx_type_info_ = 0;
	sys_pending_ = 0;
	sys_mask_ = 0;
	layer2_status_ = 0;
	slot_active_ = false;
	slot_ts_ = 0;
	slot_next_apb_ = 0;
	deframe_due_apb_ = 0;
	deframe_pending_ = false;
	rx_burst_ = {};
	audio_control_ = 0;
	fm_addr_sw_ = 0;
	fm_half_ = 0;
	tx_half_ = 0;
	fm_half_next_apb_ = 0;
	fm_samples_.clear();
	phy_last_valid_ = false;
}

uint32_t HrC7000Modem::read(uint32_t off, uint32_t size) const
{
	(void)size;
	switch (off) {
	case 0x100: return work_mode_;
	case 0x384: return local_cc_;
	case 0x390: return rx_type_info_;
	case 0x398: return sys_pending_;
	case 0x39C: return sys_mask_;
	case 0x408: return regs_[(off - 0x100u) / 4u];
	case 0x418: return send_type_;
	case 0x41C: return layer2_status_;
	default: break;
	}
	if (off == 0x080u) return audio_control_;
	if (off == 0x56Cu) return fm_addr_sw_;
	if (off < 0x100u || off >= 0x600u) return 0;
	return regs_[(off - 0x100u) / 4u];
}

void HrC7000Modem::write(uint32_t off, uint32_t value, uint32_t size, uint64_t apb_cycles)
{
	if (off == 0x080u) { audio_control_ = value; return; }
	if (off < 0x100u || off >= 0x600u) return;
	if (size != 4) {
		const uint32_t idx = (off - 0x100u) / 4u;
		const uint32_t shift = (off & 3u) * 8u;
		const uint32_t mask = 0xFFu << shift;
		regs_[idx] = (regs_[idx] & ~mask) | ((value & 0xFFu) << shift);
		return;
	}
	switch (off) {
	case 0x100: {
		const bool was_phy = phy_mode(work_mode_);
		work_mode_ = value;
		if (was_phy && !phy_mode(value)) flush_phy();
		const bool active = dmr_mode(value) || phy_mode(value);
		if (active && !slot_active_) {
			slot_active_ = true;
			slot_ts_ = 0;
			slot_next_apb_ = apb_cycles + SLOT_APB;
		} else if (!active) {
			slot_active_ = false;
			slot_ts_ = 0;
			deframe_pending_ = false;
		}
		break;
	}
	case 0x384: local_cc_ = value; break;
	case 0x390: break;
	case 0x398: break;
	case 0x39C: sys_mask_ = value; break;
	case 0x3A0: sys_pending_ &= ~value; break;
	case 0x3B0: break;
	case 0x408:
		regs_[(off - 0x100u) / 4u] = value;
		if ((value & 0x80u) && !(value & 0x40u)) {
			if (phy_mode(work_mode_)) capture_phy(apb_cycles);
			else capture_tx(apb_cycles);
		}
		break;
	case 0x418: send_type_ = value; break;
	case 0x41C: break;
	case 0x56C: fm_addr_sw_ = value; break;
	default:
		regs_[(off - 0x100u) / 4u] = value;
		break;
	}
}

void HrC7000Modem::capture_tx(uint64_t apb_cycles)
{
	(void)apb_cycles;
	if (!dmr_mode(work_mode_)) return;
	DmrBurst b;
	b.cc = (uint8_t)(local_cc_ & 0xFu);
	b.ts = ts_override_ ? ts_override_ : (slot_ts_ == 0 ? 1 : slot_ts_);
	const uint32_t st = send_type_;
	if (st & 0x08u) {
		b.kind = 0x10;
		b.voice_index = (uint8_t)((st >> 4) & 7u);
		b.len = 27;
		std::copy(ram_.begin() + 0x960, ram_.begin() + 0x960 + 27, b.payload);
	} else {
		b.kind = (uint8_t)((st >> 4) & 0xFu);
		if (b.kind == 0x1Fu) return;
		b.len = b.kind == 0x8u ? 18 : b.kind == 0xAu ? 24 : 12;
		std::copy(ram_.begin(), ram_.begin() + b.len, b.payload);
		if (b.kind == 0x1u || b.kind == 0x2u) {
			uint8_t p[3];
			const uint8_t mask = b.kind == 0x1u ? 0x96u : 0x99u;
			rs129_parity(b.payload, p);
			b.payload[9] = (uint8_t)(p[0] ^ mask);
			b.payload[10] = (uint8_t)(p[1] ^ mask);
			b.payload[11] = (uint8_t)(p[2] ^ mask);
		}
	}
	if (burst_sink_) burst_sink_(b);
	irq_(32); // RF TX start: the driver keys the RF path and reports the slot
}

void HrC7000Modem::capture_phy(uint64_t apb_cycles)
{
	(void)apb_cycles;
	if (!phy_sink_) return;
	// The chip sets up a slot without its last byte; that byte is written at
	// the next boundary, so send the previous half now that it is complete.
	if (phy_last_valid_) phy_sink_(ram_.data() + phy_last_half_ * 0x24u, 36);
	phy_last_half_ = tx_half_;
	phy_last_valid_ = true;
	tx_half_ ^= 1u;
	irq_(32);
}

void HrC7000Modem::flush_phy()
{
	if (!phy_last_valid_) return;
	phy_last_valid_ = false;
	if (phy_sink_) phy_sink_(ram_.data() + phy_last_half_ * 0x24u, 36);
	if (phy_end_sink_) phy_end_sink_();
}

void HrC7000Modem::inject_discriminator_samples(const int16_t *samples, size_t count)
{
	fm_samples_.insert(fm_samples_.end(), samples, samples + count);
}

void HrC7000Modem::emit_fm_half()
{
	constexpr uint32_t off = 0x4E0u;
	constexpr uint32_t n = 256u;
	for (uint32_t i = 0; i < n; i++) {
		const int16_t s = i < fm_samples_.size() ? fm_samples_[i] : 0;
		ram_[off + (uint32_t)fm_half_ * n * 2u + i * 2u] = (uint8_t)((uint16_t)s >> 8);
		ram_[off + (uint32_t)fm_half_ * n * 2u + i * 2u + 1u] = (uint8_t)s;
	}
	fm_samples_.erase(fm_samples_.begin(), fm_samples_.begin() + std::min<size_t>(n, fm_samples_.size()));
	fm_addr_sw_ = (uint8_t)(fm_half_ + 1u);
	fm_half_ ^= 1u;
	sys_pending_ |= 1u << 14;
	irq_(29);
}

void HrC7000Modem::inject_rx()
{
	const DmrBurst b = rx_burst_;
	const uint8_t ts = b.ts == 2 ? 2 : 1;
	const uint32_t sync_family = ts == 2 ? 3u : 2u; // DMR_SYNC_TS2 / DMR_SYNC_TS1
	uint32_t rti = (sync_family << 2) | 2u;
	rti |= (uint32_t)(b.cc & 0xFu) << 4;
	if (b.kind == 0x10u) {
		rti |= ((uint32_t)(b.voice_index + 1u) & 7u) << 12;
		std::copy(b.payload, b.payload + 27, ram_.begin() + 0x960);
	} else {
		rti |= ((uint32_t)b.kind & 0xFu) << 8;
		std::copy(b.payload, b.payload + b.len, ram_.begin() + 0x4B0);
	}
	rx_type_info_ = rti;
	sys_pending_ |= 1u << 3; // SYS_DEFRAME
	irq_(29);
}

void HrC7000Modem::tick(uint64_t apb_cycles)
{
	if (!fm_samples_.empty() && fm_half_next_apb_ == 0) fm_half_next_apb_ = apb_cycles + FM_HALF_APB;
	if (fm_half_next_apb_ != 0 && apb_cycles >= fm_half_next_apb_) {
		emit_fm_half();
		fm_half_next_apb_ += FM_HALF_APB;
		if (fm_half_next_apb_ <= apb_cycles) fm_half_next_apb_ = apb_cycles + FM_HALF_APB;
	}
	if (!slot_active_) return;
	if (apb_cycles >= slot_next_apb_) {
		slot_ts_ = slot_ts_ == 1 ? 2 : 1;
		layer2_status_ = slot_ts_ == 2 ? 0xC0000000u : 0u;
		irq_(30);
		irq_(31);
		slot_next_apb_ += SLOT_APB;
		if (slot_next_apb_ <= apb_cycles) slot_next_apb_ = apb_cycles + SLOT_APB;
	}
}

void HrC7000Modem::inject_burst(const DmrBurst &burst)
{
	rx_burst_ = burst;
	if (ts_override_) rx_burst_.ts = ts_override_;
	inject_rx();
}
