// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "machine.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>

namespace {

constexpr uint32_t SYS_BASE = 0x11000000;
constexpr uint32_t LCD_BASE = 0x12000000;
constexpr uint32_t TIMER_BASE = 0x14000000;
constexpr uint32_t WDT_BASE = 0x14010000;
constexpr uint32_t GPIOA_BASE = 0x14020000;
constexpr uint32_t UART0_BASE = 0x14030000;
constexpr uint32_t UART1_BASE = 0x14040000;
constexpr uint32_t UART2_BASE = 0x14050000;
constexpr uint32_t I2C0_BASE = 0x14060000;
constexpr uint32_t I2C1_BASE = 0x14070000;
constexpr uint32_t I2C2_BASE = 0x14080000;
constexpr uint32_t UART3_BASE = 0x14090000;
constexpr uint32_t SPI0_BASE = 0x140A0000;
constexpr uint32_t SPI1_BASE = 0x140B0000;
constexpr uint32_t PWM_BASE = 0x140C0000;
constexpr uint32_t ADC_BASE = 0x140D0000;
constexpr uint32_t SPI2_BASE = 0x140E0000;
constexpr uint32_t DAC_BASE = 0x140F0000;
constexpr uint32_t GPIOB_BASE = 0x14100000;
constexpr uint32_t GPIOC_BASE = 0x14110000;
constexpr uint32_t PIC_BASE = 0x17000000;
constexpr uint32_t CACHE_BASE = 0xE000F000;

constexpr uint32_t UART_RBR = 0x00;
constexpr uint32_t UART_IER = 0x04;
constexpr uint32_t UART_FCR = 0x08;
constexpr uint32_t UART_LCR = 0x0C;
constexpr uint32_t UART_LSR = 0x14;
constexpr uint32_t UART_USR = 0x7C;

constexpr uint32_t LCD_W = 240;
constexpr uint32_t LCD_H = 320;

inline uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline void wr16le(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}
inline void wr32le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

bool in(uint32_t addr, uint32_t base, uint32_t len) { return addr >= base && addr < base + len; }

} // namespace

Machine::Machine() : modem_(modem_ram_, [this](int source) { pic_raise(source); })
{
	ram_.resize(RAM_SIZE, 0);
	flash_.resize(FLASH_SIZE, 0xFF);
	sys_sram_.resize(SYS_SRAM_SIZE, 0);
	modem_ram_.resize(MODEM_RAM_SIZE, 0);
	data_flash_.resize(16u * 1024u * 1024u, 0xFF);
	lcd_.fb.assign(LCD_W * LCD_H, 0);

	uarts_[0].base = UART0_BASE;
	uarts_[0].host = true;
	uarts_[0].irq_source = 12;
	uarts_[1].base = UART1_BASE;
	uarts_[1].irq_source = 10;
	uarts_[2].base = UART2_BASE;
	uarts_[2].irq_source = 11;
	uarts_[3].base = UART3_BASE;
	uarts_[3].irq_source = 13;

	ssi_[0].ctrlr0 = 0x07;

	reset();
}

Machine::~Machine()
{
	if (iq_channel_) iq_channel_->remove_receiver(iq_id_);
}

void Machine::transmit_dmr(const DmrBurst &burst)
{
	if (!packet_ || !iq_enabled_ || !iq_channel_) return;
	uint8_t bytes[36]{};
	bytes[0] = 0x44;
	bytes[1] = 0x4D;
	bytes[2] = burst.source;
	bytes[3] = burst.kind;
	bytes[4] = burst.voice_index;
	bytes[5] = burst.cc;
	bytes[6] = burst.ts;
	bytes[7] = burst.len;
	bytes[8] = dmr_seq_++;
	std::copy_n(burst.payload, std::min<size_t>(burst.len, 27), bytes + 9);
	uint8_t dibits[144]{};
	for (size_t k = 0; k < 144; k++) {
		const uint8_t byte = bytes[k / 4];
		dibits[k] = (uint8_t)((byte >> (6 - 2 * (k & 3))) & 3u);
	}
	packet_->transmit_dibits(dibits, 144);
}

void Machine::receive_dibits(const uint8_t *dibits, size_t count)
{
	if (dibits == nullptr || count == 0) return;
	dmr_rx_dibits_.insert(dmr_rx_dibits_.end(), dibits, dibits + count);
	while (dmr_rx_dibits_.size() >= 144) {
		uint8_t bytes[36]{};
		for (size_t k = 0; k < 144; k++)
			bytes[k / 4] = (uint8_t)(bytes[k / 4] | (dmr_rx_dibits_[k] << (6 - 2 * (k & 3))));
		dmr_rx_dibits_.erase(dmr_rx_dibits_.begin(), dmr_rx_dibits_.begin() + 144);
		if (bytes[0] != 0x44 || bytes[1] != 0x4D) continue;
		DmrBurst burst;
		burst.source = bytes[2];
		burst.kind = bytes[3];
		burst.voice_index = bytes[4];
		burst.cc = bytes[5];
		burst.ts = bytes[6];
		burst.len = (uint8_t)std::min<size_t>(bytes[7], 27);
		std::copy_n(bytes + 9, burst.len, burst.payload);
		modem_.inject_burst(burst);
	}
}

void Machine::inject_microphone_samples(const int16_t *samples, size_t count)
{
	if (samples == nullptr) return;
	mic_fifo_.insert(mic_fifo_.end(), samples, samples + count);
}

void Machine::fill_microphone()
{
	for (uint32_t i = 0; i < 80; i++) {
		int16_t sample;
		if (!mic_fifo_.empty()) {
			sample = mic_fifo_.front();
			mic_fifo_.pop_front();
		} else {
			sample = (int16_t)(8000.0 * std::sin(2.0 * M_PI * 1000.0 * pcm_phase_ / 8000.0));
			pcm_phase_ = (pcm_phase_ + 1u) % 8000u;
		}
		write16(SYS_SRAM_BASE + 2u * i, (uint16_t)sample);
	}
	pic_raise(28);
}

void Machine::speaker_pcm_byte(uint32_t offset, uint8_t value)
{
	if (offset < 0xA0u || offset >= 0xA0u + speaker_pcm_.size()) return;
	speaker_pcm_[offset - 0xA0u] = value;
	if (offset + 1u == 0xA0u + speaker_pcm_.size()) {
		if (audio_sink_) {
			int16_t out[80];
			for (size_t i = 0; i < 80; i++)
				out[i] = (int16_t)(speaker_pcm_[2 * i] | (speaker_pcm_[2 * i + 1] << 8));
			audio_sink_(out, 80);
		}
		speaker_pcm_len_ = 0;
	} else if (offset - 0xA0u + 1u > speaker_pcm_len_) {
		speaker_pcm_len_ = offset - 0xA0u + 1u;
	}
}

void Machine::attach_iq_channel(std::shared_ptr<VirtualRfIqChannel> channel, uint8_t id)
{
	if (iq_channel_) iq_channel_->remove_receiver(iq_id_);
	iq_channel_ = std::move(channel);
	iq_id_ = id;
	if (!iq_channel_) {
		air_.reset();
		packet_.reset();
		fm_.reset();
		modem_.set_burst_sink({});
		modem_.set_phy_block_sink({});
		modem_.set_phy_end_sink({});
		return;
	}
	iq_channel_->add_receiver(id);
	air_ = std::make_unique<C4fmFrontend>();
	air_->set_iq_sink([this](const IqSample *samples, size_t count) {
		if (iq_enabled_ && iq_channel_) iq_channel_->transmit(iq_id_, samples, count, rf_sample_time_);
	});
	air_->set_discriminator_sink([this](const int16_t *samples, size_t count) {
		modem_.inject_discriminator_samples(samples, count);
	});
	fm_ = std::make_unique<FmFrontend>();
	fm_->set_iq_sink([this](const IqSample *samples, size_t count) {
		if (iq_enabled_ && iq_channel_) iq_channel_->transmit(iq_id_, samples, count, rf_sample_time_);
	});
	fm_->set_audio_sink([this](const int16_t *samples, size_t count) {
		if (audio_sink_) audio_sink_(samples, count);
	});
	packet_ = std::make_unique<PacketFrontend>();
	packet_->set_iq_sink([this](const IqSample *samples, size_t count) {
		if (iq_enabled_ && iq_channel_) iq_channel_->transmit(iq_id_, samples, count, rf_sample_time_);
	});
	packet_->set_dibit_sink([this](const uint8_t *dibits, size_t count) {
		receive_dibits(dibits, count);
	});
	modem_.set_burst_sink([this](const DmrBurst &burst) { transmit_dmr(burst); });
	modem_.set_phy_block_sink([this](const uint8_t *block, size_t count) {
		if (count != 36 || !air_) return;
		uint8_t dibits[144];
		for (size_t k = 0; k < 144; k++) {
			const size_t bit = k * 2;
			dibits[k] = (uint8_t)((block[bit / 8] >> (6 - (bit & 6))) & 3u);
		}
		air_->transmit_dibits(dibits, 144);
	});
	modem_.set_phy_end_sink([this] { if (air_) air_->flush(); });
}

