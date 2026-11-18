#include "SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QVBoxLayout>
#include "../app/AutostartSettings.h"
#include "AppIcons.h"
#include "CompactSwitch.h"
#include "MenuComboBox.h"
#include "SmartListSettingsWidget.h"
#include "design/ThemeManager.h"
#include "design/FontManager.h"
#include "design/TypeSystem.h"

namespace kl {

SettingsDialog::SettingsDialog(AutostartSettings* autostart, QWidget* parent)
    : ModalDialog(parent), m_autostart(autostart) {
    setObjectName(QStringLiteral("SettingsDialog"));
    setDialogTitle(QStringLiteral("设置"));
    setMinimumSize(700, 520);
    resize(800, 680);

    m_leftSurface = new QWidget(card());
    m_leftSurface->setObjectName(QStringLiteral("SettingsLeftSurface"));
    m_leftSurface->lower();
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 16);
    body->setSpacing(16);
    m_categories = new QListWidget(this);
    m_categories->setObjectName(QStringLiteral("SettingsCategories"));
    m_categories->setFixedWidth(196);
    m_categories->setIconSize(QSize(20, 20));
    auto* general = new QListWidgetItem(
        AppIcons::icon(IconId::Settings, ds::ColorToken::TextSecondary, 20),
        QStringLiteral("通用"), m_categories);
    general->setData(Qt::UserRole, 0);
    auto* appearance = new QListWidgetItem(
        AppIcons::icon(IconId::Theme, ds::ColorToken::TextSecondary, 20),
        QStringLiteral("外观"), m_categories);
    appearance->setData(Qt::UserRole, 1);
    auto* smart = new QListWidgetItem(
        AppIcons::icon(IconId::SmartSettings, ds::ColorToken::TextSecondary, 20),
        QStringLiteral("智能清单"), m_categories);
    smart->setData(Qt::UserRole, 2);
    auto* widgets = new QListWidgetItem(
        AppIcons::icon(IconId::DesktopWidget, ds::ColorToken::TextSecondary, 20),
        QStringLiteral("桌面小部件"), m_categories);
    widgets->setData(Qt::UserRole, 3);
    body->addWidget(m_categories);

    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("SettingsPages"));
    auto* generalPage = new QWidget(this);
    generalPage->setObjectName(QStringLiteral("GeneralSettingsPage"));
    auto* generalLayout = new QVBoxLayout(generalPage);
    generalLayout->setContentsMargins(12, 0, 12, 0);
    generalLayout->setSpacing(14);
    auto* generalTitle = new QLabel(QStringLiteral("通用"), generalPage);
    generalTitle->setFont(ds::qfont(ds::FontScene::Section));
    generalLayout->addWidget(generalTitle);
    auto* startupSection = new QFrame(generalPage);
    startupSection->setObjectName(QStringLiteral("GeneralSettingsSection"));
    auto* startupRow = new QHBoxLayout(startupSection);
    startupRow->setContentsMargins(18, 12, 14, 12);
    startupRow->setSpacing(8);
    auto* startupLabel = new QLabel(QStringLiteral("开机自启动"), startupSection);
    startupLabel->setObjectName(QStringLiteral("AutostartLabel"));
    startupLabel->setFont(ds::qfont(ds::FontScene::Body));
    startupRow->addWidget(startupLabel, 1);
    m_autostartSwitch = new CompactSwitch(startupSection);
    m_autostartSwitch->setObjectName(QStringLiteral("GeneralAutostart"));
    m_autostartSwitch->setAccessibleName(QStringLiteral("开机自启动"));
    startupRow->addWidget(m_autostartSwitch);
    generalLayout->addWidget(startupSection);
    m_autostartHint = new QLabel(
        QStringLiteral("登录系统后自动运行，并恢复上次打开的便签与任务小部件。"), generalPage);
    m_autostartHint->setObjectName(QStringLiteral("GeneralAutostartHint"));
    m_autostartHint->setWordWrap(true);
    m_autostartHint->setFont(ds::qfont(ds::FontScene::Caption));
    if (m_autostart && m_autostart->mode() == AutostartSettings::Mode::Preview) {
        m_autostartHint->setText(
            QStringLiteral("当前为开发预览：开启后会在登录时使用当前数据目录启动并恢复便签；"
                           "正式安装版默认开启。"));
    } else if (!m_autostart || m_autostart->mode() == AutostartSettings::Mode::Isolated
               || m_autostart->mode() == AutostartSettings::Mode::Unavailable) {
        m_autostartHint->setText(QStringLiteral("当前运行方式不登记系统开机启动。"));
    }
    generalLayout->addWidget(m_autostartHint);
    m_autostartError = new QLabel(generalPage);
    m_autostartError->setObjectName(QStringLiteral("GeneralAutostartError"));
    m_autostartError->setFont(ds::qfont(ds::FontScene::Caption));
    m_autostartError->setWordWrap(true);
    m_autostartError->hide();
    generalLayout->addWidget(m_autostartError);
    generalLayout->addStretch(1);
    m_pages->addWidget(generalPage);
    m_appearance = new QWidget(this);
    m_appearance->setObjectName(QStringLiteral("AppearanceSettingsPage"));
    auto* appearanceLayout = new QVBoxLayout(m_appearance);
    appearanceLayout->setContentsMargins(12, 0, 12, 0);
    appearanceLayout->setSpacing(14);
    auto* appearanceTitle = new QLabel(QStringLiteral("外观"), m_appearance);
    appearanceTitle->setObjectName(QStringLiteral("AppearanceSettingsTitle"));
    appearanceTitle->setFont(ds::qfont(ds::FontScene::Section));
    appearanceLayout->addWidget(appearanceTitle);
    auto* themeSection = new QFrame(m_appearance);
    themeSection->setObjectName(QStringLiteral("AppearanceSettingsSection"));
    auto* themeRow = new QHBoxLayout(themeSection);
    themeRow->setContentsMargins(18, 12, 14, 12);
    themeRow->setSpacing(8);
    auto* themeLabel = new QLabel(QStringLiteral("主题"), themeSection);
    themeLabel->setFont(ds::qfont(ds::FontScene::Body));
    themeRow->addWidget(themeLabel, 1);
    m_theme = new MenuComboBox(themeSection);
    m_theme->setObjectName(QStringLiteral("AppearanceTheme"));
    m_theme->setAccessibleName(QStringLiteral("主题模式"));
    m_theme->addItem(QStringLiteral("浅色"), static_cast<int>(ThemeManager::Preference::Light));
    m_theme->addItem(QStringLiteral("深色"), static_cast<int>(ThemeManager::Preference::Dark));
    m_theme->addItem(QStringLiteral("跟随系统"), static_cast<int>(ThemeManager::Preference::System));
    m_theme->setMinimumWidth(132);
    m_theme->setFixedHeight(30);
    m_theme->setCurrentIndex(qMax(0, m_theme->findData(
        static_cast<int>(ThemeManager::instance().preference()))));
    themeRow->addWidget(m_theme);

    auto* fontSection = new QFrame(m_appearance);
    fontSection->setObjectName(QStringLiteral("AppearanceSettingsSection"));
    auto* fontRow = new QHBoxLayout(fontSection);
    fontRow->setContentsMargins(18, 12, 14, 12);
    auto* fontLabel = new QLabel(QStringLiteral("字体"), fontSection);
    fontLabel->setFont(ds::qfont(ds::FontScene::Body));
    fontRow->addWidget(fontLabel, 1);
    m_font = new MenuComboBox(fontSection);
    m_font->setObjectName(QStringLiteral("AppearanceFont"));
    m_font->setAccessibleName(QStringLiteral("界面字体"));
    const QStringList fontChoices = FontManager::instance().availableChoices();
    for (const QString& id : fontChoices) {
        const QString label = id == QLatin1String("default") ? QStringLiteral("默认")
            : id == QLatin1String("noto-sans-sc") ? QStringLiteral("Noto Sans CJK SC")
            : QStringLiteral("霞鹜文楷");
        m_font->addItem(label, id);
    }
    m_font->setMinimumWidth(168);
    m_font->setFixedHeight(30);
    m_font->setCurrentIndex(qMax(0, m_font->findData(FontManager::instance().preference())));
    fontRow->addWidget(m_font);
    appearanceLayout->addWidget(themeSection);
    appearanceLayout->addWidget(fontSection);
    appearanceLayout->addStretch(1);
    m_pages->addWidget(m_appearance);

    m_smartLists = new SmartListSettingsWidget(this);
    m_pages->addWidget(m_smartLists);

    auto* widgetPage = new QWidget(m_pages);
    widgetPage->setObjectName(QStringLiteral("DesktopWidgetSettingsPage"));
    auto* widgetLayout = new QVBoxLayout(widgetPage);
    widgetLayout->setContentsMargins(12, 0, 12, 0);
    widgetLayout->setSpacing(14);
    auto* widgetTitle = new QLabel(QStringLiteral("桌面小部件"), widgetPage);
    widgetTitle->setFont(ds::qfont(ds::FontScene::Section));
    widgetLayout->addWidget(widgetTitle);
    auto* widgetSection = new QFrame(widgetPage);
    widgetSection->setObjectName(QStringLiteral("DesktopWidgetSettingsSection"));
    auto* widgetRow = new QHBoxLayout(widgetSection);
    widgetRow->setContentsMargins(18, 12, 14, 12);
    auto* taskLabel = new QLabel(QStringLiteral("任务"), widgetSection);
    taskLabel->setFont(ds::qfont(ds::FontScene::Body));
    widgetRow->addWidget(taskLabel, 1);
    auto* addTaskWidget = new QPushButton(QStringLiteral("添加到桌面"), widgetSection);
    addTaskWidget->setObjectName(QStringLiteral("SettingsAddTaskWidget"));
    addTaskWidget->setIcon(AppIcons::icon(IconId::Add, 16));
    addTaskWidget->setCursor(Qt::PointingHandCursor);
    addTaskWidget->setAutoDefault(false);
    widgetRow->addWidget(addTaskWidget);
    connect(addTaskWidget, &QPushButton::clicked, this, [this]() {
        emit taskWidgetRequested();
        accept();
    });
    widgetLayout->addWidget(widgetSection);
    widgetLayout->addStretch(1);
    m_pages->addWidget(widgetPage);
    body->addWidget(m_pages, 1);
    contentLayout()->addLayout(body, 1);
    connect(m_categories, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row == 0) syncAutostart();
        m_pages->setCurrentIndex(row);
    });
    if (m_autostart) {
        connect(m_autostart, &AutostartSettings::enabledChanged, this, &SettingsDialog::syncAutostart);
        connect(m_autostart, &QObject::destroyed, this, [this]() {
            m_autostartSwitch->setEnabled(false);
            m_autostartSwitch->setToolTip(QStringLiteral("自启设置在应用重新启动后可用"));
        });
    } else {
        m_autostartSwitch->setEnabled(false);
        m_autostartSwitch->setToolTip(QStringLiteral("独立测试预览不登记系统自启"));
    }
    connect(m_autostartSwitch, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!m_autostart) return;
        QString error;
        const QPointer<SettingsDialog> guard(this);
        if (!m_autostart->setEnabled(enabled, &error) && guard) {
            const QSignalBlocker blocker(m_autostartSwitch);
            syncAutostart();
            m_autostartError->setText(error.isEmpty()
                ? QStringLiteral("无法更改开机自启动，请重试。") : error);
            m_autostartError->show();
        } else if (guard) m_autostartError->hide();
    });
    connect(m_smartLists, &SmartListSettingsWidget::visibilityChanged,
            this, &SettingsDialog::smartListVisibilityChanged);
    connect(m_theme, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        ThemeManager::instance().setPreference(
            static_cast<ThemeManager::Preference>(m_theme->itemData(index).toInt()));
    });
    connect(m_font, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        FontManager::instance().setPreference(m_font->itemData(index).toString());
    });
    connect(&FontManager::instance(), &FontManager::fontChanged, this, [this](const QString&) {
        const QSignalBlocker blocker(m_font);
        m_font->setCurrentIndex(qMax(0, m_font->findData(FontManager::instance().preference())));
        applyTheme();
    });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this](ds::Theme) {
        const QSignalBlocker blocker(m_theme);
        m_theme->setCurrentIndex(qMax(0, m_theme->findData(
            static_cast<int>(ThemeManager::instance().preference()))));
    });
    m_categories->setCurrentRow(0);
    syncAutostart();
    applyTheme();
    syncChromeGeometry();
}

