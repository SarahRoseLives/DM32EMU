// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "dual_radio_window.hpp"

#include "emulator_thread.hpp"
#include "radio_widget.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

DualRadioWindow::DualRadioWindow(QWidget *parent) : QMainWindow(parent)
{
	setWindowTitle(QStringLiteral("DM32EMU - Dual Radio"));
	setMinimumSize(1100, 760);
	resize(1500, 1000);
	bus_ = std::make_shared<VirtualRfBus>();

	QDir docs(QDir::homePath() + QStringLiteral("/Documents"));
	const QStringList backups = docs.entryList({QStringLiteral("DM32UV_Backup_*.zip")}, QDir::Files, QDir::Time);
	if (!backups.isEmpty()) codecBackup_ = docs.filePath(backups.first());

	buildUi();
	statsTimer_ = new QTimer(this);
	connect(statsTimer_, &QTimer::timeout, this, &DualRadioWindow::updateLinkStats);
	statsTimer_->start(200);
	setCodecLabel();
	updateLinkStats();
}

DualRadioWindow::~DualRadioWindow()
{
	for (auto &p : panes_) {
		if (p.emu) p.emu->stop();
	}
}

void DualRadioWindow::buildUi()
{
	auto *central = new QWidget(this);
	auto *root = new QVBoxLayout(central);

	auto *shared = new QGroupBox(QStringLiteral("Virtual RF and codec"), central);
	auto *sharedRow = new QHBoxLayout(shared);
	codecLabel_ = new QLabel(shared);
	codecLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	codecLabel_->setMinimumWidth(380);
	auto *codecButton = new QPushButton(QStringLiteral("Codec backup..."), shared);
	linkToggle_ = new QCheckBox(QStringLiteral("Virtual RF link"), shared);
	linkToggle_->setChecked(true);
	tsCombo_ = new QComboBox(shared);
	tsCombo_->addItem(QStringLiteral("TS1"), 1);
	tsCombo_->addItem(QStringLiteral("TS2"), 2);
	bus_->set_timeslot(1);
	linkStats_ = new QLabel(shared);
	auto *clear = new QPushButton(QStringLiteral("Clear RF queue"), shared);
	sharedRow->addWidget(codecLabel_, 1);
	sharedRow->addWidget(codecButton);
	sharedRow->addSpacing(16);
	sharedRow->addWidget(linkToggle_);
	sharedRow->addWidget(new QLabel(QStringLiteral("RF TS"), shared));
	sharedRow->addWidget(tsCombo_);
	sharedRow->addWidget(linkStats_);
	sharedRow->addWidget(clear);
	root->addWidget(shared);

	auto *radios = new QHBoxLayout();
	root->addLayout(radios, 1);
	buildPane(0);
	buildPane(1);
	radios->addWidget(panes_[0].box, 1);
	radios->addWidget(panes_[1].box, 1);

	auto *hint = new QLabel(QStringLiteral(
		"Use a DMR channel on both radios (same timeslot and color code). "
		"Select the matching RF timeslot above, then press PTT on either keypad."), central);
	hint->setWordWrap(true);
	root->addWidget(hint);

	setCentralWidget(central);
	statusBar()->showMessage(QStringLiteral("Open a firmware for each radio"));

	connect(codecButton, &QPushButton::clicked, this, &DualRadioWindow::chooseCodec);
	connect(clear, &QPushButton::clicked, this, [this] { bus_->clear(); updateLinkStats(); });
	connect(tsCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
		bus_->set_timeslot((uint8_t)tsCombo_->currentData().toUInt());
		updateLinkStats();
	});
	connect(linkToggle_, &QCheckBox::toggled, this, [this](bool on) {
		for (auto &p : panes_) p.emu->setVirtualRfEnabled(on);
		statusBar()->showMessage(on ? QStringLiteral("Virtual RF link on") : QStringLiteral("Virtual RF link off"), 3000);
	});
}