void Machine::reset()
{
	std::fill(ram_.begin(), ram_.end(), 0);
	std::fill(flash_.begin(), flash_.end(), 0xFF);
	std::fill(sys_sram_.begin(), sys_sram_.end(), 0);
	std::fill(modem_ram_.begin(), modem_ram_.end(), 0);
	std::fill(data_flash_.begin(), data_flash_.end(), 0xFF);
	lcd_ = Lcd{};
	lcd_.fb.assign(LCD_W * LCD_H, 0);
	frame_count_ = 0;
	lcd_generation_ = 0;
	uart_out_.clear();
	sys_ = {};
	sys_.lcsfc_baudr = 4;
	cache_ = {};
	pic_ = {};
	pic_.mask = pic_.mask1 = 0xFFFFFFFFu;
	pic_.po = pic_.po1 = 0xFFFFFFFFu;
	irq_asserted_ = false;
	modem_.reset();
	apb_cycles_ = 0;
	timers_ = {};
	for (auto &t : timers_) t.ctrl = 0x4;
	for (auto &g : gpio_) g = Gpio{};
	gpio_[0].input = 0xFFFFFFFFu;
	gpio_[1].input = 0xFFFFFFFFu;
	gpio_[2].input = 0xFFFFFFFFu;
	// Inputs with a known idle level: power switch on (0), PTT and keys released (1).
	gpio_[0].input &= ~(1u << 0x10);
	gpio_set_input(0x2B, true);
	for (int p = 0x31; p <= 0x34; p++) gpio_set_input(p, true);
	for (int p = 0x26; p <= 0x27; p++) gpio_set_input(p, true);
	gpio_set_input(0x3A, true);
	gpio_set_input(0x3B, true);
	gpio_set_input(0x08, true);
	for (auto &u : uarts_) {
		u.ier = u.lcr = u.mcr = u.fcr = 0;
		u.dll = 1; u.dlh = 0;
		u.rx_head = u.rx_tail = 0;
	}
	for (auto &s : ssi_) {
		s.rx_head = s.rx_tail = 0;
		s.ser = 0;
	}
	nor_ = SpiNor{};
	nor_.cs = true;
	wdt_lock_ = wdt_en_ = 0;
	adc_ctrl_ = adc_start_ = 0;
	adc_data_ab_ = 0;
	adc_data_cd_ = 672u | (512u << 16);
	dac_pd_ctrl_ = dac_pd_mode_ = 0;
	dac_data_ = {};
	pwm_cfg_ = pwm_per_ = pwm_fp_ = 0;
	i2c2_ = {};
	i2c2_.regs[0x11] = 0x07;
	i2c2_.regs[0x12] = 0x01;
	fd_ = {};
	gps_ = {};
	mic_fifo_.clear();
	speaker_pcm_ = {};
	speaker_pcm_len_ = 0;
	fm_tx_next_apb_ = 0;
}

bool Machine::load_elf_image(const ElfImage &image, std::string &err)
{
	for (const auto &seg : image.segments) {
		if (seg.filesz) {
			if (in(seg.paddr, FLASH_BASE, FLASH_SIZE)) {
				std::copy(seg.data.begin(), seg.data.end(), flash_.begin() + (seg.paddr - FLASH_BASE));
			} else if (seg.paddr < RAM_SIZE) {
				std::copy(seg.data.begin(), seg.data.end(), ram_.begin() + seg.paddr);
			} else if (in(seg.paddr, SYS_SRAM_BASE, SYS_SRAM_SIZE)) {
				std::copy(seg.data.begin(), seg.data.end(), sys_sram_.begin() + (seg.paddr - SYS_SRAM_BASE));
			} else {
				err = "ELF segment outside the emulated memory map";
				return false;
			}
		}
		if (seg.memsz > seg.filesz) {
			const uint32_t zero_addr = seg.vaddr + seg.filesz;
			const uint32_t zero_len = seg.memsz - seg.filesz;
			if (zero_addr < RAM_SIZE) {
				const uint32_t n = std::min(zero_len, RAM_SIZE - zero_addr);
				std::fill(ram_.begin() + zero_addr, ram_.begin() + zero_addr + n, 0);
			} else if (in(zero_addr, SYS_SRAM_BASE, SYS_SRAM_SIZE)) {
				const uint32_t off = zero_addr - SYS_SRAM_BASE;
				const uint32_t n = std::min(zero_len, SYS_SRAM_SIZE - off);
				std::fill(sys_sram_.begin() + off, sys_sram_.begin() + off + n, 0);
			}
		}
	}
	return true;
}

bool Machine::load_bin(const std::vector<uint8_t> &image, uint32_t base, std::string &err)
{
	if (base < FLASH_BASE || (uint64_t)base + image.size() > (uint64_t)FLASH_BASE + FLASH_SIZE) {
		err = "raw firmware image must fit in program flash";
		return false;
	}
	std::copy(image.begin(), image.end(), flash_.begin() + (base - FLASH_BASE));
	return true;
}

bool Machine::install_codec_slot(const std::vector<uint8_t> &slot, std::string &err)
{
	constexpr uint32_t off = 0x000E8000u;
	constexpr uint32_t room = 0x00018000u;
	constexpr uint32_t magic = 0x43524444u;
	if (slot.size() < 128 || slot.size() > room) {
		err = "codec slot has an invalid size";
		return false;
	}
	const uint32_t version = rd32le(slot.data() + 4);
	const uint32_t code_off = rd32le(slot.data() + 8);
	const uint32_t code_len = rd32le(slot.data() + 12);
	const uint32_t tab_off = rd32le(slot.data() + 20);
	const uint32_t tab_len = rd32le(slot.data() + 24);
	const uint32_t tag = rd32le(slot.data() + 60);
	const uint32_t rel_off = rd32le(slot.data() + 68);
	const uint32_t rel_count = rd32le(slot.data() + 72);
	if (rd32le(slot.data()) != magic || version != 2 || (tag != 46u && tag != 49u) ||
	    code_off < 128 || code_len == 0 || (uint64_t)code_off + code_len > slot.size() ||
	    tab_len == 0 || (uint64_t)tab_off + tab_len > slot.size() ||
	    rel_count != 3 || (uint64_t)rel_off + rel_count * 4u > slot.size()) {
		err = "codec slot header is invalid";
		return false;
	}
	std::fill(flash_.begin() + off, flash_.end(), 0xFF);
	std::copy(slot.begin(), slot.end(), flash_.begin() + off);
	return true;
}

bool Machine::install_data_flash(const std::vector<uint8_t> &data, uint32_t offset, std::string &err)
{
	if ((uint64_t)offset + data.size() > data_flash_.size()) {
		err = "data backup is outside the 16 MB data flash";
		return false;
	}
	std::copy(data.begin(), data.end(), data_flash_.begin() + offset);
	return true;
}

bool Machine::set_dmr_id(uint32_t id, std::string &err)
{
	constexpr uint32_t off = 0x7000E8u; // codeplug EEPROM image, own ID
	if (id == 0 || id > 0xFFFFFFu) {
		err = "DMR ID must be between 1 and 16777215";
		return false;
	}
	if (data_flash_.size() < off + 4) {
		err = "data flash is too small for a codeplug ID";
		return false;
	}
	uint8_t bcd[4];
	uint32_t value = id;
	for (int i = 3; i >= 0; i--) {
		bcd[i] = (uint8_t)((value % 10u) | ((value / 10u % 10u) << 4));
		value /= 100u;
	}
	std::copy(bcd, bcd + 4, data_flash_.begin() + off);
	return true;
}

