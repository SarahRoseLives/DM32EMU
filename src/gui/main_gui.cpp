// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "elf.hpp"
#include "emulator_thread.hpp"
#include "radio_widget.hpp"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTextCursor>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

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

	QString label(uint32_t pc) const
	{
		const ElfSymbol *s = find(pc);
		if (!s) return {};
		return QStringLiteral("%1+0x%2").arg(QString::fromStdString(s->name)).arg(pc - s->addr, 0, 16);
	}
};

QString hexBytes(const QByteArray &data)
{
	QString out;
	for (char c : data) {
		const unsigned b = (unsigned char)c;
		if (b == '\n') out += QStringLiteral("\\n");
		else if (b == '\r') out += QStringLiteral("\\r");
		else if (b == '\t') out += QStringLiteral("\\t");
		else if (b >= 32 && b < 127) out += QChar(b);
		else out += QStringLiteral("\\x%1").arg(b, 2, 16, QChar('0'));
	}
	return out;
}

QString hexDump(uint32_t addr, const QByteArray &data)
{
	QString out;
	for (int off = 0; off < data.size(); off += 16) {
		out += QStringLiteral("%1  ").arg(addr + off, 8, 16, QChar('0'));
		for (int j = 0; j < 16; j++) {
			if (off + j < data.size()) {
				out += QStringLiteral("%1 ").arg((unsigned char)data[off + j], 2, 16, QChar('0'));
			} else {
				out += QStringLiteral("   ");
			}
			if (j == 7) out += ' ';
		}
		out += QStringLiteral(" |");
		for (int j = 0; j < 16 && off + j < data.size(); j++) {
			const unsigned char b = (unsigned char)data[off + j];
			out += (b >= 32 && b < 127) ? QChar(b) : QChar('.');
		}
		out += QStringLiteral("|\n");
	}
	return out;
}

QString registersText(const CpuSnapshot &s, const SymTab &symtab)
{
	const QString label = symtab.label(s.pc);
	QString out;
	out += QStringLiteral("PC   %1  %2\n").arg(s.pc, 8, 16, QChar('0')).arg(label);
	out += QStringLiteral("PSR  %1   EPSR %2\n")
		       .arg(s.psr, 8, 16, QChar('0'))
		       .arg(s.epsr, 8, 16, QChar('0'));
	out += QStringLiteral("EPC  %1   VBR  %2\n")
		       .arg(s.epc, 8, 16, QChar('0'))
		       .arg(s.vbr, 8, 16, QChar('0'));
	out += QStringLiteral("CYC  %1\n\n").arg((qulonglong)s.cycles);
	for (int i = 0; i < 32; i += 4) {
		out += QStringLiteral("r%1 %2  r%3 %4  r%5 %6  r%7 %8\n")
			       .arg(i, 2, 10, QChar('0')).arg(s.r[i], 8, 16, QChar('0'))
			       .arg(i + 1, 2, 10, QChar('0')).arg(s.r[i + 1], 8, 16, QChar('0'))
			       .arg(i + 2, 2, 10, QChar('0')).arg(s.r[i + 2], 8, 16, QChar('0'))
			       .arg(i + 3, 2, 10, QChar('0')).arg(s.r[i + 3], 8, 16, QChar('0'));
	}
	return out;
}

QPlainTextEdit *makeTextView(QWidget *parent)
{
	auto *view = new QPlainTextEdit(parent);
	view->setReadOnly(true);
	view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	view->setLineWrapMode(QPlainTextEdit::NoWrap);
	return view;
}

} // namespace

class MainWindow : public QMainWindow {
public:
	explicit MainWindow(QWidget *parent = nullptr);

