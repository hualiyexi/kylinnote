#include "DatePickerPopup.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QEvent>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStyle>
#include <QStylePainter>
#include <QTime>
#include <QTimeZone>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "../Common.h"
#include "../app/Settings.h"
#include "../domain/Entity.h"
#include "../domain/ReminderRule.h"
#include "AnchoredListPopup.h"
#include "AppIcons.h"
#include "CompactSwitch.h"
#include "DateCalendarPanel.h"
#include "DatePickerControls.h"
#include "DatePickerReminderEditor.h"
#include "DatePickerRecurrenceEditor.h"
#include "PopupShadow.h"
#include "platform/SystemShadow.h"
#include "TextGeometry.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {

constexpr int kCardWidth = 260;
constexpr int kShadowMargin = PopupShadowMargin;
constexpr qreal kCardRadius = 14.0;
constexpr int kCalendarCell = 30;
constexpr int kRangeControlHeight = 28;

// 子层几何（滴答网页实测）：宽 252、列表行高 34、卡片圆角 12。
constexpr int kSubCardWidth = 252;
constexpr int kSubRowHeight = 34;
constexpr int kControlHeight = 24;   // 网页研究结论：选择器 24px 高 / 6px 圆角

QString color(ds::ColorToken token)
{
    return ThemeManager::instance().color(token).name();
}

QIcon quickIcon(int index)
{
    // 今天 / 明天 / 今天 +7 天 / 下个月同日。
    static const char* const kIds[] = {"quick-today", "tomorrow-list",
                                       "quick-nextweek", "quick-nextmonth"};
    if (index < 0 || index >= 4) return QIcon();
    return AppIcons::icon(QString::fromLatin1(kIds[index]),
                          ds::ColorToken::TextSecondary, 26);
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

QListWidgetItem* makeRowItem(const QString& text)
{
    auto* item = new QListWidgetItem(text);
    item->setSizeHint(QSize(0, kSubRowHeight));
    return item;
}

// 子层通用动作按钮：AutoDefault false，避免 Enter 被对话框默认键截获。
QPushButton* makeSubButton(const QString& text, const char* objectName, QWidget* parent,
                           int height = 30)
{
    auto* button = new QPushButton(text, parent);
    button->setObjectName(QString::fromLatin1(objectName));
    button->setAutoDefault(false);
    button->setDefault(false);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setFixedHeight(height);
    return button;
}

QSpinBox* makeSpin(int minimum, int maximum, const QString& accessibleName, QWidget* parent)
{
    auto* spin = new QSpinBox(parent);
    spin->setFixedHeight(kControlHeight);
    spin->setRange(minimum, maximum);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    spin->setAccessibleName(accessibleName);
    return spin;
}

class DatePickerEntryButton final : public QPushButton {
public:
    DatePickerEntryButton(IconId iconId, const QString& text, QWidget* parent)
        : QPushButton(text, parent), m_iconId(iconId)
    {
        setObjectName(QStringLiteral("DatePickerEntry"));
        setFixedHeight(34);
        setAccessibleName(text);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStyleOptionButton option;
        initStyleOption(&option);
        option.icon = QIcon();
        option.text.clear();
        QStylePainter painter(this);
        painter.drawControl(QStyle::CE_PushButton, option);

        const QRectF content = QRectF(rect()).adjusted(16, 0, -16, 0);
        if (content.isEmpty()) return;
        const qreal center = content.center().y();
        auto& theme = ThemeManager::instance();
        const QString caption = property("entryLabel").toString();
        AppIcons::paint(&painter, caption.isEmpty() ? QStringLiteral("chevron-right")
                                                   : QStringLiteral("chevron-down"),
                        QRectF(content.right() - 12, center - 6, 12, 12),
                        theme.color(ds::ColorToken::TextTertiary));
        painter.setFont(font());
        const QFontMetricsF metrics(font());
        QRectF label = content.adjusted(0, 0, -20, 0);
        if (caption.isEmpty()) {
            AppIcons::paint(&painter, m_iconId,
                            QRectF(content.left(), center - 9, 18, 18),
                            theme.color(ds::ColorToken::TextSecondary));
            label.adjust(26, 0, 0, 0);
        } else {
            painter.setPen(theme.color(ds::ColorToken::TextSecondary));
            painter.drawText(QPointF(content.left(), textBaselineForCenter(metrics, center, caption)), caption);
            label.adjust(metrics.horizontalAdvance(caption) + 12, 0, 0, 0);
        }
        if (label.isEmpty()) return;
        painter.setPen(theme.color(ds::ColorToken::Text));
        const QString shown = metrics.elidedText(text(), Qt::ElideRight,
                                                 static_cast<int>(label.width()));
        const qreal left = caption.isEmpty() ? label.left()
                                             : label.right() - metrics.horizontalAdvance(shown);
        painter.drawText(QPointF(left, textBaselineForCenter(metrics, center, shown)), shown);
    }

private:
    IconId m_iconId;
};

} // namespace

DatePickerPopup::DatePickerPopup(QWidget* parent)
    : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setObjectName(QStringLiteral("DatePickerPopup"));
    setWindowTitle(QStringLiteral("日期选择器"));
    setAttribute(Qt::WA_TranslucentBackground);
    disableNativeWindowShadows();
    setModal(false);
    setFont(ds::qfont(ds::FontScene::Body));
    m_firstDay = configuredFirstDay();

    buildUi();
    applyTheme();
    wireEnterKey(this);

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
    m_card->setFixedWidth(kCardWidth);
    outer->addWidget(m_card, 0, Qt::AlignCenter);

    auto* cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);
    m_mainScroll = new QScrollArea(m_card);
    m_mainScroll->setObjectName(QStringLiteral("DatePickerMainScroll"));
    m_mainScroll->setFrameShape(QFrame::NoFrame);
    m_mainScroll->setWidgetResizable(true);
    m_mainScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_mainScroll->verticalScrollBar()->setObjectName(QStringLiteral("DatePickerMainScrollbar"));
    m_mainPage = buildMainPage(m_card);
    m_footer = m_okButton->parentWidget();
    m_mainPage->layout()->removeWidget(m_footer);
    m_footer->setParent(m_card);
    m_mainScroll->setWidget(m_mainPage);
    m_mainScroll->viewport()->setAutoFillBackground(false);
    m_mainPage->setAutoFillBackground(false);
    cardLayout->addWidget(m_mainScroll);
    cardLayout->addWidget(m_footer);
    updateCardGeometry();
}

void DatePickerPopup::rebuildShadow(const QColor& color)
{
    m_shadow = popupShadow(size(), QRectF(m_card->geometry()), kCardRadius, color);
    m_shadowColor = color;
}

void DatePickerPopup::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    const QColor shadowColor = ThemeManager::instance().color(ds::ColorToken::Shadow);
    if (m_shadow.size() != size() || m_shadowColor != shadowColor) rebuildShadow(shadowColor);
    if (!m_shadow.isNull()) {
        QPainter painter(this);
        painter.drawImage(0, 0, m_shadow);
    }
}

void DatePickerPopup::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    m_shadow = QImage();
    m_shadowColor = QColor();
    update();
}

