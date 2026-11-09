#include "DatePickerPopup.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QEvent>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include <QStackedWidget>
#include <QStyle>
#include <QTime>
#include <QToolButton>
#include <QVBoxLayout>

#include "../Common.h"
#include "AppIcons.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {

constexpr int kCardWidth = 260;
constexpr int kCardHeight = 540;
constexpr int kShadowMargin = 12;
constexpr int kCalendarCell = 30;

QString color(ds::ColorToken token)
{
    return ThemeManager::instance().color(token).name();
}

QIcon quickIcon(int index)
{
    // 带点日历复用集中图标；其余三个图形在 AppIcons 中无对应项，局部绘制以对齐截图。
    if (index == 2) return AppIcons::icon(IconId::Today, ds::ColorToken::TextSecondary, 20);
    QPixmap pixmap(40, 40);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(20.0 / 24.0, 20.0 / 24.0);
    QPen pen(ThemeManager::instance().color(ds::ColorToken::TextSecondary), 1.5);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    if (index == 0) {
        painter.drawEllipse(QPointF(12, 12), 4, 4);
        const QLineF rays[] = {
            QLineF(12, 2, 12, 5), QLineF(12, 19, 12, 22),
            QLineF(2, 12, 5, 12), QLineF(19, 12, 22, 12),
            QLineF(4.9, 4.9, 6.9, 6.9), QLineF(17.1, 17.1, 19.1, 19.1),
            QLineF(4.9, 19.1, 6.9, 17.1), QLineF(17.1, 6.9, 19.1, 4.9)
        };
        for (const auto& ray : rays) painter.drawLine(ray);
    } else if (index == 1) {
        QPainterPath arrow;
        arrow.moveTo(12, 16);
        arrow.lineTo(12, 4);
        arrow.moveTo(8, 8);
        arrow.lineTo(12, 4);
        arrow.lineTo(16, 8);
        arrow.moveTo(3, 14);
        arrow.lineTo(3, 19);
        arrow.lineTo(21, 19);
        arrow.lineTo(21, 14);
        painter.drawPath(arrow);
    } else {
        QPainterPath moon;
        moon.moveTo(15.5, 3.5);
        moon.cubicTo(9.5, 4.2, 5.5, 9, 5.5, 14);
        moon.cubicTo(5.5, 18, 9, 20.5, 13, 20.5);
        moon.cubicTo(16, 20.5, 19, 18.5, 20.5, 15.5);
        moon.cubicTo(14, 17, 9, 10, 15.5, 3.5);
        painter.drawPath(moon);
    }
    return QIcon(pixmap);
}

int configuredFirstDay()
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("kylinlist"), QStringLiteral("kylinlist"));
    const int value = settings.value(QStringLiteral("ui/weekFirstDay"), static_cast<int>(Qt::Monday)).toInt();
    return value >= static_cast<int>(Qt::Monday) && value <= static_cast<int>(Qt::Sunday)
        ? value : static_cast<int>(Qt::Monday);
}

void repolish(QWidget* widget)
{
    if (!widget || !widget->style()) return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

void setInputInvalid(QLineEdit* edit, bool invalid, const QString& reason = QString())
{
    edit->setProperty("invalid", invalid);
    edit->setToolTip(invalid ? reason : QString());
    repolish(edit);
}

QToolButton* makeBackButton(QWidget* parent)
{
    auto* button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("DatePickerNav"));
    button->setText(QStringLiteral("‹"));
    button->setFixedSize(28, 28);
    button->setToolTip(QStringLiteral("返回日期"));
    button->setAccessibleName(QStringLiteral("返回日期"));
    return button;
}

} // namespace

DatePickerPopup::DatePickerPopup(QWidget* parent)
    : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    , m_dateTime(appNow())
    , m_visibleMonth(QDate(toAppTime(m_dateTime).date().year(), toAppTime(m_dateTime).date().month(), 1))
{
    setObjectName(QStringLiteral("DatePickerPopup"));
    setWindowTitle(QStringLiteral("日期选择器"));
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(false);
    setFont(ds::qfont(ds::FontScene::Body));
    m_firstDay = configuredFirstDay();

    buildUi();
    applyTheme();

    const auto buttons = findChildren<QAbstractButton*>();
    for (QAbstractButton* button : buttons) {
        button->setFocusPolicy(Qt::StrongFocus);
        button->installEventFilter(this);
    }

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](ds::Theme) { applyTheme(); });
}

void DatePickerPopup::buildUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kShadowMargin, kShadowMargin, kShadowMargin, kShadowMargin);
    outer->setSpacing(0);

    m_card = new QWidget(this);
    m_card->setObjectName(QStringLiteral("DatePickerCard"));
    m_card->setAttribute(Qt::WA_StyledBackground);
    m_card->setFixedSize(kCardWidth, kCardHeight);
    auto* shadow = new QGraphicsDropShadowEffect(m_card);
    shadow->setBlurRadius(24);
    shadow->setOffset(0, 4);
    m_card->setGraphicsEffect(shadow);
    outer->addWidget(m_card, 0, Qt::AlignCenter);
    setFixedSize(kCardWidth + 2 * kShadowMargin, kCardHeight + 2 * kShadowMargin);

    auto* cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(12, 12, 12, 12);
    cardLayout->setSpacing(0);
    m_pages = new QStackedWidget(m_card);
    m_pages->setObjectName(QStringLiteral("DatePickerPages"));
    cardLayout->addWidget(m_pages);
    m_pages->addWidget(buildMainPage(m_pages));
    m_pages->addWidget(buildTimePage(m_pages));
    m_pages->addWidget(buildReminderPage(m_pages));
    m_pages->addWidget(buildRecurrencePage(m_pages));
}