	void loadFirmware(const QString &path);
	void setDataFlash(const QString &path) { data_flash_ = path; }
	void setScreenshotMode(const QString &path, int frames);
	void setWindowScreenshotMode(const QString &path, int frames);

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	void createActions();
	void createDebugDock();
	void openFirmware();
	void openDataFlash();
	void saveScreenshot();
	void togglePause(bool paused);
	void resetEmulator();
	void onFrame(const QImage &image, quint64 frames);
	void onState(const CpuSnapshot &state);
	void onMemory(quint32 addr, const QByteArray &data);
	void onFault(const QString &text);
	void onBreakpointHit(quint32 pc);
	void onBridgeReady(const QString &path);
	void loadSymbolsFor(const QString &firmware);
	void appendUart(const QByteArray &data, bool radioToHost);
	void setPausedUi(bool paused);
	void refreshMemory();
	void addBreakpointFromInput();
	void removeSelectedBreakpoint();
	void clearBreakpointUi();
	void breakAtPc();
	void sendUartLine();
	void updatePcLabel();

	EmulatorThread emulator_;
	RadioWidget *radio_ = nullptr;
	QDockWidget *debug_dock_ = nullptr;
	QTabWidget *debug_tabs_ = nullptr;
	QPlainTextEdit *registers_view_ = nullptr;
	QLineEdit *mem_addr_ = nullptr;
	QSpinBox *mem_len_ = nullptr;
	QCheckBox *mem_follow_pc_ = nullptr;
	QPlainTextEdit *mem_view_ = nullptr;
	QListWidget *bp_list_ = nullptr;
	QLineEdit *bp_addr_ = nullptr;
	QListWidget *sym_list_ = nullptr;
	QLineEdit *sym_filter_ = nullptr;
	QLabel *bridge_label_ = nullptr;
	QPlainTextEdit *uart_log_ = nullptr;
	QLineEdit *uart_input_ = nullptr;
	QLabel *pc_label_ = nullptr;

	QString firmware_;
	QString data_flash_;
	QString symbols_path_;
	QImage last_frame_;
	QString screenshot_path_;
	QString window_screenshot_path_;
	int screenshot_frames_ = 3;
	quint64 frames_seen_ = 0;
	QAction *pause_action_ = nullptr;
	QAction *idle_skip_action_ = nullptr;
	QAction *step_action_ = nullptr;
	QAction *step_over_action_ = nullptr;
	QLabel *frame_label_ = nullptr;
	SymTab symtab_;
	CpuSnapshot current_state_{};
	bool have_state_ = false;
};

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
	setWindowTitle(QStringLiteral("DM32EMU"));
	setMinimumSize(900, 720);
	resize(1220, 940);

	radio_ = new RadioWidget(this);
	setCentralWidget(radio_);
	createDebugDock();
	createActions();

	statusBar()->showMessage(QStringLiteral("Open a firmware image to begin"));
	frame_label_ = new QLabel(QStringLiteral("Frame 0"), this);
	pc_label_ = new QLabel(QStringLiteral("PC -"), this);
	pc_label_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	statusBar()->addPermanentWidget(pc_label_);
	statusBar()->addPermanentWidget(frame_label_);

	connect(&emulator_, &EmulatorThread::frameReady, this, &MainWindow::onFrame);
	connect(&emulator_, &EmulatorThread::stateReady, this, &MainWindow::onState);
	connect(&emulator_, &EmulatorThread::memoryReady, this, &MainWindow::onMemory);
	connect(&emulator_, &EmulatorThread::breakpointHit, this, &MainWindow::onBreakpointHit);
	connect(&emulator_, &EmulatorThread::bridgeReady, this, &MainWindow::onBridgeReady);
	connect(&emulator_, &EmulatorThread::fault, this, &MainWindow::onFault);
	connect(&emulator_, &EmulatorThread::statusMessage, this,
		[this](const QString &text) { statusBar()->showMessage(text, 5000); });
	connect(&emulator_, &EmulatorThread::uartTx, this,
		[this](const QByteArray &data) { appendUart(data, true); });
	connect(&emulator_, &EmulatorThread::uartRx, this,
		[this](const QByteArray &data) { appendUart(data, false); });
	connect(radio_, &RadioWidget::keyChanged, &emulator_, &EmulatorThread::setKey);
}