QWidget* DatePickerPopup::buildMainPage(QWidget* parent)
{
    auto* page = new QWidget(parent);
    page->setObjectName(QStringLiteral("DatePickerMainPage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* segmentSection = new QWidget(page);
    auto* segmentSectionLayout = new QHBoxLayout(segmentSection);
    segmentSectionLayout->setContentsMargins(16, 12, 16, 0);
    segmentSectionLayout->setSpacing(0);
    auto* segmentHost = new QWidget(segmentSection);
    segmentHost->setObjectName(QStringLiteral("DatePickerSegmentHost"));
    segmentHost->setFixedHeight(30);
    auto* segment = new QHBoxLayout(segmentHost);
    segment->setContentsMargins(2, 2, 2, 2);
    segment->setSpacing(0);
    m_dateButton = new QPushButton(QStringLiteral("日期"), segmentHost);
    m_dateButton->setObjectName(QStringLiteral("DatePickerSegment"));
    m_dateButton->setCheckable(true);
    m_dateButton->setChecked(true);
    m_dateButton->setFixedHeight(26);
    m_dateButton->setFont(ds::qfont(ds::FontScene::Menu));
    m_rangeButton = new QPushButton(QStringLiteral("时间段"), segmentHost);
    m_rangeButton->setObjectName(QStringLiteral("DatePickerSegment"));
    m_rangeButton->setCheckable(true);
    m_rangeButton->setFixedHeight(26);
    m_rangeButton->setFont(ds::qfont(ds::FontScene::Menu));
    segment->addWidget(m_dateButton, 1);
    segment->addWidget(m_rangeButton, 1);
    segmentSectionLayout->addWidget(segmentHost);
    layout->addWidget(segmentSection);

    m_datePage = buildDatePage(page);
    m_rangePage = buildRangePage(page);
    layout->addWidget(m_datePage);
    layout->addWidget(m_rangePage);

    m_calendarSection = new QWidget(page);
    auto* calendarLayout = new QHBoxLayout(m_calendarSection);
    calendarLayout->setContentsMargins(10, 0, 10, 15);
    calendarLayout->setSpacing(0);
    m_inlineCalendar = new DateCalendarPanel(m_calendarSection);
    m_inlineCalendar->setFirstDay(m_firstDay);
    m_inlineCalendar->setToday(today());
    calendarLayout->addWidget(m_inlineCalendar, 0, Qt::AlignCenter);
    layout->addWidget(m_calendarSection);
    connect(m_inlineCalendar, &DateCalendarPanel::dateActivated,
            this, &DatePickerPopup::selectDate);

    m_timeEntry = new DatePickerEntryButton(IconId::Clock, QStringLiteral("时间"), page);
    m_reminderEntry = new DatePickerEntryButton(IconId::Reminder, QStringLiteral("提醒"), page);
    m_recurrenceEntry = new DatePickerEntryButton(IconId::Repeat, QStringLiteral("重复"), page);
    // 网页三行均为 252px，悬浮背景与 260px 父卡片左右各留 4px。
    for (QPushButton* entry : {m_timeEntry, m_reminderEntry, m_recurrenceEntry})
        entry->setFixedWidth(kSubCardWidth);
    m_timeEntrySection = new QWidget(page);
    auto* timeLayout = new QVBoxLayout(m_timeEntrySection);
    timeLayout->setContentsMargins(0, 0, 0, 2);
    timeLayout->setSpacing(0);
    timeLayout->addWidget(m_timeEntry, 0, Qt::AlignHCenter);
    layout->addWidget(m_timeEntrySection);
    layout->addWidget(m_reminderEntry, 0, Qt::AlignHCenter);
    m_recurrenceEntrySection = new QWidget(page);
    auto* recurrenceLayout = new QVBoxLayout(m_recurrenceEntrySection);
    recurrenceLayout->setContentsMargins(0, 4, 0, 2);
    recurrenceLayout->setSpacing(0);
    recurrenceLayout->addWidget(m_recurrenceEntry, 0, Qt::AlignHCenter);
    layout->addWidget(m_recurrenceEntrySection);

    m_recurrenceEndSection = new QWidget(page);
    auto* recurrenceEndLayout = new QVBoxLayout(m_recurrenceEndSection);
    recurrenceEndLayout->setContentsMargins(0, 0, 0, 2);
    recurrenceEndLayout->setSpacing(0);
    m_recurrenceEndEntry = new DatePickerEntryButton(
        IconId::Repeat, QStringLiteral("一直重复"), m_recurrenceEndSection);
    m_recurrenceEndEntry->setFixedWidth(kSubCardWidth);
    m_recurrenceEndEntry->setAccessibleName(QStringLiteral("结束重复"));
    m_recurrenceEndEntry->setProperty("entryLabel", QStringLiteral("结束重复"));
    recurrenceEndLayout->addWidget(m_recurrenceEndEntry, 0, Qt::AlignHCenter);
    m_recurrenceEndSection->setVisible(false);
    layout->addWidget(m_recurrenceEndSection);
    connect(m_recurrenceEndEntry, &QPushButton::clicked,
            this, &DatePickerPopup::openRecurrenceEndPage);

    m_errorLabel = new QLabel(page);
    m_errorLabel->setObjectName(QStringLiteral("DatePickerError"));
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setContentsMargins(16, 4, 16, 0);
    m_errorLabel->setVisible(false);
    layout->addWidget(m_errorLabel);

    auto* footer = new QWidget(page);
    auto* actions = new QHBoxLayout(footer);
    actions->setContentsMargins(16, 16, 16, 16);
    actions->setSpacing(8);
    auto* clear = new QPushButton(QStringLiteral("清除"), footer);
    clear->setObjectName(QStringLiteral("DatePickerClear"));
    clear->setFixedHeight(30);
    m_okButton = new QPushButton(QStringLiteral("确定"), footer);
    m_okButton->setObjectName(QStringLiteral("DatePickerConfirm"));
    m_okButton->setFixedHeight(30);
    actions->addWidget(clear, 1);
    actions->addWidget(m_okButton, 1);
    layout->addWidget(footer);

    connect(m_dateButton, &QPushButton::clicked, this, [this]() { setRangeMode(false); });
    connect(m_rangeButton, &QPushButton::clicked, this, [this]() { setRangeMode(true); });
    connect(m_timeEntry, &QPushButton::clicked, this, &DatePickerPopup::openTimePage);
    connect(m_reminderEntry, &QPushButton::clicked, this, &DatePickerPopup::openReminderPage);
    connect(m_recurrenceEntry, &QPushButton::clicked, this, &DatePickerPopup::openRecurrencePage);
    connect(clear, &QPushButton::clicked, this, [this]() {
        m_schedule = ScheduleValue();
        m_reminders.clear();
        m_range = false;
        m_allDayRange = false;
        m_singleDate = QDate();
        m_singleTime = QTime();
        m_rangeStartDate = QDate();
        m_rangeEndDate = QDate();
        m_rangeStartTime = QTime();
        m_rangeEndTime = QTime();
        m_rangeCalendarEnd = false;
        m_cleared = true;
        emit cleared();
        reject();
    });
    connect(m_okButton, &QPushButton::clicked, this, [this]() { accept(); });

    refreshAll();
    return page;
}

QWidget* DatePickerPopup::buildDatePage(QWidget* parent)
{
    auto* page = new QWidget(parent);
    page->setObjectName(QStringLiteral("DatePickerDatePage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 24, 16, 24);
    layout->setSpacing(0);

    auto* quickRow = new QHBoxLayout;
    quickRow->setContentsMargins(6, 0, 6, 0);
    quickRow->setSpacing(0);
    const QString quickLabels[] = {
        QStringLiteral("今天"), QStringLiteral("明天"),
        QStringLiteral("7天后"), QStringLiteral("下个月同日")
    };
    for (int i = 0; i < 4; ++i) {
        auto* button = new QToolButton(page);
        button->setObjectName(QStringLiteral("DatePickerQuick"));
        button->setProperty("quickIndex", i);
        button->setIconSize(QSize(26, 26));
        button->setFixedSize(26, 26);
        button->setToolTip(i == 3 ? QStringLiteral("下个月同日（月末不足时取最后一天）") : quickLabels[i]);
        button->setAccessibleName(quickLabels[i]);
        // 快捷入口立即接受：保留既有时刻（含 00:00），无时刻保持全天。
        connect(button, &QToolButton::clicked, this, [this, i]() {
            selectDate(quickDate(i, today()));
            accept();
        });
        m_quickButtons.append(button);
        quickRow->addWidget(button);
        if (i < 3) quickRow->addStretch();
    }
    layout->addLayout(quickRow);
    return page;
}

QWidget* DatePickerPopup::buildRangePage(QWidget* parent)
{
    auto* page = new QWidget(parent);
    page->setObjectName(QStringLiteral("DatePickerRangePage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 19, 16, 21);
    layout->setSpacing(8);

    auto makeRangeRow = [page](const QString& role, QPushButton** dateButton,
                               QPushButton** timeButton) {
        auto* row = new QWidget(page);
        row->setObjectName(QStringLiteral("DatePickerRangeRow"));
        row->setFixedHeight(kRangeControlHeight);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);
        auto* roleLabel = new QLabel(role, row);
        roleLabel->setProperty("rangeRole", true);
        roleLabel->setFont(ds::qfont(ds::FontScene::Menu));
        roleLabel->setFixedWidth(30);
        roleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        auto* date = new QPushButton(QStringLiteral("选择日期"), row);
        date->setObjectName(QStringLiteral("DatePickerRangeDate"));
        date->setAutoDefault(false);
        date->setFixedHeight(kRangeControlHeight);
        date->setFont(ds::qfont(ds::FontScene::Menu));
        date->setAccessibleName(role + QStringLiteral("日期"));
        auto* time = new QPushButton(QStringLiteral("09:00"), row);
        time->setObjectName(QStringLiteral("DatePickerRangeTime"));
        time->setAutoDefault(false);
        time->setFixedSize(95, kRangeControlHeight);
        time->setFont(ds::qfont(ds::FontScene::Menu));
        time->setAccessibleName(role + QStringLiteral("时间"));
        rowLayout->addWidget(roleLabel);
        rowLayout->addWidget(date, 1);
        rowLayout->addWidget(time);
        *dateButton = date;
        *timeButton = time;
        return row;
    };
    m_rangeStartRow = makeRangeRow(QStringLiteral("开始"), &m_rangeStartDateButton,
                                   &m_rangeStartTimeButton);
    layout->addWidget(m_rangeStartRow);
    m_rangeEndRow = makeRangeRow(QStringLiteral("结束"), &m_rangeEndDateButton,
                                 &m_rangeEndTimeButton);
    layout->addWidget(m_rangeEndRow);

    auto* allDayRow = new QWidget(page);
    allDayRow->setFixedHeight(25);
    auto* allDayLayout = new QHBoxLayout(allDayRow);
    allDayLayout->setContentsMargins(0, 7, 0, 0);
    allDayLayout->setSpacing(0);
    auto* allDayLabel = new QLabel(QStringLiteral("全天"), allDayRow);
    allDayLabel->setProperty("rangeRole", true);
    allDayLabel->setFont(ds::qfont(ds::FontScene::Menu));
    allDayLabel->setFixedHeight(18);
    m_allDayCheck = new CompactSwitch(allDayRow);
    m_allDayCheck->setObjectName(QStringLiteral("DatePickerAllDayCheck"));
    m_allDayCheck->setAccessibleName(QStringLiteral("全天"));
    m_allDayCheck->setToolTip(QStringLiteral("隐藏时刻并保留时刻草稿"));
    allDayLayout->addWidget(allDayLabel);
    allDayLayout->addStretch();
    allDayLayout->addWidget(m_allDayCheck);
    layout->addWidget(allDayRow);

    connect(m_allDayCheck, &QCheckBox::toggled, this,
            [this](bool checked) { setAllDayRange(checked); });
    connect(m_rangeStartDateButton, &QPushButton::clicked, this, [this]() {
        openRangeCalendar(false);
    });
    connect(m_rangeEndDateButton, &QPushButton::clicked, this, [this]() {
        openRangeCalendar(true);
    });
    connect(m_rangeStartTimeButton, &QPushButton::clicked, this, [this]() {
        openTimePageFor(TimeTarget::RangeStart, m_rangeStartTimeButton);
    });
    connect(m_rangeEndTimeButton, &QPushButton::clicked, this, [this]() {
        openTimePageFor(TimeTarget::RangeEnd, m_rangeEndTimeButton);
    });
    return page;
}

QWidget* DatePickerPopup::ensureTimeExpansion()
{
    if (m_timeExpansion) return m_timeExpansion;
    m_timeExpansion = new QWidget(m_card);
    m_timeExpansion->setObjectName(QStringLiteral("DatePickerTimeExpansion"));
    auto* layout = new QVBoxLayout(m_timeExpansion);
    layout->setContentsMargins(4, 0, 4, 4);
    layout->setSpacing(4);

    m_timeList = new QListWidget(m_timeExpansion);
    m_timeList->setObjectName(QStringLiteral("DatePickerList"));
    m_timeList->setAccessibleName(QStringLiteral("半小时时间列表"));
    m_timeList->setVerticalScrollMode(QListWidget::ScrollPerPixel);
    m_timeList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_timeList->setUniformItemSizes(true);
    for (int minute = 0; minute < 24 * 60; minute += 30) {
        const QTime time = QTime(0, 0).addSecs(minute * 60);
        auto* item = makeRowItem(time.toString(QStringLiteral("HH:mm")));
        item->setData(Qt::UserRole, minute);
        m_timeList->addItem(item);
    }
    m_timeList->setFixedHeight(6 * kSubRowHeight);
    layout->addWidget(m_timeList);

    m_timeEdit = new QLineEdit(m_timeExpansion);
    m_timeEdit->setObjectName(QStringLiteral("DatePickerInput"));
    m_timeEdit->setPlaceholderText(QStringLiteral("设置时间"));
    m_timeEdit->setMaxLength(5);
    m_timeEdit->setFixedHeight(30);
    m_timeEdit->setAccessibleName(QStringLiteral("设置时间"));
    m_timeEdit->setClearButtonEnabled(true);
    m_timeEdit->setToolTip(QStringLiteral("输入时间，如 9:04；单日清空后回车可移除时刻"));
    layout->addWidget(m_timeEdit);
    m_timeExpansion->setVisible(false);

    auto chooseItem = [this](QListWidgetItem* item) {
        if (!item || m_inlineExpansion != InlineExpansion::Time) return;
        const QTime time = QTime(0, 0).addSecs(item->data(Qt::UserRole).toInt() * 60);
        applyTime(time);
        closeInlineExpansion(true);
    };
    connect(m_timeList, &QListWidget::itemClicked, this, chooseItem);
    connect(m_timeList, &QListWidget::itemActivated, this, chooseItem);
    connect(m_timeEdit, &QLineEdit::returnPressed, this, [this]() {
        if (applyCustomTime()) closeInlineExpansion(true);
    });
    connect(m_timeEdit, &QLineEdit::textChanged, this, [this]() {
        setInputInvalid(m_timeEdit, false);
    });
    wireEnterKey(m_timeExpansion);
    return m_timeExpansion;
}

DateCalendarPanel* DatePickerPopup::ensureInlineRangeCalendar()
{
    if (m_rangeCalendar) return m_rangeCalendar;
    m_rangeCalendar = new DateCalendarPanel(m_card);
    m_rangeCalendar->setObjectName(QStringLiteral("DatePickerRangeCalendarInline"));
    m_rangeCalendar->setCompact(true);
    m_rangeCalendar->setFirstDay(m_firstDay);
    m_rangeCalendar->setToday(today());
    m_rangeCalendar->setVisible(false);
    connect(m_rangeCalendar, &DateCalendarPanel::dateActivated,
            this, &DatePickerPopup::selectRangeDate);
    wireEnterKey(m_rangeCalendar);
    return m_rangeCalendar;
}

void DatePickerPopup::wireEnterKey(QWidget* scope)
{
    if (!scope) return;
    const auto buttons = scope->findChildren<QAbstractButton*>();
    for (QAbstractButton* button : buttons) {
        button->setFocusPolicy(Qt::StrongFocus);
        if (auto* push = qobject_cast<QPushButton*>(button)) {
            push->setAutoDefault(false);
            push->setDefault(false);
        }
        button->installEventFilter(this);
    }
}

// ---------------------------------------------------------------------------
// 子层：提醒（菜单 + 独立自定义页，稳定 UUID）
// ---------------------------------------------------------------------------

AnchoredListPopup* DatePickerPopup::ensureReminderPopup()
{
    if (m_reminderPopup) return m_reminderPopup;
    m_reminderEditor = new DatePickerReminderEditor(this);
    m_reminderPopup = m_reminderEditor->popup();
    connect(m_reminderPopup, &QDialog::finished, this, [this](int result) {
        if (m_reminderEditSnapshotValid) {
            if (result != QDialog::Accepted) m_reminders = m_reminderEditSnapshot;
            m_reminderEditSnapshot.clear();
            m_reminderEditSnapshotValid = false;
            updateEntryCaptions();
        }
        if (m_reminderEntry->isVisible()) m_reminderEntry->setFocus();
    });
    wireEnterKey(m_reminderPopup);
    applyTheme();
    return m_reminderPopup;
}


int DatePickerPopup::findReminderRowByRule(const QString& ruleJson) const
{
    if (ruleJson.isEmpty()) return -1;
    for (int i = 0; i < m_reminders.size(); ++i)
        if (m_reminders.at(i).ruleJson == ruleJson) return i;
    return -1;
}

void DatePickerPopup::addReminderRule(const QString& ruleJson)
{
    if (ruleJson.isEmpty() || findReminderRowByRule(ruleJson) >= 0) return;
    TaskReminder row;
    row.id = newUuid();
    row.ruleJson = ruleJson;
    QDateTime trigger;
    QString error;
    if (resolveDraftReminder(row, &trigger, &error)) row.triggerAt = trigger;
    m_reminders.append(row);
}

void DatePickerPopup::removeReminderRule(const QString& ruleJson)
{
    const int index = findReminderRowByRule(ruleJson);
    if (index >= 0) m_reminders.remove(index);
}

void DatePickerPopup::switchReminderDraftMode(bool allDay)
{
    if (m_reminderModeAllDay == allDay) return;
    if (m_reminderPopup && m_reminderPopup->isVisible()) m_reminderPopup->reject();
    if (m_reminderModeAllDay) m_allDayReminderDraft = m_reminders;
    else m_timedReminderDraft = m_reminders;
    m_reminderModeAllDay = allDay;
    m_reminders = allDay ? m_allDayReminderDraft : m_timedReminderDraft;
}

// ---------------------------------------------------------------------------
// 子层：重复（预置 + 类型化自定义）
// ---------------------------------------------------------------------------

AnchoredListPopup* DatePickerPopup::ensureRecurrencePopup()
{
    if (m_recurrencePopup) return m_recurrencePopup;
    m_recurrenceEditor = new DatePickerRecurrenceEditor(this);
    m_recurrencePopup = m_recurrenceEditor->popup();
    connect(m_recurrencePopup, &QDialog::finished, m_recurrenceEntry, [this]() {
        if (m_recurrenceEntry->isVisible()) m_recurrenceEntry->setFocus();
    });
    wireEnterKey(m_recurrencePopup);
    applyTheme();
    return m_recurrencePopup;
}

void DatePickerPopup::applyRecurrencePreset(const RecurrenceSpec& spec)
{
    m_schedule.repeating = true;
    m_schedule.recurrence = spec;
    clearCardError();
    updateEntryCaptions();
}

void DatePickerPopup::closeInlineExpansion(bool restoreFocus)
{
    const QPointer<QWidget> focus = m_inlineAnchor;
    if (m_timeExpansion) {
        m_timeExpansion->setVisible(false);
        if (QWidget* parent = m_timeExpansion->parentWidget())
            if (QLayout* layout = parent->layout()) layout->removeWidget(m_timeExpansion);
    }
    if (m_rangeCalendar) {
        m_rangeCalendar->setVisible(false);
        if (QWidget* parent = m_rangeCalendar->parentWidget())
            if (QLayout* layout = parent->layout()) layout->removeWidget(m_rangeCalendar);
    }
    if (m_recurrenceEndCalendar) {
        m_recurrenceEndCalendar->setVisible(false);
        if (QWidget* parent = m_recurrenceEndCalendar->parentWidget())
            if (QLayout* layout = parent->layout()) layout->removeWidget(m_recurrenceEndCalendar);
    }
    if (m_recurrenceEndCountEditor) {
        m_recurrenceEndCountEditor->setVisible(false);
        if (QWidget* parent = m_recurrenceEndCountEditor->parentWidget())
            if (QLayout* layout = parent->layout()) layout->removeWidget(m_recurrenceEndCountEditor);
    }
    m_inlineExpansion = InlineExpansion::None;
    m_inlineAnchor.clear();
    if (m_calendarSection) m_calendarSection->setVisible(!m_range);
    updateCardGeometry();
    if (restoreFocus && focus && focus->isVisible()) {
        focus->setFocus();
        if (m_mainScroll) m_mainScroll->ensureWidgetVisible(focus, 0, 4);
    }
}

void DatePickerPopup::closeSubPopups()
{
    closeInlineExpansion(false);
    if (m_reminderPopup && m_reminderPopup->isVisible()) m_reminderPopup->reject();
    if (m_recurrencePopup && m_recurrencePopup->isVisible()) m_recurrencePopup->reject();
    if (m_recurrenceEndPopup && m_recurrenceEndPopup->isVisible())
        m_recurrenceEndPopup->reject();
}

int DatePickerPopup::calendarGuardY() const
{
    if (!m_inlineCalendar || !m_inlineCalendar->isVisible()) return -1;
    const auto days = m_inlineCalendar->findChildren<QToolButton*>(QStringLiteral("DatePickerDay"));
    if (days.isEmpty()) return -1;
    return days.first()->mapToGlobal(QPoint()).y() + kCalendarCell;
}

void DatePickerPopup::openRangeCalendar(bool end)
{
    if (!m_range) return;
    QPushButton* dateButton = end ? m_rangeEndDateButton : m_rangeStartDateButton;
    QWidget* row = end ? m_rangeEndRow : m_rangeStartRow;
    const bool toggleClosed = m_inlineExpansion == InlineExpansion::RangeCalendar
        && m_rangeCalendarEnd == end && m_inlineAnchor == dateButton;
    closeSubPopups();
    if (toggleClosed || !dateButton || !row) return;

    DateCalendarPanel* calendar = ensureInlineRangeCalendar();
    m_inlineExpansion = InlineExpansion::RangeCalendar;
    m_rangeCalendarEnd = end;
    m_inlineAnchor = dateButton;
    const QDate selected = end ? m_rangeEndDate : m_rangeStartDate;
    const QDate visible = selected.isValid() ? selected : today();
    calendar->setFirstDay(m_firstDay);
    calendar->setToday(today());
    calendar->setSelectedDate(selected);
    calendar->setVisibleMonth(visible);

    auto* layout = qobject_cast<QVBoxLayout*>(m_rangePage->layout());
    if (!layout) return;
    calendar->setParent(m_rangePage);
    layout->insertWidget(layout->indexOf(row) + 1, calendar, 0, Qt::AlignHCenter);
    calendar->setVisible(true);
    updateCardGeometry();
    m_mainScroll->ensureWidgetVisible(calendar, 0, 4);
    for (QToolButton* day : calendar->findChildren<QToolButton*>(QStringLiteral("DatePickerDay"))) {
        if ((selected.isValid() && day->isChecked())
            || (!selected.isValid() && !day->property("outsideMonth").toBool())) {
            day->setFocus();
            break;
        }
    }
}

void DatePickerPopup::openTimePageFor(TimeTarget target, QWidget* anchor)
{
    const bool toggleClosed = m_inlineExpansion == InlineExpansion::Time
        && m_timeTarget == target && m_inlineAnchor == anchor;
    closeSubPopups();
    if (toggleClosed || !anchor) return;
    QWidget* expansion = ensureTimeExpansion();
    m_inlineExpansion = InlineExpansion::Time;
    m_inlineAnchor = anchor;
    m_timeTarget = target;
    QTime current;
    if (target == TimeTarget::Single) current = m_singleTime;
    else if (target == TimeTarget::RangeStart) current = m_rangeStartTime;
    else current = m_rangeEndTime;
    m_timeList->setCurrentRow(-1);
    if (current.isValid()) {
        const int minute = current.hour() * 60 + current.minute();
        if (current.second() == 0 && current.msec() == 0 && minute % 30 == 0) {
            const int row = minute / 30;
            m_timeList->setCurrentRow(row);
            if (QListWidgetItem* item = m_timeList->item(row))
                m_timeList->scrollToItem(item, QAbstractItemView::PositionAtCenter);
        }
        m_timeEdit->setText(current.toString(QStringLiteral("HH:mm")));
    } else {
        m_timeEdit->clear();
    }
    setInputInvalid(m_timeEdit, false);

    if (target == TimeTarget::Single) {
        auto* layout = qobject_cast<QVBoxLayout*>(m_timeEntrySection->parentWidget()->layout());
        if (!layout) return;
        expansion->setParent(m_timeEntrySection->parentWidget());
        layout->insertWidget(layout->indexOf(m_timeEntrySection), expansion);
        m_calendarSection->setVisible(false);
    } else {
        QWidget* row = target == TimeTarget::RangeStart ? m_rangeStartRow : m_rangeEndRow;
        auto* layout = qobject_cast<QVBoxLayout*>(m_rangePage->layout());
        if (!layout) return;
        expansion->setParent(m_rangePage);
        layout->insertWidget(layout->indexOf(row) + 1, expansion);
    }
    expansion->setVisible(true);
    updateCardGeometry();
    m_timeEdit->setFocus();
    m_timeEdit->selectAll();
    m_mainScroll->ensureWidgetVisible(m_timeEdit, 0, 4);
}

void DatePickerPopup::openTimePage()
{
    openTimePageFor(TimeTarget::Single, m_timeEntry);
}

void DatePickerPopup::openReminderPage()
{
    if (m_reminderPopup && m_reminderPopup->isVisible()) return;
    closeSubPopups();
    ensureReminderPopup();
    m_reminderEditSnapshot = m_reminders;
    m_reminderEditSnapshotValid = true;
    m_reminderEditor->open(m_reminderModeAllDay, m_range);
}

void DatePickerPopup::openRecurrencePage()
{
    closeSubPopups();
    ensureRecurrencePopup();
    m_recurrenceEditor->openPresets();
}

void DatePickerPopup::openCustomRecurrencePage()
{
    closeSubPopups();
    ensureRecurrencePopup();
    m_recurrenceEditor->openCustom();
}

void DatePickerPopup::openRecurrenceEndPage()
{
    if (!m_schedule.repeating) return;
    closeSubPopups();
    if (!m_recurrenceEndPopup) {
        m_recurrenceEndPopup = new AnchoredListPopup(this);
        m_recurrenceEndPopup->setObjectName(QStringLiteral("DatePickerRecurrenceEndLayer"));
        m_recurrenceEndPopup->setWindowTitle(QStringLiteral("结束重复"));
        m_recurrenceEndPopup->setFont(ds::qfont(ds::FontScene::Menu));
        auto* layout = new QVBoxLayout(m_recurrenceEndPopup->content());
        layout->setContentsMargins(4, 4, 4, 4);
        layout->setSpacing(0);
        const QString labels[] = {QStringLiteral("一直重复"), QStringLiteral("按日期结束重复"),
                                  QStringLiteral("按次数结束重复")};
        for (int mode = 0; mode < 3; ++mode) {
            auto* row = new DatePickerMenuRow(labels[mode], m_recurrenceEndPopup->content());
            row->setProperty("endMode", mode);
            layout->addWidget(row);
            connect(row, &QPushButton::clicked, this, [this, mode]() {
                m_recurrenceEndPopup->reject();
                closeInlineExpansion(false);
                auto& recurrence = m_schedule.recurrence;
                recurrence.endMode = static_cast<RecurrenceSpec::EndMode>(mode);
                if (recurrence.endMode == RecurrenceSpec::EndMode::Date) {
                    if (!recurrence.endDate.isValid()) recurrence.endDate = selectedDate().addMonths(1);
                } else {
                    recurrence.endDate = QDate();
                }
                if (recurrence.endMode == RecurrenceSpec::EndMode::Count)
                    recurrence.endCount = qMax(2, recurrence.endCount);
                updateEntryCaptions();
                if (recurrence.endMode != RecurrenceSpec::EndMode::Never)
                    showRecurrenceEndExpansion();
            });
        }
        m_recurrenceEndPopup->setCardWidth(kSubCardWidth);
        connect(m_recurrenceEndPopup, &QDialog::finished, this, [this]() {
            if (m_recurrenceEndEntry->isVisible()) m_recurrenceEndEntry->setFocus();
        });
        wireEnterKey(m_recurrenceEndPopup);
        applyTheme();
    }
    const auto rows = m_recurrenceEndPopup->findChildren<DatePickerMenuRow*>();
    for (DatePickerMenuRow* row : rows) {
        const int mode = row->property("endMode").toInt();
        row->setChecked(mode == static_cast<int>(m_schedule.recurrence.endMode));
        const bool unsupportedCount = mode == static_cast<int>(RecurrenceSpec::EndMode::Count)
            && m_schedule.recurrence.kind == RecurrenceSpec::Kind::ExplicitDates;
        row->setEnabled(!unsupportedCount);
        row->setToolTip(unsupportedCount ? QStringLiteral("自选日期按所选日期结束") : QString());
    }
    m_recurrenceEndPopup->setUpperBoundGlobalY(calendarGuardY());
    m_recurrenceEndPopup->popupBelow(m_recurrenceEndEntry);
}

void DatePickerPopup::showRecurrenceEndExpansion()
{
    closeInlineExpansion(false);
    QWidget* page = m_recurrenceEndSection->parentWidget();
    auto* layout = qobject_cast<QVBoxLayout*>(page->layout());
    if (!layout) return;
    m_inlineExpansion = InlineExpansion::RecurrenceEnd;
    m_inlineAnchor = m_recurrenceEndEntry;
    if (m_schedule.recurrence.endMode == RecurrenceSpec::EndMode::Date) {
        if (!m_recurrenceEndCalendar) {
            m_recurrenceEndCalendar = new DateCalendarPanel(m_card);
            m_recurrenceEndCalendar->setObjectName(QStringLiteral("DatePickerRecurrenceEndCalendar"));
            m_recurrenceEndCalendar->setCompact(true);
            connect(m_recurrenceEndCalendar, &DateCalendarPanel::dateActivated, this,
                    [this](const QDate& date) {
                if (!date.isValid()) return;
                m_schedule.recurrence.endDate = date;
                closeInlineExpansion(false);
                updateEntryCaptions();
            });
            wireEnterKey(m_recurrenceEndCalendar);
        }
        m_recurrenceEndCalendar->setFirstDay(m_firstDay);
        m_recurrenceEndCalendar->setToday(today());
        m_recurrenceEndCalendar->setSelectedDate(m_schedule.recurrence.endDate);
        m_recurrenceEndCalendar->setVisibleMonth(m_schedule.recurrence.endDate);
        m_recurrenceEndCalendar->setParent(page);
        layout->insertWidget(layout->indexOf(m_recurrenceEndSection) + 1,
                             m_recurrenceEndCalendar, 0, Qt::AlignHCenter);
        m_recurrenceEndCalendar->setVisible(true);
    } else if (m_schedule.recurrence.endMode == RecurrenceSpec::EndMode::Count) {
        if (!m_recurrenceEndCountEditor) {
            m_recurrenceEndCountEditor = new QWidget(m_card);
            m_recurrenceEndCountEditor->setObjectName(QStringLiteral("DatePickerRecurrenceEndCount"));
            auto* countLayout = new QHBoxLayout(m_recurrenceEndCountEditor);
            countLayout->setContentsMargins(16, 4, 16, 8);
            countLayout->setSpacing(8);
            auto* label = new QLabel(QStringLiteral("重复次数"), m_recurrenceEndCountEditor);
            m_recurrenceEndCountSpin = makeSpin(2, 200, QStringLiteral("结束次数"),
                                                m_recurrenceEndCountEditor);
            auto* done = makeSubButton(QStringLiteral("确定"), "DatePickerSubConfirm",
                                       m_recurrenceEndCountEditor, kControlHeight);
            countLayout->addWidget(label);
            countLayout->addWidget(m_recurrenceEndCountSpin, 1);
            countLayout->addWidget(done);
            connect(m_recurrenceEndCountSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                    this, [this](int value) {
                m_schedule.recurrence.endCount = value;
                m_recurrenceEndEntry->setText(QStringLiteral("重复%1次后结束").arg(value));
            });
            connect(done, &QPushButton::clicked, this, [this]() {
                closeInlineExpansion(true);
            });
            wireEnterKey(m_recurrenceEndCountEditor);
        }
        m_recurrenceEndCountSpin->setValue(m_schedule.recurrence.endCount);
        m_recurrenceEndCountEditor->setParent(page);
        layout->insertWidget(layout->indexOf(m_recurrenceEndSection) + 1,
                             m_recurrenceEndCountEditor);
        m_recurrenceEndCountEditor->setVisible(true);
        m_recurrenceEndCountSpin->setFocus();
        m_recurrenceEndCountSpin->selectAll();
    }
    updateCardGeometry();
    if (m_schedule.recurrence.endMode == RecurrenceSpec::EndMode::Date) {
        m_mainScroll->ensureWidgetVisible(m_recurrenceEndCalendar, 0, 4);
        const auto days = m_recurrenceEndCalendar->findChildren<QToolButton*>(QStringLiteral("DatePickerDay"));
        for (QToolButton* day : days) {
            if (day->isChecked()) {
                day->setFocus();
                break;
            }
        }
    } else {
        m_mainScroll->ensureWidgetVisible(m_recurrenceEndCountEditor, 0, 4);
    }
}

// ---------------------------------------------------------------------------
// 主题
// ---------------------------------------------------------------------------

void DatePickerPopup::applyTheme()
{
    QString qss = QStringLiteral(R"QSS(
QWidget#DatePickerCard {
    background-color: %SURFACE%;
    border: %CARD_BORDER%;
    border-radius: 14px;
}
QLabel { color: %TEXT%; background: transparent; }
QLabel[muted="true"], QLabel[weekday="true"], QLabel[chevron="true"] { color: %TEXT_TERTIARY%; }
QLabel[formCaption="true"] { color: %TEXT_SECONDARY%; }
QLabel[rangeRole="true"] { color: %TEXT_SECONDARY%; }
QLabel#DatePickerError { color: %DANGER%; background: transparent; }
QLabel#DatePickerReminderTrigger { color: %PRIMARY%; background: transparent; }
QWidget#DatePickerSegmentHost { background-color: %FIELD%; border-radius: 8px; }
QPushButton#DatePickerSegment {
    color: %TEXT_SECONDARY%;
    background: transparent;
    border: none;
    border-radius: 6px;
    padding: 0 8px;
}
QPushButton#DatePickerSegment:checked { color: %TEXT%; background-color: %SURFACE%; }
QToolButton#DatePickerQuick, QToolButton#DatePickerNav {
    color: %TEXT_SECONDARY%;
    background: transparent;
    border: 1px solid %SURFACE%;
    border-radius: 6px;
    padding: 0;
}
QToolButton#DatePickerQuick:hover, QToolButton#DatePickerNav:hover { background-color: %HOVER%; }
QToolButton#DatePickerQuick:focus, QToolButton#DatePickerNav:focus { border-color: %FOCUS_RING%; }
QToolButton#DatePickerDay {
    color: %TEXT%;
    background-color: %SURFACE%;
    border: 1px solid %SURFACE%;
    border-radius: 15px;
    padding: 0;
}
QToolButton#DatePickerDay[outsideMonth="true"] { color: %TEXT_TERTIARY%; }
QToolButton#DatePickerDay[today="true"] {
    color: %PRIMARY%;
    background-color: %PRIMARY_SOFT%;
    border-color: %PRIMARY_SOFT%;
}
QToolButton#DatePickerDay:hover { background-color: %FIELD_HOVER%; }
QToolButton#DatePickerDay[today="true"]:hover { color: %PRIMARY%; }
QToolButton#DatePickerDay:focus { border-color: %FOCUS_RING%; }
QToolButton#DatePickerDay:checked {
    color: %ON_PRIMARY%;
    background-color: %PRIMARY%;
    border-color: %PRIMARY%;
}
QToolButton#DatePickerDay:checked:hover { background-color: %PRIMARY_HOVER%; }
QToolButton#DatePickerDay:checked:focus { border-color: %FOCUS_RING%; }
QToolButton#DatePickerDay[inRange="true"] {
    color: %TEXT%;
    background-color: %PRIMARY_SOFT%;
    border-color: %PRIMARY_SOFT%;
}
QWidget#DatePickerRangeRow { background: transparent; border: none; }
QPushButton#DatePickerEntry {
    color: %TEXT%;
    background: transparent;
    border: 1px solid %SURFACE%;
    border-radius: 8px;
    text-align: left;
}
QPushButton#DatePickerEntry:hover { background-color: %HOVER%; }
QPushButton#DatePickerEntry:focus { border-color: %FOCUS_RING%; }
QPushButton#DatePickerClear {
    color: %TEXT_SECONDARY%;
    background-color: %SURFACE%;
    border: 1px solid %BORDER%;
    border-radius: 8px;
    padding: 4px 8px;
}
QPushButton#DatePickerClear:hover { background-color: %FIELD_HOVER%; }
QPushButton#DatePickerClear:focus { border-color: %FOCUS_RING%; }
QPushButton#DatePickerConfirm {
    color: %ON_PRIMARY%;
    background-color: %PRIMARY%;
    border: 1px solid %PRIMARY%;
    border-radius: 8px;
    padding: 4px 8px;
}
QPushButton#DatePickerConfirm:hover { background-color: %PRIMARY_HOVER%; }
QPushButton#DatePickerConfirm:focus { border-color: %FOCUS_RING%; }
QPushButton#DatePickerConfirm:disabled {
    color: %TEXT_TERTIARY%;
    background-color: %FIELD%;
    border-color: %FIELD%;
}
QPushButton#DatePickerRangeDate {
    color: %TEXT%;
    background-color: %SURFACE%;
    border: 1px solid %BORDER%;
    border-radius: 8px;
    padding: 0 8px;
    text-align: center;
}
QPushButton#DatePickerRangeDate:hover, QPushButton#DatePickerRangeDate:focus { border-color: %PRIMARY%; }
QPushButton#DatePickerRangeTime {
    color: %TEXT%;
    background-color: %SURFACE%;
    border: 1px solid %BORDER%;
    border-radius: 8px;
    padding: 0 8px;
    text-align: center;
}
QPushButton#DatePickerRangeTime:hover, QPushButton#DatePickerRangeTime:focus { border-color: %PRIMARY%; }
QCheckBox { color: %TEXT%; background: transparent; spacing: 6px; }
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border: 1px solid %BORDER%;
    border-radius: 4px;
    background-color: %SURFACE%;
}
QCheckBox::indicator:checked {
    background-color: %PRIMARY%;
    border-color: %PRIMARY%;
}
QPushButton[datePickerMenuRow="true"] {
    color: %TEXT%;
    background: transparent;
    border: none;
    border-radius: 0;
    padding: 0;
}
QPushButton[datePickerMenuRow="true"]:hover { background-color: %HOVER%; }
QPushButton[datePickerMenuRow="true"]:checked { background: transparent; }
QPushButton[datePickerMenuRow="true"]:checked:hover { background-color: %HOVER%; }
QPushButton[datePickerMenuRow="true"]:focus { border: none; }
QPushButton#DatePickerSubClear, QPushButton#DatePickerSubConfirm {
    border-radius: 6px;
    padding: 0 12px;
    font-size: 13px;
}
QPushButton#DatePickerSubClear {
    color: %TEXT_SECONDARY%;
    background-color: %SURFACE%;
    border: 1px solid %BORDER%;
}
QPushButton#DatePickerSubClear:hover { background-color: %FIELD_HOVER%; }
QPushButton#DatePickerSubConfirm {
    color: %ON_PRIMARY%;
    background-color: %PRIMARY%;
    border: 1px solid %PRIMARY%;
}
QPushButton#DatePickerSubConfirm:hover { background-color: %PRIMARY_HOVER%; }
QListWidget#DatePickerList {
    color: %TEXT%;
    background-color: %SURFACE%;
    border: none;
    outline: 0;
}
QListWidget#DatePickerList::item { padding: 0 8px; border-radius: 6px; }
QListWidget#DatePickerList::item:hover { background-color: %HOVER%; }
QListWidget#DatePickerList::item:selected { color: %TEXT%; background-color: %PRIMARY_SOFT%; }
QLineEdit#DatePickerInput {
    color: %TEXT%;
    background-color: %FIELD%;
    border: 1px solid %BORDER%;
    border-radius: 6px;
    padding: 4px 8px;
    selection-background-color: %PRIMARY%;
    selection-color: %ON_PRIMARY%;
}
QLineEdit#DatePickerInput:focus { border-color: %FOCUS_RING%; }
QLineEdit#DatePickerInput[invalid="true"] { border-color: %DANGER%; }
QComboBox, QSpinBox {
    color: %TEXT%;
    background-color: %FIELD%;
    border: 1px solid %BORDER%;
    border-radius: 6px;
    min-height: 24px;
    max-height: 24px;
    padding: 0 6px;
}
QComboBox:hover, QSpinBox:hover { background-color: %FIELD_HOVER%; }
QComboBox:focus, QSpinBox:focus { border-color: %FOCUS_RING%; }
QLineEdit#DatePickerFieldInput {
    color: %TEXT%;
    background-color: %FIELD%;
    border: 1px solid %BORDER%;
    border-radius: 6px;
    padding: 2px 8px;
    min-height: 24px;
    max-height: 24px;
    selection-background-color: %PRIMARY%;
    selection-color: %ON_PRIMARY%;
}
QLineEdit#DatePickerFieldInput:focus { border-color: %FOCUS_RING%; }
QLineEdit#DatePickerFieldInput[invalid="true"] { border-color: %DANGER%; }
QToolButton#DatePickerToggle {
    color: %TEXT%;
    background-color: %SURFACE%;
    border: 1px solid %BORDER%;
    border-radius: 6px;
    padding: 0;
}
QToolButton#DatePickerToggle:hover { background-color: %FIELD_HOVER%; }
QToolButton#DatePickerToggle:focus { border-color: %FOCUS_RING%; }
QToolButton#DatePickerToggle:checked {
    color: %ON_PRIMARY%;
    background-color: %PRIMARY%;
    border-color: %PRIMARY%;
}
QToolButton#DatePickerToggle[round="true"] {
    border-radius: 11px;
    border-color: %SURFACE%;
}
QToolButton#DatePickerToggle[round="true"]:checked { border-color: %PRIMARY%; }
QScrollArea#DatePickerScroll, QScrollArea#DatePickerMainScroll { border: none; background: transparent; }
QScrollArea#DatePickerScroll > QWidget > QWidget,
QScrollArea#DatePickerMainScroll > QWidget > QWidget { background: transparent; }
QScrollBar#DatePickerMainScrollbar:vertical {
    background: transparent;
    width: 4px;
    margin: 0;
    border: none;
}
QScrollBar#DatePickerMainScrollbar::handle:vertical {
    background: %BORDER%;
    min-height: 24px;
    border-radius: 2px;
}
QScrollBar#DatePickerMainScrollbar::add-line:vertical,
QScrollBar#DatePickerMainScrollbar::sub-line:vertical { height: 0; }
QScrollBar#DatePickerMainScrollbar::add-page:vertical,
QScrollBar#DatePickerMainScrollbar::sub-page:vertical { background: transparent; }
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
    qss.replace(QStringLiteral("%CARD_BORDER%"),
                ThemeManager::instance().resolvedTheme() == ds::Theme::Dark
                    ? QStringLiteral("1px solid ") + color(ds::ColorToken::Border)
                    : QStringLiteral("none"));
    m_card->setStyleSheet(qss);

    AnchoredListPopup* layers[] = {m_reminderPopup, m_recurrencePopup, m_recurrenceEndPopup};
    for (AnchoredListPopup* layer : layers) {
        if (!layer) continue;
        layer->setStyleSheet(qss);
        const auto edits = layer->findChildren<QLineEdit*>();
        for (QLineEdit* edit : edits) {
            QPalette palette = edit->palette();
            palette.setColor(QPalette::PlaceholderText,
                             ThemeManager::instance().color(ds::ColorToken::TextTertiary));
            edit->setPalette(palette);
        }
    }

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
        palette.setColor(QPalette::PlaceholderText,
                         ThemeManager::instance().color(ds::ColorToken::TextTertiary));
        edit->setPalette(palette);
    }
    m_shadow = QImage();
    m_shadowColor = QColor();
    m_card->update();
    updateCardGeometry();
}

