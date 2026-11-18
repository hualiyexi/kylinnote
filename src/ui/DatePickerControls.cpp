#include "DatePickerControls.h"

#include <QAbstractSpinBox>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStyle>
#include <QStylePainter>
#include <QTime>
#include <QToolButton>

#include "AppIcons.h"
#include "MenuComboBox.h"
#include "TextGeometry.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace DatePickerUi {

QPushButton* actionButton(const QString& text, const char* objectName, QWidget* parent, int height)
{
    auto* button = new QPushButton(text, parent);
    button->setObjectName(QString::fromLatin1(objectName));
    button->setAutoDefault(false);
    button->setDefault(false);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setFixedHeight(height);
    button->setFont(ds::qfont(ds::FontScene::Menu));
    return button;
}

MenuComboBox* comboBox(const QString& accessibleName, QWidget* parent)
{
    auto* combo = new MenuComboBox(parent);
    combo->setFixedHeight(kControlHeight);
    combo->setAccessibleName(accessibleName);
    combo->setFont(ds::qfont(ds::FontScene::Menu));
    return combo;
}

QSpinBox* spinBox(int minimum, int maximum, const QString& accessibleName, QWidget* parent)
{
    auto* spin = new QSpinBox(parent);
    spin->setFixedHeight(kControlHeight);
    spin->setRange(minimum, maximum);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    spin->setAccessibleName(accessibleName);
    spin->setFont(ds::qfont(ds::FontScene::Menu));
    return spin;
}

QLineEdit* fieldInput(const QString& placeholder, const QString& accessibleName, QWidget* parent)
{
    auto* edit = new QLineEdit(parent);
    edit->setObjectName(QStringLiteral("DatePickerFieldInput"));
    edit->setFixedHeight(kControlHeight);
    edit->setPlaceholderText(placeholder);
    edit->setAccessibleName(accessibleName);
    edit->setFont(ds::qfont(ds::FontScene::Menu));
    return edit;
}

bool parseTimeText(const QString& text, QTime* result)
{
    if (!result) return false;
    static const QRegularExpression format(
        QStringLiteral("^([01]?[0-9]|2[0-3]):([0-5]?[0-9])$"));
    const QRegularExpressionMatch match = format.match(text.trimmed());
    if (!match.hasMatch()) {
        *result = QTime();
        return false;
    }
    const QTime parsed(match.captured(1).toInt(), match.captured(2).toInt());
    if (!parsed.isValid()) {
        *result = QTime();
        return false;
    }
    *result = parsed;
    return true;
}

QToolButton* toggleButton(const QString& text, QWidget* parent, const QSize& size)
{
    auto* button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("DatePickerToggle"));
    button->setCheckable(true);
    button->setText(text);
    button->setFixedSize(size);
    button->setAccessibleName(text);
    button->setFont(ds::qfont(ds::FontScene::Menu));
    return button;
}

void repolish(QWidget* widget)
{
    if (!widget || !widget->style()) return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

void setInputInvalid(QLineEdit* edit, bool invalid, const QString& reason)
{
    edit->setProperty("invalid", invalid);
    edit->setToolTip(invalid ? reason : QString());
    repolish(edit);
}

} // namespace DatePickerUi

DatePickerMenuRow::DatePickerMenuRow(const QString& text, QWidget* parent)
    : QPushButton(text, parent)
{
    setObjectName(QStringLiteral("DatePickerMenuRow"));
    setProperty("datePickerMenuRow", true);
    setCheckable(true);
    setAutoDefault(false);
    setDefault(false);
    setFixedHeight(DatePickerUi::kRowHeight);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setFont(ds::qfont(ds::FontScene::Menu));
    setAccessibleName(text);
}

void DatePickerMenuRow::setSecondaryText(const QString& text)
{
    if (m_secondaryText == text) return;
    m_secondaryText = text;
    update();
}

void DatePickerMenuRow::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        event->accept();
        click();
        return;
    }
    if ((event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) && parentWidget()) {
        const auto rows = parentWidget()->findChildren<DatePickerMenuRow*>(QString(), Qt::FindDirectChildrenOnly);
        const int current = rows.indexOf(this);
        const int direction = event->key() == Qt::Key_Up ? -1 : 1;
        for (int step = 1; current >= 0 && step <= rows.size(); ++step) {
            DatePickerMenuRow* row = rows.at((current + direction * step + rows.size()) % rows.size());
            if (row->isVisible() && row->isEnabled()) {
                row->setFocus();
                break;
            }
        }
        event->accept();
        return;
    }
    QPushButton::keyPressEvent(event);
}

void DatePickerMenuRow::paintEvent(QPaintEvent*)
{
    QStyleOptionButton option;
    initStyleOption(&option);
    option.icon = QIcon();
    option.text.clear();
    QStylePainter painter(this);
    painter.drawControl(QStyle::CE_PushButton, option);

    auto& theme = ThemeManager::instance();
    const QColor foreground = theme.color(!isEnabled() ? ds::ColorToken::TextTertiary
        : isChecked() ? ds::ColorToken::Primary : ds::ColorToken::Text);
    const QRectF content = QRectF(rect()).adjusted(12, 0, -12, 0);
    const qreal center = content.center().y();
    const QFontMetricsF mainMetrics(font());
    QFont secondaryFont = font();
    secondaryFont.setPointSizeF(qMax(9.0, secondaryFont.pointSizeF() - 1.0));
    const QFontMetricsF secondaryMetrics(secondaryFont);
    const qreal checkWidth = isChecked() ? 22.0 : 0.0;
    const qreal secondaryWidth = m_secondaryText.isEmpty()
        ? 0.0 : secondaryMetrics.horizontalAdvance(m_secondaryText) + 4.0;
    const qreal mainWidth = qMax(0.0, content.width() - checkWidth - secondaryWidth);
    const QString main = mainMetrics.elidedText(text(), Qt::ElideRight, static_cast<int>(mainWidth));

    painter.setFont(font());
    painter.setPen(foreground);
    painter.drawText(QPointF(content.left(), textBaselineForCenter(mainMetrics, center, main)), main);
    if (!m_secondaryText.isEmpty() && mainWidth > 24.0) {
        const qreal x = content.left() + mainMetrics.horizontalAdvance(main) + 4.0;
        painter.setFont(secondaryFont);
        painter.setPen(theme.color(ds::ColorToken::TextTertiary));
        const QString secondary = secondaryMetrics.elidedText(
            m_secondaryText, Qt::ElideRight,
            static_cast<int>(qMax(0.0, content.right() - checkWidth - x)));
        painter.drawText(QPointF(x, textBaselineForCenter(secondaryMetrics, center, secondary)),
                         secondary);
    }
    if (isChecked()) {
        AppIcons::paint(&painter, IconId::Check,
                        QRectF(content.right() - 18.0, center - 9.0, 18.0, 18.0),
                        theme.color(ds::ColorToken::Primary));
    }
}

} // namespace kl