QWidget* DatePickerPopup::buildMainPage(QWidget* parent)
{
    auto* page = new QWidget(parent);
    page->setObjectName(QStringLiteral("DatePickerMainPage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* segmentHost = new QWidget(page);
    segmentHost->setObjectName(QStringLiteral("DatePickerSegmentHost"));
    segmentHost->setFixedHeight(32);
    auto* segment = new QHBoxLayout(segmentHost);
    segment->setContentsMargins(3, 3, 3, 3);
    segment->setSpacing(2);
    m_dateButton = new QPushButton(QStringLiteral("日期"), segmentHost);
    m_dateButton->setObjectName(QStringLiteral("DatePickerSegment"));
    m_dateButton->setCheckable(true);
    m_dateButton->setChecked(true);
    m_rangeButton = new QPushButton(QStringLiteral("时间段"), segmentHost);
    m_rangeButton->setObjectName(QStringLiteral("DatePickerSegment"));
    m_rangeButton->setCheckable(true);
    // 考据只证明两个入口可见；范围结果的写入/撤销没有实测依据。
    m_rangeButton->setToolTip(QStringLiteral("时间段（KylinList 自定）：范围写入与撤销未实现"));
    segment->addWidget(m_dateButton, 1);
    segment->addWidget(m_rangeButton, 1);
    layout->addWidget(segmentHost);

    m_modeStack = new QStackedWidget(page);
    m_modeStack->setObjectName(QStringLiteral("DatePickerModeStack"));
    layout->addWidget(m_modeStack, 1);

    auto* datePage = new QWidget(m_modeStack);
    datePage->setObjectName(QStringLiteral("DatePickerDatePage"));
    auto* dateLayout = new QVBoxLayout(datePage);
    dateLayout->setContentsMargins(0, 0, 0, 0);
    dateLayout->setSpacing(8);

    auto* quickRow = new QHBoxLayout;
    quickRow->setContentsMargins(0, 0, 0, 0);
    quickRow->setSpacing(12);
    quickRow->addStretch();
    // 四个 tooltip 与执行效果均未验证，不绑定推断命令。
    for (int i = 0; i < 4; ++i) {
        auto* button = new QToolButton(datePage);
        button->setObjectName(QStringLiteral("DatePickerQuick"));
        button->setProperty("quickIndex", i);
        button->setIconSize(QSize(20, 20));
        button->setFixedSize(40, 32);
        button->setToolTip(QStringLiteral("快捷选项（含义与行为未验证）"));
        button->setAccessibleName(QStringLiteral("快捷选项 %1（含义未验证）").arg(i + 1));
        m_quickButtons.append(button);
        quickRow->addWidget(button);
    }
    quickRow->addStretch();
    dateLayout->addLayout(quickRow);

    auto* monthRow = new QHBoxLayout;
    monthRow->setContentsMargins(0, 12, 0, 0);
    monthRow->setSpacing(2);
    m_monthLabel = new QLabel(datePage);
    m_monthLabel->setFixedHeight(28);
    monthRow->addWidget(m_monthLabel);
    monthRow->addStretch();
    auto* previous = new QToolButton(datePage);
    previous->setObjectName(QStringLiteral("DatePickerNav"));
    previous->setText(QStringLiteral("‹"));
    previous->setFixedSize(24, 24);
    previous->setToolTip(QStringLiteral("上一月"));
    previous->setAccessibleName(QStringLiteral("上一月"));
    auto* today = new QToolButton(datePage);
    today->setObjectName(QStringLiteral("DatePickerNav"));
    today->setText(QStringLiteral("○"));
    today->setFixedSize(24, 24);
    today->setToolTip(QStringLiteral("今天"));
    today->setAccessibleName(QStringLiteral("今天"));
    auto* next = new QToolButton(datePage);
    next->setObjectName(QStringLiteral("DatePickerNav"));
    next->setText(QStringLiteral("›"));
    next->setFixedSize(24, 24);
    next->setToolTip(QStringLiteral("下一月"));
    next->setAccessibleName(QStringLiteral("下一月"));
    monthRow->addWidget(previous);
    monthRow->addWidget(today);
    monthRow->addWidget(next);
    dateLayout->addLayout(monthRow);

    auto* weekdays = new QGridLayout;
    weekdays->setContentsMargins(0, 0, 0, 0);
    weekdays->setHorizontalSpacing(4);
    weekdays->setVerticalSpacing(0);
    for (int column = 0; column < 7; ++column) {
        auto* label = new QLabel(datePage);
        label->setProperty("weekday", true);
        label->setAlignment(Qt::AlignCenter);
        label->setFixedSize(kCalendarCell, 20);
        weekdays->addWidget(label, 0, column);
        m_weekdayLabels.append(label);
    }
    dateLayout->addLayout(weekdays);

    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 8, 0, 0);
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(0);
    for (int index = 0; index < 42; ++index) {
        auto* button = new QToolButton(datePage);
        button->setObjectName(QStringLiteral("DatePickerDay"));
        button->setCheckable(true);
        button->setFixedSize(kCalendarCell, kCalendarCell);
        grid->addWidget(button, index / 7, index % 7);
        m_dayButtons.append(button);
        connect(button, &QToolButton::clicked, this, [this, button]() {
            selectDate(button->property("date").toDate());
        });
    }
    dateLayout->addLayout(grid);
    dateLayout->addStretch(1);

    auto* entries = new QVBoxLayout;
    entries->setContentsMargins(0, 0, 0, 0);
    entries->setSpacing(2);
    auto makeEntry = [datePage](IconId id, const QString& text) {
        auto* button = new QPushButton(datePage);
        button->setObjectName(QStringLiteral("DatePickerEntry"));
        button->setFixedHeight(32);
        button->setAccessibleName(text);
        auto* row = new QHBoxLayout(button);
        row->setContentsMargins(8, 0, 8, 0);
        row->setSpacing(8);
        auto* icon = new QLabel(button);
        icon->setProperty("iconId", static_cast<int>(id));
        icon->setFixedSize(20, 20);
        icon->setAlignment(Qt::AlignCenter);
        icon->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto* caption = new QLabel(text, button);
        caption->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto* chevron = new QLabel(QStringLiteral("›"), button);
        chevron->setProperty("chevron", true);
        chevron->setAttribute(Qt::WA_TransparentForMouseEvents);
        row->addWidget(icon);
        row->addWidget(caption);
        row->addStretch();
        row->addWidget(chevron);
        return button;
    };
    auto* timeEntry = makeEntry(IconId::Clock, QStringLiteral("时间"));
    auto* reminderEntry = makeEntry(IconId::Reminder, QStringLiteral("提醒"));
    auto* recurrenceEntry = makeEntry(IconId::Repeat, QStringLiteral("重复"));
    entries->addWidget(timeEntry);
    entries->addWidget(reminderEntry);
    entries->addWidget(recurrenceEntry);
    dateLayout->addLayout(entries);
    m_modeStack->addWidget(datePage);

    auto* rangePage = new QWidget(m_modeStack);
    rangePage->setObjectName(QStringLiteral("DatePickerRangePage"));
    auto* rangeLayout = new QVBoxLayout(rangePage);
    rangeLayout->setContentsMargins(12, 12, 12, 12);
    auto* rangeNotice = new QLabel(QStringLiteral("时间段（KylinList 自定）\n\n范围写入与撤销未实现"), rangePage);
    rangeNotice->setProperty("muted", true);
    rangeNotice->setAlignment(Qt::AlignCenter);
    rangeNotice->setWordWrap(true);
    rangeLayout->addWidget(rangeNotice);
    m_modeStack->addWidget(rangePage);

    auto* actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(8);
    auto* clear = new QPushButton(QStringLiteral("清除"), page);
    clear->setObjectName(QStringLiteral("DatePickerClear"));
    clear->setFixedHeight(32);
    m_okButton = new QPushButton(QStringLiteral("确定"), page);
    m_okButton->setObjectName(QStringLiteral("DatePickerConfirm"));
    m_okButton->setFixedHeight(32);
    actions->addWidget(clear, 1);
    actions->addWidget(m_okButton, 1);
    layout->addLayout(actions);

    connect(m_dateButton, &QPushButton::clicked, this, [this]() { setRangeMode(false); });
    connect(m_rangeButton, &QPushButton::clicked, this, [this]() { setRangeMode(true); });
    connect(previous, &QToolButton::clicked, this, [this]() {
        m_visibleMonth = m_visibleMonth.addMonths(-1);
        updateMonthLabel();
        buildCalendarGrid();
    });
    connect(next, &QToolButton::clicked, this, [this]() {
        m_visibleMonth = m_visibleMonth.addMonths(1);
        updateMonthLabel();
        buildCalendarGrid();
    });
    connect(today, &QToolButton::clicked, this, [this]() { selectDate(toAppTime(appNow()).date()); });
    connect(timeEntry, &QPushButton::clicked, this, &DatePickerPopup::openTimePage);
    connect(reminderEntry, &QPushButton::clicked, this, &DatePickerPopup::openReminderPage);
    connect(recurrenceEntry, &QPushButton::clicked, this, &DatePickerPopup::openRecurrencePage);
    connect(clear, &QPushButton::clicked, this, [this]() {
        m_dateTime = QDateTime();
        m_hasDate = false;
        m_reminder = QDateTime();
        m_recurrence = QStringLiteral("none");
        m_persistentReminder = false;
        m_cleared = true;
        emit cleared();
        reject();
    });
    connect(m_okButton, &QPushButton::clicked, this, &QDialog::accept);

    refreshWeekdayLabels();
    updateMonthLabel();
    buildCalendarGrid();
    return page;
}

QWidget* DatePickerPopup::buildTimePage(QWidget* parent)
{
    auto* page = new QWidget(parent);
    page->setObjectName(QStringLiteral("DatePickerTimePage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* header = new QHBoxLayout;
    auto* back = makeBackButton(page);
    auto* title = new QLabel(QStringLiteral("时间"), page);
    header->addWidget(back);
    header->addWidget(title);
    header->addStretch();
    layout->addLayout(header);

    m_timeList = new QListWidget(page);
    m_timeList->setObjectName(QStringLiteral("DatePickerList"));
    m_timeList->setAccessibleName(QStringLiteral("半小时时间列表"));
    for (int minute = 0; minute < 24 * 60; minute += 30) {
        const QTime time = QTime(0, 0).addSecs(minute * 60);
        auto* item = new QListWidgetItem(time.toString(QStringLiteral("HH:mm")), m_timeList);
        item->setData(Qt::UserRole, minute);
    }
    layout->addWidget(m_timeList, 1);
    m_timeEdit = new QLineEdit(page);
    m_timeEdit->setObjectName(QStringLiteral("DatePickerInput"));
    m_timeEdit->setPlaceholderText(QStringLiteral("设置时间"));
    m_timeEdit->setMaxLength(5);
    m_timeEdit->setAccessibleName(QStringLiteral("设置时间"));
    layout->addWidget(m_timeEdit);

    connect(back, &QToolButton::clicked, this, &DatePickerPopup::showMainPage);
    auto chooseItem = [this](QListWidgetItem* item) {
        if (!item) return;
        const QTime time = QTime(0, 0).addSecs(item->data(Qt::UserRole).toInt() * 60);
        applyTime(time);
        // 子层草稿交互为 KylinList 自定；考据未执行选择或保存。
        showMainPage();
    };
    connect(m_timeList, &QListWidget::itemClicked, this, chooseItem);
    connect(m_timeList, &QListWidget::itemActivated, this, chooseItem);
    connect(m_timeEdit, &QLineEdit::returnPressed, this, [this]() {
        if (applyCustomTime()) showMainPage();
    });
    connect(m_timeEdit, &QLineEdit::textChanged, this, [this]() { setInputInvalid(m_timeEdit, false); });
    return page;
}

QWidget* DatePickerPopup::buildReminderPage(QWidget* parent)
{
    auto* page = new QWidget(parent);
    page->setObjectName(QStringLiteral("DatePickerReminderPage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* header = new QHBoxLayout;
    auto* back = makeBackButton(page);
    auto* title = new QLabel(QStringLiteral("提醒"), page);
    header->addWidget(back);
    header->addWidget(title);
    header->addStretch();
    layout->addLayout(header);

    m_reminderList = new QListWidget(page);
    m_reminderList->setObjectName(QStringLiteral("DatePickerList"));
    m_reminderList->setAccessibleName(QStringLiteral("提醒选项"));
    const QString labels[] = {
        QStringLiteral("当天（09:00）"), QStringLiteral("提前 1 天（09:00）"),
        QStringLiteral("提前 2 天（09:00）"), QStringLiteral("提前 3 天（09:00）"),
        QStringLiteral("提前 1 周（09:00）"), QStringLiteral("自定义")
    };
    const int offsets[] = {0, -1, -2, -3, -7, 0};
    for (int i = 0; i < 6; ++i) {
        auto* item = new QListWidgetItem(labels[i], m_reminderList);
        item->setData(Qt::UserRole, offsets[i]);
    }
    layout->addWidget(m_reminderList, 1);

    m_customReminderEdit = new QLineEdit(page);
    m_customReminderEdit->setObjectName(QStringLiteral("DatePickerInput"));
    m_customReminderEdit->setPlaceholderText(QStringLiteral("yyyy-MM-dd HH:mm"));
    m_customReminderEdit->setAccessibleName(QStringLiteral("自定义提醒时间"));
    m_customReminderEdit->hide();
    layout->addWidget(m_customReminderEdit);
    m_persistentCheck = new QCheckBox(QStringLiteral("持续提醒"), page);
    m_persistentCheck->setObjectName(QStringLiteral("DatePickerCheck"));
    layout->addWidget(m_persistentCheck);

    auto* actions = new QHBoxLayout;
    auto* cancel = new QPushButton(QStringLiteral("取消"), page);
    cancel->setObjectName(QStringLiteral("DatePickerClear"));
    cancel->setFixedHeight(30);
    auto* confirm = new QPushButton(QStringLiteral("确定"), page);
    confirm->setObjectName(QStringLiteral("DatePickerConfirm"));
    confirm->setFixedHeight(30);
    actions->addWidget(cancel, 1);
    actions->addWidget(confirm, 1);
    layout->addLayout(actions);

    connect(back, &QToolButton::clicked, this, &DatePickerPopup::showMainPage);
    connect(cancel, &QPushButton::clicked, this, &DatePickerPopup::showMainPage);
    connect(confirm, &QPushButton::clicked, this, [this]() {
        if (applyReminder()) showMainPage();
    });
    connect(m_reminderList, &QListWidget::currentRowChanged, this, [this](int row) {
        m_customReminderEdit->setVisible(row == 5);
        if (row == 5) m_customReminderEdit->setFocus();
    });
    connect(m_customReminderEdit, &QLineEdit::returnPressed, this, [this]() {
        if (applyReminder()) showMainPage();
    });
    connect(m_customReminderEdit, &QLineEdit::textChanged, this,
            [this]() { setInputInvalid(m_customReminderEdit, false); });
    return page;
}

QWidget* DatePickerPopup::buildRecurrencePage(QWidget* parent)
{
    auto* page = new QWidget(parent);
    page->setObjectName(QStringLiteral("DatePickerRecurrencePage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* header = new QHBoxLayout;
    auto* back = makeBackButton(page);
    auto* title = new QLabel(QStringLiteral("重复"), page);
    header->addWidget(back);
    header->addWidget(title);
    header->addStretch();
    layout->addLayout(header);

    m_recurrenceList = new QListWidget(page);
    m_recurrenceList->setObjectName(QStringLiteral("DatePickerList"));
    m_recurrenceList->setAccessibleName(QStringLiteral("重复选项"));
    const QString labels[] = {
        QStringLiteral("每天"), QStringLiteral("每周"), QStringLiteral("每月"),
        QStringLiteral("每年"), QStringLiteral("工作日"), QStringLiteral("自定义")
    };
    const QString rules[] = {
        QStringLiteral("daily"), QStringLiteral("weekly"), QStringLiteral("monthly"),
        QStringLiteral("RRULE:FREQ=YEARLY"),
        QStringLiteral("RRULE:FREQ=WEEKLY;BYDAY=MO,TU,WE,TH,FR"), QString()
    };
    // 每年/工作日的 RRULE 仅作为选择结果返回；当前本地引擎是否支持由持久化层校验。
    for (int i = 0; i < 6; ++i) {
        auto* item = new QListWidgetItem(labels[i], m_recurrenceList);
        item->setData(Qt::UserRole, rules[i]);
    }
    layout->addWidget(m_recurrenceList, 1);
    m_customRecurrenceEdit = new QLineEdit(page);
    m_customRecurrenceEdit->setObjectName(QStringLiteral("DatePickerInput"));
    m_customRecurrenceEdit->setPlaceholderText(QStringLiteral("FREQ=DAILY;INTERVAL=1"));
    m_customRecurrenceEdit->setAccessibleName(QStringLiteral("自定义重复规则"));
    m_customRecurrenceEdit->hide();
    layout->addWidget(m_customRecurrenceEdit);

    connect(back, &QToolButton::clicked, this, &DatePickerPopup::showMainPage);
    connect(m_recurrenceList, &QListWidget::currentRowChanged, this, [this](int row) {
        m_customRecurrenceEdit->setVisible(row == 5);
        if (row == 5) m_customRecurrenceEdit->setFocus();
    });
    auto chooseItem = [this](QListWidgetItem* item) {
        if (!item) return;
        const int row = m_recurrenceList->row(item);
        if (row == 5) {
            m_customRecurrenceEdit->setFocus();
            return;
        }
        m_recurrence = item->data(Qt::UserRole).toString();
        m_cleared = false;
        // 单击返回主层为 KylinList 自定草稿交互，不代表已经写入任务。
        showMainPage();
    };
    connect(m_recurrenceList, &QListWidget::itemClicked, this, chooseItem);
    connect(m_recurrenceList, &QListWidget::itemActivated, this, chooseItem);
    connect(m_customRecurrenceEdit, &QLineEdit::returnPressed, this, [this]() {
        if (applyCustomRecurrence()) showMainPage();
    });
    connect(m_customRecurrenceEdit, &QLineEdit::textChanged, this,
            [this]() { setInputInvalid(m_customRecurrenceEdit, false); });
    return page;
}

void DatePickerPopup::applyTheme()
{
    QString qss = QStringLiteral(R"QSS(
QWidget#DatePickerCard {
    background-color: %SURFACE%;
    border: 1px solid %BORDER%;
    border-radius: 12px;
}
QLabel { color: %TEXT%; background: transparent; }
QLabel[muted="true"], QLabel[weekday="true"], QLabel[chevron="true"] { color: %TEXT_TERTIARY%; }
QWidget#DatePickerSegmentHost { background-color: %FIELD%; border-radius: 10px; }
QPushButton#DatePickerSegment {
    color: %TEXT_SECONDARY%;
    background: transparent;
    border: none;
    border-radius: 7px;
    padding: 3px 8px;
}
QPushButton#DatePickerSegment:checked { color: %TEXT%; background-color: %SURFACE%; }
QPushButton#DatePickerSegment:focus { border: 1px solid %FOCUS_RING%; }
QToolButton#DatePickerQuick, QToolButton#DatePickerNav {
    color: %TEXT_SECONDARY%;
    background: transparent;
    border: 1px solid %SURFACE%;
    border-radius: 6px;
}
QToolButton#DatePickerQuick:hover, QToolButton#DatePickerNav:hover { background-color: %HOVER%; }
QToolButton#DatePickerQuick:focus, QToolButton#DatePickerNav:focus { border-color: %FOCUS_RING%; }
QToolButton#DatePickerDay {
    color: %TEXT%;
    background-color: %SURFACE%;
    border: 1px solid %SURFACE%;
    border-radius: 15px;
}
QToolButton#DatePickerDay[outsideMonth="true"] { color: %TEXT_TERTIARY%; }
QToolButton#DatePickerDay:hover { background-color: %FIELD_HOVER%; }
QToolButton#DatePickerDay:focus { border-color: %FOCUS_RING%; }
QToolButton#DatePickerDay:checked {
    color: %ON_PRIMARY%;
    background-color: %PRIMARY%;
    border-color: %PRIMARY%;
}
QToolButton#DatePickerDay:checked:hover { background-color: %PRIMARY_HOVER%; }
QToolButton#DatePickerDay:checked:focus { border-color: %FOCUS_RING%; }
QPushButton#DatePickerEntry {
    color: %TEXT%;
    background: transparent;
    border: 1px solid %SURFACE%;
    border-radius: 6px;
    text-align: left;
}
QPushButton#DatePickerEntry:hover { background-color: %HOVER%; }
QPushButton#DatePickerEntry:focus { border-color: %FOCUS_RING%; }
QPushButton#DatePickerClear {
    color: %TEXT_SECONDARY%;
    background-color: %SURFACE%;
    border: 1px solid %BORDER%;
    border-radius: 6px;
    padding: 4px 8px;
}
QPushButton#DatePickerClear:hover { background-color: %FIELD_HOVER%; }
QPushButton#DatePickerClear:focus { border-color: %FOCUS_RING%; }
QPushButton#DatePickerConfirm {
    color: %ON_PRIMARY%;
    background-color: %PRIMARY%;
    border: 1px solid %PRIMARY%;
    border-radius: 6px;
    padding: 4px 8px;
}
QPushButton#DatePickerConfirm:hover { background-color: %PRIMARY_HOVER%; }
QPushButton#DatePickerConfirm:focus { border-color: %FOCUS_RING%; }
QPushButton#DatePickerConfirm:disabled {
    color: %TEXT_TERTIARY%;
    background-color: %FIELD%;
    border-color: %FIELD%;
}
QListWidget#DatePickerList {
    color: %TEXT%;
    background-color: %SURFACE%;
    border: none;
    outline: 0;
}
QListWidget#DatePickerList::item { padding: 5px 8px; border-radius: 6px; }
QListWidget#DatePickerList::item:hover { background-color: %HOVER%; }
QListWidget#DatePickerList::item:selected { color: %TEXT%; background-color: %PRIMARY_SOFT%; }
QListWidget#DatePickerList:focus { border: 1px solid %FOCUS_RING%; }
QLineEdit#DatePickerInput {
    color: %TEXT%;
    background-color: %FIELD%;
    border: 1px solid %BORDER%;
    border-radius: 6px;
    padding: 6px 8px;
    selection-background-color: %PRIMARY%;
    selection-color: %ON_PRIMARY%;
}
QLineEdit#DatePickerInput:focus { border-color: %FOCUS_RING%; }
QLineEdit#DatePickerInput[invalid="true"] { border-color: %DANGER%; }
QCheckBox#DatePickerCheck { color: %TEXT%; background: transparent; spacing: 6px; }
)QSS");
    struct Replacement {
        const char* key;
        ds::ColorToken token;
    };
    const Replacement replacements[] = {
        {"%TEXT%", ds::ColorToken::Text},
        {"%TEXT_SECONDARY%", ds::ColorToken::TextSecondary},
        {"%TEXT_TERTIARY%", ds::ColorToken::TextTertiary},
        {"%ON_PRIMARY%", ds::ColorToken::OnPrimary},
        {"%PRIMARY%", ds::ColorToken::Primary},
        {"%PRIMARY_HOVER%", ds::ColorToken::PrimaryHover},
        {"%PRIMARY_SOFT%", ds::ColorToken::PrimarySoft},
        {"%FOCUS_RING%", ds::ColorToken::FocusRing},
        {"%SURFACE%", ds::ColorToken::Surface},
        {"%BORDER%", ds::ColorToken::Border},
        {"%FIELD%", ds::ColorToken::Field},
        {"%FIELD_HOVER%", ds::ColorToken::FieldHover},
        {"%HOVER%", ds::ColorToken::Hover},
        {"%DANGER%", ds::ColorToken::Danger}
    };
    for (const auto& replacement : replacements)
        qss.replace(QLatin1String(replacement.key), color(replacement.token));
    m_card->setStyleSheet(qss);

    for (QToolButton* button : m_quickButtons)
        button->setIcon(quickIcon(button->property("quickIndex").toInt()));
    const auto labels = m_card->findChildren<QLabel*>();
    for (QLabel* label : labels) {
        if (!label->property("iconId").isValid()) continue;
        const IconId id = static_cast<IconId>(label->property("iconId").toInt());
        label->setPixmap(AppIcons::icon(id, 16).pixmap(16, 16));
    }
    const auto edits = m_card->findChildren<QLineEdit*>();
    for (QLineEdit* edit : edits) {
        QPalette palette = edit->palette();
        palette.setColor(QPalette::PlaceholderText, ThemeManager::instance().color(ds::ColorToken::TextTertiary));
        edit->setPalette(palette);
    }
    if (auto* effect = qobject_cast<QGraphicsDropShadowEffect*>(m_card->graphicsEffect())) {
        QColor shadow = ThemeManager::instance().color(ds::ColorToken::Text);
        shadow.setAlpha(40);
        effect->setColor(shadow);
    }
    m_card->update();
}

