// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "dual_radio_window.hpp"

#include "emulator_thread.hpp"
#include "system_location.hpp"
#include "radio_widget.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>

#include <functional>

DualRadioWindow::DualRadioWindow(QWidget *parent) : QMainWindow(parent)
{
	setWindowTitle(QStringLiteral("DM32EMU - Dual Radio"));
	setMinimumSize(1150, 780);
	resize(1700, 1100);
	iq_ = std::make_shared<VirtualRfIqChannel>();

	QDir docs(QDir::homePath() + QStringLiteral("/Documents"));
	const QStringList backups = docs.entryList({QStringLiteral("DM32UV_Backup_*.zip")}, QDir::Files, QDir::Time);
	if (!backups.isEmpty()) codecBackup_ = docs.filePath(backups.first());

	buildUi();
	location_ = new SystemLocation(this);
	connect(location_, &SystemLocation::fixReady, this, [this](const Machine::GpsFix &fix) {
		for (auto &p : panes_) {
			if (p.emu) p.emu->setGpsFix(fix);
		}
	});
	statsTimer_ = new QTimer(this);
	connect(statsTimer_, &QTimer::timeout, this, &DualRadioWindow::updateLinkStats);
	statsTimer_->start(200);
	setCodecLabel();
	updateLinkStats();
	loadSettings();
	QTimer::singleShot(0, this, [this] {
		if (auto_restore_) restoreLastSession();
	});
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
	root->setContentsMargins(4, 4, 4, 4);
	root->setSpacing(4);

	auto *radios = new QHBoxLayout();
	radios->setSpacing(6);
	root->addLayout(radios, 1);
	buildPane(0);
	buildPane(1);
	radios->addWidget(panes_[0].box, 1);
	radios->addWidget(panes_[1].box, 1);

	setCentralWidget(central);

	toolbar_ = addToolBar(QStringLiteral("Radios"));
	toolbar_->setMovable(false);
	toolbar_->setToolButtonStyle(Qt::ToolButtonTextOnly);

	auto addRadioAction = [&](const QString &text, const std::function<void()> &fn) {
		QAction *a = toolbar_->addAction(text);
		connect(a, &QAction::triggered, this, fn);
		return a;
	};

	for (int side = 0; side < 2; side++) {
		const QString prefix = side == 0 ? QStringLiteral("A ") : QStringLiteral("B ");
		addRadioAction(prefix + QStringLiteral("Firmware..."), [this, side] { openFirmware(side); });
		addRadioAction(prefix + QStringLiteral("Codeplug..."), [this, side] { openData(side); });
		QAction *pause = toolbar_->addAction(prefix + QStringLiteral("Pause"));
		pause->setCheckable(true);
		panes_[side].pauseAction = pause;
		connect(pause, &QAction::toggled, this, [this, side, pause](bool on) {
			panes_[side].emu->setPaused(on);
			pause->setText((side == 0 ? QStringLiteral("A ") : QStringLiteral("B ")) +
				       (on ? QStringLiteral("Resume") : QStringLiteral("Pause")));
		});
		addRadioAction(prefix + QStringLiteral("Reset"), [this, side] { reload(side); });
		if (side == 0) toolbar_->addSeparator();
	}

	toolbar_->addSeparator();
	addRadioAction(QStringLiteral("Codec backup..."), [this] { chooseCodec(); });
	codecLabel_ = new QLabel(toolbar_);
	codecLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	codecLabel_->setMinimumWidth(120);
	toolbar_->addWidget(codecLabel_);
	toolbar_->addSeparator();

	linkToggle_ = new QCheckBox(QStringLiteral("Link"), toolbar_);
	linkToggle_->setChecked(true);
	toolbar_->addWidget(linkToggle_);
	toolbar_->addWidget(new QLabel(QStringLiteral("DMR TS"), toolbar_));
	tsCombo_ = new QComboBox(toolbar_);
	tsCombo_->addItem(QStringLiteral("Auto"), 0);
	tsCombo_->addItem(QStringLiteral("TS1"), 1);
	tsCombo_->addItem(QStringLiteral("TS2"), 2);
	toolbar_->addWidget(tsCombo_);
	addRadioAction(QStringLiteral("Clear IQ"), [this] { iq_->clear(); updateLinkStats(); });

	linkStats_ = new QLabel(this);
	statusBar()->addPermanentWidget(linkStats_);
	statusBar()->showMessage(QStringLiteral("Both radios share one IQ channel; use the toolbar to load and control each radio."));

	connect(tsCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
		const uint8_t ts = (uint8_t)tsCombo_->currentData().toUInt();
		for (auto &p : panes_) p.emu->setDmrTimeslot(ts);
	});
	connect(linkToggle_, &QCheckBox::toggled, this, [this](bool on) {
		for (auto &p : panes_) p.emu->setIqEnabled(on);
		statusBar()->showMessage(on ? QStringLiteral("Virtual RF link on") : QStringLiteral("Virtual RF link off"), 3000);
	});
}

