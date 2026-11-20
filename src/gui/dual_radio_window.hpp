// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include "rf_channel.hpp"

#include <QMainWindow>
#include <QString>
#include <memory>

class RadioWidget;
class EmulatorThread;
class QCheckBox;
class QAction;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QTimer;
class QToolBar;

// Two independent radios with a shared virtual RF medium.
class DualRadioWindow : public QMainWindow {
public:
	explicit DualRadioWindow(QWidget *parent = nullptr);
	~DualRadioWindow() override;

	void setCodecBackup(const QString &path);
	void setAutoRestore(bool on) { auto_restore_ = on; }
	void restoreLastSession();
	void loadRadio(int side, const QString &firmware, const QString &dataBackup = QString(), uint32_t dmrId = 0);
	void loadRadioFromPath(int side, const QString &firmware);

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	struct Pane {
		QGroupBox *box = nullptr;
		RadioWidget *radio = nullptr;
		EmulatorThread *emu = nullptr;
		QLabel *title = nullptr;
		QLabel *status = nullptr;
		QLabel *pc = nullptr;
		QLineEdit *dmrId = nullptr;
		QAction *pauseAction = nullptr;
		QString firmwarePath;
		QString dataPath;
	};

	void buildUi();
	void buildPane(int side);
	void openFirmware(int side);
	void openData(int side);
	void reload(int side);
	void reloadAll();
	void chooseCodec();
	void updateLinkStats();
	void setCodecLabel();
	void loadSettings();
	void saveSettings();

	std::shared_ptr<VirtualRfIqChannel> iq_;
	Pane panes_[2];
	QString codecBackup_;
	QToolBar *toolbar_ = nullptr;
	QCheckBox *linkToggle_ = nullptr;
	QComboBox *tsCombo_ = nullptr;
	QLabel *linkStats_ = nullptr;
	QLabel *codecLabel_ = nullptr;
	QTimer *statsTimer_ = nullptr;
	bool auto_restore_ = true;
};
