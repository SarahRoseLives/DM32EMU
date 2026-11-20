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

std::vector<IqSample> VirtualRfIqChannel::impair(const std::vector<IqSample> &in)
{
	if (!impairments_.enabled) return in;
	std::vector<IqSample> out(in.size());
	const float c = std::cos(impairments_.phase) * impairments_.gain;
	const float s = std::sin(impairments_.phase) * impairments_.gain;
	std::normal_distribution<float> noise(0.0f, impairments_.noise_rms);
	for (size_t n = 0; n < in.size(); n++) {
		out[n].i = in[n].i * c - in[n].q * s + noise(rng_);
		out[n].q = in[n].i * s + in[n].q * c + noise(rng_);
	}
	return out;
}

void VirtualRfIqChannel::transmit(uint8_t source, const IqSample *samples, size_t count)
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
		queue.push_back({source, impair(block)});
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
