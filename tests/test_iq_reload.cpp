// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "machine.hpp"
#include "rf_channel.hpp"

#include "test_util.hpp"

#include <memory>
#include <vector>

int main()
{
	auto iq = std::make_shared<VirtualRfIqChannel>();
	auto old_machine = std::make_unique<Machine>();
	old_machine->attach_iq_channel(iq, 1);

	auto new_machine = std::make_unique<Machine>();
	new_machine->attach_iq_channel(iq, 1);

	// Replacing a radio must not let the old machine remove the receiver
	// mailbox that the new one just registered.
	old_machine.reset();

	IqSample sample{1.0f, 0.0f};
	iq->transmit(0, &sample, 1);
	std::vector<IqSample> received;
	CHECK(iq->receive(1, received));
	CHECK_EQ(received.size(), 1u);
	CHECK_EQ(received[0].i, 1.0f);
	return 0;
}
