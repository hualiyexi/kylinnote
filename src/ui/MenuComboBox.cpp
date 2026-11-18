#include "MenuComboBox.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QActionGroup>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include "AppIcons.h"
#include "PopupMenu.h"
#include "TextGeometry.h"
#include "design/ThemeManager.h"

namespace kl {

MenuComboBox::MenuComboBox(QWidget* parent) : QComboBox(parent) {
    setStyleSheet(QStringLiteral("QComboBox::down-arrow{image:none;}"));
}

void MenuComboBox::showPopup() {
    if (count() == 0 || (m_popup && m_popup->isVisible())) return;
    auto* menu = new PopupMenu(this);
    menu->setObjectName(QStringLiteral("ComboChoiceMenu"));
    m_popup = menu;
    auto* group = new QActionGroup(menu);
    group->setExclusive(true);
    for (int row = 0; row < count(); ++row) {
        QAction* action = menu->addAction(itemIcon(row), itemText(row));
        action->setData(row);
        action->setCheckable(true);
        action->setChecked(row == currentIndex());
        action->setEnabled(model()->flags(model()->index(row, modelColumn(), rootModelIndex()))
                               .testFlag(Qt::ItemIsEnabled));
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, row]() {
            setCurrentIndex(row);
            emit activated(row);
            emit activated(itemText(row));
        });
    }
    connect(menu, &QMenu::aboutToHide, this, [this, menu]() {
        if (m_popup == menu) m_popup.clear();
        QComboBox::hidePopup();
        menu->deleteLater();
    });
    menu->setMinimumWidth(width() + 2 * PopupMenu::shadowMargin());
    menu->popup(mapToGlobal(QPoint(0, height())));
}

void MenuComboBox::hidePopup() {
    if (m_popup) m_popup->hide();
    QComboBox::hidePopup();
}

void MenuComboBox::paintEvent(QPaintEvent*) {
    QStylePainter painter(this);
    QStyleOptionComboBox option;
    initStyleOption(&option);
    option.state &= ~QStyle::State_HasFocus;
    painter.drawComplexControl(QStyle::CC_ComboBox, option);
    const QRect field = style()->subControlRect(QStyle::CC_ComboBox, &option,
                                                QStyle::SC_ComboBoxEditField, this);
    // QSS 的垂直 padding 在 Qt 5.12 中会把 CE_ComboBoxLabel 压到小于实际
    // 字体墨迹。水平仍尊重各区域的 compact/普通样式，垂直统一按完整控件居中。
    QRectF label(field.left(), 0, field.width(), height());
    label.setRight(qMin(label.right(), static_cast<qreal>(width() - 24)));
    if (label.isEmpty()) return;
    const qreal center = label.center().y();
    if (!option.currentIcon.isNull() && label.width() > 26) {
        const QRect glyph(qRound(label.left()), qRound(center - 9), 18, 18);
        option.currentIcon.paint(&painter, glyph, Qt::AlignCenter,
                                 isEnabled() ? QIcon::Normal : QIcon::Disabled);
        label.adjust(26, 0, 0, 0);
    }
    painter.setFont(font());
    painter.setPen(option.palette.text().color());
    const QFontMetricsF metrics(font());
    const QString shown = metrics.elidedText(option.currentText, Qt::ElideRight,
                                             qMax(0, static_cast<int>(label.width())));
    painter.drawText(QPointF(label.left(), textBaselineForCenter(metrics, center, shown)), shown);
    const QRectF arrow(width() - 20, (height() - 12) / 2.0, 12, 12);
    AppIcons::paint(&painter, IconId::ChevronDown, arrow,
                    ThemeManager::instance().color(ds::ColorToken::TextSecondary));
}

}  // namespace kl