uint32_t Machine::dmr_id() const
{
	constexpr uint32_t off = 0x7000E8u;
	if (data_flash_.size() < off + 4) return 0;
	const uint8_t *p = data_flash_.data() + off;
	uint32_t value = 0;
	for (int i = 0; i < 4; i++) {
		const uint8_t hi = (uint8_t)(p[i] >> 4);
		const uint8_t lo = (uint8_t)(p[i] & 0x0Fu);
		if (hi > 9 || lo > 9) return 0;
		value = value * 100u + (uint32_t)(hi * 10u + lo);
	}
	return value;
}

bool Machine::load_data_flash(const std::string &path, std::string &err)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		err = "cannot open " + path;
		return false;
	}
	f.read((char *)data_flash_.data(), (std::streamsize)std::min<size_t>(data_flash_.size(), 0x1000000));
	if (f.bad()) {
		err = "cannot read " + path;
		return false;
	}
	return true;
}

uint8_t Machine::read8(uint32_t addr)
{
	if (addr < RAM_SIZE) return ram_[addr];
	if (in(addr, FLASH_BASE, FLASH_SIZE)) return flash_[addr - FLASH_BASE];
	if (in(addr, SYS_SRAM_BASE, SYS_SRAM_SIZE)) return sys_sram_[addr - SYS_SRAM_BASE];
	if (in(addr, MODEM_RAM_BASE, MODEM_RAM_SIZE)) return modem_ram_[addr - MODEM_RAM_BASE];
	return (uint8_t)mmio_read(addr, 1);
}

uint16_t Machine::read16(uint32_t addr)
{
	if (addr + 1 < RAM_SIZE) return rd16le(ram_.data() + addr);
	if (in(addr, FLASH_BASE, FLASH_SIZE - 1)) return rd16le(flash_.data() + (addr - FLASH_BASE));
	if (in(addr, SYS_SRAM_BASE, SYS_SRAM_SIZE - 1)) return rd16le(sys_sram_.data() + (addr - SYS_SRAM_BASE));
	if (in(addr, MODEM_RAM_BASE, MODEM_RAM_SIZE - 1)) return rd16le(modem_ram_.data() + (addr - MODEM_RAM_BASE));
	return (uint16_t)mmio_read(addr, 2);
}

uint32_t Machine::read32(uint32_t addr)
{
	if (addr + 3 < RAM_SIZE) return rd32le(ram_.data() + addr);
	if (in(addr, FLASH_BASE, FLASH_SIZE - 3)) return rd32le(flash_.data() + (addr - FLASH_BASE));
	if (in(addr, SYS_SRAM_BASE, SYS_SRAM_SIZE - 3)) return rd32le(sys_sram_.data() + (addr - SYS_SRAM_BASE));
	if (in(addr, MODEM_RAM_BASE, MODEM_RAM_SIZE - 3)) return rd32le(modem_ram_.data() + (addr - MODEM_RAM_BASE));
	return mmio_read(addr, 4);
}

void Machine::write8(uint32_t addr, uint8_t value)
{
	if (addr < RAM_SIZE) { ram_[addr] = value; return; }
	if (in(addr, FLASH_BASE, FLASH_SIZE)) { flash_[addr - FLASH_BASE] = value; return; }
	if (in(addr, SYS_SRAM_BASE, SYS_SRAM_SIZE)) { sys_sram_[addr - SYS_SRAM_BASE] = value; speaker_pcm_byte(addr - SYS_SRAM_BASE, value); return; }
	if (in(addr, MODEM_RAM_BASE, MODEM_RAM_SIZE)) { modem_ram_[addr - MODEM_RAM_BASE] = value; return; }
	mmio_write(addr, value, 1);
}

void Machine::write16(uint32_t addr, uint16_t value)
{
	if (addr + 1 < RAM_SIZE) { wr16le(ram_.data() + addr, value); return; }
	if (in(addr, FLASH_BASE, FLASH_SIZE - 1)) { wr16le(flash_.data() + (addr - FLASH_BASE), value); return; }
	if (in(addr, SYS_SRAM_BASE, SYS_SRAM_SIZE - 1)) {
		wr16le(sys_sram_.data() + (addr - SYS_SRAM_BASE), value);
		speaker_pcm_byte(addr - SYS_SRAM_BASE, (uint8_t)value);
		speaker_pcm_byte(addr - SYS_SRAM_BASE + 1u, (uint8_t)(value >> 8));
		return;
	}
	if (in(addr, MODEM_RAM_BASE, MODEM_RAM_SIZE - 1)) { wr16le(modem_ram_.data() + (addr - MODEM_RAM_BASE), value); return; }
	mmio_write(addr, value, 2);
}

void Machine::write32(uint32_t addr, uint32_t value)
{
	if (addr + 3 < RAM_SIZE) { wr32le(ram_.data() + addr, value); return; }
	if (in(addr, FLASH_BASE, FLASH_SIZE - 3)) { wr32le(flash_.data() + (addr - FLASH_BASE), value); return; }
	if (in(addr, SYS_SRAM_BASE, SYS_SRAM_SIZE - 3)) {
		wr32le(sys_sram_.data() + (addr - SYS_SRAM_BASE), value);
		for (int i = 0; i < 4; i++) speaker_pcm_byte(addr - SYS_SRAM_BASE + (uint32_t)i, (uint8_t)(value >> (8 * i)));
		return;
	}
	if (in(addr, MODEM_RAM_BASE, MODEM_RAM_SIZE - 3)) { wr32le(modem_ram_.data() + (addr - MODEM_RAM_BASE), value); return; }
	mmio_write(addr, value, 4);
}

void Machine::log_unknown(uint32_t addr, bool write)
{
	if (!verbose_) return;
	if (logged_mmio_.size() > 200) return;
	if (logged_mmio_.insert(addr).second) {
		std::fprintf(stderr, "[mmio] %s unknown 0x%08x\n", write ? "write" : "read ", addr);
	}
}

bool Machine::set_key(const std::string &name, bool pressed, std::string &err)
{
	struct KeyDef { const char *name; int row; int col; };
	static const KeyDef keys[] = {
		{"ok", 0, 0}, {"green", 0, 0}, {"menu", 0, 0},
		{"3", 0, 1}, {"2", 0, 2}, {"1", 0, 3},
		{"left", 1, 0}, {"6", 1, 1}, {"5", 1, 2}, {"4", 1, 3},
		{"right", 2, 0}, {"9", 2, 1}, {"8", 2, 2}, {"7", 2, 3},
		{"back", 3, 0}, {"red", 3, 0}, {"#", 3, 1}, {"0", 3, 2}, {"*", 3, 3},
		{"up", 4, 0}, {"down", 4, 1},
	};
	for (const auto &k : keys) {
		if (name == k.name) {
			keys_[k.row][k.col] = pressed;
			return true;
		}
	}
	if (name == "sk1" || name == "side1") {
		side_key1_ = pressed;
		return true;
	}
	if (name == "sk2" || name == "side2") {
		side_key2_ = pressed;
		return true;
	}
	if (name == "ptt") {
		gpio_set_input(0x2B, !pressed);
		return true;
	}
	err = "unknown key: " + name;
	return false;
}

uint32_t Machine::gpio_ext(int bank) const
{
	const Gpio &g = gpio_[bank];
	uint32_t v = 0;
	for (int i = 0; i < 32; i++) {
		const uint32_t bit = 1u << i;
		const bool out = (g.ddr & bit) != 0;
		if (out ? (g.dr & bit) : (g.input & bit)) v |= bit;
	}
	if (bank == 0) {
		const bool data_out = (g.ddr & (1u << 8)) != 0;
		if (!data_out && fd_.cs && fd_.reading) {
			if (fd_.read_bit) v |= 1u << 8;
			else v &= ~(1u << 8);
		}
	}
	if (bank == 1) {
		// Keypad columns are GPIO B 17..20 (pin index 0x31..0x34).
		for (int c = 0; c < 4; c++) {
			bool low = false;
			for (int r = 0; r < 5; r++) {
				const int row_pin = 0x47 + r; // GPIOC 7..11
				const uint32_t row_bit = 1u << ((row_pin - 64) & 31);
				const bool row_out = (gpio_[2].ddr & row_bit) != 0;
				const bool row_low = row_out && (gpio_[2].dr & row_bit) == 0;
				if (row_low && keys_[r][c]) low = true;
			}
			if (c == 2 && side_key1_) low = true;
			if (c == 3 && side_key2_) low = true;
			if (low) v &= ~(1u << (0x31 + c - 32));
		}
	}
	return v;
}