void DatePickerPopup::refreshWeekdayLabels()
{
    const QString names[] = {
        QStringLiteral("一"), QStringLiteral("二"), QStringLiteral("三"), QStringLiteral("四"),
        QStringLiteral("五"), QStringLiteral("六"), QStringLiteral("日")
    };
    for (int column = 0; column < m_weekdayLabels.size(); ++column) {
        const int day = ((m_firstDay - 1 + column) % 7) + 1;
        m_weekdayLabels.at(column)->setText(names[day - 1]);
    }
}

void DatePickerPopup::buildCalendarGrid()
{
    if (m_dayButtons.size() != 42 || !m_visibleMonth.isValid()) return;
    const QDate first(m_visibleMonth.year(), m_visibleMonth.month(), 1);
    const int offset = (7 + first.dayOfWeek() - m_firstDay) % 7;
    const QDate start = first.addDays(-offset);
    const QDate selected = m_dateTime.isValid() ? toAppTime(m_dateTime).date() : QDate();
    for (int index = 0; index < 42; ++index) {
        const QDate date = start.addDays(index);
        QToolButton* button = m_dayButtons.at(index);
        button->setText(QString::number(date.day()));
        button->setProperty("date", date);
        button->setProperty("outsideMonth", date.month() != m_visibleMonth.month());
        button->setChecked(m_hasDate && date == selected);
        button->setToolTip(date.toString(QStringLiteral("yyyy年M月d日")));
        button->setAccessibleName(date.toString(QStringLiteral("yyyy年M月d日 dddd")));
        repolish(button);
    }
}

