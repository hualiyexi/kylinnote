#include "DisplaySettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include "MenuComboBox.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {

DisplaySettingsDialog::DisplaySettingsDialog(bool countdownDates, bool popupDetail,
                                             QWidget* parent)
    : ModalDialog(parent) {
    setObjectName(QStringLiteral("DisplaySettingsDialog"));
    setDialogTitle(QStringLiteral("显示设置"));
    setMinimumWidth(420);

    auto* completedSection = new QFrame(this);
    completedSection->setObjectName(QStringLiteral("DisplaySettingsSection"));
    auto* completedRow = new QHBoxLayout(completedSection);
    completedRow->setContentsMargins(18, 12, 18, 12);
    m_showCompleted = new QCheckBox(QStringLiteral("显示已完成和已放弃"), completedSection);
    m_showCompleted->setObjectName(QStringLiteral("ShowCompletedEntities"));
    completedRow->addWidget(m_showCompleted);
    contentLayout()->addWidget(completedSection);
    connect(m_showCompleted, &QCheckBox::toggled, this, &DisplaySettingsDialog::completedVisibilityChanged);

    auto* dateSection = new QFrame(this);
    dateSection->setObjectName(QStringLiteral("DisplaySettingsSection"));
    auto* dateRow = new QHBoxLayout(dateSection);
    dateRow->setContentsMargins(18, 12, 18, 12);
    dateRow->setSpacing(8);
    auto* dateLabel = new QLabel(QStringLiteral("日期显示为"), dateSection);
    dateRow->addWidget(dateLabel, 1);
    m_dateDisplay = new MenuComboBox(dateSection);
    m_dateDisplay->setObjectName(QStringLiteral("DateDisplayMode"));
    m_dateDisplay->setAccessibleName(QStringLiteral("日期显示方式"));
    m_dateDisplay->setFixedHeight(30);
    m_dateDisplay->addItem(QStringLiteral("任务时间"), false);
    m_dateDisplay->addItem(QStringLiteral("倒数日"), true);
    m_dateDisplay->setCurrentIndex(countdownDates ? 1 : 0);
    dateRow->addWidget(m_dateDisplay);
    contentLayout()->addWidget(dateSection);

    auto* detailSection = new QFrame(this);
    detailSection->setObjectName(QStringLiteral("DisplaySettingsSection"));
    auto* detailLayout = new QVBoxLayout(detailSection);
    detailLayout->setContentsMargins(18, 12, 18, 14);
    detailLayout->setSpacing(8);
    detailLayout->addWidget(new QLabel(QStringLiteral("任务详情样式"), detailSection));
    auto* hint = new QLabel(QStringLiteral("当窗口宽度不足时，任务详情将根据你的选择显示。"),
                            detailSection);
    hint->setObjectName(QStringLiteral("DisplaySettingsHint"));
    hint->setFont(ds::qfont(ds::FontScene::Caption));
    hint->setWordWrap(true);
    detailLayout->addWidget(hint);
    auto* choices = new QHBoxLayout;
    choices->setContentsMargins(0, 8, 0, 0);
    choices->setSpacing(24);
    m_sidebar = new QRadioButton(QStringLiteral("侧边栏"), detailSection);
    m_sidebar->setObjectName(QStringLiteral("DetailSidebar"));
    m_popup = new QRadioButton(QStringLiteral("弹窗"), detailSection);
    m_popup->setObjectName(QStringLiteral("DetailPopup"));
    m_popup->setChecked(popupDetail);
    m_sidebar->setChecked(!popupDetail);
    choices->addWidget(m_sidebar);
    choices->addWidget(m_popup);
    choices->addStretch(1);
    detailLayout->addLayout(choices);
    contentLayout()->addWidget(detailSection);

    const auto emitOptions = [this]() {
        emit optionsChanged(this->countdownDates(), this->popupDetail());
    };
    connect(m_dateDisplay, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, emitOptions);
    connect(m_sidebar, &QRadioButton::toggled, this, emitOptions);
    applyTheme();
}

bool DisplaySettingsDialog::countdownDates() const {
    return m_dateDisplay->currentData().toBool();
}

bool DisplaySettingsDialog::popupDetail() const {
    return m_popup->isChecked();
}

void DisplaySettingsDialog::setCompletedVisible(bool visible) {
    const QSignalBlocker blocker(m_showCompleted);
    m_showCompleted->setChecked(visible);
}

void DisplaySettingsDialog::applyTheme() {
    ModalDialog::applyTheme();
    const auto& theme = ThemeManager::instance();
    card()->setStyleSheet(card()->styleSheet() + QStringLiteral(
        "QFrame#DisplaySettingsSection{background:%1;border:0;border-radius:6px;}"
        "QLabel#DisplaySettingsHint{color:%2;}"
        "QRadioButton{background:transparent;}")
        .arg(theme.color(ds::ColorToken::Field).name(),
             theme.color(ds::ColorToken::TextTertiary).name()));
}

}  // namespace kl