void Machine::gpio_set_input(int pin, bool level)
{
	if (pin < 0 || pin >= 96) return;
	const int bank = pin / 32;
	const uint32_t bit = 1u << (pin & 31);
	Gpio &g = gpio_[bank];
	const uint32_t old = g.input;
	if (level) g.input |= bit;
	else g.input &= ~bit;
	if (g.input == old) return;

	// Edge detection for enabled pins. The firmware uses level interrupts, but
	// keys and the encoder are edge-driven in a few places.
	const uint32_t changed = g.input ^ old;
	if ((g.inten & changed) == 0) return;
	const bool rising = (g.input & changed) != 0;
	const bool want = g.polarity & changed ? rising : !rising;
	if (g.inttype & changed) {
		// edge
		if (want || (g.bothedge & changed)) {
			g.intstatus |= changed;
			pic_raise(bank == 0 ? 15 : bank == 1 ? 14 : 17);
		}
	} else {
		if (want) {
			g.intstatus |= changed;
			pic_raise(bank == 0 ? 15 : bank == 1 ? 14 : 17);
		}
	}
}

uint16_t Machine::fd_read_reg(uint8_t reg) const
{
	switch (reg) {
	case 0x0C: return 0x0200;		// synthesizer lock
	case 0x67: return fd_.signal ? 0x01B8 : 0x00A0;
	case 0x65: return fd_.signal ? 0x0008 : 0x0050;
	case 0x30: return fd_.state;
	case 0x38: return (uint16_t)(fd_.freq_10hz & 0xFFFFu);
	case 0x39: return (uint16_t)(fd_.freq_10hz >> 16);
	case 0x43: return fd_.bandwidth;
	default: return fd_.regs[reg & 0x7Fu];
	}
}

void Machine::fd_write_reg(uint8_t reg, uint16_t value)
{
	fd_.regs[reg & 0x7Fu] = value;
	switch (reg) {
	case 0x30: fd_.state = value; break;
	case 0x43: fd_.bandwidth = value; break;
	case 0x38: fd_.freq_10hz = (fd_.freq_10hz & 0xFFFF0000u) | value; break;
	case 0x39: fd_.freq_10hz = (fd_.freq_10hz & 0xFFFFu) | ((uint32_t)value << 16); break;
	default: break;
	}
}

void Machine::fd_pins()
{
	const Gpio &g = gpio_[0];
	const bool cs = !(((g.ddr >> 0x18) & 1u) && ((g.dr >> 0x18) & 1u));
	const bool clk = ((g.ddr >> 7) & 1u) && ((g.dr >> 7) & 1u);
	const bool data = ((g.ddr >> 8) & 1u) && ((g.dr >> 8) & 1u);
	if (cs != fd_.cs) {
		fd_.cs = cs;
		if (cs) {
			fd_.bits = 0;
			fd_.addr = 0;
			fd_.value = 0;
			fd_.reading = false;
			fd_.write_frame = false;
		} else {
			fd_.reading = false;
		}
	}
	if (!fd_.cs) {
		fd_.clk = clk;
		return;
	}
	if (clk && !fd_.clk) {
		if (fd_.write_frame) {
			fd_.value = (uint16_t)((fd_.value << 1) | (data ? 1u : 0u));
			if (++fd_.bits == 16) {
				fd_write_reg(fd_.addr, fd_.value);
				fd_.bits = 0;
				fd_.write_frame = false;
			}
		} else if (fd_.reading) {
			if (fd_.read_pos < 16) {
				fd_.read_bit = ((fd_.read_value >> (15 - fd_.read_pos)) & 1u) != 0;
				fd_.read_pos++;
			}
		} else {
			fd_.addr = (uint8_t)((fd_.addr << 1) | (data ? 1u : 0u));
			if (++fd_.bits == 8) {
				fd_.bits = 0;
				if (fd_.addr & 0x80u) {
					fd_.reading = true;
					fd_.read_value = fd_read_reg((uint8_t)(fd_.addr & 0x7Fu));
					fd_.read_pos = 0;
					fd_.read_bit = false;
				} else {
					fd_.write_frame = true;
					fd_.value = 0;
				}
			}
		}
	}
	fd_.clk = clk;
}

void Machine::gpio_apply_outputs(int bank, uint32_t old_dr)
{
	(void)old_dr;
	if (bank == 1) {
		const Gpio &g = gpio_[1];
		const uint32_t bit = 1u << (0x2Cu - 32u);
		const bool on = ((g.ddr & bit) != 0) && ((g.dr & bit) == 0);
		if (on != gps_.power_on) {
			gps_.power_on = on;
			gps_.next_apb = on ? apb_cycles_ + 84000000ull : 0;
		}
		return;
	}
	if (bank != 0) return;
	const Gpio &g = gpio_[0];
	// PTA18 is the data-flash chip select, low active.
	const bool cs = ((g.ddr >> 0x12) & 1u) && ((g.dr >> 0x12) & 1u);
	nor_cs(!cs);
	fd_pins();
}

uint32_t Machine::gpio_read(int bank, uint32_t off, uint32_t size)
{
	(void)size;
	Gpio &g = gpio_[bank];
	switch (off) {
	case 0x00: return g.dr;
	case 0x04: return g.ddr;
	case 0x30: return g.inten;
	case 0x34: return g.intmask;
	case 0x38: return g.inttype;
	case 0x3C: return g.polarity;
	case 0x40: return g.intstatus;
	case 0x44: return g.intstatus;
	case 0x48: return g.bothedge;
	case 0x4C: return g.eoi;
	case 0x50: return gpio_ext(bank);
	case 0x78: return g.bothedge;
	default: return 0;
	}
}

void Machine::gpio_write(int bank, uint32_t off, uint32_t value, uint32_t size)
{
	(void)size;
	Gpio &g = gpio_[bank];
	switch (off) {
	case 0x00: {
		const uint32_t old = g.dr;
		g.dr = value;
		gpio_apply_outputs(bank, old);
		break;
	}
	case 0x04: {
		const uint32_t old = g.dr;
		g.ddr = value;
		gpio_apply_outputs(bank, old);
		break;
	}
	case 0x30: g.inten = value; break;
	case 0x34: g.intmask = value; break;
	case 0x38: g.inttype = value; break;
	case 0x3C: g.polarity = value; break;
	case 0x40:
	case 0x44: g.intstatus &= ~value; break;
	case 0x48: g.bothedge = value; break;
	case 0x4C: {
		g.intstatus &= ~value;
		g.eoi = value;
		const int source = bank == 0 ? 15 : bank == 1 ? 14 : 17;
		if (g.intstatus == 0) pic_lower(source);
		break;
	}
	default: break;
	}
}

void Machine::gps_sentence(const char *body)
{
	uint8_t checksum = 0;
	for (const char *p = body; *p != '\0'; p++) checksum ^= (uint8_t)*p;
	char line[160];
	std::snprintf(line, sizeof line, "$%s*%02X\r\n", body, checksum);
	for (const char *p = line; *p != '\0'; p++) feed_uart(1, (uint8_t)*p);
}