void MainWindow::createActions()
{
	auto *open = new QAction(QStringLiteral("&Open firmware..."), this);
	open->setShortcut(QKeySequence::Open);
	connect(open, &QAction::triggered, this, &MainWindow::openFirmware);

	auto *openFlash = new QAction(QStringLiteral("Open &data flash..."), this);
	connect(openFlash, &QAction::triggered, this, &MainWindow::openDataFlash);

	auto *shot = new QAction(QStringLiteral("&Save screenshot..."), this);
	shot->setShortcut(QKeySequence::Save);
	connect(shot, &QAction::triggered, this, &MainWindow::saveScreenshot);

	auto *quit = new QAction(QStringLiteral("&Quit"), this);
	quit->setShortcut(QKeySequence::Quit);
	connect(quit, &QAction::triggered, qApp, &QApplication::quit);

	pause_action_ = new QAction(QStringLiteral("&Pause"), this);
	pause_action_->setCheckable(true);
	pause_action_->setShortcut(QKeySequence(QStringLiteral("Ctrl+P")));
	connect(pause_action_, &QAction::toggled, this, &MainWindow::togglePause);

	step_action_ = new QAction(QStringLiteral("S&tep"), this);
	step_action_->setShortcut(QKeySequence(Qt::Key_F11));
	connect(step_action_, &QAction::triggered, this, [this] {
		emulator_.step();
		setPausedUi(true);
	});

	step_over_action_ = new QAction(QStringLiteral("Step &over"), this);
	step_over_action_->setShortcut(QKeySequence(Qt::Key_F10));
	connect(step_over_action_, &QAction::triggered, this, [this] {
		emulator_.stepOver();
		setPausedUi(false);
	});

	auto *break_pc = new QAction(QStringLiteral("Break at &PC"), this);
	break_pc->setShortcut(QKeySequence(QStringLiteral("Ctrl+B")));
	connect(break_pc, &QAction::triggered, this, &MainWindow::breakAtPc);

	auto *clear_bp = new QAction(QStringLiteral("Clear breakpoints"), this);
	connect(clear_bp, &QAction::triggered, this, &MainWindow::clearBreakpointUi);

	idle_skip_action_ = new QAction(QStringLiteral("Skip &idle"), this);
	idle_skip_action_->setCheckable(true);
	idle_skip_action_->setChecked(true);
	idle_skip_action_->setToolTip(QStringLiteral("Fast-forward the FreeRTOS idle loop; turn off for timing-faithful debugging"));
	connect(idle_skip_action_, &QAction::toggled, &emulator_, &EmulatorThread::setIdleSkip);

	auto *reset = new QAction(QStringLiteral("&Reset"), this);
	reset->setShortcut(QKeySequence(QStringLiteral("Ctrl+R")));
	connect(reset, &QAction::triggered, this, &MainWindow::resetEmulator);

	QMenu *file = menuBar()->addMenu(QStringLiteral("&File"));
	file->addAction(open);
	file->addAction(openFlash);
	file->addAction(shot);
	file->addSeparator();
	file->addAction(quit);

	QMenu *emulation = menuBar()->addMenu(QStringLiteral("&Emulator"));
	emulation->addAction(pause_action_);
	emulation->addAction(step_action_);
	emulation->addAction(step_over_action_);
	emulation->addAction(reset);
	emulation->addSeparator();
	emulation->addAction(idle_skip_action_);
	emulation->addAction(break_pc);
	emulation->addAction(clear_bp);

	QToolBar *toolbar = addToolBar(QStringLiteral("Main"));
	toolbar->setMovable(false);
	toolbar->addAction(open);
	toolbar->addAction(pause_action_);
	toolbar->addAction(step_action_);
	toolbar->addAction(step_over_action_);
	toolbar->addAction(break_pc);
	toolbar->addAction(reset);
	toolbar->addAction(shot);

	QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
	view->addAction(debug_dock_->toggleViewAction());
}

