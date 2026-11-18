#include "DatePickerReminderEditor.h"

#include <QComboBox>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include "../app/Settings.h"
#include "../Common.h"
#include "../domain/Entity.h"
#include "../domain/ReminderRule.h"
#include "AnchoredListPopup.h"
#include "DatePickerControls.h"
#include "DatePickerPopup.h"
#include "MenuComboBox.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {

QString reminderRule(ReminderSpec::Anchor anchor, ReminderSpec::Unit unit, int offset)
{
    ReminderSpec spec;
    spec.anchor = anchor;
    spec.unit = unit;
    spec.offset = offset;
    return reminderRuleText(spec);
}

QString allDayRule(int dayOffset, const QTime& localTime = QTime(9, 0))
{
    ReminderSpec spec;
    spec.allDay = true;
    spec.dayOffset = dayOffset;
    spec.localTime = localTime;
    return reminderRuleText(spec);
}

bool hasLegacyReminder(const QVector<TaskReminder>& reminders, qint64 triggerMs)
{
    if (triggerMs <= 0) return false;
    for (const TaskReminder& row : reminders)
        if (row.ruleJson.isEmpty() && utcMs(row.triggerAt) == triggerMs) return true;
    return false;
}

} // namespace

DatePickerReminderEditor::DatePickerReminderEditor(DatePickerPopup* owner)
    : QObject(owner), m_owner(owner)
{
    m_popup = new AnchoredListPopup(owner);
    m_popup->setObjectName(QStringLiteral("DatePickerReminderLayer"));
    m_popup->setWindowTitle(QStringLiteral("提醒"));
    m_popup->setFont(ds::qfont(ds::FontScene::Menu));
    m_popup->content()->setObjectName(QStringLiteral("DatePickerReminderPage"));
    auto* layout = new QVBoxLayout(m_popup->content());
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_pages = new QWidget(m_popup->content());
    auto* pagesLayout = new QVBoxLayout(m_pages);
    pagesLayout->setContentsMargins(0, 0, 0, 0);
    pagesLayout->setSpacing(0);
    m_rootPage = buildRootPage();
    m_customPage = buildCustomPage();
    pagesLayout->addWidget(m_rootPage);
    pagesLayout->addWidget(m_customPage);
    m_customPage->setVisible(false);
    layout->addWidget(m_pages);
    m_popup->setCardWidth(DatePickerUi::kSubCardWidth);
    connect(m_popup, &QDialog::finished, this, [this](int result) {
        recordRecentRules(result == QDialog::Accepted);
    });
}

