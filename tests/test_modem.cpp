// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "machine.hpp"
#include "virtual_rf.hpp"

#include "test_util.hpp"

#include <memory>
#include <vector>

namespace {

constexpr uint32_t SYS_BASE = 0x11000000;
constexpr uint32_t CPU_PER_SLOT = 5040000; // 30 ms at 168 MHz CPU / 84 MHz APB

uint32_t sys_read(Machine &m, uint32_t off) { return m.read32(SYS_BASE + off); }
void sys_write(Machine &m, uint32_t off, uint32_t value) { m.write32(SYS_BASE + off, value); }

} // namespace

int main()
{
	Machine tx;
	sys_write(tx, 0x100, 0x6Eu);
	CHECK_EQ(sys_read(tx, 0x100), 0x6Eu);
	sys_write(tx, 0x384, 1);
	CHECK_EQ(sys_read(tx, 0x384), 1);

	auto bus = std::make_shared<VirtualRfBus>();
	tx.attach_virtual_rf(bus, 0);
	bus->add_receiver(1);
	tx.set_virtual_rf_enabled(true);

	for (int i = 0; i < 27; i++) tx.write8(Machine::MODEM_RAM_BASE + 0x960 + i, (uint8_t)(0xA0 + i));
	sys_write(tx, 0x418, 0x08u | (2u << 4));
	sys_write(tx, 0x408, 0x80);
	CHECK_EQ(bus->sent(), 1);

	VirtualRfBurst out;
	CHECK(bus->receive(1, out));
	CHECK_EQ(out.source, 0);
	CHECK_EQ(out.kind, 0x10);
	CHECK_EQ(out.voice_index, 2);
	CHECK_EQ(out.cc, 1);
	CHECK_EQ(out.ts, 1);
	CHECK_EQ(out.len, 27);
	for (int i = 0; i < 27; i++) CHECK_EQ(out.payload[i], (uint8_t)(0xA0 + i));

	// A voice LC header gets RS(12,9) parity, so the receiver's call stack
	// can decode it instead of seeing the TX RAM's zero padding.
	const uint8_t lc[9] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x2F, 0xFB, 0xA5};
	for (int i = 0; i < 9; i++) tx.write8(Machine::MODEM_RAM_BASE + i, lc[i]);
	sys_write(tx, 0x418, 1u << 4);
	sys_write(tx, 0x408, 0x80);
	CHECK_EQ(bus->sent(), 2);
	CHECK(bus->receive(1, out));
	CHECK_EQ(out.kind, 1);
	CHECK_EQ(out.len, 12);
	for (int i = 0; i < 9; i++) CHECK_EQ(out.payload[i], lc[i]);
	CHECK_EQ(out.payload[9], 0xB8);
	CHECK_EQ(out.payload[10], 0x05);
	CHECK_EQ(out.payload[11], 0xEE);

	// The TX slot alternates. The second boundary is TS2, and the burst carries
	// that number so a TS2 listener can accept it.
	auto bus2 = std::make_shared<VirtualRfBus>();
	Machine tx2;
	tx2.attach_virtual_rf(bus2, 0);
	bus2->add_receiver(1);
	tx2.set_virtual_rf_enabled(true);
	sys_write(tx2, 0x100, 0x6Eu);
	sys_write(tx2, 0x384, 1);
	tx2.tick(CPU_PER_SLOT);
	tx2.tick(CPU_PER_SLOT * 2);
	sys_write(tx2, 0x418, 1u << 4);
	sys_write(tx2, 0x408, 0x80);
	CHECK_EQ(bus2->sent(), 1);
	CHECK(bus2->receive(1, out));
	CHECK_EQ(out.kind, 1);
	CHECK_EQ(out.ts, 2);

	// RX: one slot delivers the burst, then the deframe delay raises RTI and
	// SYS_DEFRAME and copies the payload into modem RAM.
	auto rx_bus = std::make_shared<VirtualRfBus>();
	Machine rx;
	rx.attach_virtual_rf(rx_bus, 0);
	rx.set_virtual_rf_enabled(true);
	sys_write(rx, 0x100, 0x6Eu);
	sys_write(rx, 0x384, 1);

	VirtualRfBurst in;
	in.source = 1;
	in.kind = 1;
	in.cc = 1;
	in.ts = 1;
	in.len = 12;
	for (int i = 0; i < 12; i++) in.payload[i] = (uint8_t)(0x40 + i);
	rx_bus->send(in);
	rx.tick(CPU_PER_SLOT);
	rx.tick(CPU_PER_SLOT + 336000); // 168,000 APB deframe delay
	CHECK_EQ(sys_read(rx, 0x390), 0x11Au); // TS1 sync family/type, CC 1, header
	CHECK_EQ(sys_read(rx, 0x398) & 8u, 8u);
	for (int i = 0; i < 12; i++) CHECK_EQ(rx.read8(Machine::MODEM_RAM_BASE + 0x4B0 + i), in.payload[i]);
	sys_write(rx, 0x3A0, 8);
	in.kind = 0x10;
	in.voice_index = 3;
	in.len = 27;
	for (int i = 0; i < 27; i++) in.payload[i] = (uint8_t)(0x60 + i);
	rx_bus->send(in);
	rx.tick(CPU_PER_SLOT * 2);
	rx.tick(CPU_PER_SLOT * 2 + 336000);
	CHECK_EQ(sys_read(rx, 0x390), 0x401Au); // voice index 4 -> bits 12..14
	CHECK_EQ(sys_read(rx, 0x398) & 8u, 8u);
	for (int i = 0; i < 27; i++) CHECK_EQ(rx.read8(Machine::MODEM_RAM_BASE + 0x960 + i), in.payload[i]);

	// TS2 header: sync family 3, sync type 2, CC 1, kind 1.
	sys_write(rx, 0x3A0, 8);
	in.kind = 1;
	in.ts = 2;
	in.len = 12;
	for (int i = 0; i < 12; i++) in.payload[i] = (uint8_t)(0x70 + i);
	rx_bus->send(in);
	rx.tick(CPU_PER_SLOT * 3);
	rx.tick(CPU_PER_SLOT * 3 + 336000);
	CHECK_EQ(sys_read(rx, 0x390), 0x11Eu);
	CHECK_EQ(sys_read(rx, 0x398) & 8u, 8u);
	for (int i = 0; i < 12; i++) CHECK_EQ(rx.read8(Machine::MODEM_RAM_BASE + 0x4B0 + i), in.payload[i]);

	return 0;
}