void MainWindow::createDebugDock()
{
	debug_dock_ = new QDockWidget(QStringLiteral("Debugger"), this);
	debug_dock_->setObjectName(QStringLiteral("debugger"));
	debug_dock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

	debug_tabs_ = new QTabWidget(debug_dock_);

	// Registers
	auto *reg_tab = new QWidget(debug_tabs_);
	auto *reg_layout = new QVBoxLayout(reg_tab);
	registers_view_ = makeTextView(reg_tab);
	auto *refresh_regs = new QPushButton(QStringLiteral("Refresh"), reg_tab);
	connect(refresh_regs, &QPushButton::clicked, &emulator_, &EmulatorThread::requestState);
	reg_layout->addWidget(registers_view_);
	reg_layout->addWidget(refresh_regs);
	debug_tabs_->addTab(reg_tab, QStringLiteral("Registers"));

	// Memory
	auto *mem_tab = new QWidget(debug_tabs_);
	auto *mem_layout = new QVBoxLayout(mem_tab);
	auto *mem_row = new QHBoxLayout();
	mem_addr_ = new QLineEdit(QStringLiteral("0300c000"), mem_tab);
	mem_len_ = new QSpinBox(mem_tab);
	mem_len_->setRange(16, 4096);
	mem_len_->setSingleStep(16);
	mem_len_->setValue(256);
	auto *mem_refresh = new QPushButton(QStringLiteral("Read"), mem_tab);
	mem_follow_pc_ = new QCheckBox(QStringLiteral("Follow PC"), mem_tab);
	mem_row->addWidget(new QLabel(QStringLiteral("Address"), mem_tab));
	mem_row->addWidget(mem_addr_);
	mem_row->addWidget(new QLabel(QStringLiteral("Bytes"), mem_tab));
	mem_row->addWidget(mem_len_);
	mem_row->addWidget(mem_refresh);
	mem_row->addWidget(mem_follow_pc_);
	mem_view_ = makeTextView(mem_tab);
	mem_view_->setToolTip(QStringLiteral("RAM and flash only; peripheral reads are not side-effect-free"));
	connect(mem_refresh, &QPushButton::clicked, this, &MainWindow::refreshMemory);
	connect(mem_follow_pc_, &QCheckBox::toggled, this, [this](bool on) {
		if (on) refreshMemory();
	});
	connect(mem_addr_, &QLineEdit::returnPressed, this, &MainWindow::refreshMemory);
	mem_layout->addLayout(mem_row);
	mem_layout->addWidget(mem_view_);
	debug_tabs_->addTab(mem_tab, QStringLiteral("Memory"));

	// Breakpoints
	auto *bp_tab = new QWidget(debug_tabs_);
	auto *bp_layout = new QVBoxLayout(bp_tab);
	auto *bp_row = new QHBoxLayout();
	bp_addr_ = new QLineEdit(bp_tab);
	bp_addr_->setPlaceholderText(QStringLiteral("address (hex or decimal)"));
	auto *bp_add = new QPushButton(QStringLiteral("Add"), bp_tab);
	auto *bp_del = new QPushButton(QStringLiteral("Remove"), bp_tab);
	auto *bp_clear = new QPushButton(QStringLiteral("Clear"), bp_tab);
	bp_row->addWidget(bp_addr_);
	bp_row->addWidget(bp_add);
	bp_row->addWidget(bp_del);
	bp_row->addWidget(bp_clear);
	bp_list_ = new QListWidget(bp_tab);
	connect(bp_add, &QPushButton::clicked, this, &MainWindow::addBreakpointFromInput);
	connect(bp_addr_, &QLineEdit::returnPressed, this, &MainWindow::addBreakpointFromInput);
	connect(bp_del, &QPushButton::clicked, this, &MainWindow::removeSelectedBreakpoint);
	connect(bp_clear, &QPushButton::clicked, this, &MainWindow::clearBreakpointUi);
	connect(bp_list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
		mem_addr_->setText(QStringLiteral("%1").arg(item->data(Qt::UserRole).toUInt(), 8, 16, QChar('0')));
		refreshMemory();
	});
	bp_layout->addLayout(bp_row);
	bp_layout->addWidget(bp_list_);
	debug_tabs_->addTab(bp_tab, QStringLiteral("Breakpoints"));

	// Symbols
	auto *sym_tab = new QWidget(debug_tabs_);
	auto *sym_layout = new QVBoxLayout(sym_tab);
	sym_filter_ = new QLineEdit(sym_tab);
	sym_filter_->setPlaceholderText(QStringLiteral("filter symbols"));
	sym_list_ = new QListWidget(sym_tab);
	connect(sym_filter_, &QLineEdit::textChanged, this, [this](const QString &text) {
		for (int i = 0; i < sym_list_->count(); i++) {
			sym_list_->item(i)->setHidden(!text.isEmpty() &&
						     !sym_list_->item(i)->text().contains(text, Qt::CaseInsensitive));
		}
	});
	connect(sym_list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
		const uint32_t addr = item->data(Qt::UserRole).toUInt();
		emulator_.addBreakpoint(addr);
		bool found = false;
		for (int i = 0; i < bp_list_->count(); i++) {
			if (bp_list_->item(i)->data(Qt::UserRole).toUInt() == addr) {
				found = true;
				break;
			}
		}
		if (!found) {
			auto *bp = new QListWidgetItem(
				QStringLiteral("%1  %2").arg(addr, 8, 16, QChar('0')).arg(item->text().section(QLatin1Char(' '), 0, 0)),
				bp_list_);
			bp->setData(Qt::UserRole, addr);
		}
		mem_addr_->setText(QStringLiteral("%1").arg(addr, 8, 16, QChar('0')));
		refreshMemory();
	});
	sym_layout->addWidget(sym_filter_);
	sym_layout->addWidget(sym_list_);
	debug_tabs_->addTab(sym_tab, QStringLiteral("Symbols"));

	// UART / debug link
	auto *uart_tab = new QWidget(debug_tabs_);
	auto *uart_layout = new QVBoxLayout(uart_tab);
	auto *uart_row = new QHBoxLayout();
	bridge_label_ = new QLabel(QStringLiteral("UART PTY: -"), uart_tab);
	bridge_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	bridge_label_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	auto *copy_pty = new QPushButton(QStringLiteral("Copy"), uart_tab);
	connect(copy_pty, &QPushButton::clicked, this, [this] {
		QApplication::clipboard()->setText(bridge_label_->text().section(QLatin1Char(' '), 2));
	});
	uart_row->addWidget(bridge_label_, 1);
	uart_row->addWidget(copy_pty);
	uart_log_ = makeTextView(uart_tab);
	auto *send_row = new QHBoxLayout();
	uart_input_ = new QLineEdit(uart_tab);
	uart_input_->setPlaceholderText(QStringLiteral("text to send to UART0 (Enter sends CRLF)"));
	auto *send_button = new QPushButton(QStringLiteral("Send"), uart_tab);
	auto *clear_button = new QPushButton(QStringLiteral("Clear"), uart_tab);
	connect(uart_input_, &QLineEdit::returnPressed, this, &MainWindow::sendUartLine);
	connect(send_button, &QPushButton::clicked, this, &MainWindow::sendUartLine);
	connect(clear_button, &QPushButton::clicked, uart_log_, &QPlainTextEdit::clear);
	send_row->addWidget(uart_input_);
	send_row->addWidget(send_button);
	send_row->addWidget(clear_button);
	uart_layout->addLayout(uart_row);
	uart_layout->addWidget(uart_log_);
	uart_layout->addLayout(send_row);
	debug_tabs_->addTab(uart_tab, QStringLiteral("UART"));

	debug_dock_->setWidget(debug_tabs_);
	addDockWidget(Qt::RightDockWidgetArea, debug_dock_);
	resizeDocks({debug_dock_}, {460}, Qt::Horizontal);
}