void DualRadioWindow::buildPane(int side)
{
	Pane &p = panes_[side];
	p.box = new QGroupBox(side == 0 ? QStringLiteral("Radio A") : QStringLiteral("Radio B"), this);
	auto *layout = new QVBoxLayout(p.box);
	auto *buttons = new QHBoxLayout();
	auto *fw = new QPushButton(QStringLiteral("Firmware..."), p.box);
	auto *data = new QPushButton(QStringLiteral("Codeplug..."), p.box);
	auto *pause = new QPushButton(QStringLiteral("Pause"), p.box);
	pause->setCheckable(true);
	auto *reset = new QPushButton(QStringLiteral("Reset"), p.box);
	buttons->addWidget(fw);
	buttons->addWidget(data);
	buttons->addWidget(pause);
	buttons->addWidget(reset);
	buttons->addStretch(1);
	layout->addLayout(buttons);

	p.firmware = new QLabel(QStringLiteral("firmware: -"), p.box);
	p.data = new QLabel(QStringLiteral("codeplug: -"), p.box);
	p.status = new QLabel(QStringLiteral("stopped"), p.box);
	p.pc = new QLabel(QStringLiteral("PC -"), p.box);
	p.firmware->setTextInteractionFlags(Qt::TextSelectableByMouse);
	p.data->setTextInteractionFlags(Qt::TextSelectableByMouse);
	p.pc->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	layout->addWidget(p.firmware);
	layout->addWidget(p.data);
	layout->addWidget(p.status);
	layout->addWidget(p.pc);

	p.radio = new RadioWidget(p.box);
	layout->addWidget(p.radio, 1);

	p.emu = new EmulatorThread(this);
	p.emu->setVirtualRf(bus_, (uint8_t)side);
	p.emu->setVirtualRfEnabled(linkToggle_->isChecked());
	p.emu->setIdleSkip(true);

	connect(fw, &QPushButton::clicked, this, [this, side] { openFirmware(side); });
	connect(data, &QPushButton::clicked, this, [this, side] { openData(side); });
	connect(reset, &QPushButton::clicked, this, [this, side] { reload(side); });
	connect(pause, &QPushButton::toggled, this, [this, side, pause](bool on) {
		panes_[side].emu->setPaused(on);
		pause->setText(on ? QStringLiteral("Resume") : QStringLiteral("Pause"));
	});
	connect(p.radio, &RadioWidget::keyChanged, p.emu, &EmulatorThread::setKey);
	connect(p.emu, &EmulatorThread::frameReady, this,
		[this, side](const QImage &img, quint64) { panes_[side].radio->setFrame(img); });
	connect(p.emu, &EmulatorThread::stateReady, this, [this, side](const CpuSnapshot &s) {
		panes_[side].pc->setText(QStringLiteral("PC %1").arg(s.pc, 8, 16, QChar('0')));
	});
	connect(p.emu, &EmulatorThread::statusMessage, this,
		[this, side](const QString &text) { panes_[side].status->setText(text); });
	connect(p.emu, &EmulatorThread::fault, this, [this, side](const QString &text) {
		panes_[side].status->setText(QStringLiteral("fault: ") + text);
		QMessageBox::critical(this, (side == 0 ? QStringLiteral("Radio A") : QStringLiteral("Radio B")) +
						  QStringLiteral(" fault"), text);
	});
}

void DualRadioWindow::setCodecBackup(const QString &path)
{
	codecBackup_ = path;
	setCodecLabel();
	reloadAll();
}

void DualRadioWindow::setCodecLabel()
{
	if (codecBackup_.isEmpty()) {
		codecLabel_->setText(QStringLiteral("Codec: none (FM only)"));
		codecLabel_->setToolTip({});
	} else {
		codecLabel_->setText(QStringLiteral("Codec: ") + QFileInfo(codecBackup_).fileName());
		codecLabel_->setToolTip(codecBackup_);
	}
}

