// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The DM32EMU Authors
#include "emulator_thread.hpp"
#include "radio_widget.hpp"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QPalette>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QStringList>
#include <QTimer>
#include <QToolBar>

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
	void openFirmware();
	void openDataFlash();
	void saveScreenshot();
	void togglePause(bool paused);
	void resetEmulator();
	void onFrame(const QImage &image, quint64 frames);
	void onFault(const QString &text);

	EmulatorThread emulator_;
	RadioWidget *radio_ = nullptr;
	QString firmware_;
	QString data_flash_;
	QImage last_frame_;
	QString screenshot_path_;
	QString window_screenshot_path_;
	int screenshot_frames_ = 3;
	quint64 frames_seen_ = 0;
	QAction *pause_action_ = nullptr;
	QLabel *frame_label_ = nullptr;
};

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
	setWindowTitle("DM32EMU");
	setMinimumSize(480, 720);
	resize(600, 940);

	radio_ = new RadioWidget(this);
	setCentralWidget(radio_);
	createActions();

	statusBar()->showMessage(QStringLiteral("Open a firmware image to begin"));
	frame_label_ = new QLabel(QStringLiteral("Frame 0"), this);
	statusBar()->addPermanentWidget(frame_label_);

	connect(&emulator_, &EmulatorThread::frameReady, this, &MainWindow::onFrame);
	connect(&emulator_, &EmulatorThread::statusMessage, this,
		[this](const QString &text) { statusBar()->showMessage(text, 5000); });
	connect(&emulator_, &EmulatorThread::fault, this, &MainWindow::onFault);
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
	emulation->addAction(reset);

	QToolBar *toolbar = addToolBar(QStringLiteral("Main"));
	toolbar->setMovable(false);
	toolbar->addAction(open);
	toolbar->addAction(pause_action_);
	toolbar->addAction(reset);
	toolbar->addAction(shot);
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
	radio_->setFrame(QImage());
	frame_label_->setText(QStringLiteral("Frame 0"));
	if (pause_action_ && pause_action_->isChecked()) {
		pause_action_->setChecked(false);
	}
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

void MainWindow::togglePause(bool paused)
{
	emulator_.setPaused(paused);
	pause_action_->setText(paused ? QStringLiteral("&Resume") : QStringLiteral("&Pause"));
	statusBar()->showMessage(paused ? QStringLiteral("Paused") : QStringLiteral("Running"), 3000);
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

void MainWindow::onFault(const QString &text)
{
	statusBar()->showMessage(QStringLiteral("CPU fault"), 10000);
	QMessageBox::critical(this, QStringLiteral("Emulator fault"), text);
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

	if (parser.isSet(dataFlash)) {
		window.setDataFlash(parser.value(dataFlash));
	}
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