// ---------------------------------------------------------------------------
// 刷新
// ---------------------------------------------------------------------------

void DatePickerPopup::updateEntriesVisibility()
{
    if (!m_timeEntrySection) return;
    // 时间段模式的时刻在开始/结束行内编辑，父卡片不再放“时间”入口。
    m_timeEntrySection->setVisible(!m_range);
    m_recurrenceEntrySection->setVisible(m_recurrenceEnabled);
    if (m_recurrenceEndSection)
        m_recurrenceEndSection->setVisible(m_recurrenceEnabled && m_schedule.repeating);
}

void DatePickerPopup::setRecurrenceEnabled(bool enabled)
{
    m_recurrenceEnabled = enabled;
    if (!enabled && m_recurrencePopup && m_recurrencePopup->isVisible()) m_recurrencePopup->reject();
    if (!enabled && m_recurrenceEndPopup && m_recurrenceEndPopup->isVisible())
        m_recurrenceEndPopup->reject();
    if (!enabled) closeInlineExpansion(false);
    updateEntriesVisibility();
    updateCardGeometry();
}

QString DatePickerPopup::reminderCaption(const TaskReminder& row) const
{
    ReminderSpec spec;
    if (!row.ruleJson.isEmpty() && reminderFromRuleText(row.ruleJson, &spec)) {
        if (spec.anchor == ReminderSpec::Anchor::Absolute) {
            const QDateTime local = toZoneLocal(spec.absoluteAt.isValid() ? spec.absoluteAt : row.triggerAt);
            return local.isValid() ? local.toString(QStringLiteral("yyyy年M月d日 HH:mm"))
                                   : QStringLiteral("自定义提醒");
        }
        if (spec.allDay && spec.localTime == QTime(9, 0)) {
            if (spec.dayOffset == 0) return QStringLiteral("当天");
            if (spec.dayOffset % 7 == 0)
                return QStringLiteral("提前%1周").arg(spec.dayOffset / 7);
            return QStringLiteral("提前%1天").arg(spec.dayOffset);
        }
        return reminderSummary(spec);
    }
    if (row.triggerAt.isValid()) {
        const QDateTime local = toZoneLocal(row.triggerAt);
        if (local.isValid()) return local.toString(QStringLiteral("yyyy年M月d日 HH:mm"));
    }
    return QStringLiteral("自定义提醒");
}