void MainWindow::loadFirmware(const QString &path)
{
	emulator_.stop();
	QString error;
	if (!emulator_.loadFirmware(path, data_flash_, error)) {
		QMessageBox::critical(this, QStringLiteral("Cannot load firmware"), error);
		statusBar()->showMessage(QStringLiteral("Load failed"), 5000);
		return;
	}
	firmware_ = path;
	frames_seen_ = 0;
	last_frame_ = QImage();
	have_state_ = false;
	current_state_ = {};
	radio_->setFrame(QImage());
	frame_label_->setText(QStringLiteral("Frame 0"));
	pc_label_->setText(QStringLiteral("PC -"));
	registers_view_->clear();
	mem_view_->clear();
	bp_list_->clear();
	clearBreakpointUi();
	loadSymbolsFor(path);
	if (pause_action_ && pause_action_->isChecked()) pause_action_->setChecked(false);
	setWindowTitle(QStringLiteral("DM32EMU - ") + QFileInfo(path).fileName());
	statusBar()->showMessage(QStringLiteral("Loaded ") + path, 5000);
	emulator_.start();
}

void MainWindow::setScreenshotMode(const QString &path, int frames)
{
	screenshot_path_ = path;
	screenshot_frames_ = frames;
}

void MainWindow::setWindowScreenshotMode(const QString &path, int frames)
{
	window_screenshot_path_ = path;
	screenshot_frames_ = frames;
}