QWidget* DatePickerReminderEditor::buildRootPage()
{
    auto* page = new QWidget(m_pages);
    page->setObjectName(QStringLiteral("DatePickerReminderMenu"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(4, 4, 4, 0);
    layout->setSpacing(0);
    m_menuScroll = new QScrollArea(page);
    m_menuScroll->setObjectName(QStringLiteral("DatePickerScroll"));
    m_menuScroll->setWidgetResizable(true);
    m_menuScroll->setFrameShape(QFrame::NoFrame);
    m_menuScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_menuScroll);
    layout->addLayout(buildFooter(page, false));
    return page;
}

QWidget* DatePickerReminderEditor::buildCustomPage()
{
    auto* page = new QWidget(m_pages);
    page->setObjectName(QStringLiteral("DatePickerReminderCustomPage"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 12, 16, 16);
    layout->setSpacing(4);
    m_customForms = new QWidget(page);
    auto* formsLayout = new QVBoxLayout(m_customForms);
    formsLayout->setContentsMargins(0, 0, 0, 0);
    formsLayout->setSpacing(0);
    m_timedForm = buildTimedForm();
    m_allDayForm = buildAllDayForm();
    formsLayout->addWidget(m_timedForm);
    formsLayout->addWidget(m_allDayForm);
    m_allDayForm->setVisible(false);
    layout->addWidget(m_customForms);

    m_customSummary = new QLabel(page);
    m_customSummary->setFont(ds::qfont(ds::FontScene::Caption));
    m_customSummary->setProperty("muted", true);
    layout->addWidget(m_customSummary);
    m_customTrigger = new QLabel(page);
    m_customTrigger->setObjectName(QStringLiteral("DatePickerReminderTrigger"));
    m_customTrigger->setFont(ds::qfont(ds::FontScene::Caption));
    layout->addWidget(m_customTrigger);
    m_customError = new QLabel(page);
    m_customError->setObjectName(QStringLiteral("DatePickerError"));
    m_customError->setWordWrap(true);
    m_customError->setVisible(false);
    layout->addWidget(m_customError);
    QHBoxLayout* footer = buildFooter(page, true);
    footer->setContentsMargins(0, 8, 0, 0);
    layout->addLayout(footer);
    return page;
}

QWidget* DatePickerReminderEditor::buildTimedForm()
{
    auto* form = new QWidget(m_customForms);
    form->setObjectName(QStringLiteral("DatePickerReminderTimedCustom"));
    auto* layout = new QHBoxLayout(form);
    layout->setContentsMargins(0, 0, 0, 8);
    layout->setSpacing(8);
    m_offsetSpin = DatePickerUi::spinBox(1, 100000, QStringLiteral("提醒提前量"), form);
    m_offsetSpin->setValue(30);
    m_unitCombo = DatePickerUi::comboBox(QStringLiteral("提醒单位"), form);
    m_unitCombo->addItem(QStringLiteral("分钟"), static_cast<int>(ReminderSpec::Unit::Minutes));
    m_unitCombo->addItem(QStringLiteral("小时"), static_cast<int>(ReminderSpec::Unit::Hours));
    m_unitCombo->addItem(QStringLiteral("天"), static_cast<int>(ReminderSpec::Unit::Days));
    m_unitCombo->addItem(QStringLiteral("周"), static_cast<int>(ReminderSpec::Unit::Weeks));
    layout->addWidget(m_offsetSpin, 1);
    layout->addWidget(m_unitCombo, 1);
    connect(m_offsetSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int) { updateCustomPreview(); });
    connect(m_unitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateCustomPreview(); });
    return form;
}

QWidget* DatePickerReminderEditor::buildAllDayForm()
{
    auto* form = new QWidget(m_customForms);
    form->setObjectName(QStringLiteral("DatePickerReminderAllDayCustom"));
    auto* layout = new QVBoxLayout(form);
    layout->setContentsMargins(0, 0, 0, 8);
    layout->setSpacing(8);
    m_allDayModeCombo = DatePickerUi::comboBox(QStringLiteral("全天提醒提前方式"), form);
    m_allDayModeCombo->addItem(QStringLiteral("按天提前"));
    m_allDayModeCombo->addItem(QStringLiteral("按周提前"));
    layout->addWidget(m_allDayModeCombo);

    auto* labels = new QHBoxLayout;
    labels->setSpacing(8);
    auto* days = new QLabel(QStringLiteral("提前天数"), form);
    days->setProperty("formCaption", true);
    auto* time = new QLabel(QStringLiteral("提醒时间"), form);
    time->setProperty("formCaption", true);
    labels->addWidget(days, 1);
    labels->addWidget(time, 1);
    layout->addLayout(labels);
    auto* controls = new QHBoxLayout;
    controls->setSpacing(8);
    m_allDayOffsetSpin = DatePickerUi::spinBox(0, 3650, QStringLiteral("提醒提前天数"), form);
    m_allDayOffsetSpin->setValue(1);
    m_allDayTimeEdit = DatePickerUi::fieldInput(QStringLiteral("09:00"),
                                                 QStringLiteral("提醒本地时间"), form);
    m_allDayTimeEdit->setMaxLength(5);
    m_allDayTimeEdit->setText(QStringLiteral("09:00"));
    controls->addWidget(m_allDayOffsetSpin, 1);
    controls->addWidget(m_allDayTimeEdit, 1);
    layout->addLayout(controls);
    connect(m_allDayModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, days](int index) {
        days->setText(index == 1 ? QStringLiteral("提前周数") : QStringLiteral("提前天数"));
        m_allDayOffsetSpin->setMaximum(index == 1 ? 521 : 3650);
        updateCustomPreview();
    });
    connect(m_allDayOffsetSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int) { updateCustomPreview(); });
    connect(m_allDayTimeEdit, &QLineEdit::textChanged,
            this, [this]() { updateCustomPreview(); });
    return form;
}