void Machine::gps_tick()
{
	if (!gps_.power_on) {
		gps_.next_apb = 0;
		return;
	}
	if (gps_.next_apb == 0) gps_.next_apb = apb_cycles_ + 84000000ull;
	if (apb_cycles_ < gps_.next_apb) return;

	const GpsFix &f = gps_.fix;
	std::time_t now = std::time(nullptr);
	std::tm tm{};
	gmtime_r(&now, &tm);
	char utc[16], date[16];
	std::snprintf(utc, sizeof utc, "%02d%02d%02d.00", tm.tm_hour, tm.tm_min, tm.tm_sec);
	std::snprintf(date, sizeof date, "%02d%02d%02d", tm.tm_mday, tm.tm_mon + 1, (tm.tm_year + 1900) % 100);

	char rmc[128];
	if (f.valid) {
		const bool south = f.latitude < 0, west = f.longitude < 0;
		const double alat = std::fabs(f.latitude), alon = std::fabs(f.longitude);
		const int lat_deg = (int)alat, lon_deg = (int)alon;
		const double lat_min = (alat - lat_deg) * 60.0, lon_min = (alon - lon_deg) * 60.0;
		std::snprintf(rmc, sizeof rmc, "GPRMC,%s,A,%02d%07.4f,%c,%03d%07.4f,%c,%.1f,%.1f,%s,0.0,E,A",
			      utc, lat_deg, lat_min, south ? 'S' : 'N', lon_deg, lon_min, west ? 'W' : 'E',
			      f.speed_mps * 1.943844, f.course_deg, date);
	} else {
		std::snprintf(rmc, sizeof rmc, "GPRMC,%s,V,,,,,,,0.0,0.0,%s,0.0,E,N", utc, date);
	}
	gps_sentence(rmc);

	char gga[128];
	if (f.valid) {
		const bool south = f.latitude < 0, west = f.longitude < 0;
		const double alat = std::fabs(f.latitude), alon = std::fabs(f.longitude);
		const int lat_deg = (int)alat, lon_deg = (int)alon;
		const double lat_min = (alat - lat_deg) * 60.0, lon_min = (alon - lon_deg) * 60.0;
		std::snprintf(gga, sizeof gga, "GPGGA,%s,%02d%07.4f,%c,%03d%07.4f,%c,1,%02u,1.0,%.1f,M,0.0,M,,",
			      utc, lat_deg, lat_min, south ? 'S' : 'N', lon_deg, lon_min, west ? 'W' : 'E',
			      (unsigned)f.satellites, f.altitude_m);
	} else {
		std::snprintf(gga, sizeof gga, "GPGGA,%s,,,,,0,00,99.9,,,,,", utc);
	}
	gps_sentence(gga);

	gps_.next_apb += 84000000ull;
	if (gps_.next_apb <= apb_cycles_) gps_.next_apb = apb_cycles_ + 84000000ull;
}

uint32_t Machine::timer_value(int n) const
{
	const Timer &t = timers_[n];
	if (!(t.ctrl & 1u)) return t.load;
	const uint64_t period = (uint64_t)t.load + 1u;
	const uint64_t elapsed = apb_cycles_ - t.start_apb;
	const uint32_t rem = (uint32_t)(elapsed % period);
	return t.load - rem;
}

void Machine::tick(uint64_t cycles)
{
	apb_cycles_ = cycles / 2; // CPU/AHB 168 MHz, APB 84 MHz
	rf_sample_time_ = cycles / 700u; // 240 kHz RF sample timeline
	gps_tick();
	for (int i = 0; i < 6; i++) {
		Timer &t = timers_[i];
		if (!(t.ctrl & 1u)) continue;
		const uint64_t period = (uint64_t)t.load + 1u;
		const uint64_t elapsed = apb_cycles_ - t.start_apb;
		const uint64_t periods = elapsed / period;
		if (periods <= t.periods_done) continue;
		t.periods_done = periods;
		if (!(t.ctrl & 4u)) { // interrupt not masked
			if (!t.irq) {
				t.irq = true;
				pic_raise(i + 1);
			}
		}
	}
	if (iq_enabled_ && iq_channel_) {
		std::vector<IqSample> iq;
		while (iq_channel_->receive(iq_id_, iq)) {
			fd_.signal = true;
			fd_.signal_apb = apb_cycles_;
			if (air_) air_->receive_iq(iq.data(), iq.size());
			if (packet_) packet_->receive_iq(iq.data(), iq.size());
			if (fm_ && fd_.bandwidth != 0x4048u) fm_->receive_iq(iq.data(), iq.size());
		}
		if (fd_.signal && apb_cycles_ - fd_.signal_apb > 8400000ull) fd_.signal = false;
	}
	modem_.tick(apb_cycles_);
	if (modem_.pcm_mic_enabled()) {
		if (pcm_next_apb_ == 0) pcm_next_apb_ = apb_cycles_ + 840000ull;
		if (apb_cycles_ >= pcm_next_apb_) {
			fill_microphone();
			pcm_next_apb_ += 840000ull;
			if (pcm_next_apb_ <= apb_cycles_) pcm_next_apb_ = apb_cycles_ + 840000ull;
		}
	} else {
		pcm_next_apb_ = 0;
	}
	if (fm_ && (fd_.state == 0xC1FEu || modem_.fm_tx_active()) &&
	    (fd_.bandwidth == 0x3028u || fd_.bandwidth == 0x4008u)) {
		if (fm_tx_next_apb_ == 0) fm_tx_next_apb_ = apb_cycles_;
		int loops = 0;
		while (apb_cycles_ >= fm_tx_next_apb_ && loops++ < 64) {
			int16_t samples[80];
			for (int i = 0; i < 80; i++) {
				if (!mic_fifo_.empty()) {
					samples[i] = mic_fifo_.front();
					mic_fifo_.pop_front();
				} else {
					samples[i] = (int16_t)(8000.0 * std::sin(2.0 * M_PI * 1000.0 * pcm_phase_ / 8000.0));
					pcm_phase_ = (pcm_phase_ + 1u) % 8000u;
				}
			}
			fm_->modulate(samples, 80);
			fm_tx_next_apb_ += 840000ull;
		}
	} else {
		fm_tx_next_apb_ = 0;
	}
}

void Machine::update_irq()
{
	irq_asserted_ = ((pic_.level_lo & ~pic_.mask) | (pic_.level_hi & ~pic_.mask1)) != 0;
}

uint64_t Machine::cpu_cycles_to_next_timer(uint64_t cycles) const
{
	if (irq_asserted_) return 0;
	const uint64_t apb = cycles / 2;
	uint64_t best = 0;
	for (int i = 0; i < 6; i++) {
		const Timer &t = timers_[i];
		if (!(t.ctrl & 1u) || (t.ctrl & 4u) || t.irq) continue;
		const uint64_t period = (uint64_t)t.load + 1u;
		if (!(t.ctrl & 2u) && t.periods_done >= 1) continue;
		const uint64_t next = t.start_apb + (t.periods_done + 1u) * period;
		if (next <= apb) return 0;
		if (best == 0 || next < best) best = next;
	}
	if (best == 0) return 0;
	return best * 2u;
}

void Machine::pic_raise(int source)
{
	if (source < 0 || source >= 64) return;
	if (source < 32) pic_.level_lo |= 1u << source;
	else pic_.level_hi |= 1u << (source - 32);
	if (source < 32) pic_.latched_lo |= 1u << source;
	else pic_.latched_hi |= 1u << (source - 32);
	update_irq();
}

void Machine::pic_lower(int source)
{
	if (source < 0 || source >= 64) return;
	if (source < 32) pic_.level_lo &= ~(1u << source);
	else pic_.level_hi &= ~(1u << (source - 32));
	update_irq();
}

int Machine::irq_source() const
{
	const uint32_t lo = (pic_.level_lo) & ~pic_.mask;
	const uint32_t hi = (pic_.level_hi) & ~pic_.mask1;
	if (lo) return __builtin_ctz(lo);
	if (hi) return 32 + __builtin_ctz(hi);
	return -1;
}

void Machine::clear_irq(int source) { pic_lower(source); }

void Machine::dump_uart(std::FILE *out) const
{
	if (!uart_out_.empty()) {
		std::fwrite(uart_out_.data(), 1, uart_out_.size(), out);
		std::fflush(out);
	}
}

uint32_t Machine::uart_read(int idx, uint32_t off, uint32_t size)
{
	(void)size;
	Uart &u = uarts_[idx];
	switch (off) {
	case UART_RBR:
		if (u.lcr & 0x80u) return u.dll;
		if (u.rx_head != u.rx_tail) {
			const uint8_t b = u.rx[u.rx_tail];
			u.rx_tail = (u.rx_tail + 1) % u.rx.size();
			if (u.rx_head == u.rx_tail) pic_lower(u.irq_source);
			return b;
		}
		return 0;
	case UART_IER:
		return (u.lcr & 0x80u) ? u.dlh : u.ier;
	case UART_FCR:
		if (u.rx_head != u.rx_tail && (u.ier & 1u)) return 0xC1;
		return 0x01;
	case UART_LCR:
		return u.lcr;
	case UART_LSR: {
		uint32_t v = 0x60; // THR empty, transmitter empty
		if (u.rx_head != u.rx_tail) v |= 0x01;
		return v;
	}
	case UART_USR:
		return 0x02;
	default:
		return 0;
	}
}

