// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "rf_channel.hpp"
#include "rf_frontend.hpp"

#include "test_util.hpp"

#include <cmath>
#include <vector>

int main()
{
	constexpr int hz = 1000;
	constexpr int rate = 8000;
	constexpr int samples = rate / 10;
	std::vector<int16_t> audio(samples);
	for (int i = 0; i < samples; i++)
		audio[i] = (int16_t)std::lround(12000.0 * std::sin(2.0 * M_PI * hz * i / rate));

	VirtualRfIqChannel channel;
	channel.add_receiver(0);
	channel.add_receiver(1);
	FmFrontend tx;
	FmFrontend rx;
	std::vector<IqSample> iq;
	tx.set_iq_sink([&](const IqSample *s, size_t n) { iq.assign(s, s + n); });
	tx.modulate(audio.data(), audio.size());
	channel.transmit(0, iq.data(), iq.size());

	std::vector<int16_t> recovered;
	rx.set_audio_sink([&](const int16_t *s, size_t n) { recovered.assign(s, s + n); });
	std::vector<IqSample> from_channel;
	CHECK(channel.receive(1, from_channel));
	rx.receive_iq(from_channel.data(), from_channel.size());
	CHECK_EQ(recovered.size(), (size_t)samples);

	int crossings = 0;
	for (size_t i = 1; i < recovered.size(); i++) {
		if (recovered[i - 1] <= 0 && recovered[i] > 0) crossings++;
	}
	// 100 ms of 1 kHz: about 100 positive zero crossings.
	CHECK(crossings >= 90);
	CHECK(crossings <= 110);
	return 0;
}
