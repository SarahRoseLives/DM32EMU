// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "virtual_rf.hpp"

#include "test_util.hpp"

#include <cstring>

int main()
{
	VirtualRfBus bus;
	bus.add_receiver(0);
	bus.add_receiver(1);
	bus.add_receiver(2);
	bus.add_receiver(2); // idempotent

	int observed = 0;
	bus.set_observer([&](const VirtualRfBurst &) { observed++; });

	VirtualRfBurst burst;
	burst.source = 0;
	burst.kind = 1;
	burst.voice_index = 2;
	burst.cc = 1;
	burst.ts = 1;
	burst.len = 3;
	burst.payload[0] = 0x12;
	burst.payload[1] = 0x34;
	burst.payload[2] = 0x56;
	bus.send(burst);

	CHECK_EQ(bus.sent(), 1);
	CHECK_EQ(bus.dropped(), 0);
	CHECK_EQ(observed, 1);

	VirtualRfBurst out;
	CHECK(!bus.receive(0, out)); // never hear ourselves
	CHECK(bus.receive(1, out));
	CHECK_EQ(out.source, 0);
	CHECK_EQ(out.kind, 1);
	CHECK_EQ(out.voice_index, 2);
	CHECK_EQ(out.cc, 1);
	CHECK_EQ(out.ts, 1);
	CHECK_EQ(out.len, 3);
	CHECK_EQ(out.payload[0], 0x12);
	CHECK_EQ(out.payload[2], 0x56);
	CHECK_EQ(bus.delivered(), 1);
	CHECK(bus.receive(2, out));
	CHECK_EQ(bus.delivered(), 2);
	CHECK(!bus.receive(1, out));
	CHECK(!bus.receive(2, out));

	burst.source = 1;
	bus.send(burst);
	CHECK(!bus.receive(1, out));
	CHECK(bus.receive(0, out));
	CHECK(bus.receive(2, out));

	bus.clear();
	CHECK(!bus.receive(1, out));
	CHECK_EQ(bus.sent(), 2);
	CHECK_EQ(bus.delivered(), 4);

	bus.set_timeslot(2);
	CHECK_EQ(bus.timeslot(), 2);
	burst.source = 0;
	burst.ts = 1;
	bus.send(burst);
	CHECK(bus.receive(1, out));
	CHECK_EQ(out.ts, 2);
	CHECK(bus.receive(2, out));
	CHECK_EQ(out.ts, 2);
	bus.set_timeslot(0);
	CHECK_EQ(bus.timeslot(), 0);
	bus.clear();

	bus.remove_receiver(2);
	bus.send(burst);
	CHECK(bus.receive(1, out));
	CHECK(!bus.receive(2, out));

	VirtualRfBus overflow;
	overflow.add_receiver(1);
	for (int i = 0; i < 257; i++) {
		burst.payload[0] = (uint8_t)i;
		overflow.send(burst);
	}
	CHECK_EQ(overflow.sent(), 257);
	CHECK_EQ(overflow.dropped(), 1);
	CHECK(overflow.receive(1, out));
	CHECK_EQ(out.payload[0], 1); // the oldest burst was dropped

	return 0;
}