void DualRadioWindow::chooseCodec()
{
	const QString path = QFileDialog::getOpenFileName(
		this, QStringLiteral("Codec backup (program-memory.bin or DM32 backup ZIP)"),
		QDir::homePath() + QStringLiteral("/Documents"),
		QStringLiteral("Backups (*.zip *.bin);;All files (*)"));
	if (path.isEmpty()) return;
	setCodecBackup(path);
}

void DualRadioWindow::loadRadio(int side, const QString &firmware, const QString &dataBackup)
{
	if (side < 0 || side > 1) return;
	Pane &p = panes_[side];
	p.firmwarePath = firmware;
	if (!dataBackup.isNull()) p.dataPath = dataBackup;
	p.firmware->setText(QStringLiteral("firmware: ") + QFileInfo(firmware).fileName());
	p.data->setText(p.dataPath.isEmpty() ? QStringLiteral("codeplug: built-in/default")
					     : QStringLiteral("codeplug: ") + QFileInfo(p.dataPath).fileName());
	reload(side);
}

void DualRadioWindow::loadRadioFromPath(int side, const QString &firmware)
{
	if (side < 0 || side > 1) return;
	Pane &p = panes_[side];
	p.firmwarePath = firmware;
	p.firmware->setText(QStringLiteral("firmware: ") + QFileInfo(firmware).fileName());
	reload(side);
}

void DualRadioWindow::openFirmware(int side)
{
	QString dir = QDir::homePath();
	if (QDir(dir + QStringLiteral("/Projects/RadioFoundry/build/dm32")).exists())
		dir += QStringLiteral("/Projects/RadioFoundry/build/dm32");
	const QString path = QFileDialog::getOpenFileName(
		this, QStringLiteral("Open firmware"), dir,
		QStringLiteral("Firmware (*.bin *.elf);;All files (*)"));
	if (!path.isEmpty()) loadRadio(side, path, panes_[side].dataPath);
}

void DualRadioWindow::openData(int side)
{
	const QString path = QFileDialog::getOpenFileName(
		this, QStringLiteral("Open codeplug/data backup"), QDir::homePath() + QStringLiteral("/Documents"),
		QStringLiteral("Radio backups (*.zip *.bin);;All files (*)"));
	if (path.isEmpty()) return;
	panes_[side].dataPath = path;
	panes_[side].data->setText(QStringLiteral("codeplug: ") + QFileInfo(path).fileName());
	if (!panes_[side].firmwarePath.isEmpty()) reload(side);
}

void DualRadioWindow::reload(int side)
{
	Pane &p = panes_[side];
	if (p.firmwarePath.isEmpty()) return;
	p.emu->stop();
	QString error;
	if (!p.emu->loadFirmware(p.firmwarePath, p.dataPath, codecBackup_, error)) {
		p.status->setText(QStringLiteral("load failed: ") + error);
		QMessageBox::critical(this, (side == 0 ? QStringLiteral("Radio A") : QStringLiteral("Radio B")) +
						  QStringLiteral(" load failed"), error);
		return;
	}
	p.emu->setVirtualRfEnabled(linkToggle_->isChecked());
	p.emu->start();
	p.status->setText(QStringLiteral("running"));
	statusBar()->showMessage((side == 0 ? QStringLiteral("Radio A: ") : QStringLiteral("Radio B: ")) +
				 QFileInfo(p.firmwarePath).fileName(), 5000);
}

void DualRadioWindow::reloadAll()
{
	for (int i = 0; i < 2; i++) {
		if (!panes_[i].firmwarePath.isEmpty()) reload(i);
	}
}

void DualRadioWindow::updateLinkStats()
{
	linkStats_->setText(QStringLiteral("RF %1/%2/%3 TS%4")
		.arg(bus_->sent()).arg(bus_->delivered()).arg(bus_->dropped()).arg(bus_->timeslot() ? bus_->timeslot() : 0));
}

void DualRadioWindow::closeEvent(QCloseEvent *event)
{
	for (auto &p : panes_) {
		if (p.emu) p.emu->stop();
	}
	QMainWindow::closeEvent(event);
}