void Machine::uart_write(int idx, uint32_t off, uint32_t value, uint32_t size)
{
	(void)size;
	Uart &u = uarts_[idx];
	switch (off) {
	case UART_RBR:
		if (u.lcr & 0x80u) {
			u.dll = value & 0xFFu;
		} else if (u.host) {
			const uint8_t b = (uint8_t)value;
			uart_out_.push_back(b);
			if (uart_tx_sink_) uart_tx_sink_(b);
			if (uart_echo_) {
				std::fputc(b, stderr);
				std::fflush(stderr);
			}
			if (trace_uart_) {
				std::fprintf(stderr, "[uart0] %02x\n", b);
			}
		}
		break;
	case UART_IER:
		if (u.lcr & 0x80u) u.dlh = value & 0xFFu;
		else u.ier = value & 0xFFu;
		break;
	case UART_FCR:
		u.fcr = value & 0xFFu;
		if (value & 0x02u) u.rx_head = u.rx_tail = 0;
		break;
	case UART_LCR:
		u.lcr = value & 0xFFu;
		break;
	default:
		break;
	}
}

uint8_t Machine::peek8(uint32_t addr) const
{
	if (addr < RAM_SIZE) return ram_[addr];
	if (in(addr, FLASH_BASE, FLASH_SIZE)) return flash_[addr - FLASH_BASE];
	if (in(addr, SYS_SRAM_BASE, SYS_SRAM_SIZE)) return sys_sram_[addr - SYS_SRAM_BASE];
	if (in(addr, MODEM_RAM_BASE, MODEM_RAM_SIZE)) return modem_ram_[addr - MODEM_RAM_BASE];
	return 0;
}

void Machine::uart_push_rx(int idx, uint8_t b)
{
	Uart &u = uarts_[idx];
	const uint32_t next = (u.rx_head + 1) % u.rx.size();
	if (next == u.rx_tail) return;
	u.rx[u.rx_head] = b;
	u.rx_head = next;
	if (u.ier & 1u) pic_raise(u.irq_source);
}

void Machine::nor_cs(bool selected)
{
	if (selected == !nor_.cs) return;
	nor_.cs = !selected;
	if (selected) {
		nor_.pos = 0;
		nor_.addr = 0;
		nor_.cmd = 0;
		nor_.reading = false;
	}
}

uint8_t Machine::nor_transfer(uint8_t out)
{
	if (nor_.cs) return 0xFF;

	if (nor_.pos == 0) {
		nor_.cmd = out;
		nor_.pos = 1;
		nor_.addr = 0;
		if (out == 0x06) nor_.wel = true;
		if (verbose_) std::fprintf(stderr, "[nor] cmd %02x\n", out);
		return 0xFF;
	}

	const uint32_t step = nor_.pos++;

	switch (nor_.cmd) {
	case 0x03: // read
	case 0x0B: // fast read
		if (step <= 3) {
			nor_.addr = (nor_.addr << 8) | out;
			return 0xFF;
		}
		if (nor_.cmd == 0x0B && step == 4) return 0xFF;
		if (nor_.addr < data_flash_.size()) return data_flash_[nor_.addr++];
		return 0xFF;
	case 0x02: // page program
		if (step <= 3) {
			nor_.addr = (nor_.addr << 8) | out;
			return 0xFF;
		}
		if (nor_.addr < data_flash_.size()) data_flash_[nor_.addr++] = out;
		return 0xFF;
	case 0x20: // 4 KB sector erase
	case 0x52: // 32 KB block erase
	case 0xD8: // 64 KB block erase
		if (step <= 3) {
			nor_.addr = (nor_.addr << 8) | out;
			if (step == 3) {
				uint32_t len = nor_.cmd == 0x20 ? 0x1000u : nor_.cmd == 0x52 ? 0x8000u : 0x10000u;
				nor_.addr &= ~(len - 1u);
				for (uint32_t i = 0; i < len && nor_.addr + i < data_flash_.size(); i++) {
					data_flash_[nor_.addr + i] = 0xFF;
				}
			}
		}
		return 0xFF;
	case 0xC7:
	case 0x60:
		std::fill(data_flash_.begin(), data_flash_.end(), 0xFF);
		return 0xFF;
	case 0x9F: { // JEDEC
		static const uint8_t id[3] = {0xC8, 0x40, 0x18};
		return step <= 3 ? id[step - 1] : 0xFF;
	}
	case 0x4B: { // unique ID
		static const uint8_t uid[16] = {
			0x44, 0x4D, 0x33, 0x32, 0x45, 0x4D, 0x55, 0x00,
			0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		};
		if (step <= 4) return 0xFF;
		return step - 5 < 16 ? uid[step - 5] : 0xFF;
	}
	case 0x05:
		return (nor_.wel ? 0x02 : 0x00);
	case 0x35:
	case 0x15:
	case 0x01:
	case 0x31:
	case 0x11:
		return 0x00;
	case 0x48: // read security register
		if (step <= 4) return 0xFF;
		return 0xFF;
	case 0xAB:
	case 0xB9:
		return 0xFF;
	default:
		return 0xFF;
	}
}

uint32_t Machine::ssi_read(int idx, uint32_t off, uint32_t size)
{
	(void)size;
	Ssi &s = ssi_[idx];
	switch (off) {
	case 0x00: return s.ctrlr0;
	case 0x04: return s.ctrlr1;
	case 0x08: return s.ssi_enr;
	case 0x10: return s.ser;
	case 0x14: return s.baudr;
	case 0x18: return s.txftlr;
	case 0x1C: return s.rxftlr;
	case 0x20: return 0;
	case 0x24: return (s.rx_head + s.rx.size() - s.rx_tail) % s.rx.size();
	case 0x28: return 0x06; // TX FIFO not full and empty, not busy
	case 0x2C: return s.imr;
	case 0x30: return 0;
	case 0x4C: return s.dmacr;
	case 0x60: {
		if (s.rx_head == s.rx_tail) return 0xFF;
		const uint8_t b = s.rx[s.rx_tail];
		s.rx_tail = (s.rx_tail + 1) % s.rx.size();
		return b;
	}
	default: return 0;
	}
}

void Machine::ssi_write(int idx, uint32_t off, uint32_t value, uint32_t size)
{
	(void)size;
	Ssi &s = ssi_[idx];
	switch (off) {
	case 0x00: s.ctrlr0 = value; break;
	case 0x04: s.ctrlr1 = value; break;
	case 0x08: s.ssi_enr = value; break;
	case 0x10: s.ser = value; break;
	case 0x14: s.baudr = value; break;
	case 0x18: s.txftlr = value; break;
	case 0x1C: s.rxftlr = value; break;
	case 0x2C: s.imr = value; break;
	case 0x4C: s.dmacr = value; break;
	case 0x60: {
		const uint8_t rx = idx == 0 ? nor_transfer((uint8_t)value) : 0xFF;
		const uint32_t next = (s.rx_head + 1) % s.rx.size();
		if (next != s.rx_tail) {
			s.rx[s.rx_head] = rx;
			s.rx_head = next;
		}
		break;
	}
	default:
		break;
	}
}

void Machine::lcd_command(uint8_t cmd)
{
	lcd_.cmd = cmd;
	lcd_.param_count = 0;
	switch (cmd) {
	case 0x2A: lcd_.param_count = 0; break;
	case 0x2B: lcd_.param_count = 0; break;
	case 0x2C:
		lcd_.x = lcd_.x0;
		lcd_.y = lcd_.y0;
		lcd_.written = 0;
		lcd_.pixel_high = true;
		break;
	case 0x21: lcd_.invert = true; break;
	case 0x20: lcd_.invert = false; break;
	case 0x29: lcd_.display_on = true; break;
	case 0x28: lcd_.display_on = false; break;
	default: break;
	}
}