void DatePickerPopup::updateEntryCaptions()
{
    if (m_timeEntry) {
        m_timeEntry->setText(m_singleTime.isValid()
            ? m_singleTime.toString(QStringLiteral("HH:mm"))
            : QStringLiteral("时间"));
    }
    if (m_reminderEntry) {
        QString text = QStringLiteral("提醒");
        if (!m_reminders.isEmpty()) {
            QVector<TaskReminder> ordered = m_reminders;
            const auto parseRule = [](const TaskReminder& row, ReminderSpec* spec) {
                return !row.ruleJson.isEmpty() && reminderFromRuleText(row.ruleJson, spec);
            };
            std::stable_sort(ordered.begin(), ordered.end(), [&](const TaskReminder& left,
                                                                  const TaskReminder& right) {
                ReminderSpec a, b;
                const bool validA = parseRule(left, &a), validB = parseRule(right, &b);
                if (validA != validB) return validA;
                if (!validA) return left.id < right.id;
                const auto group = [](const ReminderSpec& spec) {
                    if (spec.anchor == ReminderSpec::Anchor::Absolute) return 2;
                    return spec.allDay ? 1 : 0;
                };
                if (group(a) != group(b)) return group(a) < group(b);
                if (a.anchor == ReminderSpec::Anchor::Absolute)
                    return a.absoluteAt < b.absoluteAt;
                if (a.allDay != b.allDay) return a.allDay;
                if (a.allDay) {
                    if (a.dayOffset != b.dayOffset) return a.dayOffset < b.dayOffset;
                    return a.localTime < b.localTime;
                }
                const int anchorA = a.anchor == ReminderSpec::Anchor::End ? 1 : 0;
                const int anchorB = b.anchor == ReminderSpec::Anchor::End ? 1 : 0;
                if (anchorA != anchorB) return anchorA < anchorB;
                const auto offsetMinutes = [](const ReminderSpec& spec) {
                    const int factor = spec.unit == ReminderSpec::Unit::Minutes ? 1
                        : spec.unit == ReminderSpec::Unit::Hours ? 60
                        : spec.unit == ReminderSpec::Unit::Days ? 1440 : 10080;
                    return spec.offset * factor;
                };
                const int offsetA = offsetMinutes(a), offsetB = offsetMinutes(b);
                if (offsetA != offsetB) return offsetA < offsetB;
                return left.id < right.id;
            });
            QStringList labels;
            for (const TaskReminder& row : ordered) labels.append(reminderCaption(row));
            text = labels.join(QStringLiteral(", "));
            if (!remindersAllResolvable()) text += QStringLiteral("（需重选）");
        }
        m_reminderEntry->setText(text);
    }
    if (m_recurrenceEntry) {
        m_recurrenceEntry->setText(m_schedule.repeating
            ? recurrenceSummary(m_schedule.recurrence)
            : QStringLiteral("重复"));
    }
    if (m_recurrenceEndEntry) {
        const auto& recurrence = m_schedule.recurrence;
        if (recurrence.endMode == RecurrenceSpec::EndMode::Date && recurrence.endDate.isValid())
            m_recurrenceEndEntry->setText(recurrence.endDate.toString(QStringLiteral("直到yyyy/M/d")));
        else if (recurrence.endMode == RecurrenceSpec::EndMode::Count)
            m_recurrenceEndEntry->setText(QStringLiteral("重复%1次后结束").arg(recurrence.endCount));
        else
            m_recurrenceEndEntry->setText(QStringLiteral("一直重复"));
        m_recurrenceEndEntry->setAccessibleName(QStringLiteral("结束重复"));
    }
    updateEntriesVisibility();
    updateCardGeometry();
}