void MainWindow::loadSymbolsFor(const QString &firmware)
{
	symbols_path_.clear();
	symtab_.syms.clear();
	sym_list_->clear();

	QString path = firmware;
	if (!path.endsWith(QStringLiteral(".elf"))) {
		QString candidate = path;
		if (candidate.endsWith(QStringLiteral(".bin"))) candidate.chop(4);
		candidate += QStringLiteral(".elf");
		if (QFileInfo::exists(candidate)) path = candidate;
		else path.clear();
	}
	if (path.isEmpty()) return;

	std::string err;
	if (!load_symbols(path.toStdString(), symtab_.syms, err)) {
		statusBar()->showMessage(QStringLiteral("Symbols: ") + QString::fromStdString(err), 5000);
		return;
	}
	symbols_path_ = path;
	QSignalBlocker blocker(sym_list_);
	for (const auto &s : symtab_.syms) {
		auto *item = new QListWidgetItem(
			QStringLiteral("%1  %2").arg(QString::fromStdString(s.name))
				.arg(s.addr, 8, 16, QChar('0')),
			sym_list_);
		item->setData(Qt::UserRole, s.addr);
	}
	statusBar()->showMessage(QStringLiteral("Symbols: %1").arg(path), 3000);
}

void MainWindow::openFirmware()
{
	const QString path = QFileDialog::getOpenFileName(
		this, QStringLiteral("Open firmware"), QDir::homePath(),
		QStringLiteral("Firmware (*.bin *.elf);;All files (*)"));
	if (!path.isEmpty()) loadFirmware(path);
}

void MainWindow::openDataFlash()
{
	const QString path = QFileDialog::getOpenFileName(
		this, QStringLiteral("Open 16 MB SPI NOR dump"), QDir::homePath(),
		QStringLiteral("Flash dumps (*.bin *.flash);;All files (*)"));
	if (path.isEmpty()) return;
	data_flash_ = path;
	statusBar()->showMessage(QStringLiteral("Data flash: ") + path, 5000);
	if (!firmware_.isEmpty()) loadFirmware(firmware_);
}

void MainWindow::saveScreenshot()
{
	if (last_frame_.isNull()) {
		QMessageBox::information(this, QStringLiteral("No frame"),
					 QStringLiteral("The radio has not drawn a frame yet."));
		return;
	}
	const QString path = QFileDialog::getSaveFileName(
		this, QStringLiteral("Save screenshot"), QDir::homePath() + QStringLiteral("/dm32.png"),
		QStringLiteral("PNG image (*.png);;All files (*)"));
	if (path.isEmpty()) return;
	if (!last_frame_.save(path)) {
		QMessageBox::critical(this, QStringLiteral("Save failed"),
				      QStringLiteral("Could not write ") + path);
	}
}

void MainWindow::setPausedUi(bool paused)
{
	if (!pause_action_) return;
	QSignalBlocker blocker(pause_action_);
	pause_action_->setChecked(paused);
	pause_action_->setText(paused ? QStringLiteral("&Resume") : QStringLiteral("&Pause"));
	if (paused) statusBar()->showMessage(QStringLiteral("Paused"), 3000);
}

void MainWindow::togglePause(bool paused)
{
	if (paused) emulator_.pause();
	else emulator_.resumeExecution();
	setPausedUi(paused);
}

void MainWindow::resetEmulator()
{
	if (!firmware_.isEmpty()) loadFirmware(firmware_);
}

