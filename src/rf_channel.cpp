// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "rf_channel.hpp"

#include <algorithm>
#include <cmath>

void VirtualRfIqChannel::add_receiver(uint8_t id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	mailboxes_.try_emplace(id);
}

void VirtualRfIqChannel::remove_receiver(uint8_t id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	mailboxes_.erase(id);
}

void VirtualRfIqChannel::clear()
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &[id, queue] : mailboxes_) queue.clear();
}

void VirtualRfIqChannel::set_impairments(const RfImpairments &impairments)
{
	std::lock_guard<std::mutex> lock(mutex_);
	impairments_ = impairments;
}

RfImpairments VirtualRfIqChannel::impairments() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return impairments_;
}

std::vector<IqSample> VirtualRfIqChannel::impair(const std::vector<IqSample> &in, uint64_t start_sample)
{
	if (!impairments_.enabled) return in;
	const double clock_ratio = 1.0 + impairments_.sample_clock_ppm * 1.0e-6;
	const size_t out_count = clock_ratio == 1.0 ? in.size() :
		std::max<size_t>(1, (size_t)std::llround((double)in.size() / clock_ratio));
	std::vector<IqSample> out(out_count);
	const double base = impairments_.phase;
	const double freq_step = 2.0 * M_PI * impairments_.frequency_offset_hz / SAMPLE_RATE_HZ;
	const double fade_step = 2.0 * M_PI * impairments_.fading_rate_hz / SAMPLE_RATE_HZ;
	std::normal_distribution<float> noise(0.0f, impairments_.noise_rms);
	for (size_t n = 0; n < out_count; n++) {
		const double src = (double)n * clock_ratio;
		const size_t i0 = std::min(in.size() - 1, (size_t)src);
		const size_t i1 = std::min(in.size() - 1, i0 + 1);
		const float frac = (float)(src - (double)i0);
		const float ii = in[i0].i + (in[i1].i - in[i0].i) * frac;
		const float qq = in[i0].q + (in[i1].q - in[i0].q) * frac;
		const double fade = 1.0 + impairments_.fading_depth * std::sin(fading_phase_);
		fading_phase_ += fade_step;
		if (fading_phase_ > 2.0 * M_PI) fading_phase_ -= 2.0 * M_PI;
		impair_phase_ += freq_step;
		if (impair_phase_ > 2.0 * M_PI) impair_phase_ -= 2.0 * M_PI;
		const double c = std::cos(base + impair_phase_) * impairments_.gain * fade;
		const double s = std::sin(base + impair_phase_) * impairments_.gain * fade;
		out[n].i = (float)(ii * c - qq * s + noise(rng_));
		out[n].q = (float)(ii * s + qq * c + noise(rng_));
	}
	return out;
}

void VirtualRfIqChannel::transmit(uint8_t source, const IqSample *samples, size_t count)
{
	transmit(source, samples, count, 0);
}

void VirtualRfIqChannel::transmit(uint8_t source, const IqSample *samples, size_t count, uint64_t start_sample)
{
	if (samples == nullptr || count == 0) return;
	std::vector<IqSample> block(samples, samples + count);
	transmitted_samples_.fetch_add(count, std::memory_order_relaxed);
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &[id, queue] : mailboxes_) {
		if (id == source) continue;
		if (queue.size() >= MAX_BLOCKS) {
			queue.pop_front();
			dropped_blocks_.fetch_add(1, std::memory_order_relaxed);
		}
		queue.push_back({source, start_sample + impairments_.delay_samples, impair(block, start_sample)});
	}
}

bool VirtualRfIqChannel::receive(uint8_t receiver, std::vector<IqSample> &out)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = mailboxes_.find(receiver);
	if (it == mailboxes_.end() || it->second.empty()) return false;
	out = std::move(it->second.front().samples);
	it->second.pop_front();
	delivered_samples_.fetch_add(out.size(), std::memory_order_relaxed);
	return true;
}

bool VirtualRfIqChannel::receive_at(uint8_t receiver, uint64_t time, std::vector<IqSample> &out)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = mailboxes_.find(receiver);
	if (it == mailboxes_.end() || it->second.empty()) return false;
	Block &block = it->second.front();
	const uint64_t ready = block.start_sample + block.samples.size() + jitter_samples_;
	if (ready > time) return false;
	out = std::move(block.samples);
	it->second.pop_front();
	delivered_samples_.fetch_add(out.size(), std::memory_order_relaxed);
	return true;
}
