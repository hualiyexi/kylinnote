#include "SmartListSettingsWidget.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include "MenuComboBox.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {

SmartListSettingsWidget::SmartListSettingsWidget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("SmartListSettingsPage"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 22, 24, 22);
    root->setSpacing(14);
    auto* title = new QLabel(QStringLiteral("智能清单"), this);
    title->setObjectName(QStringLiteral("SmartListSettingsTitle"));
    title->setFont(ds::qfont(ds::FontScene::Section));
    root->addWidget(title);

    root->addWidget(buildSection({
        {QStringLiteral("all"), QStringLiteral("所有"), IconId::All},
        {QStringLiteral("today"), QStringLiteral("今天"), IconId::Today},
        {QStringLiteral("tomorrow"), QStringLiteral("明天"), IconId::Tomorrow},
        {QStringLiteral("next7days"), QStringLiteral("最近7天"), IconId::Upcoming},
        {QStringLiteral("inbox"), QStringLiteral("收集箱"), IconId::Inbox},
    }));
    root->addWidget(buildSection({
        {QStringLiteral("overdue"), QStringLiteral("已过期"), IconId::Overdue},
        {QStringLiteral("completed"), QStringLiteral("已完成"), IconId::Done},
        {QStringLiteral("abandoned"), QStringLiteral("已放弃"), IconId::Abandoned},
        {QStringLiteral("trash"), QStringLiteral("垃圾桶"), IconId::Trash},
    }));
    root->addStretch(1);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, &SmartListSettingsWidget::applyTheme);
    applyTheme();
}

QFrame* SmartListSettingsWidget::buildSection(const QVector<Entry>& entries) {
    auto* section = new QFrame(this);
    section->setObjectName(QStringLiteral("SmartListSettingsSection"));
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 4, 0, 4);
    layout->setSpacing(0);
    for (Entry entry : entries) {
        auto* row = new QWidget(section);
        row->setMinimumHeight(40);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(18, 4, 14, 4);
        rowLayout->setSpacing(8);
        entry.iconLabel = new QLabel(row);
        entry.iconLabel->setFixedSize(20, 20);
        rowLayout->addWidget(entry.iconLabel);
        auto* label = new QLabel(entry.label, row);
        label->setFont(ds::qfont(ds::FontScene::Body));
        rowLayout->addWidget(label, 1);

        entry.combo = new MenuComboBox(row);
        entry.combo->setObjectName(QStringLiteral("SmartVisibility_") + entry.key);
        entry.combo->setAccessibleName(entry.label + QStringLiteral("显示方式"));
        entry.combo->setToolTip(QStringLiteral("显示：始终保留入口\n有内容时显示：仅有实体时保留入口\n隐藏：不显示入口"));
        entry.combo->addItem(QStringLiteral("显示"),
                             static_cast<int>(Settings::SmartListVisibility::Show));
        entry.combo->addItem(QStringLiteral("有内容时显示"),
                             static_cast<int>(Settings::SmartListVisibility::WhenNonEmpty));
        entry.combo->addItem(QStringLiteral("隐藏"),
                             static_cast<int>(Settings::SmartListVisibility::Hide));
        entry.combo->setMinimumWidth(112);
        entry.combo->setFixedHeight(30);
        const int current = entry.combo->findData(
            static_cast<int>(Settings::instance().smartListVisibility(entry.key)));
        entry.combo->setCurrentIndex(qMax(0, current));
        rowLayout->addWidget(entry.combo);
        layout->addWidget(row);
        const QString key = entry.key;
        QComboBox* combo = entry.combo;
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, key, combo](int index) {
            const auto value = static_cast<Settings::SmartListVisibility>(
                combo->itemData(index).toInt());
            Settings::instance().setSmartListVisibility(key, value);
            emit visibilityChanged();
        });
        m_entries.append(entry);
    }
    return section;
}

void SmartListSettingsWidget::applyTheme() {
    setStyleSheet(QStringLiteral(
        "QFrame#SmartListSettingsSection{background:%1;border:0;border-radius:8px;}"
        "QFrame#SmartListSettingsSection QWidget{background:transparent;}")
        .arg(ThemeManager::instance().color(ds::ColorToken::Field).name()));
    for (const Entry& entry : m_entries)
        entry.iconLabel->setPixmap(
            AppIcons::icon(entry.icon, ds::ColorToken::TextSecondary, 20).pixmap(20, 20));
}

}  // namespace kl
