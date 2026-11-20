// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#include "radio_widget.hpp"

#include <QFont>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QtMath>

namespace {

constexpr double CANVAS_W = 520.0;
constexpr double CANVAS_H = 980.0;

const QRectF BODY(70, 60, 380, 880);
const QRectF SCREEN_BEZEL(126, 110, 268, 332);
const QRectF SCREEN(140, 116, 240, 320);

QPointF toCanvas(const QPointF &p, double scale, const QPointF &origin)
{
	return (p - origin) / scale;
}

} // namespace

RadioWidget::RadioWidget(QWidget *parent) : QWidget(parent)
{
	setFocusPolicy(Qt::StrongFocus);
	setMouseTracking(true);
	rebuild();
}

void RadioWidget::rebuild()
{
	buttons_.clear();
	const double kx[3] = {146, 236, 326};
	const double ky[4] = {574, 642, 710, 778};
	const double kw = 72, kh = 54;
	const QColor num("#2d3239");
	const QColor green("#2f8f57");
	const QColor red("#a83232");
	const QColor nav("#353b45");

	auto add = [&](const QString &name, const QString &label, const QRectF &rect,
		       const QColor &color, int fontPx = 18, const QString &sub = {}) {
		buttons_.push_back({name, label, rect, color, false, fontPx, sub, 10});
	};
	auto key = [&](const QString &name, const QString &label, int col, int row,
		       const QString &sub = {}, int fontPx = 19) {
		add(name, label, QRectF(kx[col], ky[row], kw, kh), num, fontPx, sub);
	};

	// The four keys in the row under the screen: the left soft/menu key, the
	// arrows printed P1/P2, and the green up/down navigation. BACK is the red
	// button below the soft key, as on the radio.
	add("menu", "MENU", QRectF(120, 470, 64, 44), green, 11);
	add("left", "P1", QRectF(196, 470, 60, 44), nav, 15, "\u25C0");
	add("right", "P2", QRectF(262, 470, 60, 44), nav, 15, "\u25B6");
	add("up", "\u25B2", QRectF(328, 470, 40, 40), green, 19);
	add("down", "\u25BC", QRectF(328, 514, 40, 40), nav, 16);
	add("back", "BACK", QRectF(120, 514, 64, 40), red, 11);

	key("1", "1", 0, 0);
	key("2", "2", 1, 0, "ABC");
	key("3", "3", 2, 0, "DEF");
	key("4", "4", 0, 1, "GHI");
	key("5", "5", 1, 1, "JKL");
	key("6", "6", 2, 1, "MNO");
	key("7", "7", 0, 2, "PQRS");
	key("8", "8", 1, 2, "TUV");
	key("9", "9", 2, 2, "WXYZ");
	key("*", "*", 0, 3, {}, 20);
	key("0", "0", 1, 3, "+", 19);
	key("#", "#", 2, 3, {}, 20);

	buttons_.push_back({"ptt", "PTT", QRectF(30, 340, 34, 132), QColor("#b06426"), true, 14, {}, 10});
	buttons_.push_back({"sk1", "SK1", QRectF(456, 340, 34, 70), QColor("#3b4048"), true, 13, {}, 10});
	buttons_.push_back({"sk2", "SK2", QRectF(456, 420, 34, 70), QColor("#3b4048"), true, 13, {}, 10});
}

QSize RadioWidget::sizeHint() const { return QSize(560, 980); }
QSize RadioWidget::minimumSizeHint() const { return QSize(380, 760); }

void RadioWidget::setFrame(const QImage &frame)
{
	frame_ = frame;
	update();
}

const RadioWidget::Button *RadioWidget::hitTest(const QPointF &canvas) const
{
	for (const auto &b : buttons_) {
		if (b.rect.contains(canvas)) return &b;
	}
	return nullptr;
}

void RadioWidget::releasePressed()
{
	if (!pressed_.isEmpty()) {
		emit keyChanged(pressed_, false);
		pressed_.clear();
	}
}