void DatePickerPopup::updateRangeRows()
{
    if (!m_rangeStartDateButton) return;
    const auto dateText = [](const QDate& date) {
        return date.isValid() ? date.toString(QStringLiteral("M月d日"))
                              : QStringLiteral("选择日期");
    };
    m_rangeStartDateButton->setText(dateText(m_rangeStartDate));
    m_rangeEndDateButton->setText(dateText(m_rangeEndDate));
    const bool timed = !m_allDayRange;
    m_rangeStartTimeButton->setVisible(timed);
    m_rangeEndTimeButton->setVisible(timed);
    if (timed) {
        m_rangeStartTimeButton->setText((m_rangeStartTime.isValid() ? m_rangeStartTime : QTime(9, 0))
                                            .toString(QStringLiteral("HH:mm")));
        m_rangeEndTimeButton->setText((m_rangeEndTime.isValid() ? m_rangeEndTime : QTime(10, 0))
                                          .toString(QStringLiteral("HH:mm")));
    }
}

void DatePickerPopup::applyModeToUi()
{
    if (!m_dateButton) return;
    m_dateButton->setChecked(!m_range);
    m_rangeButton->setChecked(m_range);
    m_datePage->setVisible(!m_range);
    m_rangePage->setVisible(m_range);
    m_calendarSection->setVisible(!m_range
        && !(m_inlineExpansion == InlineExpansion::Time && m_timeTarget == TimeTarget::Single));
    if (m_allDayCheck) m_allDayCheck->setChecked(m_allDayRange);
    updateEntriesVisibility();
}

