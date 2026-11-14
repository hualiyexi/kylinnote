#include "MenuComboBox.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QActionGroup>
#include <QPainter>
#include "AppIcons.h"
#include "PopupMenu.h"
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

void MenuComboBox::paintEvent(QPaintEvent* event) {
    QComboBox::paintEvent(event);
    QPainter painter(this);
    const QRectF arrow(width() - 20, (height() - 12) / 2.0, 12, 12);
    AppIcons::paint(&painter, IconId::ChevronDown, arrow,
                    ThemeManager::instance().color(ds::ColorToken::TextSecondary));
}

}  // namespace kl