void RadioWidget::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.fillRect(rect(), palette().window());

	const double scale = qMin(width() / CANVAS_W, height() / CANVAS_H);
	const QPointF origin((width() - CANVAS_W * scale) / 2.0, (height() - CANVAS_H * scale) / 2.0);
	p.translate(origin);
	p.scale(scale, scale);

	// Body and a little shadow.
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(0, 0, 0, 90));
	p.drawRoundedRect(BODY.adjusted(8, 10, 10, 14), 42, 42);
	QLinearGradient bodyGrad(BODY.topLeft(), BODY.bottomRight());
	bodyGrad.setColorAt(0.0, QColor("#252a31"));
	bodyGrad.setColorAt(0.55, QColor("#1b1f25"));
	bodyGrad.setColorAt(1.0, QColor("#121519"));
	p.setBrush(bodyGrad);
	p.drawRoundedRect(BODY, 38, 38);
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(QColor("#3a424d"), 2));
	p.drawRoundedRect(BODY.adjusted(2, 2, -2, -2), 36, 36);

	// Antenna and the two top knobs.
	p.setPen(QPen(QColor(0, 0, 0, 80), 9, Qt::SolidLine, Qt::RoundCap));
	p.drawLine(QPointF(248, 0), QPointF(248, 66));
	p.setPen(QPen(QColor("#1a1e24"), 6, Qt::SolidLine, Qt::RoundCap));
	p.drawLine(QPointF(248, 0), QPointF(248, 66));
	p.setPen(QPen(QColor("#4a525e"), 2, Qt::SolidLine, Qt::RoundCap));
	p.drawLine(QPointF(245, 4), QPointF(245, 56));
	auto knob = [&](double cx, double cy, double radius) {
		QRadialGradient grad(QPointF(cx - radius * 0.35, cy - radius * 0.4), radius * 1.8);
		grad.setColorAt(0.0, QColor("#4b535e"));
		grad.setColorAt(0.55, QColor("#292e35"));
		grad.setColorAt(1.0, QColor("#101318"));
		p.setPen(Qt::NoPen);
		p.setBrush(grad);
		p.drawEllipse(QPointF(cx, cy), radius, radius);
		p.setPen(QPen(QColor("#0c0e12"), 2));
		for (int i = -2; i <= 2; i++)
			p.drawLine(QPointF(cx - radius * 0.55, cy + i * radius * 0.28),
				   QPointF(cx + radius * 0.55, cy + i * radius * 0.28));
	};
	knob(192, 38, 18);
	knob(310, 38, 20);

	// Brand and speaker grille.
	p.setPen(QColor("#d6b45a"));
	QFont brand("DejaVu Sans", 1);
	brand.setPixelSize(24);
	brand.setBold(true);
	p.setFont(brand);
	p.drawText(QRectF(BODY.left(), 62, BODY.width(), 28), Qt::AlignHCenter | Qt::AlignVCenter,
			 "DM-32UV");
	p.setPen(QPen(QColor("#454d58"), 2));
	for (int i = 0; i < 4; i++) {
		p.drawLine(QPointF(214, 94 + i * 5), QPointF(306, 94 + i * 5));
	}

	// Screen recess and image.
	p.setPen(QPen(QColor("#050607"), 2));
	p.setBrush(QColor("#050607"));
	p.drawRoundedRect(SCREEN_BEZEL, 10, 10);
	if (!frame_.isNull()) {
		p.setRenderHint(QPainter::SmoothPixmapTransform, false);
		p.drawImage(SCREEN, frame_);
	} else {
		p.setPen(QColor("#59616d"));
		QFont f("DejaVu Sans");
		f.setPixelSize(14);
		p.setFont(f);
		p.drawText(SCREEN, Qt::AlignCenter, "Open a firmware image");
	}

	// Buttons.
	for (const auto &b : buttons_) {
		const bool down = (b.name == pressed_);
		const bool hover = has_mouse_ && b.rect.contains(mouse_canvas_);
		QColor fill = down ? b.color.darker(145) : (hover ? b.color.lighter(118) : b.color);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 70));
		p.drawRoundedRect(b.rect.translated(0, 2), 9, 9);
		p.setBrush(fill);
		p.drawRoundedRect(b.rect, 9, 9);
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(down ? b.color.darker(180) : QColor(255, 255, 255, 30), 1));
		p.drawRoundedRect(b.rect.adjusted(0.5, 0.5, -0.5, -0.5), 9, 9);

		p.setPen(QColor("#f2f3f5"));
		QFont f("DejaVu Sans");
		f.setPixelSize(b.fontPx);
		f.setBold(b.fontPx >= 16 || b.name == "ptt");
		p.setFont(f);
		if (b.vertical) {
			p.save();
			p.translate(b.rect.center());
			p.rotate(-90);
			p.drawText(QRectF(-b.rect.height() / 2, -b.rect.width() / 2, b.rect.height(), b.rect.width()),
					 Qt::AlignCenter, b.label);
			p.restore();
		} else if (!b.sublabel.isEmpty()) {
			p.drawText(b.rect.adjusted(0, 2, 0, -12), Qt::AlignCenter, b.label);
			QFont small("DejaVu Sans");
			small.setPixelSize(b.subPx);
			small.setBold(true);
			p.setFont(small);
			p.setPen(QColor("#b8c0cb"));
			p.drawText(b.rect.adjusted(0, 24, 0, -2), Qt::AlignCenter, b.sublabel);
		} else {
			p.drawText(b.rect, Qt::AlignCenter, b.label);
		}
	}

	// Small model line at the bottom.
	p.setPen(QColor("#6b7480"));
	QFont small("DejaVu Sans");
	small.setPixelSize(11);
	p.setFont(small);
	p.drawText(QRectF(BODY.left(), BODY.bottom() - 38, BODY.width(), 24),
			 Qt::AlignHCenter | Qt::AlignVCenter, "DM32EMU  -  C-SKY CK803S");
}