void MainWindow::onFrame(const QImage &image, quint64 frames)
{
	last_frame_ = image;
	radio_->setFrame(image);
	if (frames != frames_seen_) {
		frames_seen_ = frames;
		frame_label_->setText(QStringLiteral("Frame %1").arg(frames));
	}
	if (screenshot_frames_ > 0 && frames >= (quint64)screenshot_frames_) {
		bool done = false;
		if (!screenshot_path_.isEmpty()) {
			last_frame_.save(screenshot_path_);
			done = true;
		}
		if (!window_screenshot_path_.isEmpty()) {
			grab().save(window_screenshot_path_);
			done = true;
		}
		if (done) QTimer::singleShot(0, qApp, &QApplication::quit);
	}
}

void MainWindow::updatePcLabel()
{
	if (!have_state_) return;
	const QString label = symtab_.label(current_state_.pc);
	pc_label_->setText(QStringLiteral("PC %1 %2")
				   .arg(current_state_.pc, 8, 16, QChar('0'))
				   .arg(label));
}

void MainWindow::onState(const CpuSnapshot &state)
{
	current_state_ = state;
	have_state_ = true;
	registers_view_->setPlainText(registersText(state, symtab_));
	updatePcLabel();
	if (mem_follow_pc_->isChecked()) {
		mem_addr_->setText(QStringLiteral("%1").arg(state.pc, 8, 16, QChar('0')));
		refreshMemory();
	}
	if (emulator_.paused()) setPausedUi(true);
}

void MainWindow::onMemory(quint32 addr, const QByteArray &data)
{
	mem_view_->setPlainText(hexDump(addr, data));
}

void MainWindow::onFault(const QString &text)
{
	statusBar()->showMessage(QStringLiteral("CPU fault"), 10000);
	setPausedUi(true);
	QMessageBox::critical(this, QStringLiteral("Emulator fault"), text);
}

void MainWindow::onBreakpointHit(quint32 pc)
{
	statusBar()->showMessage(QStringLiteral("Breakpoint at %1 %2")
					 .arg(pc, 8, 16, QChar('0'))
					 .arg(symtab_.label(pc)),
				 10000);
	setPausedUi(true);
}

void MainWindow::onBridgeReady(const QString &path)
{
	if (path.isEmpty()) {
		bridge_label_->setText(QStringLiteral("UART PTY: unavailable"));
	} else {
		bridge_label_->setText(QStringLiteral("UART PTY: %1").arg(path));
		bridge_label_->setToolTip(QStringLiteral("Point the debug link or CPS tool at this PTY"));
	}
}

void MainWindow::appendUart(const QByteArray &data, bool radioToHost)
{
	if (data.isEmpty()) return;
	uart_log_->appendPlainText((radioToHost ? QStringLiteral(">> ") : QStringLiteral("<< ")) +
				   hexBytes(data));
	QTextCursor cursor = uart_log_->textCursor();
	cursor.movePosition(QTextCursor::End);
	uart_log_->setTextCursor(cursor);
}

void MainWindow::refreshMemory()
{
	bool ok = false;
	const uint32_t addr = mem_addr_->text().trimmed().toUInt(&ok, 0);
	if (!ok) {
		statusBar()->showMessage(QStringLiteral("Bad memory address"), 3000);
		return;
	}
	emulator_.requestMemory(addr, (uint32_t)mem_len_->value());
}

void MainWindow::addBreakpointFromInput()
{
	bool ok = false;
	const uint32_t addr = bp_addr_->text().trimmed().toUInt(&ok, 0);
	if (!ok) {
		statusBar()->showMessage(QStringLiteral("Bad breakpoint address"), 3000);
		return;
	}
	for (int i = 0; i < bp_list_->count(); i++) {
		if (bp_list_->item(i)->data(Qt::UserRole).toUInt() == addr) return;
	}
	emulator_.addBreakpoint(addr);
	auto *item = new QListWidgetItem(QStringLiteral("%1  %2")
						 .arg(addr, 8, 16, QChar('0'))
						 .arg(symtab_.label(addr)),
					 bp_list_);
	item->setData(Qt::UserRole, addr);
}

void MainWindow::removeSelectedBreakpoint()
{
	const auto selected = bp_list_->selectedItems();
	for (QListWidgetItem *item : selected) {
		emulator_.removeBreakpoint(item->data(Qt::UserRole).toUInt());
		delete item;
	}
}

