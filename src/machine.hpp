// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include "elf.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

class Machine {
public:
	static constexpr uint32_t FLASH_BASE = 0x03000000;
	static constexpr uint32_t FLASH_SIZE = 0x00100000;
	static constexpr uint32_t APP_BASE = 0x0300C000;
	static constexpr uint32_t RAM_SIZE = 0x00058000;
	static constexpr uint32_t SYS_SRAM_BASE = 0x18000000;
	static constexpr uint32_t SYS_SRAM_SIZE = 0x00008000;
	static constexpr uint32_t MODEM_RAM_BASE = 0x16000000;
	static constexpr uint32_t MODEM_RAM_SIZE = 0x00010000;

	Machine();
	~Machine();

	void reset();
	bool load_elf_image(const ElfImage &image, std::string &err);
	bool load_bin(const std::vector<uint8_t> &image, uint32_t base, std::string &err);
	bool load_data_flash(const std::string &path, std::string &err);

	uint8_t read8(uint32_t addr);
	uint16_t read16(uint32_t addr);
	uint32_t read32(uint32_t addr);
	void write8(uint32_t addr, uint8_t value);
	void write16(uint32_t addr, uint16_t value);
	void write32(uint32_t addr, uint32_t value);

	void tick(uint64_t cycles);
	bool irq_pending() const;
	int irq_source() const;
	void clear_irq(int source);

	void set_uart_echo(bool on) { uart_echo_ = on; }
	void set_verbose(bool on) { verbose_ = on; }
	void set_trace_uart(bool on) { trace_uart_ = on; }
	bool set_key(const std::string &name, bool pressed, std::string &err);

	static constexpr uint32_t DISPLAY_WIDTH = 240;
	static constexpr uint32_t DISPLAY_HEIGHT = 320;

	uint64_t frame_count() const { return frame_count_; }
	uint64_t framebuffer_generation() const { return lcd_generation_; }
	const std::vector<uint16_t> &framebuffer() const { return lcd_.fb; }
	bool save_framebuffer_ppm(const std::string &path, std::string &err) const;
	void dump_uart(std::FILE *out) const;
	void print_state(std::FILE *out) const;

	// For the CPU interrupt entry.
	uint32_t read_vector(uint32_t vector) { return read32(vbr_ + vector * 4); }
	void set_vbr(uint32_t v) { vbr_ = v; }

private:
	std::vector<uint8_t> ram_;
	std::vector<uint8_t> flash_;
	std::vector<uint8_t> sys_sram_;
	std::vector<uint8_t> modem_ram_;
	std::vector<uint8_t> data_flash_;

	std::vector<uint8_t> uart_out_;
	bool uart_echo_ = true;
	bool verbose_ = false;
	bool trace_uart_ = false;
	std::unordered_set<uint32_t> logged_mmio_;

	uint32_t vbr_ = 0;

	// System / clock block (0x11000000).
	struct {
		uint32_t soft_rstn = 0;
		uint32_t clk_ctrl = 0;
		uint32_t clk_pll = 0;
		uint32_t clk_div = 0;
		uint32_t clk_usb_sdio = 0;
		uint32_t clk_gates = 0;
		uint32_t lcsfc_baudr = 4;
		uint32_t io_diplex0 = 0;
		uint32_t io_diplex1 = 0;
		uint32_t io_diplex2 = 0;
		uint32_t iomgr_ren0 = 0;
		uint32_t iomgr_ren1 = 0;
		uint32_t iomgr_ren2 = 0;
		uint32_t quad_enable = 0;
	} sys_;

	// Cache controller.
	struct {
		uint32_t ctrl = 0;
		uint32_t inv = 0;
		uint32_t region = 0;
	} cache_;

	// PIC (0x17000000).
	struct {
		uint32_t mode = 0, mode1 = 0;
		uint32_t po = 0xFFFFFFFFu, po1 = 0xFFFFFFFFu;
		uint32_t mask = 0xFFFFFFFFu, mask1 = 0xFFFFFFFFu;
		uint32_t vector = 0;
		uint32_t int_st = 0, int_st1 = 0;
		uint32_t level_lo = 0, level_hi = 0;
		uint32_t latched_lo = 0, latched_hi = 0;
	} pic_;

	// GPIO banks A, B, C.
	struct Gpio {
		uint32_t dr = 0, ddr = 0;
		uint32_t inten = 0, intmask = 0, inttype = 0, polarity = 0;
		uint32_t intstatus = 0, eoi = 0, ext = 0, bothedge = 0;
		uint32_t input = 0xFFFFFFFFu;
	};
	std::array<Gpio, 3> gpio_{};
	bool keys_[5][4]{};
	bool side_key1_ = false, side_key2_ = false;
	uint32_t gpio_ext(int bank) const;
	void gpio_apply_outputs(int bank, uint32_t old_dr);
	uint32_t gpio_read(int bank, uint32_t off, uint32_t size);
	void gpio_write(int bank, uint32_t off, uint32_t value, uint32_t size);
	void gpio_set_input(int pin, bool level);

