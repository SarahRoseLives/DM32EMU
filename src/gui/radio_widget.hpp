// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Sarah Rose
#pragma once

#include <QColor>
#include <QImage>
#include <QRectF>
#include <QString>
#include <QWidget>
#include <vector>

class RadioWidget : public QWidget {
	Q_OBJECT
public:
	explicit RadioWidget(QWidget *parent = nullptr);

	void setFrame(const QImage &frame);
	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

signals:
	void keyChanged(const QString &name, bool pressed);

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void leaveEvent(QEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void keyReleaseEvent(QKeyEvent *event) override;

private:
	struct Button {
		QString name;
		QString label;
		QRectF rect;
		QColor color;
		bool vertical = false;
		int fontPx = 18;
	};

	void rebuild();
	const Button *hitTest(const QPointF &canvas) const;
	void releasePressed();
	QString keyForEvent(QKeyEvent *event) const;

	QImage frame_;
	std::vector<Button> buttons_;
	QString pressed_;
	QPointF mouse_canvas_{-1, -1};
	bool has_mouse_ = false;
};