QHBoxLayout* DatePickerReminderEditor::buildFooter(QWidget* parent, bool custom)
{
    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(12, 8, 12, 12);
    footer->setSpacing(12);
    auto* cancel = DatePickerUi::actionButton(QStringLiteral("取消"), "DatePickerSubClear", parent);
    auto* done = DatePickerUi::actionButton(QStringLiteral("确定"), "DatePickerSubConfirm", parent);
    cancel->setMinimumWidth(60);
    done->setMinimumWidth(60);
    footer->addWidget(cancel, 1);
    footer->addWidget(done, 1);
    connect(cancel, &QPushButton::clicked, m_popup, &QDialog::reject);
    if (custom) connect(done, &QPushButton::clicked, this, [this]() { confirmCustom(); });
    else connect(done, &QPushButton::clicked, m_popup, &QDialog::accept);
    return footer;
}

void DatePickerReminderEditor::open(bool allDay, bool range)
{
    m_allDay = allDay;
    m_range = range;
    m_pendingRecentRules.clear();
    m_offsetSpin->setValue(30);
    m_unitCombo->setCurrentIndex(0);
    m_allDayModeCombo->setCurrentIndex(0);
    m_allDayOffsetSpin->setValue(1);
    m_allDayTimeEdit->setText(QStringLiteral("09:00"));
    rebuildMenu();
    m_rootPage->setVisible(true);
    m_customPage->setVisible(false);
    m_popup->setUpperBoundGlobalY(m_owner->calendarGuardY());
    m_popup->popupBelow(m_owner->m_reminderEntry);
}