void Machine::lcd_data(uint8_t data)
{
	if (lcd_.cmd == 0x2A || lcd_.cmd == 0x2B) {
		if (lcd_.param_count < 4) lcd_.params[lcd_.param_count++] = data;
		if (lcd_.param_count == 4) {
			const uint32_t a = ((uint32_t)lcd_.params[0] << 8) | lcd_.params[1];
			const uint32_t b = ((uint32_t)lcd_.params[2] << 8) | lcd_.params[3];
			if (lcd_.cmd == 0x2A) {
				lcd_.x0 = std::min(a, LCD_W - 1);
				lcd_.x1 = std::min(b, LCD_W - 1);
			} else {
				lcd_.y0 = std::min(a, LCD_H - 1);
				lcd_.y1 = std::min(b, LCD_H - 1);
			}
			lcd_.param_count = 0;
		}
		return;
	}

	if (lcd_.cmd == 0x2C) {
		if (lcd_.pixel_high) {
			lcd_.pixel_byte = data;
			lcd_.pixel_high = false;
		} else {
			const uint16_t color = (uint16_t)((lcd_.pixel_byte << 8) | data);
			lcd_.pixel_high = true;
			if (lcd_.fb.size() == LCD_W * LCD_H && lcd_.x < LCD_W && lcd_.y < LCD_H) {
				lcd_.fb[lcd_.y * LCD_W + lcd_.x] = color;
				lcd_.written++;
				lcd_generation_++;
			}
			lcd_.x++;
			if (lcd_.x > lcd_.x1) {
				lcd_.x = lcd_.x0;
				lcd_.y++;
				if (lcd_.y > lcd_.y1) {
					lcd_.y = lcd_.y0;
					if (lcd_.written >= 200u * 300u) {
						frame_count_++;
						lcd_.written = 0;
					}
				}
			}
		}
		return;
	}

	if (lcd_.param_count < sizeof(lcd_.params)) {
		lcd_.params[lcd_.param_count++] = data;
	}
}

uint32_t Machine::lcd_read(uint32_t off)
{
	switch (off) {
	case 0x28: return lcd_.en | 0x02u;
	default: return 0;
	}
}

void Machine::lcd_write(uint32_t off, uint32_t value)
{
	if (verbose_ && off == 0x00) std::fprintf(stderr, "[lcd] cmd %02x\n", (unsigned)(value & 0xFF));
	switch (off) {
	case 0x00: lcd_command((uint8_t)value); break;
	case 0x04: lcd_data((uint8_t)value); break;
	case 0x10: lcd_.cfg = value; break;
	case 0x28:
		lcd_.en = value;
		if (value & 1u) lcd_.en |= 2u;
		break;
	default:
		break;
	}
}

uint32_t Machine::mmio_read(uint32_t addr, uint32_t size)
{
	if (in(addr, SYS_BASE, 0x1000)) {
		const uint32_t off = addr - SYS_BASE;
		if (off >= 0x100u && off < 0x600u) return modem_.read(off, size);
		switch (off) {
		case 0x000: return sys_.soft_rstn;
		case 0x004: return sys_.clk_ctrl | 0xC0000000u; // PLL locked and clock switch done
		case 0x010: return sys_.clk_pll;
		case 0x024: return sys_.clk_div;
		case 0x028: return sys_.clk_usb_sdio;
		case 0x02C: return sys_.clk_gates;
		case 0x030: return sys_.lcsfc_baudr;
		case 0x034: return sys_.io_diplex0;
		case 0x038: return sys_.io_diplex1;
		case 0x03C: return sys_.io_diplex2;
		case 0x040: return sys_.iomgr_ren0;
		case 0x044: return sys_.iomgr_ren1;
		case 0x048: return sys_.iomgr_ren2;
		case 0x05C: return sys_.quad_enable;
		default: return 0;
		}
	}
	if (in(addr, CACHE_BASE, 0x1000)) {
		switch (addr - CACHE_BASE) {
		case 0x000: return cache_.ctrl;
		case 0x004: return cache_.inv;
		case 0x008: return cache_.region;
		default: return 0;
		}
	}
	if (in(addr, LCD_BASE, 0x1000)) return lcd_read(addr - LCD_BASE);
	if (in(addr, PIC_BASE, 0x1000)) {
		const uint32_t off = addr - PIC_BASE;
		switch (off) {
		case 0x00: return pic_.mode;
		case 0x04: return pic_.po;
		case 0x08: return pic_.mask;
		case 0x0C: return pic_.vector;
		case 0x10: return 0;
		case 0x44: return pic_.level_lo;
		case 0x48: return pic_.level_hi;
		case 0x60: return pic_.mode1;
		case 0x64: return pic_.po1;
		case 0x68: return pic_.mask1;
		default: return 0;
		}
	}
	if (in(addr, TIMER_BASE, 0x80)) {
		const uint32_t off = addr - TIMER_BASE;
		const int n = (int)(off / 0x14);
		const uint32_t reg = off % 0x14;
		if (n < 0 || n >= 6) return 0;
		switch (reg) {
		case 0x00: return timers_[n].load;
		case 0x04: return timer_value(n);
		case 0x08: return timers_[n].ctrl;
		case 0x0C:
			timers_[n].irq = false;
			pic_lower(n + 1);
			return 0;
		case 0x10: return timers_[n].irq ? 1u : 0u;
		default: return 0;
		}
	}
	if (in(addr, WDT_BASE, 0x1000)) {
		switch (addr - WDT_BASE) {
		case 0x00: return wdt_lock_;
		case 0x10: return wdt_en_;
		default: return wdt_lock_;
		}
	}
	if (in(addr, GPIOA_BASE, 0x1000)) return gpio_read(0, addr - GPIOA_BASE, size);
	if (in(addr, GPIOB_BASE, 0x1000)) return gpio_read(1, addr - GPIOB_BASE, size);
	if (in(addr, GPIOC_BASE, 0x1000)) return gpio_read(2, addr - GPIOC_BASE, size);
	if (in(addr, UART0_BASE, 0x1000)) return uart_read(0, addr - UART0_BASE, size);
	if (in(addr, UART1_BASE, 0x1000)) return uart_read(1, addr - UART1_BASE, size);
	if (in(addr, UART2_BASE, 0x1000)) return uart_read(2, addr - UART2_BASE, size);
	if (in(addr, UART3_BASE, 0x1000)) return uart_read(3, addr - UART3_BASE, size);
	if (in(addr, SPI0_BASE, 0x1000)) return ssi_read(0, addr - SPI0_BASE, size);
	if (in(addr, SPI1_BASE, 0x1000)) return ssi_read(1, addr - SPI1_BASE, size);
	if (in(addr, SPI2_BASE, 0x1000)) return ssi_read(2, addr - SPI2_BASE, size);
	if (in(addr, ADC_BASE, 0x1000)) {
		switch (addr - ADC_BASE) {
		case 0x00: return adc_ctrl_;
		case 0x0C: return 0x1001Cu; // channel C and D conversion complete
		case 0x14: return adc_start_;
		case 0x30: return adc_data_ab_;
		case 0x34: return adc_data_cd_;
		default: return 0;
		}
	}
	if (in(addr, DAC_BASE, 0x1000)) {
		switch (addr - DAC_BASE) {
		case 0x00: return dac_pd_ctrl_;
		case 0x04: return dac_pd_mode_;
		case 0x08: return dac_data_[0];
		case 0x0C: return dac_data_[1];
		case 0x10: return dac_data_[2];
		default: return 0;
		}
	}
	if (in(addr, PWM_BASE, 0x1000)) {
		switch (addr - PWM_BASE) {
		case 0x00: return pwm_cfg_;
		case 0x08: return pwm_per_;
		case 0x0C: return pwm_fp_;
		default: return 0;
		}
	}
	if (in(addr, I2C2_BASE, 0x1000)) {
		switch (addr - I2C2_BASE) {
		case 0x00: return i2c2_.con;
		case 0x04: return i2c2_.tar;
		case 0x10: {
			if (!i2c2_.rx_fifo.empty()) {
				const uint32_t v = i2c2_.rx_fifo.front();
				i2c2_.rx_fifo.erase(i2c2_.rx_fifo.begin());
				return v;
			}
			return i2c2_.data_cmd;
		}
		case 0x30: return i2c2_.intr_mask;
		case 0x38: return i2c2_.rx_tl;
		case 0x3C: return i2c2_.tx_tl;
		case 0x6C: return i2c2_.enable;
		case 0x70: return 0x04; // TX empty
		case 0x9C: return i2c2_.enable_status;
		default: return 0;
		}
	}
	if (in(addr, I2C0_BASE, 0x1000) || in(addr, I2C1_BASE, 0x1000)) return 0;
	if (in(addr, 0x04000000, 0x10000)) return 0;
	if (in(addr, 0x13000000, 0x40000)) return 0;
	if (in(addr, 0x15000000, 0x10000)) return 0;

	log_unknown(addr, false);
	return 0;
}

