// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "machine.hpp"
#include "rf_channel.hpp"

#include "test_util.hpp"

#include <memory>

namespace {

constexpr uint32_t SYS_BASE = 0x11000000;
constexpr uint32_t CPU_PER_SLOT = 5040000;

uint32_t sys_read(Machine &m, uint32_t off) { return m.read32(SYS_BASE + off); }
void sys_write(Machine &m, uint32_t off, uint32_t value) { m.write32(SYS_BASE + off, value); }

void attach(Machine &m, const std::shared_ptr<VirtualRfIqChannel> &iq, uint8_t id)
{
	m.attach_iq_channel(iq, id);
	m.set_iq_enabled(true);
}

} // namespace

int main()
{
	auto iq = std::make_shared<VirtualRfIqChannel>();
	Machine tx;
	Machine rx;
	attach(tx, iq, 0);
	attach(rx, iq, 1);

	std::string id_err;
	CHECK(tx.set_dmr_id(3144614, id_err));
	CHECK_EQ(tx.dmr_id(), 3144614);
	CHECK(!tx.set_dmr_id(0, id_err));
	CHECK(!tx.set_dmr_id(0x1000000u, id_err));
	CHECK_EQ(tx.dmr_id(), 3144614);

	sys_write(tx, 0x100, 0x6Eu);
	sys_write(tx, 0x384, 1);
	sys_write(rx, 0x100, 0x6Eu);
	sys_write(rx, 0x384, 1);

	// A voice payload goes through the DMR hardware model, then the IQ
	// channel, and is handed back to the receive modem.
	for (int i = 0; i < 27; i++) tx.write8(Machine::MODEM_RAM_BASE + 0x960 + i, (uint8_t)(0xA0 + i));
	sys_write(tx, 0x418, 0x08u | (2u << 4));
	sys_write(tx, 0x408, 0x80);
	CHECK(iq->transmitted_samples() > 0);
	rx.tick(CPU_PER_SLOT);
	CHECK_EQ(sys_read(rx, 0x390), 0x301Au);
	CHECK_EQ(sys_read(rx, 0x398) & 8u, 8u);
	for (int i = 0; i < 27; i++) CHECK_EQ(rx.read8(Machine::MODEM_RAM_BASE + 0x960 + i), (uint8_t)(0xA0 + i));
	sys_write(rx, 0x3A0, 8);

	// A voice LC header gets RS(12,9) parity and arrives unchanged.
	const uint8_t lc[9] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x2F, 0xFB, 0xA5};
	for (int i = 0; i < 9; i++) tx.write8(Machine::MODEM_RAM_BASE + i, lc[i]);
	sys_write(tx, 0x418, 1u << 4);
	sys_write(tx, 0x408, 0x80);
	rx.tick(CPU_PER_SLOT * 2);
	CHECK_EQ(sys_read(rx, 0x390), 0x11Au);
	CHECK_EQ(sys_read(rx, 0x398) & 8u, 8u);
	for (int i = 0; i < 9; i++) CHECK_EQ(rx.read8(Machine::MODEM_RAM_BASE + 0x4B0 + i), lc[i]);
	CHECK_EQ(rx.read8(Machine::MODEM_RAM_BASE + 0x4B0 + 9), 0xB8);
	CHECK_EQ(rx.read8(Machine::MODEM_RAM_BASE + 0x4B0 + 10), 0x05);
	CHECK_EQ(rx.read8(Machine::MODEM_RAM_BASE + 0x4B0 + 11), 0xEE);
	sys_write(rx, 0x3A0, 8);

	// The TX slot alternates, and the burst carries TS2.
	Machine tx2;
	auto iq2 = std::make_shared<VirtualRfIqChannel>();
	Machine rx2;
	attach(tx2, iq2, 0);
	attach(rx2, iq2, 1);
	sys_write(tx2, 0x100, 0x6Eu);
	sys_write(tx2, 0x384, 1);
	sys_write(rx2, 0x100, 0x6Eu);
	sys_write(rx2, 0x384, 1);
	tx2.tick(CPU_PER_SLOT);
	tx2.tick(CPU_PER_SLOT * 2);
	sys_write(tx2, 0x418, 1u << 4);
	sys_write(tx2, 0x408, 0x80);
	rx2.tick(CPU_PER_SLOT * 3);
	CHECK_EQ(sys_read(rx2, 0x390), 0x11Eu);
	CHECK_EQ(sys_read(rx2, 0x398) & 8u, 8u);

	return 0;
}
