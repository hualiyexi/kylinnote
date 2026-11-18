#include "ReminderPopup.h"
#include <QAction>
#include <QCursor>
#include <QEvent>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include "../domain/ViewSpec.h"
#include "../app/Branding.h"
#include "AppIcons.h"
#include "PopupMenu.h"
#include "WindowChrome.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"
namespace kl {
namespace {
constexpr int kCardWidth = 320;
constexpr int kShadowMargin = 12;
constexpr int kScreenMargin = 24;

QString reminderSchedule(const Entity& task, const QString& reminderId) {
    for (const TaskReminder& reminder : task.reminders) {
        if (reminder.id != reminderId || !reminder.triggerAt.isValid()) continue;
        const QDateTime local = toAppTime(reminder.triggerAt);
        const QDate today = toAppTime(appNow()).date();
        QString date;
        if (local.date() == today) date = QStringLiteral("今天");
        else if (local.date() == today.addDays(1)) date = QStringLiteral("明天");
        else date = local.toString(QStringLiteral("M月d日"));
        return date + local.toString(QStringLiteral(" HH:mm"));
    }
    return scheduleLabel(task);
}
}  // namespace

ReminderPopup::ReminderPopup(const Entity& task, QWidget* parent)
    : ReminderPopup(task, QString(), parent) {
}
ReminderPopup::ReminderPopup(const Entity& task, QString reminderId, QWidget* parent)
    : QDialog(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                      | Qt::NoDropShadowWindowHint),
      m_entityId(task.id), m_reminderId(reminderId) {
    if (m_reminderId.isEmpty()) {
        const int primary = primaryReminderIndex(task);
        if (primary >= 0)
            m_reminderId = task.reminders.at(primary).id;
    }
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(QStringLiteral("%1提醒 · %2")
        .arg(task.kind == EntityKind::Note ? QStringLiteral("笔记") : QStringLiteral("待办"), Branding::displayName()));
    setObjectName(QStringLiteral("ReminderPopup"));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kShadowMargin, kShadowMargin, kShadowMargin, kShadowMargin);
    outer->setSpacing(0);
    m_card = new QWidget(this);
    m_card->setObjectName(QStringLiteral("ReminderCard"));
    m_card->setAttribute(Qt::WA_StyledBackground);
    m_card->setFixedWidth(kCardWidth);
    auto* shadow = new QGraphicsDropShadowEffect(m_card);
    shadow->setBlurRadius(24);
    shadow->setOffset(0, 4);
    m_card->setGraphicsEffect(shadow);
    outer->addWidget(m_card);

