// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include "dmr_burst.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// The HR_C7000's DMR burst path: modem registers, slot timing, and the bridge
// to the virtual RF medium. The 64 KB modem RAM belongs to Machine.
class HrC7000Modem {
public:
	using IrqFn = std::function<void(int)>;

	HrC7000Modem(std::vector<uint8_t> &ram, IrqFn irq);
	~HrC7000Modem();

	void reset();
	uint32_t read(uint32_t off, uint32_t size) const;
	void write(uint32_t off, uint32_t value, uint32_t size, uint64_t apb_cycles);
	void tick(uint64_t apb_cycles);

	using BurstSink = std::function<void(const DmrBurst &)>;
	void set_burst_sink(BurstSink sink) { burst_sink_ = std::move(sink); }
	void set_timeslot(uint8_t ts) { ts_override_ = (ts == 1 || ts == 2) ? ts : 0; }
	void inject_burst(const DmrBurst &burst);

	using PhySink = std::function<void(const uint8_t *, size_t)>;
	void set_phy_block_sink(PhySink sink) { phy_sink_ = std::move(sink); }
	void set_phy_end_sink(std::function<void()> sink) { phy_end_sink_ = std::move(sink); }
	void inject_discriminator_samples(const int16_t *samples, size_t count);
	bool pcm_mic_enabled() const { return (audio_control_ & (1u << 4)) != 0; }
	bool fm_tx_active() const { return fm_tx_; }

private:
	void capture_tx(uint64_t apb_cycles);
	void capture_phy(uint64_t apb_cycles);
	void flush_phy();
	void inject_rx();
	void emit_fm_half();

	std::vector<uint8_t> &ram_;
	IrqFn irq_;
	std::array<uint32_t, 0x200> regs_{};
	uint32_t work_mode_ = 0;
	uint32_t local_cc_ = 0;
	uint32_t send_type_ = 0;
	uint32_t rx_type_info_ = 0;
	uint32_t sys_pending_ = 0;
	uint32_t sys_mask_ = 0;
	uint32_t layer2_status_ = 0;
	bool slot_active_ = false;
	uint8_t slot_ts_ = 0;
	uint64_t slot_next_apb_ = 0;
	uint64_t deframe_due_apb_ = 0;
	bool deframe_pending_ = false;
	DmrBurst rx_burst_{};
	BurstSink burst_sink_;
	PhySink phy_sink_;
	std::function<void()> phy_end_sink_;
	uint32_t audio_control_ = 0;
	bool fm_tx_ = false;
	uint8_t ts_override_ = 0;
	uint8_t fm_addr_sw_ = 0;
	uint8_t fm_half_ = 0;
	uint8_t tx_half_ = 0;
	uint8_t phy_last_half_ = 0;
	bool phy_last_valid_ = false;
	uint64_t fm_half_next_apb_ = 0;
	std::vector<int16_t> fm_samples_;
};