void Machine::mmio_write(uint32_t addr, uint32_t value, uint32_t size)
{
	if (in(addr, SYS_BASE, 0x1000)) {
		const uint32_t sys_off = addr - SYS_BASE;
		if (sys_off >= 0x100u && sys_off < 0x600u) {
			modem_.write(sys_off, value, size, apb_cycles_);
			return;
		}
		switch (addr - SYS_BASE) {
		case 0x000: sys_.soft_rstn = value; break;
		case 0x004: sys_.clk_ctrl = value & ~0xC0000000u; break;
		case 0x010: sys_.clk_pll = value; break;
		case 0x024: sys_.clk_div = value; break;
		case 0x028: sys_.clk_usb_sdio = value; break;
		case 0x02C: sys_.clk_gates = value; break;
		case 0x030: sys_.lcsfc_baudr = value; break;
		case 0x034: sys_.io_diplex0 = value; break;
		case 0x038: sys_.io_diplex1 = value; break;
		case 0x03C: sys_.io_diplex2 = value; break;
		case 0x040: sys_.iomgr_ren0 = value; break;
		case 0x044: sys_.iomgr_ren1 = value; break;
		case 0x048: sys_.iomgr_ren2 = value; break;
		case 0x05C: sys_.quad_enable = value; break;
		default: break;
		}
		return;
	}
	if (in(addr, CACHE_BASE, 0x1000)) {
		switch (addr - CACHE_BASE) {
		case 0x000: cache_.ctrl = value; break;
		case 0x004: cache_.inv = value; break;
		case 0x008: cache_.region = value; break;
		default: break;
		}
		return;
	}
	if (in(addr, LCD_BASE, 0x1000)) { lcd_write(addr - LCD_BASE, value); return; }
	if (in(addr, PIC_BASE, 0x1000)) {
		switch (addr - PIC_BASE) {
		case 0x00: pic_.mode = value; break;
		case 0x04: pic_.po = value; break;
		case 0x08: pic_.mask = value; update_irq(); break;
		case 0x0C: pic_.vector = value; break;
		case 0x10: break; // end of interrupt
		case 0x44: pic_.int_st &= ~value; pic_.latched_lo &= ~value; break;
		case 0x48: pic_.int_st1 &= ~value; pic_.latched_hi &= ~value; break;
		case 0x60: pic_.mode1 = value; break;
		case 0x64: pic_.po1 = value; break;
		case 0x68: pic_.mask1 = value; update_irq(); break;
		default: break;
		}
		return;
	}
	if (in(addr, TIMER_BASE, 0x80)) {
		const uint32_t off = addr - TIMER_BASE;
		const int n = (int)(off / 0x14);
		const uint32_t reg = off % 0x14;
		if (n < 0 || n >= 6) return;
		Timer &t = timers_[n];
		switch (reg) {
		case 0x00:
			t.load = value;
			t.start_apb = apb_cycles_;
			t.periods_done = 0;
			break;
		case 0x04: break;
		case 0x08: {
			const bool was = (t.ctrl & 1u) != 0;
			t.ctrl = value;
			const bool now = (value & 1u) != 0;
			if (now && !was) {
				t.start_apb = apb_cycles_;
				t.periods_done = 0;
			}
			if (!now) { t.irq = false; pic_lower(n + 1); }
			break;
		}
		case 0x0C:
			t.irq = false;
			pic_lower(n + 1);
			break;
		default: break;
		}
		return;
	}
	if (in(addr, WDT_BASE, 0x1000)) {
		switch (addr - WDT_BASE) {
		case 0x00: wdt_lock_ = value; break;
		case 0x10: wdt_en_ = value; break;
		default: break;
		}
		return;
	}
	if (in(addr, GPIOA_BASE, 0x1000)) { gpio_write(0, addr - GPIOA_BASE, value, size); return; }
	if (in(addr, GPIOB_BASE, 0x1000)) { gpio_write(1, addr - GPIOB_BASE, value, size); return; }
	if (in(addr, GPIOC_BASE, 0x1000)) { gpio_write(2, addr - GPIOC_BASE, value, size); return; }
	if (in(addr, UART0_BASE, 0x1000)) { uart_write(0, addr - UART0_BASE, value, size); return; }
	if (in(addr, UART1_BASE, 0x1000)) { uart_write(1, addr - UART1_BASE, value, size); return; }
	if (in(addr, UART2_BASE, 0x1000)) { uart_write(2, addr - UART2_BASE, value, size); return; }
	if (in(addr, UART3_BASE, 0x1000)) { uart_write(3, addr - UART3_BASE, value, size); return; }
	if (in(addr, SPI0_BASE, 0x1000)) { ssi_write(0, addr - SPI0_BASE, value, size); return; }
	if (in(addr, SPI1_BASE, 0x1000)) { ssi_write(1, addr - SPI1_BASE, value, size); return; }
	if (in(addr, SPI2_BASE, 0x1000)) { ssi_write(2, addr - SPI2_BASE, value, size); return; }
	if (in(addr, ADC_BASE, 0x1000)) {
		switch (addr - ADC_BASE) {
		case 0x00: adc_ctrl_ = value; break;
		case 0x14: adc_start_ = value; adc_data_cd_ = 672u | (512u << 16); break;
		default: break;
		}
		return;
	}
	if (in(addr, DAC_BASE, 0x1000)) {
		switch (addr - DAC_BASE) {
		case 0x00: dac_pd_ctrl_ = value; break;
		case 0x04: dac_pd_mode_ = value; break;
		case 0x08: dac_data_[0] = value; break;
		case 0x0C: dac_data_[1] = value; break;
		case 0x10: dac_data_[2] = value; break;
		default: break;
		}
		return;
	}
	if (in(addr, PWM_BASE, 0x1000)) {
		switch (addr - PWM_BASE) {
		case 0x00: pwm_cfg_ = value; break;
		case 0x08: pwm_per_ = value; break;
		case 0x0C: pwm_fp_ = value; break;
		default: break;
		}
		return;
	}
	if (in(addr, I2C2_BASE, 0x1000)) {
		switch (addr - I2C2_BASE) {
		case 0x00: i2c2_.con = value; break;
		case 0x04: i2c2_.tar = value; break;
		case 0x10: i2c2_.data_cmd = value; break;
		case 0x30: i2c2_.intr_mask = value; break;
		case 0x38: i2c2_.rx_tl = value; break;
		case 0x3C: i2c2_.tx_tl = value; break;
		case 0x40: i2c2_.rx_fifo.clear(); break; // clear interrupts
		case 0x6C: i2c2_.enable = value; i2c2_.enable_status = 0; break;
		case 0xA0: // non-standard START
			i2c2_.enable_status = 0;
			break;
		default: break;
		}
		return;
	}
	if (in(addr, I2C0_BASE, 0x1000) || in(addr, I2C1_BASE, 0x1000)) return;
	if (in(addr, 0x04000000, 0x10000)) return;
	if (in(addr, 0x13000000, 0x40000)) return;
	if (in(addr, 0x15000000, 0x10000)) return;

	log_unknown(addr, true);
}

bool Machine::save_framebuffer_ppm(const std::string &path, std::string &err) const
{
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) {
		err = "cannot create " + path;
		return false;
	}
	std::fprintf(f, "P6\n%u %u\n255\n", LCD_W, LCD_H);
	for (uint32_t y = 0; y < LCD_H; y++) {
		for (uint32_t x = 0; x < LCD_W; x++) {
			uint16_t c = lcd_.fb[y * LCD_W + x];
			uint8_t rgb[3] = {
				(uint8_t)(((c >> 11) & 0x1F) << 3),
				(uint8_t)(((c >> 5) & 0x3F) << 2),
				(uint8_t)((c & 0x1F) << 3),
			};
			std::fwrite(rgb, 1, 3, f);
		}
	}
	std::fclose(f);
	return true;
}

void Machine::print_state(std::FILE *out) const
{
	std::fprintf(out, "frames=%" PRIu64 " uart_bytes=%zu\n", frame_count_, uart_out_.size());
}
