// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "c4fm_frontend.hpp"

#include "test_util.hpp"

#include <vector>

int main()
{
	C4fmFrontend tx;
	C4fmFrontend rx;
	std::vector<IqSample> iq;
	tx.set_iq_sink([&](const IqSample *s, size_t n) { iq.insert(iq.end(), s, s + n); });
	std::vector<int16_t> disc;
	rx.set_discriminator_sink([&](const int16_t *s, size_t n) { disc.insert(disc.end(), s, s + n); });

	// 30 ms of outer positive symbols (dibit 01), then outer negative (11).
	std::vector<uint8_t> dibits(288, 1);
	tx.add_dibits(dibits.data(), dibits.size(), 400);
	tx.generate(14400);
	CHECK_EQ(iq.size(), 14400u);
	rx.receive_iq(iq.data(), iq.size());
	CHECK(disc.size() > 300);

	int positive = 0, negative = 0;
	for (size_t i = 100; i < disc.size() - 100; i++) {
		if (disc[i] > 3000) positive++;
		if (disc[i] < -3000) negative++;
	}
	CHECK(positive > 0);
	CHECK_EQ(negative, 0);

	// Dibit 11 is the other outer level.
	std::fill(dibits.begin(), dibits.end(), 3);
	tx.reset();
	rx.reset();
	iq.clear();
	disc.clear();
	tx.add_dibits(dibits.data(), dibits.size(), 400);
	tx.generate(14400);
	rx.receive_iq(iq.data(), iq.size());
	for (size_t i = 100; i < disc.size() - 100; i++) {
		CHECK(disc[i] < -3000);
	}
	return 0;
}