	// DesignWare APB timers (0x14000000).
	struct Timer {
		uint32_t load = 0;
		uint32_t ctrl = 0x4;
		uint64_t start_apb = 0;
		uint64_t periods_done = 0;
		bool irq = false;
	};
	std::array<Timer, 6> timers_{};
	uint64_t apb_cycles_ = 0;
	uint32_t timer_value(int n) const;

	// UARTs.
	struct Uart {
		uint32_t base = 0;
		uint32_t ier = 0;
		uint32_t lcr = 0;
		uint32_t mcr = 0;
		uint32_t fcr = 0;
		uint32_t dll = 1, dlh = 0;
		uint32_t rx_head = 0, rx_tail = 0;
		std::array<uint8_t, 1024> rx{};
		uint32_t irq_source = 0;
		bool host = false;
	};
	std::array<Uart, 4> uarts_{};
	uint32_t uart_read(int idx, uint32_t off, uint32_t size);
	void uart_write(int idx, uint32_t off, uint32_t value, uint32_t size);
	void uart_push_rx(int idx, uint8_t b);

	// SPI NOR data flash on SPI0.
	struct SpiNor {
		bool cs = true;
		uint32_t pos = 0;
		uint32_t addr = 0;
		uint8_t cmd = 0;
		bool wel = false;
		bool reading = false;
		std::array<uint8_t, 8> header{};
	};
	SpiNor nor_;
	uint8_t nor_transfer(uint8_t out);
	void nor_cs(bool selected);

	// SSI master.
	struct Ssi {
		uint32_t ctrlr0 = 0, ctrlr1 = 0, ssi_enr = 0, ser = 0, baudr = 0;
		uint32_t txftlr = 0, rxftlr = 0, imr = 0, dmacr = 0;
		std::array<uint8_t, 256> rx{};
		uint32_t rx_head = 0, rx_tail = 0;
	};
	std::array<Ssi, 4> ssi_{};
	uint32_t ssi_read(int idx, uint32_t off, uint32_t size);
	void ssi_write(int idx, uint32_t off, uint32_t value, uint32_t size);

	// i8080 LCD controller + ST7789.
	struct Lcd {
		uint32_t cfg = 0;
		uint32_t en = 0;
		uint8_t cmd = 0;
		uint8_t params[8]{};
		uint32_t param_count = 0;
		uint32_t x0 = 0, x1 = 239, y0 = 0, y1 = 319;
		uint32_t x = 0, y = 0;
		bool pixel_high = true;
		uint8_t pixel_byte = 0;
		bool invert = false;
		bool display_on = false;
		uint32_t written = 0;
		std::vector<uint16_t> fb;
	};
	Lcd lcd_{};
	uint64_t frame_count_ = 0;
	uint64_t lcd_generation_ = 0;
	uint32_t lcd_read(uint32_t off);
	void lcd_write(uint32_t off, uint32_t value);
	void lcd_command(uint8_t cmd);
	void lcd_data(uint8_t data);
	void lcd_pixel(uint16_t color);

	// Watchdog, ADC, DAC, PWM and modem stubs.
	uint32_t wdt_lock_ = 0, wdt_en_ = 0;
	uint32_t adc_ctrl_ = 0, adc_start_ = 0, adc_data_ab_ = 0, adc_data_cd_ = 0;
	uint32_t dac_pd_ctrl_ = 0, dac_pd_mode_ = 0;
	std::array<uint32_t, 3> dac_data_{};
	uint32_t pwm_cfg_ = 0, pwm_per_ = 0, pwm_fp_ = 0;

	// I2C2 plus the simple RTC model.
	struct {
		uint32_t con = 0, tar = 0, enable = 0, intr_mask = 0;
		uint32_t scl_h = 0, scl_l = 0, tx_tl = 0, rx_tl = 0;
		uint32_t status = 0, enable_status = 1;
		uint32_t data_cmd = 0;
		std::vector<uint8_t> tx_fifo;
		std::vector<uint8_t> rx_fifo;
		uint8_t regs[0x20]{};
		bool read_pending = false;
	} i2c2_;

	uint32_t mmio_read(uint32_t addr, uint32_t size);
	void mmio_write(uint32_t addr, uint32_t value, uint32_t size);
	bool in_range(uint32_t addr, uint32_t size, uint32_t base, uint32_t len) const
	{
		return addr >= base && (uint64_t)addr + size <= (uint64_t)base + len;
	}
	void log_unknown(uint32_t addr, bool write);
	void pic_raise(int source);
	void pic_lower(int source);
};