void DatePickerPopup::updateMonthLabel()
{
    if (m_monthLabel)
        m_monthLabel->setText(QStringLiteral("%1年%2月").arg(m_visibleMonth.year()).arg(m_visibleMonth.month()));
}

void DatePickerPopup::selectDate(const QDate& date)
{
    if (!date.isValid()) return;
    const QDateTime current = toAppTime(m_dateTime);
    const QTime time = m_hasDate && current.isValid() ? current.time() : QTime(0, 0);
    m_dateTime = fromAppTime(QDateTime(date, time));
    m_hasDate = true;
    m_cleared = false;
    m_visibleMonth = QDate(date.year(), date.month(), 1);
    updateMonthLabel();
    buildCalendarGrid();
}

void DatePickerPopup::setRangeMode(bool range)
{
    m_rangeButton->setChecked(range);
    m_dateButton->setChecked(!range);
    m_modeStack->setCurrentIndex(range ? 1 : 0);
    m_okButton->setEnabled(!range);
}

void DatePickerPopup::showMainPage()
{
    m_pages->setCurrentIndex(0);
}

void DatePickerPopup::openTimePage()
{
    const QDateTime local = toAppTime(m_dateTime);
    m_timeList->setCurrentRow(-1);
    if (m_hasDate && local.isValid()) {
        const int minute = local.time().hour() * 60 + local.time().minute();
        if (local.time().second() == 0 && local.time().msec() == 0 && minute % 30 == 0) {
            const int row = minute / 30;
            m_timeList->setCurrentRow(row);
            if (QListWidgetItem* item = m_timeList->item(row)) m_timeList->scrollToItem(item);
        }
        m_timeEdit->setText(local.time().toString(QStringLiteral("HH:mm")));
    } else {
        m_timeEdit->clear();
    }
    setInputInvalid(m_timeEdit, false);
    m_pages->setCurrentIndex(1);
    m_timeList->setFocus();
}