void SettingsDialog::syncChromeGeometry() {
    constexpr int sideWidth = 212;
    contentLayout()->setContentsMargins(0, 0, 16, 0);
    buttonLayout()->setContentsMargins(0, 0, 0, 0);
    m_leftSurface->setGeometry(0, 0, sideWidth, card()->height());
    auto* layout = qobject_cast<QHBoxLayout*>(header()->layout());
    if (layout) layout->setContentsMargins(18, 12, 10, 10);
    header()->setMinimumWidth(0);
    header()->setMaximumWidth(QWIDGETSIZE_MAX);
}

void SettingsDialog::syncAutostart() {
    if (!m_autostartSwitch || !m_autostartError) return;
    const QSignalBlocker blocker(m_autostartSwitch);
    const bool available = m_autostart && m_autostart->isAvailable();
    m_autostartSwitch->setEnabled(available);
    m_autostartSwitch->setChecked(available && m_autostart->isEnabled());
    QString tooltip = QStringLiteral("当前运行方式不登记系统开机启动");
    if (available) {
        tooltip = m_autostart->mode() == AutostartSettings::Mode::Preview
            ? QStringLiteral("登录时使用当前数据目录启动")
            : QStringLiteral("开机自启动");
    }
    m_autostartSwitch->setToolTip(tooltip);
}

