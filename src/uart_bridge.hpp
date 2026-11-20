// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>

class Machine;

// Bridges the emulated UART0 to a host-side PTY or a localhost TCP socket.
// The debug link, CPS tools and HIL scripts can use the PTY like the radio's
// serial programming cable.
class UartBridge {
public:
	~UartBridge();

	bool open_pty(std::string &path, std::string &err);
	bool open_tcp(uint16_t port, std::string &info, std::string &err);
	void close();
	bool active() const { return pty_master_ >= 0 || listen_fd_ >= 0; }

	void poll(Machine &machine);
	void write_byte(uint8_t b);
	void set_rx_callback(std::function<void(uint8_t)> cb) { rx_cb_ = std::move(cb); }

private:
	int pty_master_ = -1;
	int listen_fd_ = -1;
	int client_fd_ = -1;
	std::deque<uint8_t> tx_;
	std::function<void(uint8_t)> rx_cb_;

	void read_fd(int fd, Machine &machine);
	void flush_to(int fd);
	void accept_client();
};