    auto* layout = new QVBoxLayout(m_card);
    layout->setContentsMargins(16, 12, 12, 16);
    layout->setSpacing(0);
    auto* dragArea = new WindowDragArea(m_card);
    dragArea->setObjectName(QStringLiteral("ReminderDragArea"));
    dragArea->setDoubleClickMaximizes(false);
    dragArea->setFixedHeight(28);
    auto* header = new QHBoxLayout(dragArea);
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);
    m_scheduleIcon = new QLabel(dragArea);
    m_scheduleIcon->setObjectName(QStringLiteral("ReminderScheduleIcon"));
    m_scheduleIcon->setFixedSize(20, 28);
    m_scheduleIcon->setAlignment(Qt::AlignCenter);
    header->addWidget(m_scheduleIcon);
    auto* schedule = new QLabel(reminderSchedule(task, m_reminderId), dragArea);
    schedule->setObjectName(QStringLiteral("ReminderSchedule"));
    schedule->setFont(ds::qfont(ds::FontScene::Caption));
    schedule->setTextFormat(Qt::PlainText);
    header->addWidget(schedule);
    header->addStretch();
    m_close = new QToolButton(dragArea);
    m_close->setObjectName(QStringLiteral("ReminderDismissButton"));
    m_close->setFixedSize(28, 28);
    m_close->setAccessibleName(QStringLiteral("关闭提醒"));
    m_close->setToolTip(QStringLiteral("关闭提醒，不修改任务状态"));
    m_close->setAutoRaise(true);
    header->addWidget(m_close);
    layout->addWidget(dragArea);

    m_title = new QPushButton(m_card);
    m_title->setObjectName(QStringLiteral("ReminderOpenButton"));
    m_title->setProperty("fullText", task.title);
    m_title->setFlat(true);
    m_title->setCursor(Qt::PointingHandCursor);
    m_title->setMinimumHeight(34);
    m_title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    QFont titleFont = ds::qfont(ds::FontScene::Body);
    titleFont.setPointSizeF(ds::pointSize(ds::TypeScale::Subtitle));
    m_title->setFont(titleFont);
    const QString entityLabel = task.kind == EntityKind::Note ? QStringLiteral("笔记") : QStringLiteral("任务");
    m_title->setAccessibleName(QStringLiteral("查看%1：%2").arg(entityLabel, task.title));
    m_title->setToolTip(QStringLiteral("%1\n点击查看%2").arg(task.title, entityLabel));
    m_title->installEventFilter(this);
    layout->addWidget(m_title);
    layout->addSpacing(14);

    auto* buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->setSpacing(10);
    auto* done = new QPushButton(QStringLiteral("完成"), m_card);
    done->setObjectName(QStringLiteral("ReminderDoneButton"));
    done->setProperty("primary", true);
    done->setFixedHeight(36);
    done->setAccessibleName(QStringLiteral("标记完成"));
    done->setToolTip(QStringLiteral("标记任务完成"));
    buttons->addWidget(done, 1);
    done->setVisible(task.kind == EntityKind::Task);

    m_snooze = new QToolButton(m_card);
    m_snooze->setObjectName(QStringLiteral("ReminderSnoozeButton"));
    m_snooze->setText(QStringLiteral("稍后提醒"));
    m_snooze->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_snooze->setPopupMode(QToolButton::InstantPopup);
    m_snooze->setFixedHeight(36);
    m_snooze->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_snooze->setAccessibleName(QStringLiteral("稍后提醒"));
    m_snooze->setToolTip(QStringLiteral("选择稍后提醒时间"));
    auto* menu = new PopupMenu(m_snooze);
    menu->setObjectName(QStringLiteral("ReminderSnoozeMenu"));
    const auto addSnoozeAction = [this, menu](const QString& text, const QString& name, int minutes) {
        QAction* action = menu->addAction(text);
        action->setObjectName(name);
        action->setData(minutes);
        connect(action, &QAction::triggered, this, [this, minutes]() {
            emit snoozeRequested(m_entityId, m_reminderId, minutes);
        });
    };
    addSnoozeAction(QStringLiteral("15 分钟"), QStringLiteral("ReminderSnooze15"), 15);
    addSnoozeAction(QStringLiteral("30 分钟"), QStringLiteral("ReminderSnooze30"), 30);
    addSnoozeAction(QStringLiteral("1 小时"), QStringLiteral("ReminderSnooze60"), 60);
    addSnoozeAction(QStringLiteral("3 小时"), QStringLiteral("ReminderSnooze180"), 180);
    m_snooze->setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [this, menu]() {
        QTimer::singleShot(0, menu, [this, menu]() { positionSnoozeMenu(menu); });
    });
    buttons->addWidget(m_snooze, 1);
    layout->addLayout(buttons);

    connect(done, &QPushButton::clicked, this, [this]() { emit completeRequested(m_entityId); });
    connect(m_title, &QPushButton::clicked, this, [this]() {
        emit openRequested(m_entityId);
        close();
    });
    connect(m_close, &QToolButton::clicked, this, &QDialog::close);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](ds::Theme) { applyTheme(); });
    applyTheme();
    updateTitleElision();
    adjustSize();
    positionOnScreen();
}

