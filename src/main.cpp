// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The DM32EMU Authors
#include "cpu.hpp"
#include "elf.hpp"
#include "firmware.hpp"
#include "machine.hpp"

#include <algorithm>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

void usage(const char *argv0)
{
	std::fprintf(stderr,
		"usage: %s FIRMWARE [options]\n"
		"\n"
		"FIRMWARE is a raw image linked for 0x0300c000 or a C-SKY ELF file.\n"
		"\n"
		"  --data-flash FILE    load a 16 MB SPI NOR dump\n"
		"  --steps N            instruction budget (default 500000000)\n"
		"  --frames N           stop after N complete LCD frames\n"
		"  --trace              print every instruction to stderr\n"
		"  --symbols FILE       read function names from an ELF file\n"
		"  --break ADDR         stop when the PC reaches ADDR (repeatable)\n"
		"  --dump ADDR:LEN      print memory on stop (repeatable)\n"
		"  --key NAME[@STEP[:N]] press a key at instruction STEP for N steps\n"
		"  --key-hold N         default key hold in instructions (20000000)\n"
		"  --no-uart-echo       collect UART output without printing it live\n"
		"  --fb FILE            write the LCD as a PPM on exit\n"
		"  --fb-every N         write a PPM every N completed frames\n"
		"  --verbose            log unknown MMIO reads and writes\n"
		"  --self-test          run built-in checks and exit\n"
		"\n"
		"Keys: ok/green/menu 0-9 * # up down left right back/red sk1 sk2 ptt\n",
		argv0);
}

uint64_t parse_u64(const char *s)
{
	char *end = nullptr;
	const unsigned long long v = std::strtoull(s, &end, 0);
	if (!end || *end) {
		std::fprintf(stderr, "bad number: %s\n", s);
		std::exit(2);
	}
	return (uint64_t)v;
}

struct SymTab {
	std::vector<ElfSymbol> syms;

	const ElfSymbol *find(uint32_t pc) const
	{
		auto it = std::upper_bound(syms.begin(), syms.end(), pc,
					   [](uint32_t a, const ElfSymbol &b) { return a < b.addr; });
		if (it == syms.begin()) return nullptr;
		--it;
		if (it->size && pc >= it->addr + it->size) return nullptr;
		return &*it;
	}

	std::string label(uint32_t pc) const
	{
		const ElfSymbol *s = find(pc);
		if (!s) return {};
		char buf[256];
		std::snprintf(buf, sizeof(buf), "%s+0x%x", s->name.c_str(), (unsigned)(pc - s->addr));
		return buf;
	}
};

struct KeyEvent {
	std::string name;
	uint64_t start = 0;
	uint64_t duration = 0;
	bool down = false;
	bool up = false;
};

struct DumpRange {
	uint32_t addr = 0;
	uint32_t len = 0;
};

bool self_test()
{
	Machine m;
	m.write32(0x00010000, 0x12345678);
	if (m.read32(0x00010000) != 0x12345678) return false;
	m.write8(0x00010004, 0xAB);
	if (m.read8(0x00010004) != 0xAB) return false;
	m.write32(0x14030000, 'O');
	m.write32(0x14030000, 'K');
	std::FILE *out = std::tmpfile();
	if (!out) return false;
	m.dump_uart(out);
	std::rewind(out);
	char buf[8]{};
	std::fread(buf, 1, sizeof(buf) - 1, out);
	std::fclose(out);
	if (std::strcmp(buf, "OK") != 0) return false;
	m.write32(Machine::FLASH_BASE + 0x100, 0xDEADBEEF);
	if (m.read32(Machine::FLASH_BASE + 0x100) != 0xDEADBEEF) return false;

	// A tiny CK803S program: r0 = 5, r1 = 7, r0 += r1, store to 0x20,
	// load it back into r3, return through r15.
	const uint16_t code[] = {
		0x3005, // movi r0, 5
		0x3107, // movi r1, 7
		0x6004, // addu r0, r1
		0x3220, // movi r2, 0x20
		0xb200, // st.w r0, (r2, 0)
		0x9260, // ld.w r3, (r2, 0)
		0x783c, // rts (jmp r15)
	};
	for (size_t i = 0; i < sizeof(code) / sizeof(code[0]); i++) {
		m.write16(Machine::APP_BASE + (uint32_t)i * 2, code[i]);
	}
	Cpu cpu(m);
	cpu.reset(Machine::APP_BASE);
	for (size_t i = 0; i < sizeof(code) / sizeof(code[0]); i++) cpu.step();
	if (cpu.reg(0) != 12 || cpu.reg(3) != 12) return false;
	if (m.read32(0x20) != 12) return false;
	return true;
}

} // namespace

