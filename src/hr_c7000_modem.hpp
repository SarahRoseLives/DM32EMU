// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include "virtual_rf.hpp"

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

	void attach_virtual_rf(std::shared_ptr<VirtualRfBus> bus, uint8_t id);
	void set_virtual_rf_enabled(bool on) { rf_enabled_ = on; }
	bool virtual_rf_enabled() const { return rf_enabled_; }

private:
	void capture_tx();
	void inject_rx();

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
	VirtualRfBurst rx_burst_{};
	std::shared_ptr<VirtualRfBus> rf_bus_;
	uint8_t rf_id_ = 0;
	bool rf_enabled_ = false;
};
