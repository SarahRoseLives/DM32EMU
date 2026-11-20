// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "rf_channel.hpp"

#include "test_util.hpp"

#include <cmath>
#include <vector>

int main()
{
	VirtualRfIqChannel channel;
	channel.add_receiver(0);
	channel.add_receiver(1);
	channel.add_receiver(2);

	std::vector<IqSample> tx = {{1.0f, 0.0f}, {0.0f, 1.0f}, {-1.0f, 0.0f}};
	channel.transmit(0, tx.data(), tx.size());
	CHECK_EQ(channel.transmitted_samples(), 3);

	std::vector<IqSample> rx;
	CHECK(!channel.receive(0, rx));
	CHECK(channel.receive(1, rx));
	CHECK_EQ(rx.size(), 3);
	CHECK_EQ(rx[0].i, 1.0f);
	CHECK_EQ(rx[1].q, 1.0f);
	CHECK(channel.receive(2, rx));
	CHECK_EQ(rx.size(), 3);
	CHECK_EQ(channel.delivered_samples(), 6);

	RfImpairments impairments;
	impairments.enabled = true;
	impairments.gain = 0.5f;
	impairments.phase = (float)(M_PI / 2.0);
	channel.set_impairments(impairments);
	channel.transmit(0, tx.data(), tx.size());
	CHECK(channel.receive(1, rx));
	CHECK(std::fabs(rx[0].i) < 0.001f);
	CHECK(std::fabs(rx[0].q - 0.5f) < 0.001f);

	channel.clear();
	CHECK(!channel.receive(1, rx));
	channel.remove_receiver(2);
	channel.transmit(1, tx.data(), tx.size());
	CHECK(channel.receive(0, rx));
	CHECK(!channel.receive(2, rx));
	return 0;
}