int main(int argc, char **argv)
{
	std::string firmware;
	std::string data_flash;
	std::string fb_path;
	std::string symbols_path;
	uint64_t max_steps = 500000000ULL;
	uint64_t fb_every = 0;
	uint64_t stop_frames = 0;
	uint64_t default_hold = 20000000ULL;
	bool trace = false;
	bool uart_echo = true;
	bool verbose = false;
	std::vector<uint32_t> breakpoints;
	std::vector<DumpRange> dumps;
	std::vector<KeyEvent> keys;

	for (int i = 1; i < argc; i++) {
		const std::string a = argv[i];
		if (a == "--help" || a == "-h") {
			usage(argv[0]);
			return 0;
		} else if (a == "--self-test") {
			return self_test() ? 0 : 1;
		} else if (a == "--steps" && i + 1 < argc) {
			max_steps = parse_u64(argv[++i]);
		} else if (a == "--frames" && i + 1 < argc) {
			stop_frames = parse_u64(argv[++i]);
		} else if (a == "--data-flash" && i + 1 < argc) {
			data_flash = argv[++i];
		} else if (a == "--fb" && i + 1 < argc) {
			fb_path = argv[++i];
		} else if (a == "--fb-every" && i + 1 < argc) {
			fb_every = parse_u64(argv[++i]);
		} else if (a == "--symbols" && i + 1 < argc) {
			symbols_path = argv[++i];
		} else if (a == "--key-hold" && i + 1 < argc) {
			default_hold = parse_u64(argv[++i]);
		} else if (a == "--trace") {
			trace = true;
		} else if (a == "--no-uart-echo") {
			uart_echo = false;
		} else if (a == "--verbose") {
			verbose = true;
		} else if (a == "--break" && i + 1 < argc) {
			breakpoints.push_back((uint32_t)parse_u64(argv[++i]));
		} else if (a == "--dump" && i + 1 < argc) {
			const std::string v = argv[++i];
			const size_t colon = v.find(':');
			if (colon == std::string::npos) {
				std::fprintf(stderr, "expected ADDR:LEN, got %s\n", v.c_str());
				return 2;
			}
			DumpRange r;
			r.addr = (uint32_t)parse_u64(v.substr(0, colon).c_str());
			r.len = (uint32_t)parse_u64(v.substr(colon + 1).c_str());
			dumps.push_back(r);
		} else if (a == "--key" && i + 1 < argc) {
			const std::string v = argv[++i];
			KeyEvent ev;
			const size_t at = v.find('@');
			if (at == std::string::npos) {
				ev.name = v;
				ev.start = 0;
				ev.duration = default_hold;
			} else {
				ev.name = v.substr(0, at);
				const std::string rest = v.substr(at + 1);
				const size_t colon = rest.find(':');
				if (colon == std::string::npos) {
					ev.start = parse_u64(rest.c_str());
					ev.duration = default_hold;
				} else {
					ev.start = parse_u64(rest.substr(0, colon).c_str());
					ev.duration = parse_u64(rest.substr(colon + 1).c_str());
				}
			}
			keys.push_back(std::move(ev));
		} else if (!a.empty() && a[0] == '-') {
			std::fprintf(stderr, "unknown option: %s\n", a.c_str());
			return 2;
		} else if (firmware.empty()) {
			firmware = a;
		} else {
			std::fprintf(stderr, "unexpected argument: %s\n", a.c_str());
			return 2;
		}
	}

	if (firmware.empty()) {
		usage(argv[0]);
		return 2;
	}

	Machine machine;
	machine.set_uart_echo(uart_echo);
	machine.set_verbose(verbose);
	std::string err;
	if (!data_flash.empty() && !machine.load_data_flash(data_flash, err)) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}

	FirmwareInfo info;
	if (!load_firmware(machine, firmware, info, err)) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	const uint32_t entry = info.entry;
	if (info.elf && symbols_path.empty()) symbols_path = firmware;

	SymTab symtab;
	if (!symbols_path.empty()) {
		if (!load_symbols(symbols_path, symtab.syms, err)) {
			std::fprintf(stderr, "symbols: %s\n", err.c_str());
		}
	}

	Cpu cpu(machine);
	cpu.reset(entry);

	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);

	const std::string start_label = symtab.label(entry);
	std::fprintf(stderr, "dm32emu: %s, entry 0x%08x%s, %" PRIu64 " instruction budget\n",
			 firmware.c_str(), entry, start_label.empty() ? "" : (" (" + start_label + ")").c_str(),
			 max_steps);

	uint64_t last_frame = 0;
	uint64_t steps = 0;
	bool break_hit = false;
	int rc = 0;
	for (; steps < max_steps && !g_stop; steps++) {
		for (auto &k : keys) {
			if (!k.down && steps >= k.start && (k.duration == 0 || steps < k.start + k.duration)) {
				std::string e;
				if (!machine.set_key(k.name, true, e)) {
					std::fprintf(stderr, "%s\n", e.c_str());
					rc = 2;
					g_stop = 1;
				}
				k.down = true;
			}
			if (k.down && !k.up && k.duration != 0 && steps >= k.start + k.duration) {
				std::string e;
				if (!machine.set_key(k.name, false, e)) std::fprintf(stderr, "%s\n", e.c_str());
				k.up = true;
			}
		}

		if (std::find(breakpoints.begin(), breakpoints.end(), cpu.pc()) != breakpoints.end()) {
			std::fprintf(stderr, "\nbreakpoint at 0x%08x%s\n", cpu.pc(),
					 symtab.label(cpu.pc()).empty() ? "" : (" (" + symtab.label(cpu.pc()) + ")").c_str());
			break_hit = true;
			break;
		}

		try {
			if (trace) {
				const std::string label = symtab.label(cpu.pc());
				std::fprintf(stderr, "[%08x]%s psr=%08x\n", cpu.pc(),
						 label.empty() ? "" : (" " + label).c_str(), cpu.psr());
			}
			cpu.step();
			machine.tick(cpu.cycles());
		} catch (const CpuFault &f) {
			std::fprintf(stderr, "\nCPU fault: %s\n", f.what());
			const std::string label = symtab.label(f.pc);
			if (!label.empty()) std::fprintf(stderr, "  at %s\n", label.c_str());
			cpu.dump(stderr);
			machine.print_state(stderr);
			rc = 1;
			break;
		}

		const uint64_t frames = machine.frame_count();
		if (frames != last_frame) {
			last_frame = frames;
			std::fprintf(stderr, "\n[frame %" PRIu64 " at pc=%08x, %" PRIu64 " instructions]\n",
					 frames, cpu.pc(), steps);
			if (fb_every && frames % fb_every == 0 && !fb_path.empty()) {
				std::string e;
				char path[512];
				std::snprintf(path, sizeof(path), "%s.%llu.ppm", fb_path.c_str(),
						  (unsigned long long)frames);
				if (!machine.save_framebuffer_ppm(path, e)) std::fprintf(stderr, "%s\n", e.c_str());
			}
			if (stop_frames && frames >= stop_frames) break;
		}
	}

	machine.dump_uart(stdout);
	if (!fb_path.empty()) {
		std::string e;
		if (!machine.save_framebuffer_ppm(fb_path, e)) {
			std::fprintf(stderr, "%s\n", e.c_str());
			rc = 1;
		}
	}
	for (const auto &r : dumps) {
		std::fprintf(stderr, "\nmemory %08x..%08x:\n", r.addr, r.addr + r.len);
		for (uint32_t off = 0; off < r.len; off += 16) {
			std::fprintf(stderr, "%08x:", r.addr + off);
			for (uint32_t j = 0; j < 16 && off + j < r.len; j++) {
				std::fprintf(stderr, " %02x", machine.read8(r.addr + off + j));
			}
			std::fprintf(stderr, "\n");
		}
	}
	const std::string stop_label = symtab.label(cpu.pc());
	std::fprintf(stderr, "\ndm32emu: stopped after %" PRIu64 " instructions, pc=%08x%s%s\n", steps,
			 cpu.pc(), stop_label.empty() ? "" : (" (" + stop_label + ")").c_str(),
			 break_hit ? " [breakpoint]" : "");
	cpu.dump(stderr);
	machine.print_state(stderr);
	return rc;
}
