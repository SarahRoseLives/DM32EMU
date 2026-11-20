// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "uart_bridge.hpp"
#include "machine.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

namespace {

void set_nonblock(int fd)
{
	const int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void make_raw(int fd)
{
	termios tio{};
	if (tcgetattr(fd, &tio) != 0) return;
	cfmakeraw(&tio);
	tcsetattr(fd, TCSANOW, &tio);
}

} // namespace

UartBridge::~UartBridge() { close(); }

bool UartBridge::open_pty(std::string &path, std::string &err)
{
	close();
	const int master = posix_openpt(O_RDWR | O_NOCTTY);
	if (master < 0) {
		err = std::strerror(errno);
		return false;
	}
	if (grantpt(master) != 0 || unlockpt(master) != 0) {
		err = std::strerror(errno);
		::close(master);
		return false;
	}
	char name[128]{};
	if (ptsname_r(master, name, sizeof(name)) != 0) {
		err = std::strerror(errno);
		::close(master);
		return false;
	}
	const int slave = ::open(name, O_RDWR | O_NOCTTY);
	if (slave >= 0) {
		make_raw(slave);
		::close(slave);
	}
	set_nonblock(master);
	pty_master_ = master;
	path = name;
	return true;
}

bool UartBridge::open_tcp(uint16_t port, std::string &info, std::string &err)
{
	close();
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		err = std::strerror(errno);
		return false;
	}
	const int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);
	if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
		err = std::strerror(errno);
		::close(fd);
		return false;
	}
	if (listen(fd, 1) != 0) {
		err = std::strerror(errno);
		::close(fd);
		return false;
	}
	socklen_t len = sizeof(addr);
	if (getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) == 0) {
		info = "127.0.0.1:" + std::to_string(ntohs(addr.sin_port));
	} else {
		info = "127.0.0.1:" + std::to_string(port);
	}
	set_nonblock(fd);
	listen_fd_ = fd;
	return true;
}

void UartBridge::close()
{
	if (pty_master_ >= 0) ::close(pty_master_);
	if (client_fd_ >= 0) ::close(client_fd_);
	if (listen_fd_ >= 0) ::close(listen_fd_);
	pty_master_ = listen_fd_ = client_fd_ = -1;
	tx_.clear();
}

void UartBridge::write_byte(uint8_t b)
{
	if (!active()) return;
	if (tx_.size() < (1u << 20)) tx_.push_back(b);
}

void UartBridge::read_fd(int fd, Machine &machine)
{
	uint8_t buf[512];
	for (;;) {
		const ssize_t n = ::read(fd, buf, sizeof(buf));
		if (n > 0) {
			for (ssize_t i = 0; i < n; i++) {
				if (rx_cb_) rx_cb_(buf[i]);
				machine.feed_uart(0, buf[i]);
			}
			continue;
		}
		if (n == 0) {
			if (fd == client_fd_) {
				::close(client_fd_);
				client_fd_ = -1;
			}
			return;
		}
		if (errno == EINTR) continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			if (fd == client_fd_) {
				::close(client_fd_);
				client_fd_ = -1;
			}
		}
		return;
	}
}

void UartBridge::flush_to(int fd)
{
	while (!tx_.empty()) {
		uint8_t buf[256];
		const size_t n = std::min<size_t>(tx_.size(), sizeof(buf));
		std::copy_n(tx_.begin(), n, buf);
		ssize_t w;
		if (fd == client_fd_ && listen_fd_ >= 0) {
			w = ::send(fd, buf, n, MSG_NOSIGNAL);
		} else {
			w = ::write(fd, buf, n);
		}
		if (w > 0) {
			tx_.erase(tx_.begin(), tx_.begin() + w);
			continue;
		}
		if (w < 0 && errno == EINTR) continue;
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
		// No slave on the PTY yet, or the TCP peer went away. Drop the bytes
		// rather than growing the buffer forever.
		if (w < 0 && (errno == EIO || errno == EPIPE || errno == ECONNRESET)) {
			tx_.clear();
		}
		return;
	}
}

void UartBridge::accept_client()
{
	if (client_fd_ >= 0) return;
	sockaddr_in addr{};
	socklen_t len = sizeof(addr);
	const int fd = accept(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len);
	if (fd < 0) return;
	set_nonblock(fd);
	client_fd_ = fd;
}

void UartBridge::poll(Machine &machine)
{
	if (listen_fd_ >= 0) {
		accept_client();
		if (client_fd_ >= 0) {
			read_fd(client_fd_, machine);
			flush_to(client_fd_);
		}
	}
	if (pty_master_ >= 0) {
		read_fd(pty_master_, machine);
		flush_to(pty_master_);
	}
}