void DatePickerPopup::updateCardGeometry()
{
    if (!m_card || !m_mainPage || !m_footer || !m_mainScroll) return;
    QLayout* content = m_mainPage->layout();
    if (!content) return;
    m_mainPage->setMinimumHeight(0);
    QWidget* sections[] = {m_datePage, m_rangePage, m_calendarSection};
    for (QWidget* section : sections) {
        if (!section || !section->layout()) continue;
        section->setMinimumHeight(0);
        section->layout()->invalidate();
        section->layout()->activate();
    }
    content->invalidate();
    content->activate();
    const int naturalHeight = content->sizeHint().height();
    const int footerHeight = m_footer->sizeHint().height();
    QScreen* screen = m_popupPositioned ? QGuiApplication::screenAt(m_popupAnchor.center()) : nullptr;
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QRect area = screen ? screen->availableGeometry() : QRect(0, 0, 1024, 768);
    const int availableBody = qMax(1, area.height() - 2 * kShadowMargin - footerHeight);
    const int bodyHeight = qMin(naturalHeight, availableBody);
    const bool constrained = naturalHeight > availableBody;
    m_mainScroll->setVerticalScrollBarPolicy(constrained ? Qt::ScrollBarAlwaysOn : Qt::ScrollBarAlwaysOff);
    // 仅在需要滚动时让出 4px 轨道；主体仍容纳 240px 月历和 252px 菜单行。
    if (m_calendarSection && m_calendarSection->layout()) {
        const int side = constrained ? 8 : 10;
        m_calendarSection->layout()->setContentsMargins(side, 0, side, 15);
    }
    m_mainPage->setMinimumHeight(naturalHeight);
    m_mainScroll->setFixedHeight(bodyHeight);
    const int cardHeight = bodyHeight + footerHeight;
    m_card->setFixedSize(kCardWidth, cardHeight);
    setFixedSize(kCardWidth + 2 * kShadowMargin, cardHeight + 2 * kShadowMargin);
    if (isVisible() && m_popupPositioned) {
        QPoint position(m_popupAnchor.left() - kShadowMargin,
                        m_popupAnchor.bottom() + 6 - kShadowMargin);
        position.setX(qMax(area.left(), qMin(position.x(), area.right() - width() + 1)));
        position.setY(qMax(area.top(), qMin(position.y(), area.bottom() - height() + 1)));
        move(position);
    }
}

void DatePickerPopup::refreshAll()
{
    applyModeToUi();
    updateRangeRows();
    if (m_inlineCalendar) {
        m_inlineCalendar->setFirstDay(m_firstDay);
        m_inlineCalendar->setToday(today());
        m_inlineCalendar->setSelectedDate(m_singleDate);
    }
    updateEntryCaptions();
    updateCardGeometry();
}

void DatePickerPopup::syncFromDraft()
{
    const QDate anchor = anchorDate();
    if (m_inlineCalendar)
        m_inlineCalendar->setVisibleMonth(anchor.isValid() ? anchor : today());
    refreshAll();
}

void DatePickerPopup::showCardError(const QString& message)
{
    if (!m_errorLabel) return;
    m_errorLabel->setText(message);
    m_errorLabel->setVisible(true);
    const int errorHeight = m_errorLabel->heightForWidth(kCardWidth);
    if (errorHeight > 0) m_errorLabel->setFixedHeight(errorHeight);
    updateCardGeometry();
    if (m_okButton) m_okButton->setFocus();
}

void DatePickerPopup::clearCardError()
{
    if (m_errorLabel && m_errorLabel->isVisible()) {
        m_errorLabel->clear();
        m_errorLabel->setVisible(false);
        updateCardGeometry();
    }
}

// ---------------------------------------------------------------------------
// 草稿换算
// ---------------------------------------------------------------------------

QTimeZone DatePickerPopup::zone() const
{
    const QTimeZone result(m_schedule.timeZone.toUtf8());
    return result.isValid() ? result : appTimeZone();
}