void DualRadioWindow::buildPane(int side)
{
	Pane &p = panes_[side];
	p.box = new QGroupBox(this);
	auto *layout = new QVBoxLayout(p.box);
	layout->setContentsMargins(6, 6, 6, 6);
	layout->setSpacing(4);

	auto *header = new QHBoxLayout();
	p.title = new QLabel(side == 0 ? QStringLiteral("Radio A") : QStringLiteral("Radio B"), p.box);
	QFont titleFont = p.title->font();
	titleFont.setBold(true);
	titleFont.setPixelSize(15);
	p.title->setFont(titleFont);
	p.dmrId = new QLineEdit(p.box);
	p.dmrId->setPlaceholderText(QStringLiteral("DMR ID"));
	p.dmrId->setValidator(new QIntValidator(1, 0xFFFFFF, p.dmrId));
	p.dmrId->setMaximumWidth(110);
	p.dmrId->setToolTip(QStringLiteral("Optional own DMR ID; applied on Reset or firmware load."));
	header->addWidget(p.title);
	header->addStretch(1);
	header->addWidget(new QLabel(QStringLiteral("ID"), p.box));
	header->addWidget(p.dmrId);
	layout->addLayout(header);

	p.radio = new RadioWidget(p.box);
	layout->addWidget(p.radio, 1);

	auto *footer = new QHBoxLayout();
	p.status = new QLabel(QStringLiteral("stopped"), p.box);
	p.pc = new QLabel(QStringLiteral("PC -"), p.box);
	p.pc->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
	footer->addWidget(p.status);
	footer->addStretch(1);
	footer->addWidget(p.pc);
	layout->addLayout(footer);

	p.emu = new EmulatorThread(this);
	p.emu->setIqChannel(iq_, (uint8_t)side);
	p.emu->setIqEnabled(linkToggle_ ? linkToggle_->isChecked() : true);
	p.emu->setDmrTimeslot(tsCombo_ ? (uint8_t)tsCombo_->currentData().toUInt() : 0);
	p.emu->setIdleSkip(true);

	connect(p.dmrId, &QLineEdit::returnPressed, this, [this, side] { reload(side); });
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
	saveSettings();
	reloadAll();
}

void DualRadioWindow::loadSettings()
{
	QSettings settings;
	const QString codec = settings.value(QStringLiteral("dual/codec")).toString();
	if (!codec.isEmpty() && QFileInfo::exists(codec)) codecBackup_ = codec;
	tsCombo_->setCurrentIndex(settings.value(QStringLiteral("dual/ts"), 0).toInt());
	linkToggle_->setChecked(settings.value(QStringLiteral("dual/link"), true).toBool());
	for (int i = 0; i < 2; i++) {
		Pane &p = panes_[i];
		const QString side = i == 0 ? QStringLiteral("a") : QStringLiteral("b");
		p.firmwarePath = settings.value(QStringLiteral("dual/%1/firmware").arg(side)).toString();
		p.dataPath = settings.value(QStringLiteral("dual/%1/data").arg(side)).toString();
		p.dmrId->setText(settings.value(QStringLiteral("dual/%1/dmr_id").arg(side)).toString());
		QString tip;
		if (!p.firmwarePath.isEmpty()) tip += QStringLiteral("Firmware: ") + p.firmwarePath;
		if (!p.dataPath.isEmpty()) tip += (tip.isEmpty() ? QString() : QStringLiteral("\n")) +
						QStringLiteral("Codeplug: ") + p.dataPath;
		p.title->setToolTip(tip);
	}
	setCodecLabel();
	updateLinkStats();
}