void ReminderPopup::positionSnoozeMenu(PopupMenu* menu) {
    if (!menu || !m_snooze) return;
    constexpr int gap = 4;
    const int margin = PopupMenu::shadowMargin();
    QSize menuSize = menu->size();
    if (menuSize.isEmpty()) menuSize = menu->sizeHint();
    const QSize cardSize = menuSize - QSize(2 * margin, 2 * margin);
    const QRect button(m_snooze->mapToGlobal(QPoint(0, 0)), m_snooze->size());
    QScreen* screen = QGuiApplication::screenAt(button.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    const QRect available = screen->availableGeometry();
    const int x = qBound(available.left(), button.left(),
                         qMax(available.left(), available.right() - cardSize.width() + 1));
    int y = button.bottom() + gap + 1;
    const int aboveY = button.top() - gap - cardSize.height() + 1;
    if (y + cardSize.height() - 1 > available.bottom() && aboveY >= available.top())
        y = aboveY;
    else
        y = qBound(available.top(), y, qMax(available.top(), available.bottom() - cardSize.height() + 1));
    // Qt 默认定位完成后再移动透明整窗，使可见卡片而不是阴影边距
    // 贴住按钮，并由可见卡片高度决定是否上翻。
    menu->move(QPoint(x, y) - QPoint(margin, margin));
}

void ReminderPopup::applyTheme() {
    const auto& theme = ThemeManager::instance();
    const auto color = [&theme](ds::ColorToken token) { return theme.color(token).name(); };
    m_card->setStyleSheet(QStringLiteral(R"QSS(
QWidget#ReminderCard {
    background-color: %1;
    border: 1px solid %2;
    border-radius: 12px;
}
QLabel { background: transparent; border: none; }
QLabel#ReminderSchedule { color: %3; }
QPushButton#ReminderOpenButton {
    color: %4;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 6px;
    padding: 0 4px;
    text-align: left;
}
QPushButton#ReminderOpenButton:hover { background-color: %5; }
QPushButton#ReminderOpenButton:focus { border-color: %6; }
QPushButton#ReminderDoneButton, QToolButton#ReminderSnoozeButton {
    border-radius: 7px;
    padding: 0 12px;
}
QPushButton#ReminderDoneButton {
    color: %7;
    background-color: %11;
    border: 1px solid %11;
}
QPushButton#ReminderDoneButton:hover, QPushButton#ReminderDoneButton:pressed {
    background-color: %8;
    border-color: %8;
}
QToolButton#ReminderSnoozeButton {
    color: %4;
    background-color: %1;
    border: 1px solid %9;
}
QToolButton#ReminderSnoozeButton:hover { background-color: %10; }
QToolButton#ReminderSnoozeButton:pressed { background-color: %5; }
QToolButton#ReminderSnoozeButton::menu-arrow,
QToolButton#ReminderSnoozeButton::menu-indicator { image: none; width: 0; height: 0; }
QPushButton#ReminderDoneButton:focus, QToolButton#ReminderSnoozeButton:focus { border-color: %6; }
QToolButton#ReminderDismissButton {
    color: %3;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 6px;
    padding: 0;
}
QToolButton#ReminderDismissButton:hover { color: %4; background-color: %5; }
QToolButton#ReminderDismissButton:focus { border-color: %6; }
)QSS")
        .arg(color(ds::ColorToken::MenuBg))
        .arg(color(ds::ColorToken::Divider))
        .arg(color(ds::ColorToken::TextSecondary))
        .arg(color(ds::ColorToken::Text))
        .arg(color(ds::ColorToken::Hover))
        .arg(color(ds::ColorToken::FocusRing))
        .arg(color(ds::ColorToken::OnPrimary))
        .arg(color(ds::ColorToken::PrimaryHover))
        .arg(color(ds::ColorToken::Border))
        .arg(color(ds::ColorToken::FieldHover))
        .arg(color(ds::ColorToken::Primary)));
    m_scheduleIcon->setPixmap(AppIcons::icon(IconId::Today, ds::ColorToken::Warning, 18).pixmap(18, 18));
    m_close->setIcon(AppIcons::icon(IconId::Close, ds::ColorToken::TextSecondary, 16));
    if (auto* effect = qobject_cast<QGraphicsDropShadowEffect*>(m_card->graphicsEffect())) {
        QColor shadow = theme.color(ds::ColorToken::Text);
        shadow.setAlpha(40);
        effect->setColor(shadow);
    }
    m_card->update();
}

void ReminderPopup::positionOnScreen() {
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    const QRect area = screen->availableGeometry();
    const int edgeMargin = kScreenMargin - kShadowMargin;
    const int x = area.right() - width() - edgeMargin + 1;
    const int y = area.bottom() - height() - edgeMargin + 1;
    move(qBound(area.left(), x, qMax(area.left(), area.right() - width() + 1)),
         qBound(area.top(), y, qMax(area.top(), area.bottom() - height() + 1)));
}

void ReminderPopup::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    updateTitleElision();
}

bool ReminderPopup::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_title && event->type() == QEvent::Resize)
        updateTitleElision();
    return QDialog::eventFilter(watched, event);
}

void ReminderPopup::updateTitleElision() {
    if (!m_title || m_title->width() <= 20) return;
    const QString fullText = m_title->property("fullText").toString();
    m_title->setText(m_title->fontMetrics().elidedText(fullText, Qt::ElideRight, m_title->width() - 12));
}
}  // namespace kl