void DatePickerReminderEditor::rebuildMenu()
{
    m_rows.clear();
    m_presetRules.clear();
    if (m_allDay) {
        const int offsets[] = {0, 1, 2, 3, 7};
        for (int offset : offsets) m_presetRules.append(allDayRule(offset));
    } else {
        const ReminderSpec::Unit units[] = {
            ReminderSpec::Unit::Minutes, ReminderSpec::Unit::Minutes,
            ReminderSpec::Unit::Minutes, ReminderSpec::Unit::Minutes,
            ReminderSpec::Unit::Hours, ReminderSpec::Unit::Days};
        const int offsets[] = {0, 5, 15, 30, 1, 1};
        for (int i = 0; i < 6; ++i)
            m_presetRules.append(reminderRule(ReminderSpec::Anchor::Schedule, units[i], offsets[i]));
        if (m_range) {
            m_presetRules.append(reminderRule(ReminderSpec::Anchor::End,
                                              ReminderSpec::Unit::Minutes, 0));
        }
    }

    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("DatePickerReminderMenuContent"));
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_menuScroll->setWidget(content);
    m_menuScroll->viewport()->setAutoFillBackground(false);
    content->setAutoFillBackground(false);

    QStringList recent = Settings::instance().recentReminderRules(m_allDay);
    for (const QString& preset : m_presetRules) recent.removeAll(preset);
    QHash<QString, qint64> legacyByRule;
    for (const TaskReminder& row : m_owner->m_reminders) {
        QString rule = row.ruleJson;
        if (rule.isEmpty()) {
            if (!row.triggerAt.isValid()) continue;
            ReminderSpec absolute;
            absolute.anchor = ReminderSpec::Anchor::Absolute;
            absolute.absoluteAt = row.triggerAt;
            rule = reminderRuleText(absolute);
            legacyByRule.insert(rule, utcMs(row.triggerAt));
        }
        if (!m_presetRules.contains(rule) && !recent.contains(rule)) recent.append(rule);
    }
    for (const QString& rule : recent) {
        TaskReminder row;
        row.ruleJson = rule;
        addMenuRow(rule, m_owner->reminderCaption(row), QString(), legacyByRule.value(rule, 0));
    }

    if (m_allDay) {
        const QString labels[] = {QStringLiteral("当天"), QStringLiteral("提前1天"),
                                  QStringLiteral("提前2天"), QStringLiteral("提前3天"),
                                  QStringLiteral("提前1周")};
        for (int i = 0; i < m_presetRules.size(); ++i)
            addMenuRow(m_presetRules.at(i), labels[i], QStringLiteral("(09:00)"));
    } else {
        QStringList labels = {QStringLiteral("准时"), QStringLiteral("提前5分钟"),
                              QStringLiteral("提前15分钟"), QStringLiteral("提前30分钟"),
                              QStringLiteral("提前1小时"), QStringLiteral("提前1天")};
        if (m_range) labels.append(QStringLiteral("结束时"));
        for (int i = 0; i < m_presetRules.size(); ++i)
            addMenuRow(m_presetRules.at(i), labels.at(i));
    }

    auto* custom = new DatePickerMenuRow(QStringLiteral("自定义"), content);
    custom->setObjectName(QStringLiteral("DatePickerReminderCustomEntry"));
    custom->setCheckable(false);
    layout->addWidget(custom);
    m_rows.append(custom);
    connect(custom, &QPushButton::clicked, this, [this]() { showCustomPage(); });
    refreshMenuChecks();

    const int visibleRows = qBound(2, static_cast<int>(m_rows.size()), 8);
    m_menuScroll->setFixedHeight(visibleRows * DatePickerUi::kRowHeight);
    m_popup->setFlexibleWidget(m_menuScroll, visibleRows * DatePickerUi::kRowHeight,
                               2 * DatePickerUi::kRowHeight);
    m_popup->setInitialFocusWidget(m_rows.isEmpty() ? custom : m_rows.first());
}

void DatePickerReminderEditor::addMenuRow(const QString& ruleJson, const QString& text,
                                           const QString& secondary, qint64 legacyMs)
{
    auto* content = m_menuScroll->widget();
    auto* layout = qobject_cast<QVBoxLayout*>(content->layout());
    auto* row = new DatePickerMenuRow(text, content);
    row->setProperty("ruleJson", ruleJson);
    row->setProperty("legacyMs", legacyMs);
    row->setSecondaryText(secondary);
    layout->addWidget(row);
    m_rows.append(row);
    connect(row, &QPushButton::clicked, this, [this, ruleJson, legacyMs]() {
        const bool exists = m_owner->findReminderRowByRule(ruleJson) >= 0
            || hasLegacyReminder(m_owner->m_reminders, legacyMs);
        if (exists) {
            m_owner->removeReminderRule(ruleJson);
            if (legacyMs > 0) {
                for (int i = m_owner->m_reminders.size() - 1; i >= 0; --i) {
                    const TaskReminder& reminder = m_owner->m_reminders.at(i);
                    if (reminder.ruleJson.isEmpty() && utcMs(reminder.triggerAt) == legacyMs)
                        m_owner->m_reminders.remove(i);
                }
            }
        } else {
            m_owner->addReminderRule(ruleJson);
            if (!m_presetRules.contains(ruleJson)) {
                m_pendingRecentRules.removeAll(ruleJson);
                m_pendingRecentRules.append(ruleJson);
            }
        }
        refreshMenuChecks();
        m_owner->updateEntryCaptions();
    });
}