void DatePickerPopup::openReminderPage()
{
    int selectedRow = 0;
    const QDateTime local = toAppTime(m_reminder);
    const QTime nine(9, 0);
    if (local.isValid()) {
        selectedRow = 5;
        for (int row = 0; row < 5; ++row) {
            const int offset = m_reminderList->item(row)->data(Qt::UserRole).toInt();
            if (local.date() == selectedDate().addDays(offset) && local.time() == nine) {
                selectedRow = row;
                break;
            }
        }
        if (selectedRow == 5)
            m_customReminderEdit->setText(local.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        else
            m_customReminderEdit->setText(QDateTime(selectedDate(), nine).toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    } else {
        m_customReminderEdit->setText(QDateTime(selectedDate(), nine).toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    }
    m_persistentCheck->setChecked(m_persistentReminder);
    setInputInvalid(m_customReminderEdit, false);
    m_reminderList->setCurrentRow(selectedRow);
    m_pages->setCurrentIndex(2);
    if (selectedRow == 5) m_customReminderEdit->setFocus();
    else m_reminderList->setFocus();
}

void DatePickerPopup::openRecurrencePage()
{
    int row = -1;
    for (int index = 0; index < 5; ++index) {
        if (m_recurrenceList->item(index)->data(Qt::UserRole).toString() == m_recurrence) {
            row = index;
            break;
        }
    }
    if (row < 0 && !m_recurrence.isEmpty() && m_recurrence != QLatin1String("none")) row = 5;
    if (row == 5) m_customRecurrenceEdit->setText(m_recurrence);
    setInputInvalid(m_customRecurrenceEdit, false);
    m_recurrenceList->setCurrentRow(row);
    m_pages->setCurrentIndex(3);
    if (row == 5) m_customRecurrenceEdit->setFocus();
    else m_recurrenceList->setFocus();
}

void DatePickerPopup::applyTime(const QTime& time)
{
    if (!time.isValid()) return;
    m_dateTime = fromAppTime(QDateTime(selectedDate(), time));
    m_hasDate = true;
    m_cleared = false;
    m_timeEdit->setText(time.toString(QStringLiteral("HH:mm")));
    setInputInvalid(m_timeEdit, false);
    buildCalendarGrid();
}

bool DatePickerPopup::applyCustomTime()
{
    // 自定义输入的校验与提交方式是 KylinList 自定；考据没有执行输入校验。
    static const QRegularExpression format(QStringLiteral("^(?:[01][0-9]|2[0-3]):[0-5][0-9]$"));
    const QString text = m_timeEdit->text().trimmed();
    const QTime time = QTime::fromString(text, QStringLiteral("HH:mm"));
    if (!format.match(text).hasMatch() || !time.isValid()) {
        setInputInvalid(m_timeEdit, true, QStringLiteral("请输入 00:00–23:59 时间"));
        m_timeEdit->setFocus();
        m_timeEdit->selectAll();
        return false;
    }
    applyTime(time);
    return true;
}

bool DatePickerPopup::applyReminder()
{
    const int row = m_reminderList->currentRow();
    if (row < 0) return false;
    if (row < 5) {
        const int offset = m_reminderList->item(row)->data(Qt::UserRole).toInt();
        m_reminder = fromAppTime(QDateTime(selectedDate().addDays(offset), QTime(9, 0)));
    } else {
        // 自定义提醒表单与校验为 KylinList 自定；现场研究未进入该深层表单。
        static const QRegularExpression format(
            QStringLiteral("^[0-9]{4}-[0-9]{2}-[0-9]{2} (?:[01][0-9]|2[0-3]):[0-5][0-9]$"));
        const QString text = m_customReminderEdit->text().trimmed();
        const QDateTime local = QDateTime::fromString(text, QStringLiteral("yyyy-MM-dd HH:mm"));
        if (!format.match(text).hasMatch() || !local.isValid()
            || local.toString(QStringLiteral("yyyy-MM-dd HH:mm")) != text) {
            setInputInvalid(m_customReminderEdit, true, QStringLiteral("请输入有效的 yyyy-MM-dd HH:mm"));
            m_customReminderEdit->setFocus();
            m_customReminderEdit->selectAll();
            return false;
        }
        m_reminder = fromAppTime(local);
    }
    m_persistentReminder = m_persistentCheck->isChecked();
    m_cleared = false;
    return true;
}

bool DatePickerPopup::applyCustomRecurrence()
{
    const QString rule = m_customRecurrenceEdit->text().trimmed();
    if (rule.isEmpty()) {
        setInputInvalid(m_customRecurrenceEdit, true, QStringLiteral("请输入重复规则"));
        m_customRecurrenceEdit->setFocus();
        return false;
    }
    m_recurrence = rule;
    m_cleared = false;
    setInputInvalid(m_customRecurrenceEdit, false);
    return true;
}

QDate DatePickerPopup::selectedDate() const
{
    const QDateTime local = toAppTime(m_dateTime);
    return local.isValid() ? local.date() : toAppTime(appNow()).date();
}

void DatePickerPopup::setDateTime(const QDateTime& dt)
{
    m_dateTime = dt.isValid() ? dt.toUTC() : QDateTime();
    if (!m_dateTime.isValid()) m_hasDate = false;
    if (m_dateTime.isValid()) {
        const QDate date = toAppTime(m_dateTime).date();
        m_visibleMonth = QDate(date.year(), date.month(), 1);
    }
    m_cleared = false;
    updateMonthLabel();
    buildCalendarGrid();
}

void DatePickerPopup::setHasDate(bool has)
{
    if (has && !m_dateTime.isValid())
        m_dateTime = fromAppTime(QDateTime(selectedDate(), QTime(0, 0)));
    m_hasDate = has;
    m_cleared = false;
    buildCalendarGrid();
}

void DatePickerPopup::setReminder(const QDateTime& reminder)
{
    m_reminder = reminder.isValid() ? reminder.toUTC() : QDateTime();
    if (!m_reminder.isValid()) m_persistentReminder = false;
    m_cleared = false;
}

void DatePickerPopup::setRecurrence(const QString& rule)
{
    const QString trimmed = rule.trimmed();
    m_recurrence = trimmed.isEmpty() ? QStringLiteral("none") : trimmed;
    m_cleared = false;
}

QDateTime DatePickerPopup::dateTime() const
{
    return m_hasDate && m_dateTime.isValid() ? m_dateTime.toUTC() : QDateTime();
}

bool DatePickerPopup::hasDate() const
{
    return m_hasDate;
}

QDateTime DatePickerPopup::reminder() const
{
    return m_reminder.isValid() ? m_reminder.toUTC() : QDateTime();
}

QString DatePickerPopup::recurrence() const
{
    return m_recurrence;
}

bool DatePickerPopup::cleared() const
{
    return m_cleared;
}

void DatePickerPopup::popupBelow(QWidget* anchor)
{
    if (!anchor) return;
    m_cleared = false;
    setRangeMode(false);
    showMainPage();
    const int firstDay = configuredFirstDay();
    if (m_firstDay != firstDay) {
        m_firstDay = firstDay;
        refreshWeekdayLabels();
        buildCalendarGrid();
    }

    const QPoint anchorBottom = anchor->mapToGlobal(QPoint(0, anchor->height()));
    QPoint position(anchorBottom.x() - kShadowMargin, anchorBottom.y() + 6 - kShadowMargin);
    const QPoint anchorCenter = anchor->mapToGlobal(anchor->rect().center());
    QScreen* screen = QGuiApplication::screenAt(anchorCenter);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect area = screen->availableGeometry();
        position.setX(qMax(area.left(), qMin(position.x(), area.right() - width() + 1)));
        position.setY(qMax(area.top(), qMin(position.y(), area.bottom() - height() + 1)));
    }
    move(position);
    show();
    raise();
    activateWindow();

    bool focused = false;
    if (m_hasDate) {
        for (QToolButton* button : m_dayButtons) {
            if (button->isChecked() && button->property("outsideMonth").toBool() == false) {
                button->setFocus();
                focused = true;
                break;
            }
        }
    }
    if (!focused) m_dateButton->setFocus();
}

bool DatePickerPopup::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        auto* button = qobject_cast<QAbstractButton*>(watched);
        if (button && button->isVisible() && button->isEnabled()
            && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)) {
            button->click();
            event->accept();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

} // namespace kl
