#pragma once

#include <QCheckBox>
#include <QColor>
#include <QPainter>
#include "design/ThemeManager.h"

namespace kl {

// 日期与通用设置共用的紧凑开关；业务状态仍由所属编辑器管理。
class CompactSwitch final : public QCheckBox {
public:
    explicit CompactSwitch(QWidget* parent = nullptr) : QCheckBox(parent) {
        setFixedSize(30, 18);
        setCursor(Qt::PointingHandCursor);
    }
    QSize sizeHint() const override { return QSize(30, 18); }
    QSize minimumSizeHint() const override { return QSize(30, 18); }

protected:
    bool hitButton(const QPoint& point) const override { return rect().contains(point); }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        auto& theme = ThemeManager::instance();
        QColor track = theme.color(isChecked() ? ds::ColorToken::Primary
                                               : ds::ColorToken::TextTertiary);
        if (!isChecked()) track.setAlpha(underMouse() ? 64 : 40);
        if (!isEnabled()) track.setAlpha(24);
        painter.setPen(Qt::NoPen);
        painter.setBrush(track);
        painter.drawRoundedRect(QRectF(rect()), 9, 9);
        painter.setBrush(theme.color(ds::ColorToken::OnPrimary));
        const qreal x = isChecked() ? width() - 17.0 : 1.0;
        painter.drawEllipse(QRectF(x, 1, 16, 16));
    }
};

} // namespace kl