void MainWindow::clearBreakpointUi()
{
	emulator_.clearBreakpoints();
	bp_list_->clear();
}

void MainWindow::breakAtPc()
{
	if (!have_state_) return;
	bp_addr_->setText(QStringLiteral("%1").arg(current_state_.pc, 8, 16, QChar('0')));
	addBreakpointFromInput();
}

void MainWindow::sendUartLine()
{
	QByteArray data = uart_input_->text().toUtf8();
	data.append("\r\n");
	emulator_.sendUart(data);
	appendUart(data, false);
	uart_input_->clear();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
	emulator_.stop();
	QMainWindow::closeEvent(event);
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QApplication::setApplicationName(QStringLiteral("DM32EMU"));
	QApplication::setOrganizationName(QStringLiteral("DM32EMU"));
	QApplication::setStyle(QStringLiteral("Fusion"));

	QPalette palette;
	palette.setColor(QPalette::Window, QColor("#181b20"));
	palette.setColor(QPalette::WindowText, QColor("#e6e8eb"));
	palette.setColor(QPalette::Base, QColor("#121418"));
	palette.setColor(QPalette::AlternateBase, QColor("#22262c"));
	palette.setColor(QPalette::ToolTipBase, QColor("#22262c"));
	palette.setColor(QPalette::ToolTipText, QColor("#e6e8eb"));
	palette.setColor(QPalette::Text, QColor("#e6e8eb"));
	palette.setColor(QPalette::Button, QColor("#2a2f36"));
	palette.setColor(QPalette::ButtonText, QColor("#e6e8eb"));
	palette.setColor(QPalette::BrightText, Qt::red);
	palette.setColor(QPalette::Highlight, QColor("#2f6fbf"));
	palette.setColor(QPalette::HighlightedText, Qt::white);
	app.setPalette(palette);

	qRegisterMetaType<CpuSnapshot>("CpuSnapshot");

	QCommandLineParser parser;
	parser.setApplicationDescription(QStringLiteral("DM-32UV firmware emulator GUI"));
	parser.addHelpOption();
	parser.addPositionalArgument(QStringLiteral("firmware"),
				     QStringLiteral("Raw .bin or C-SKY .elf firmware image"));
	QCommandLineOption dataFlash(QStringLiteral("data-flash"),
				     QStringLiteral("Load a 16 MB SPI NOR dump."),
				     QStringLiteral("file"));
	QCommandLineOption screenshot(QStringLiteral("screenshot"),
				      QStringLiteral("Save the LCD to a PNG after --frames and quit."),
				      QStringLiteral("file"));
	QCommandLineOption windowShot(QStringLiteral("window-screenshot"),
				      QStringLiteral("Save the whole GUI window to a PNG after --frames and quit."),
				      QStringLiteral("file"));
	QCommandLineOption frames(QStringLiteral("frames"),
				  QStringLiteral("Full LCD frames to wait for in screenshot mode."),
				  QStringLiteral("n"), QStringLiteral("3"));
	QCommandLineOption smoke(QStringLiteral("smoke-test"),
				 QStringLiteral("Start the window and exit; used by CTest."));
	parser.addOption(dataFlash);
	parser.addOption(screenshot);
	parser.addOption(windowShot);
	parser.addOption(frames);
	parser.addOption(smoke);
	parser.process(app);

	MainWindow window;
	window.show();

	if (parser.isSet(dataFlash)) window.setDataFlash(parser.value(dataFlash));
	if (parser.isSet(screenshot)) {
		window.setScreenshotMode(parser.value(screenshot), parser.value(frames).toInt());
	}
	if (parser.isSet(windowShot)) {
		window.setWindowScreenshotMode(parser.value(windowShot), parser.value(frames).toInt());
	}
	const QStringList positional = parser.positionalArguments();
	if (!positional.isEmpty()) {
		QTimer::singleShot(0, &window, [&] { window.loadFirmware(positional.first()); });
	}
	if (parser.isSet(smoke)) {
		QTimer::singleShot(200, &app, &QApplication::quit);
	}
	return app.exec();
}