QString RadioWidget::keyForEvent(QKeyEvent *event) const
{
	switch (event->key()) {
	case Qt::Key_Return:
	case Qt::Key_Enter:
	case Qt::Key_M: return "menu";
	case Qt::Key_Escape:
	case Qt::Key_Backspace:
	case Qt::Key_B: return "back";
	case Qt::Key_Left: return "left";
	case Qt::Key_Right: return "right";
	case Qt::Key_Up: return "up";
	case Qt::Key_Down: return "down";
	case Qt::Key_Asterisk: return "*";
	case Qt::Key_NumberSign: return "#";
	case Qt::Key_0: return "0";
	case Qt::Key_1: return "1";
	case Qt::Key_2: return "2";
	case Qt::Key_3: return "3";
	case Qt::Key_4: return "4";
	case Qt::Key_5: return "5";
	case Qt::Key_6: return "6";
	case Qt::Key_7: return "7";
	case Qt::Key_8: return "8";
	case Qt::Key_9: return "9";
	case Qt::Key_P: return "ptt";
	case Qt::Key_F1: return "sk1";
	case Qt::Key_F2: return "sk2";
	default: break;
	}
	if (event->text() == "#") return "#";
	if (event->text() == "*") return "*";
	return {};
}

void RadioWidget::keyPressEvent(QKeyEvent *event)
{
	if (event->isAutoRepeat()) {
		event->accept();
		return;
	}
	const QString name = keyForEvent(event);
	if (!name.isEmpty()) {
		if (name != pressed_) {
			releasePressed();
			pressed_ = name;
			emit keyChanged(name, true);
			update();
		}
		event->accept();
		return;
	}
	QWidget::keyPressEvent(event);
}

void RadioWidget::keyReleaseEvent(QKeyEvent *event)
{
	if (event->isAutoRepeat()) {
		event->accept();
		return;
	}
	const QString name = keyForEvent(event);
	if (!name.isEmpty() && name == pressed_) {
		releasePressed();
		update();
		event->accept();
		return;
	}
	QWidget::keyReleaseEvent(event);
}

void RadioWidget::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton) {
		QWidget::mousePressEvent(event);
		return;
	}
	const double scale = qMin(width() / CANVAS_W, height() / CANVAS_H);
	const QPointF origin((width() - CANVAS_W * scale) / 2.0, (height() - CANVAS_H * scale) / 2.0);
	const QPointF canvas = toCanvas(event->position(), scale, origin);
	if (const Button *b = hitTest(canvas)) {
		releasePressed();
		pressed_ = b->name;
		emit keyChanged(pressed_, true);
		update();
		event->accept();
		return;
	}
	QWidget::mousePressEvent(event);
}

void RadioWidget::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton && !pressed_.isEmpty()) {
		releasePressed();
		update();
		event->accept();
		return;
	}
	QWidget::mouseReleaseEvent(event);
}

void RadioWidget::mouseMoveEvent(QMouseEvent *event)
{
	const double scale = qMin(width() / CANVAS_W, height() / CANVAS_H);
	const QPointF origin((width() - CANVAS_W * scale) / 2.0, (height() - CANVAS_H * scale) / 2.0);
	mouse_canvas_ = toCanvas(event->position(), scale, origin);
	has_mouse_ = true;
	update();
	QWidget::mouseMoveEvent(event);
}

void RadioWidget::leaveEvent(QEvent *event)
{
	has_mouse_ = false;
	releasePressed();
	update();
	QWidget::leaveEvent(event);
}
