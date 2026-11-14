#include "SettingsDialog.h"

#include <QHBoxLayout>
#include <QListWidget>
#include <QStackedWidget>
#include "AppIcons.h"
#include "SmartListSettingsWidget.h"
#include "design/ThemeManager.h"

namespace kl {

SettingsDialog::SettingsDialog(QWidget* parent) : ModalDialog(parent) {
    setObjectName(QStringLiteral("SettingsDialog"));
    setDialogTitle(QStringLiteral("设置"));
    setMinimumSize(700, 520);
    resize(740, 570);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(12);
    m_categories = new QListWidget(this);
    m_categories->setObjectName(QStringLiteral("SettingsCategories"));
    m_categories->setFixedWidth(168);
    m_categories->setIconSize(QSize(20, 20));
    auto* smart = new QListWidgetItem(
        AppIcons::icon(IconId::SmartSettings, ds::ColorToken::TextSecondary, 20),
        QStringLiteral("智能清单"), m_categories);
    smart->setData(Qt::UserRole, 0);
    m_categories->setCurrentRow(0);
    body->addWidget(m_categories);

    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("SettingsPages"));
    m_smartLists = new SmartListSettingsWidget(this);
    m_pages->addWidget(m_smartLists);
    body->addWidget(m_pages, 1);
    contentLayout()->addLayout(body, 1);
    connect(m_categories, &QListWidget::currentRowChanged,
            m_pages, &QStackedWidget::setCurrentIndex);
    connect(m_smartLists, &SmartListSettingsWidget::visibilityChanged,
            this, &SettingsDialog::smartListVisibilityChanged);
    applyTheme();
}

void SettingsDialog::applyTheme() {
    ModalDialog::applyTheme();
    auto& theme = ThemeManager::instance();
    m_categories->setStyleSheet(QStringLiteral(
        "QListWidget#SettingsCategories{background:%1;border:0;border-radius:8px;outline:0;padding:6px;}"
        "QListWidget#SettingsCategories::item{height:36px;padding:0 10px;border-radius:7px;color:%2;}"
        "QListWidget#SettingsCategories::item:selected{background:%3;color:%4;}"
        "QListWidget#SettingsCategories::item:hover{background:%5;}")
        .arg(theme.color(ds::ColorToken::Field).name())
        .arg(theme.color(ds::ColorToken::TextSecondary).name())
        .arg(theme.color(ds::ColorToken::Selection).name())
        .arg(theme.color(ds::ColorToken::Text).name())
        .arg(theme.color(ds::ColorToken::Hover).name()));
}

}  // namespace kl