void SettingsDialog::resizeEvent(QResizeEvent* event) {
    ModalDialog::resizeEvent(event);
    syncChromeGeometry();
}

void SettingsDialog::applyTheme() {
    const auto& theme = ThemeManager::instance();
    ModalDialog::applyTheme();
    card()->setStyleSheet(QStringLiteral(
        "QWidget#ModalCard{background:%1;border:0;border-radius:20px;}"
        "QWidget#ModalHeader{background:transparent;border:0;}"
        "QLabel#ModalTitle{color:%2;background:transparent;border:0;}"
        "QFrame#GeneralSettingsSection,QFrame#DesktopWidgetSettingsSection{background:%5;border:0;border-radius:10px;}"
        "QFrame#GeneralSettingsSection QWidget,QFrame#DesktopWidgetSettingsSection QLabel{background:transparent;}"
        "QLabel#GeneralAutostartHint{color:%6;background:transparent;border:0;}"
        "QLabel#GeneralAutostartError{color:%7;background:transparent;border:0;}"
        "QToolButton#ModalClose{background:transparent;border:1px solid transparent;"
        "border-radius:6px;padding:0;}"
        "QToolButton#ModalClose:hover{background:%3;}"
        "QToolButton#ModalClose:pressed{background:%4;}")
        .arg(theme.color(ds::ColorToken::SettingsContentBg).name())
        .arg(theme.color(ds::ColorToken::Text).name())
        .arg(theme.color(ds::ColorToken::Hover).name())
        .arg(theme.color(ds::ColorToken::FieldHover).name())
        .arg(theme.color(ds::ColorToken::SettingsSectionBg).name(QColor::HexArgb))
        .arg(theme.color(ds::ColorToken::TextSecondary).name())
        .arg(theme.color(ds::ColorToken::Danger).name()));
    m_leftSurface->setStyleSheet(QStringLiteral(
        "QWidget#SettingsLeftSurface{background:%1;border:0;"
        "border-top-left-radius:20px;border-bottom-left-radius:20px;}")
        .arg(theme.color(ds::ColorToken::SettingsRailBg).name()));
    m_categories->setStyleSheet(QStringLiteral(
        "QListWidget#SettingsCategories{background:transparent;border:0;border-radius:0;outline:0;padding:8px;}"
        "QListWidget#SettingsCategories::item{height:36px;padding:0 8px;border-radius:8px;color:%1;}"
        "QListWidget#SettingsCategories::item:selected{background:%2;color:%3;}"
        "QListWidget#SettingsCategories::item:hover{background:%4;}")
        .arg(theme.color(ds::ColorToken::TextSecondary).name())
        .arg(theme.color(ds::ColorToken::SettingsSelectedBg).name(QColor::HexArgb))
        .arg(theme.color(ds::ColorToken::Text).name())
        .arg(theme.color(ds::ColorToken::Hover).name()));
    if (m_categories->count() >= 3) {
        m_categories->item(0)->setIcon(
            AppIcons::icon(IconId::Settings, ds::ColorToken::TextSecondary, 20));
        m_categories->item(1)->setIcon(
            AppIcons::icon(IconId::Theme, ds::ColorToken::TextSecondary, 20));
        m_categories->item(2)->setIcon(
            AppIcons::icon(IconId::SmartSettings, ds::ColorToken::TextSecondary, 20));
    }
    if (m_categories->count() >= 4) {
        m_categories->item(3)->setIcon(
            AppIcons::icon(IconId::DesktopWidget, ds::ColorToken::TextSecondary, 20));
    }
    if (m_appearance) {
        m_appearance->setStyleSheet(QStringLiteral(
            "QFrame#AppearanceSettingsSection{background:%1;border:0;border-radius:10px;}"
            "QFrame#AppearanceSettingsSection QWidget{background:transparent;}"
            "QFrame#AppearanceSettingsSection QComboBox{background:transparent;border:1px solid transparent;"
            "border-radius:6px;padding:0 8px;}"
            "QFrame#AppearanceSettingsSection QComboBox:hover,"
            "QFrame#AppearanceSettingsSection QComboBox:focus{background:%2;border-color:transparent;}")
            .arg(theme.color(ds::ColorToken::SettingsSectionBg).name(QColor::HexArgb))
            .arg(theme.color(ds::ColorToken::Hover).name()));
    }
    m_pages->setStyleSheet(QStringLiteral(
        "QStackedWidget#SettingsPages{background:%1;border:0;}")
        .arg(theme.color(ds::ColorToken::SettingsContentBg).name()));
}

}  // namespace kl