QDateTime DatePickerPopup::toZoneLocal(const QDateTime& utc) const
{
    return utc.isValid() ? utc.toTimeZone(zone()) : QDateTime();
}

QDateTime DatePickerPopup::fromZoneLocal(const QDate& date, const QTime& time) const
{
    if (!date.isValid() || !time.isValid()) return QDateTime();
    return QDateTime(date, time, zone()).toUTC();
}

QDateTime DatePickerPopup::endpointFromDraft(const QDateTime& original,
                                              const QDate& date, const QTime& time) const
{
    if (original.isValid()) {
        const QDateTime local = toZoneLocal(original);
        if (local.date() == date && local.time() == time) return original.toUTC();
    }
    return fromZoneLocal(date, time);
}

QDate DatePickerPopup::today() const
{
    return toZoneLocal(appNow()).date();
}

QDate DatePickerPopup::anchorDate() const
{
    return m_range ? m_rangeStartDate : m_singleDate;
}

bool DatePickerPopup::hasAnyDate() const
{
    return hasDate();
}

Entity DatePickerPopup::buildEntity() const
{
    Entity entity;
    entity.timeZone = m_schedule.timeZone;
    if (m_range) {
        if (!m_rangeStartDate.isValid()) return entity;
        entity.hasScheduleRange = true;
        const QDate end = m_rangeEndDate.isValid() ? m_rangeEndDate : m_rangeStartDate;
        if (m_allDayRange) {
            entity.allDayDate = m_rangeStartDate;
            entity.allDayEndDate = end;
        } else {
            const QDateTime originalStart = m_schedule.hasScheduleRange ? m_schedule.startAt : QDateTime();
            const QDateTime originalEnd = m_schedule.hasScheduleRange ? m_schedule.dueAt : QDateTime();
            entity.startAt = endpointFromDraft(originalStart, m_rangeStartDate,
                                               m_rangeStartTime.isValid() ? m_rangeStartTime : QTime(9, 0));
            entity.dueAt = endpointFromDraft(originalEnd, end,
                                             m_rangeEndTime.isValid() ? m_rangeEndTime : QTime(9, 0));
        }
    } else if (m_singleDate.isValid()) {
        if (m_singleTime.isValid()) {
            const QDateTime original = m_schedule.dueAt.isValid() ? m_schedule.dueAt : m_schedule.startAt;
            entity.dueAt = endpointFromDraft(original, m_singleDate, m_singleTime);
        } else entity.allDayDate = m_singleDate;
    }
    return entity;
}

bool DatePickerPopup::resolveDraftReminder(const TaskReminder& row, QDateTime* triggerAt,
                                           QString* error) const
{
    if (row.ruleJson.isEmpty()) {
        if (row.triggerAt.isValid()) {
            if (triggerAt) *triggerAt = row.triggerAt.toUTC();
            return true;
        }
        if (error) *error = QStringLiteral("提醒时间不能为空");
        return false;
    }
    ReminderSpec spec;
    if (!reminderFromRuleText(row.ruleJson, &spec, error)) return false;
    return resolveReminder(spec, buildEntity(), triggerAt, error);
}