void DualRadioWindow::saveSettings()
{
	QSettings settings;
	settings.setValue(QStringLiteral("dual/codec"), codecBackup_);
	settings.setValue(QStringLiteral("dual/ts"), tsCombo_->currentData().toInt());
	settings.setValue(QStringLiteral("dual/link"), linkToggle_->isChecked());
	for (int i = 0; i < 2; i++) {
		const Pane &p = panes_[i];
		const QString side = i == 0 ? QStringLiteral("a") : QStringLiteral("b");
		settings.setValue(QStringLiteral("dual/%1/firmware").arg(side), p.firmwarePath);
		settings.setValue(QStringLiteral("dual/%1/data").arg(side), p.dataPath);
		settings.setValue(QStringLiteral("dual/%1/dmr_id").arg(side), p.dmrId->text().trimmed());
	}
}

void DualRadioWindow::restoreLastSession()
{
	bool restored = false;
	for (int i = 0; i < 2; i++) {
		if (!panes_[i].firmwarePath.isEmpty() && QFileInfo::exists(panes_[i].firmwarePath)) {
			reload(i);
			restored = true;
		}
	}
	if (restored) statusBar()->showMessage(QStringLiteral("Restored last session"), 3000);
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

void DualRadioWindow::loadRadio(int side, const QString &firmware, const QString &dataBackup, uint32_t dmrId)
{
	if (side < 0 || side > 1) return;
	Pane &p = panes_[side];
	p.firmwarePath = firmware;
	if (!dataBackup.isNull()) p.dataPath = dataBackup;
	if (dmrId != 0) p.dmrId->setText(QString::number(dmrId));
	saveSettings();
	QString tip = QStringLiteral("Firmware: ") + firmware;
	if (!p.dataPath.isEmpty()) tip += QStringLiteral("\nCodeplug: ") + p.dataPath;
	p.title->setToolTip(tip);
	reload(side);
}

void DualRadioWindow::loadRadioFromPath(int side, const QString &firmware)
{
	if (side < 0 || side > 1) return;
	Pane &p = panes_[side];
	p.firmwarePath = firmware;
	p.title->setToolTip(QStringLiteral("Firmware: ") + firmware);
	reload(side);
}

void DualRadioWindow::hotloadRadio(int side, const QString &firmware)
{
	if (side < 0 || side > 1) return;
	if (firmware.isEmpty()) {
		reload(side);
		return;
	}
	loadRadio(side, firmware, panes_[side].dataPath);
}

void DualRadioWindow::hotloadAll(const QString &firmware)
{
	for (int i = 0; i < 2; i++) hotloadRadio(i, firmware);
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
	panes_[side].title->setToolTip(QStringLiteral("Firmware: ") + panes_[side].firmwarePath +
				       QStringLiteral("\nCodeplug: ") + path);
	saveSettings();
	if (!panes_[side].firmwarePath.isEmpty()) reload(side);
}

void DualRadioWindow::reload(int side)
{
	Pane &p = panes_[side];
	if (p.firmwarePath.isEmpty()) return;
	uint32_t dmrId = 0;
	const QString idText = p.dmrId->text().trimmed();
	if (!idText.isEmpty()) {
		bool ok = false;
		dmrId = idText.toUInt(&ok);
		if (!ok || dmrId == 0 || dmrId > 0xFFFFFF) {
			p.status->setText(QStringLiteral("bad DMR ID"));
			QMessageBox::warning(this, QStringLiteral("Bad DMR ID"),
					     QStringLiteral("Use a decimal DMR ID between 1 and 16777215."));
			return;
		}
	}
	saveSettings();
	p.emu->stop();
	QString error;
	if (!p.emu->loadFirmware(p.firmwarePath, p.dataPath, codecBackup_, error, dmrId)) {
		p.status->setText(QStringLiteral("load failed: ") + error);
		QMessageBox::critical(this, (side == 0 ? QStringLiteral("Radio A") : QStringLiteral("Radio B")) +
						  QStringLiteral(" load failed"), error);
		return;
	}
	p.emu->setIqEnabled(linkToggle_->isChecked());
	p.emu->setDmrTimeslot((uint8_t)tsCombo_->currentData().toUInt());
	if (p.pauseAction) {
		p.pauseAction->setChecked(false);
		p.pauseAction->setText((side == 0 ? QStringLiteral("A ") : QStringLiteral("B ")) +
				       QStringLiteral("Pause"));
	}
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
	linkStats_->setText(QStringLiteral("IQ %1 sent / %2 received / %3 dropped")
		.arg(iq_->transmitted_samples()).arg(iq_->delivered_samples()).arg(iq_->dropped_blocks()));
}

void DualRadioWindow::closeEvent(QCloseEvent *event)
{
	saveSettings();
	for (auto &p : panes_) {
		if (p.emu) p.emu->stop();
	}
	QMainWindow::closeEvent(event);
}
