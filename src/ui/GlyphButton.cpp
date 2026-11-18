#include "GlyphButton.h"

#include <QPainter>

#include "AppIcons.h"
#include "ListAppearance.h"
#include "design/ThemeManager.h"

namespace kl {

GlyphButton::GlyphButton(QWidget* parent) : QToolButton(parent) {}

void GlyphButton::setGlyph(const QString& glyph)
{
    m_glyph = glyph;
    update();
}

void GlyphButton::setDefaultIconName(const QString& name)
{
    m_defaultIconName = name;
    update();
}

void GlyphButton::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto token = [](ds::ColorToken value) {
        return ThemeManager::instance().color(value);
    };
    const QRectF frame = QRectF(rect()).adjusted(1, 1, -1, -1);
    if (underMouse()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(token(ds::ColorToken::Hover));
        painter.drawRoundedRect(frame, 8, 8);
    }
    if (hasFocus() && !underMouse()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(token(ds::ColorToken::Hover));
        painter.drawRoundedRect(frame, 8, 8);
    }
    const QRectF glyphRect(rect().center().x() - 9, rect().center().y() - 9, 18, 18);
    if (m_glyph.isEmpty()) {
        AppIcons::paint(&painter, m_defaultIconName, glyphRect, token(ds::ColorToken::TextSecondary));
    } else {
        TaskList list;
        list.icon = m_glyph;
        ListAppearance::paint(&painter, list, glyphRect, token(ds::ColorToken::TextSecondary));
    }
}

} // namespace kl