void DatePickerReminderEditor::refreshMenuChecks()
{
    for (DatePickerMenuRow* row : m_rows) {
        const QString rule = row->property("ruleJson").toString();
        if (rule.isEmpty()) continue;
        row->setChecked(m_owner->findReminderRowByRule(rule) >= 0
            || hasLegacyReminder(m_owner->m_reminders, row->property("legacyMs").toLongLong()));
    }
}

void DatePickerReminderEditor::showCustomPage()
{
    m_timedForm->setVisible(!m_allDay);
    m_allDayForm->setVisible(m_allDay);
    m_rootPage->setVisible(false);
    m_customPage->setVisible(true);
    m_popup->setFlexibleWidget(nullptr, 0, 0);
    updateCustomPreview();
    m_popup->relayout();
    if (m_allDay) m_allDayOffsetSpin->setFocus();
    else m_offsetSpin->setFocus();
}

bool DatePickerReminderEditor::buildCustomSpec(ReminderSpec* spec, QString* error) const
{
    if (!spec) return false;
    ReminderSpec value;
    if (m_allDay) {
        value.allDay = true;
        const int multiplier = m_allDayModeCombo->currentIndex() == 1 ? 7 : 1;
        value.dayOffset = m_allDayOffsetSpin->value() * multiplier;
        if (!DatePickerUi::parseTimeText(m_allDayTimeEdit->text(), &value.localTime)) {
            if (error) *error = QStringLiteral("提醒时间需为 00:00–23:59");
            return false;
        }
    } else {
        value.anchor = ReminderSpec::Anchor::Schedule;
        value.unit = static_cast<ReminderSpec::Unit>(m_unitCombo->currentData().toInt());
        value.offset = m_offsetSpin->value();
    }
    if (!isValidReminder(value, error)) return false;
    *spec = value;
    return true;
}

void DatePickerReminderEditor::updateCustomPreview()
{
    ReminderSpec spec;
    QString error;
    const bool valid = buildCustomSpec(&spec, &error);
    DatePickerUi::setInputInvalid(m_allDayTimeEdit, m_allDay && !valid, error);
    m_customError->setText(error);
    m_customError->setVisible(!valid);
    if (!valid) {
        m_customSummary->clear();
        m_customTrigger->clear();
        m_popup->relayout();
        return;
    }
    m_customSummary->setText(reminderSummary(spec));
    QDateTime trigger;
    if (resolveReminder(spec, m_owner->buildEntity(), &trigger, &error)) {
        const QDateTime local = m_owner->toZoneLocal(trigger);
        m_customTrigger->setText(local.toString(QStringLiteral("yyyy年M月d日，HH:mm 提醒")));
        m_customError->setVisible(false);
    } else {
        m_customTrigger->clear();
        m_customError->setText(error);
        m_customError->setVisible(true);
    }
    m_popup->relayout();
}

void DatePickerReminderEditor::confirmCustom()
{
    ReminderSpec spec;
    QString error;
    if (!buildCustomSpec(&spec, &error)) {
        updateCustomPreview();
        return;
    }
    QDateTime trigger;
    if (!resolveReminder(spec, m_owner->buildEntity(), &trigger, &error)) {
        m_customError->setText(error);
        m_customError->setVisible(true);
        m_popup->relayout();
        return;
    }
    const QString rule = reminderRuleText(spec);
    m_owner->addReminderRule(rule);
    if (!m_presetRules.contains(rule)) m_pendingRecentRules.append(rule);
    m_owner->updateEntryCaptions();
    m_popup->accept();
}

void DatePickerReminderEditor::recordRecentRules(bool accepted)
{
    if (accepted) {
        for (const QString& rule : m_pendingRecentRules) {
            m_owner->m_recentReminderRulesPending.removeAll(rule);
            m_owner->m_recentReminderRulesPending.append(rule);
        }
    }
    m_pendingRecentRules.clear();
}

} // namespace kl