bool DatePickerPopup::remindersAllResolvable() const
{
    for (const TaskReminder& row : m_reminders) {
        QDateTime trigger;
        QString error;
        if (!resolveDraftReminder(row, &trigger, &error)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------

QDate DatePickerPopup::quickDate(int index, const QDate& todayDate)
{
    if (!todayDate.isValid()) return QDate();
    if (index == 0) return todayDate;
    if (index == 1) return todayDate.addDays(1);
    if (index == 2) return todayDate.addDays(7);
    if (index == 3) return todayDate.addMonths(1);
    return QDate();
}

void DatePickerPopup::selectDate(const QDate& date)
{
    if (!date.isValid() || m_range) return;
    m_singleDate = date;
    if (m_inlineCalendar) m_inlineCalendar->setVisibleMonth(date);
    clearCardError();
    refreshAll();
}

void DatePickerPopup::selectRangeDate(const QDate& date)
{
    if (!date.isValid() || !m_range) return;
    // 每个日历层只编辑自己的端点；非法先后由父层 accept 明确提示，绝不交换。
    if (m_rangeCalendarEnd) m_rangeEndDate = date;
    else m_rangeStartDate = date;
    clearCardError();
    closeInlineExpansion(false);
    refreshAll();
}

void DatePickerPopup::setRangeMode(bool range)
{
    if (range == m_range) {
        refreshAll();
        return;
    }
    closeSubPopups();
    if (range) {
        // 进入时间段：从单模式草稿推导开始；新计时范围默认结束晚一小时。
        const bool timed = m_singleTime.isValid() || m_rangeStartTime.isValid()
            || m_rangeEndTime.isValid();
        m_allDayRange = !timed;
        if (m_singleDate.isValid() && !m_rangeStartDate.isValid()) m_rangeStartDate = m_singleDate;
        if (m_singleTime.isValid()) m_rangeStartTime = m_singleTime;
        if (m_allDayRange) {
            if (!m_rangeEndDate.isValid()) m_rangeEndDate = m_rangeStartDate;
        } else {
            if (!m_rangeStartTime.isValid()) m_rangeStartTime = QTime(9, 0);
            if (!m_rangeEndDate.isValid()) m_rangeEndDate = m_rangeStartDate;
            if (!m_rangeEndTime.isValid() && m_rangeStartDate.isValid()) {
                const QDateTime end = QDateTime(m_rangeStartDate, m_rangeStartTime, zone()).addSecs(3600);
                m_rangeEndDate = end.date();
                m_rangeEndTime = end.time();
            }
        }
        m_rangeCalendarEnd = false;
        m_range = true;
    } else {
        m_range = false;
        if (m_rangeStartDate.isValid()) m_singleDate = m_rangeStartDate;
        m_singleTime = m_allDayRange ? QTime() : m_rangeStartTime;
    }
    clearCardError();
    switchReminderDraftMode(m_range ? m_allDayRange : !m_singleTime.isValid());
    refreshAll();
}

void DatePickerPopup::setAllDayRange(bool allDay)
{
    if (m_allDayRange == allDay) return;
    closeInlineExpansion(false);
    m_allDayRange = allDay;
    switchReminderDraftMode(allDay);
    if (!allDay) {
        // 取消全天：恢复可见的时刻，缺失时给默认（开始 09:00、结束 +1 小时）。
        if (!m_rangeStartTime.isValid()) m_rangeStartTime = QTime(9, 0);
        if (!m_rangeEndDate.isValid()) m_rangeEndDate = m_rangeStartDate;
        if (!m_rangeEndTime.isValid() && m_rangeStartDate.isValid()) {
            const QDateTime end = QDateTime(m_rangeStartDate, m_rangeStartTime, zone()).addSecs(3600);
            m_rangeEndDate = end.date();
            m_rangeEndTime = end.time();
        }
    }
    clearCardError();
    refreshAll();
}

void DatePickerPopup::applyTime(const QTime& time)
{
    if (!time.isValid()) return;
    if (m_timeTarget == TimeTarget::Single) {
        if (!m_singleDate.isValid()) m_singleDate = today();
        m_singleTime = time;
    } else if (m_timeTarget == TimeTarget::RangeStart) {
        m_rangeStartTime = time;
        if (m_rangeStartDate.isValid() && !m_rangeEndTime.isValid()) {
            const QDateTime end = QDateTime(m_rangeStartDate, time, zone()).addSecs(3600);
            m_rangeEndDate = end.date();
            m_rangeEndTime = end.time();
        }
    } else {
        m_rangeEndTime = time;
    }
    if (m_timeTarget == TimeTarget::Single) switchReminderDraftMode(false);
    if (m_timeEdit) {
        m_timeEdit->setText(time.toString(QStringLiteral("HH:mm")));
        setInputInvalid(m_timeEdit, false);
    }
    clearCardError();
    refreshAll();
}

bool DatePickerPopup::applyCustomTime()
{
    const QString text = m_timeEdit->text().trimmed();
    if (text.isEmpty() && m_timeTarget == TimeTarget::Single) {
        m_singleTime = QTime();
        switchReminderDraftMode(true);
        clearCardError();
        refreshAll();
        return true;
    }
    QTime time;
    if (!DatePickerUi::parseTimeText(text, &time)) {
        setInputInvalid(m_timeEdit, true, QStringLiteral("请输入 00:00–23:59 时间"));
        m_timeEdit->setFocus();
        m_timeEdit->selectAll();
        return false;
    }
    applyTime(time);
    return true;
}

QDate DatePickerPopup::selectedDate() const
{
    const QDate date = anchorDate();
    return date.isValid() ? date : today();
}

// ---------------------------------------------------------------------------
// 权威接口：setSchedule / schedule
// ---------------------------------------------------------------------------

void DatePickerPopup::setSchedule(const ScheduleValue& schedule)
{
    closeSubPopups();
    m_timedReminderDraft.clear();
    m_allDayReminderDraft.clear();
    m_reminderEditSnapshot.clear();
    m_reminderEditSnapshotValid = false;
    m_recentReminderRulesPending.clear();
    m_schedule = schedule;
    if (m_schedule.timeZone.trimmed().isEmpty()) m_schedule.timeZone = defaultTimeZone();
    m_reminders = schedule.reminders;
    for (TaskReminder& row : m_reminders)
        if (row.id.isEmpty()) row.id = newUuid();
    m_cleared = false;

    m_range = false;
    m_allDayRange = false;
    m_singleDate = QDate();
    m_singleTime = QTime();
    m_rangeStartDate = QDate();
    m_rangeEndDate = QDate();
    m_rangeStartTime = QTime();
    m_rangeEndTime = QTime();
    m_rangeCalendarEnd = false;

    if (schedule.allDayDate.isValid()) {
        if (schedule.hasScheduleRange && schedule.allDayEndDate.isValid()) {
            m_range = true;
            m_allDayRange = true;
            m_rangeStartDate = schedule.allDayDate;
            m_rangeEndDate = schedule.allDayEndDate;
        } else {
            m_singleDate = schedule.allDayDate;
        }
    } else if (schedule.startAt.isValid() && schedule.hasScheduleRange) {
        m_range = true;
        const QDateTime startLocal = toZoneLocal(schedule.startAt);
        m_rangeStartDate = startLocal.date();
        m_rangeStartTime = startLocal.time();
        if (schedule.dueAt.isValid()) {
            const QDateTime endLocal = toZoneLocal(schedule.dueAt);
            m_rangeEndDate = endLocal.date();
            m_rangeEndTime = endLocal.time();
        }
    } else if (schedule.startAt.isValid() && !schedule.dueAt.isValid()) {
        // 仅有 startAt 的非范围值按单时刻载入（输出时写回 dueAt）。
        const QDateTime local = toZoneLocal(schedule.startAt);
        m_singleDate = local.date();
        m_singleTime = local.time();
    } else if (schedule.dueAt.isValid()) {
        const QDateTime local = toZoneLocal(schedule.dueAt);
        m_singleDate = local.date();
        m_singleTime = local.time();
    }
    if (m_range && !m_allDayRange && !m_rangeEndTime.isValid()) {
        // 计时范围缺结束时刻：默认开始 +1 小时（跨日顺延）。
        const QDateTime start(m_rangeStartDate, m_rangeStartTime, zone());
        const QDateTime end = start.addSecs(3600);
        if (!m_rangeEndDate.isValid()) m_rangeEndDate = end.date();
        m_rangeEndTime = end.time();
    }
    m_reminderModeAllDay = !hasTime();
    syncFromDraft();
}

ScheduleValue DatePickerPopup::schedule() const
{
    ScheduleValue result = m_schedule;
    const Entity entity = buildEntity();
    result.startAt = entity.startAt;
    result.dueAt = entity.dueAt;
    result.allDayDate = entity.allDayDate;
    result.allDayEndDate = entity.allDayEndDate;
    result.hasScheduleRange = entity.hasScheduleRange;
    result.repeating = m_schedule.repeating;
    result.recurrence = m_schedule.recurrence;
    if (!hasAnyDate()) {
        // 空计划 = 清除口径：提醒一并清空，重复失去锚点。
        result.repeating = false;
        result.reminders.clear();
        return result;
    }
    result.reminders.clear();
    for (const TaskReminder& row : m_reminders) {
        TaskReminder out = row;
        QDateTime trigger;
        QString error;
        if (resolveDraftReminder(row, &trigger, &error)) out.triggerAt = trigger;
        out.sortOrder = result.reminders.size();
        result.reminders.append(out);
    }
    return result;
}

// ---------------------------------------------------------------------------
// 校验与接受
// ---------------------------------------------------------------------------

void DatePickerPopup::keyPressEvent(QKeyEvent* event)
{
    if (event && event->key() == Qt::Key_Escape && m_inlineExpansion != InlineExpansion::None) {
        event->accept();
        closeInlineExpansion(true);
        return;
    }
    QDialog::keyPressEvent(event);
}

void DatePickerPopup::accept()
{
    closeSubPopups();
    clearCardError();
    if (!hasAnyDate()) {
        // 未选日期 = 清除计划（沿用旧单值口径），交由控制器处理。
        closeSubPopups();
        m_recentReminderRulesPending.clear();
        QDialog::accept();
        return;
    }
    if (m_range) {
        if (!m_rangeStartDate.isValid()) {
            showCardError(QStringLiteral("请选择开始日期"));
            return;
        }
        const QDate end = m_rangeEndDate.isValid() ? m_rangeEndDate : m_rangeStartDate;
        if (m_allDayRange) {
            if (end < m_rangeStartDate) {
                showCardError(QStringLiteral("结束日期不能早于开始日期"));
                return;
            }
        } else {
            const Entity draft = buildEntity();
            if (!draft.startAt.isValid() || !draft.dueAt.isValid()) {
                showCardError(QStringLiteral("请选择有效的开始和结束时间"));
                return;
            }
            if (draft.dueAt < draft.startAt) {
                showCardError(QStringLiteral("结束时间不能早于开始时间"));
                return;
            }
        }
    }
    if (m_schedule.repeating) {
        QString error;
        if (!isValidRecurrence(m_schedule.recurrence, &error)) {
            showCardError(QStringLiteral("重复规则无效：%1").arg(error));
            return;
        }
    }
    for (const TaskReminder& row : m_reminders) {
        QDateTime trigger;
        QString error;
        if (!resolveDraftReminder(row, &trigger, &error)) {
            QString label = QStringLiteral("自定义提醒");
            ReminderSpec spec;
            if (!row.ruleJson.isEmpty() && reminderFromRuleText(row.ruleJson, &spec))
                label = reminderSummary(spec);
            showCardError(QStringLiteral("提醒“%1”无效：%2").arg(label).arg(error));
            return;
        }
    }
    closeSubPopups();
    for (const QString& rule : m_recentReminderRulesPending)
        if (findReminderRowByRule(rule) >= 0) Settings::instance().addRecentReminderRule(rule);
    m_recentReminderRulesPending.clear();
    QDialog::accept();
}

void DatePickerPopup::reject()
{
    closeSubPopups();
    m_recentReminderRulesPending.clear();
    QDialog::reject();
}

// ---------------------------------------------------------------------------
// 旧单值适配层
// ---------------------------------------------------------------------------

void DatePickerPopup::setDateTime(const QDateTime& dt)
{
    m_range = false;
    if (dt.isValid()) {
        const QDateTime local = toZoneLocal(dt);
        m_singleDate = local.date();
        m_singleTime = local.time();
    } else {
        m_singleDate = QDate();
        m_singleTime = QTime();
    }
    m_cleared = false;
    switchReminderDraftMode(!hasTime());
    if (m_inlineCalendar)
        m_inlineCalendar->setVisibleMonth(m_singleDate.isValid() ? m_singleDate : today());
    clearCardError();
    refreshAll();
}

void DatePickerPopup::setAllDayDate(const QDate& date)
{
    m_range = false;
    m_singleDate = date;
    m_singleTime = QTime();
    m_cleared = false;
    switchReminderDraftMode(true);
    if (m_inlineCalendar) m_inlineCalendar->setVisibleMonth(date.isValid() ? date : today());
    clearCardError();
    refreshAll();
}

void DatePickerPopup::setHasDate(bool has)
{
    if (!has) {
        m_singleDate = QDate();
        m_singleTime = QTime();
        m_range = false;
    } else if (!hasDate()) {
        m_singleDate = today();
    }
    m_cleared = false;
    switchReminderDraftMode(!hasTime());
    clearCardError();
    refreshAll();
}

void DatePickerPopup::setReminder(const QDateTime& reminder)
{
    if (m_reminderPopup && m_reminderPopup->isVisible()) m_reminderPopup->reject();
    m_timedReminderDraft.clear();
    m_allDayReminderDraft.clear();
    m_reminderEditSnapshot.clear();
    m_reminderEditSnapshotValid = false;
    m_cleared = false;
    m_reminders.clear();
    if (reminder.isValid()) {
        // 旧单提醒等价一条绝对时刻规则，保留稳定 id 语义（新建行生成新 UUID）。
        ReminderSpec spec;
        spec.anchor = ReminderSpec::Anchor::Absolute;
        spec.absoluteAt = reminder.toUTC();
        TaskReminder row;
        row.id = newUuid();
        row.ruleJson = reminderRuleText(spec);
        row.triggerAt = reminder.toUTC();
        m_reminders.append(row);
    }
    updateEntryCaptions();
}

void DatePickerPopup::setRecurrence(const QString& rule)
{
    const QString trimmed = rule.trimmed();
    m_cleared = false;
    QString frequency;
    int interval = 1;
    if (trimmed.isEmpty() || trimmed == QLatin1String("none")
        || !parseRecurrence(trimmed, &frequency, &interval) || frequency.isEmpty()) {
        m_schedule.repeating = false;
        updateEntryCaptions();
        return;
    }
    RecurrenceSpec spec;
    spec.weekStart = m_firstDay;
    spec.interval = interval;
    if (frequency == QLatin1String("DAILY")) {
        spec.frequency = RecurrenceSpec::Frequency::Daily;
    } else if (frequency == QLatin1String("WEEKLY")) {
        // 简单每周允许空星期（等价按锚点每 N 周）。
        spec.frequency = RecurrenceSpec::Frequency::Weekly;
    } else {
        spec.frequency = RecurrenceSpec::Frequency::Monthly;
        const QDate anchor = anchorDate();
        spec.monthDays.append(anchor.isValid() ? anchor.day() : 1);
    }
    if (isValidRecurrence(spec)) {
        m_schedule.recurrence = spec;
        m_schedule.repeating = true;
    } else {
        m_schedule.repeating = false;
    }
    updateEntryCaptions();
}

QDateTime DatePickerPopup::dateTime() const
{
    if (m_range || !m_singleDate.isValid() || !m_singleTime.isValid()) return QDateTime();
    return fromZoneLocal(m_singleDate, m_singleTime);
}

QDate DatePickerPopup::allDayDate() const
{
    if (m_range || !m_singleDate.isValid() || m_singleTime.isValid()) return QDate();
    return m_singleDate;
}

bool DatePickerPopup::hasDate() const
{
    return m_range ? m_rangeStartDate.isValid() : m_singleDate.isValid();
}

bool DatePickerPopup::hasTime() const
{
    if (m_range) return !m_allDayRange && m_rangeStartDate.isValid();
    return m_singleDate.isValid() && m_singleTime.isValid();
}

QDateTime DatePickerPopup::reminder() const
{
    QDateTime best;
    for (const TaskReminder& row : m_reminders) {
        QDateTime trigger;
        QString error;
        if (!resolveDraftReminder(row, &trigger, &error) || !trigger.isValid()) continue;
        if (!best.isValid() || trigger < best) best = trigger;
    }
    return best.isValid() ? best.toUTC() : QDateTime();
}

QString DatePickerPopup::recurrence() const
{
    if (!m_schedule.repeating) return QStringLiteral("none");
    const QString text = recurrenceFrequencyName(m_schedule.recurrence);
    return text.isEmpty() ? QStringLiteral("none") : text;
}

bool DatePickerPopup::cleared() const
{
    return m_cleared;
}

// ---------------------------------------------------------------------------
// 弹层定位
// ---------------------------------------------------------------------------

void DatePickerPopup::popupBelow(QWidget* anchor)
{
    if (!anchor) return;
    popupAt(QRect(anchor->mapToGlobal(QPoint()), anchor->size()));
}

void DatePickerPopup::popupAt(const QRect& globalAnchor)
{
    m_cleared = false;
    m_popupAnchor = globalAnchor;
    m_popupPositioned = true;
    closeSubPopups();
    clearCardError();
    const int firstDay = configuredFirstDay();
    if (m_firstDay != firstDay) {
        m_firstDay = firstDay;
        refreshAll();
    }
    updateCardGeometry();

    QPoint position(globalAnchor.left() - kShadowMargin,
                    globalAnchor.bottom() + 6 - kShadowMargin);
    QScreen* screen = QGuiApplication::screenAt(globalAnchor.center());
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
    if (!m_range && hasDate() && m_inlineCalendar) {
        for (QToolButton* button : m_inlineCalendar->findChildren<QToolButton*>(
                 QStringLiteral("DatePickerDay"))) {
            if (button->isChecked() && !button->property("outsideMonth").toBool()) {
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
