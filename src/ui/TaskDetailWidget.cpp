#include "TaskDetailWidget.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include "../app/TaskStore.h"
#include "MarkdownEditor.h"
#include "TaskFilter.h"

namespace kn {
TaskDetailWidget::TaskDetailWidget(TaskStore* store, QWidget* parent) : QWidget(parent), m_store(store) {
    buildUi();
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(450);
    connect(&m_saveTimer, &QTimer::timeout, this, &TaskDetailWidget::flush);
    connect(store, &TaskStore::taskChanged, this, [this](qint64 id) {
        if (id == m_id && !m_saving && m_dirty.isEmpty())
            loadFields();
    });
    connect(store, &TaskStore::tasksReset, this, [this]() {
        if (m_dirty.isEmpty())
            loadFields();
    });
    connect(store, &TaskStore::groupsChanged, this, &TaskDetailWidget::refreshGroups);
    setTask(0);
}
void TaskDetailWidget::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    m_empty = new QLabel(QStringLiteral("选择一项任务\n\n在这里安排时间、编辑内容或设置提醒。"), this);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setProperty("muted", true);
    root->addWidget(m_empty, 1);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    root->addWidget(scroll, 1);
    m_form = new QWidget(scroll);
    scroll->setWidget(m_form);
    m_form->setObjectName(QStringLiteral("TaskDetailForm"));
    auto* layout = new QVBoxLayout(m_form);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);
    auto* top = new QHBoxLayout;
    m_completed = new QCheckBox(QStringLiteral("已完成"), m_form);
    m_completed->setObjectName(QStringLiteral("TaskCompleted"));
    m_pinned = new QCheckBox(QStringLiteral("贴到桌面"), m_form);
    m_pinned->setObjectName(QStringLiteral("TaskPinned"));
    top->addWidget(m_completed);
    top->addStretch();
    top->addWidget(m_pinned);
    layout->addLayout(top);
    m_title = new QLineEdit(m_form);
    m_title->setObjectName(QStringLiteral("TaskTitle"));
    m_title->setPlaceholderText(QStringLiteral("任务标题"));
    m_title->setMaxLength(500);
    QFont heading = m_title->font();
    heading.setPointSize(14);
    heading.setBold(true);
    m_title->setFont(heading);
    layout->addWidget(m_title);
    auto* classification = new QFormLayout;
    classification->setHorizontalSpacing(12);
    m_group = new QComboBox(m_form);
    m_group->setObjectName(QStringLiteral("TaskGroup"));
    classification->addRow(QStringLiteral("分组"), m_group);
    m_priority = new QComboBox(m_form);
    m_priority->addItems({QStringLiteral("普通"), QStringLiteral("重要"), QStringLiteral("紧急")});
    classification->addRow(QStringLiteral("优先级"), m_priority);
    m_favorite = new QCheckBox(QStringLiteral("星标关注"), m_form);
    classification->addRow(QString(), m_favorite);
    layout->addLayout(classification);
    auto* timeTitle = new QLabel(QStringLiteral("安排与提醒"), m_form);
    timeTitle->setProperty("section", true);
    layout->addWidget(timeTitle);
    auto* time = new QFormLayout;
    auto addTime = [this, time](
                       const QString& label, const QString& name, QCheckBox*& check, QDateTimeEdit*& edit) {
        check = new QCheckBox(label, m_form);
        check->setObjectName(name + QStringLiteral("Enabled"));
        edit = new QDateTimeEdit(QDateTime::currentDateTime().addSecs(3600), m_form);
        edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
        edit->setCalendarPopup(true);
        edit->setObjectName(name);
        time->addRow(check, edit);
        connect(check, &QCheckBox::toggled, this, [this, edit, name](bool enabled) {
            edit->setEnabled(enabled);
            dirty(name);
        });
        connect(edit, &QDateTimeEdit::dateTimeChanged, this, [this, name]() { dirty(name); });
    };
    addTime(QStringLiteral("开始"), QStringLiteral("start"), m_hasStart, m_start);
    addTime(QStringLiteral("截止"), QStringLiteral("due"), m_hasDue, m_due);
    addTime(QStringLiteral("提醒"), QStringLiteral("reminder"), m_hasReminder, m_reminder);
    m_recurrence = new QComboBox(m_form);
    m_recurrence->addItem(QStringLiteral("不重复"), QStringLiteral("none"));
    m_recurrence->addItem(QStringLiteral("每天"), QStringLiteral("daily"));
    m_recurrence->addItem(QStringLiteral("每周"), QStringLiteral("weekly"));
    m_recurrence->addItem(QStringLiteral("每月"), QStringLiteral("monthly"));
    time->addRow(QStringLiteral("重复"), m_recurrence);
    layout->addLayout(time);
    auto* presets = new QHBoxLayout;
    auto *atStart = new QPushButton(QStringLiteral("开始时提醒"), m_form),
         *beforeDue = new QPushButton(QStringLiteral("截止前10分钟"), m_form);
    presets->addWidget(atStart);
    presets->addWidget(beforeDue);
    layout->addLayout(presets);
    connect(atStart, &QPushButton::clicked, this, [this]() {
        if (m_hasStart->isChecked()) {
            m_reminder->setDateTime(m_start->dateTime());
            m_hasReminder->setChecked(true);
        } else
            m_status->setText(QStringLiteral("请先勾选并设置开始时间。"));
    });
    connect(beforeDue, &QPushButton::clicked, this, [this]() {
        if (m_hasDue->isChecked()) {
            m_reminder->setDateTime(m_due->dateTime().addSecs(-600));
            m_hasReminder->setChecked(true);
        } else
            m_status->setText(QStringLiteral("请先勾选并设置截止时间。"));
    });
    auto* hint = new QLabel(
        QStringLiteral(
            "提醒通过系统通知和应用弹窗显示。应用需保持运行；休眠或关机期间的提醒会在恢复运行后补发。"),
        m_form);
    hint->setWordWrap(true);
    hint->setProperty("muted", true);
    layout->addWidget(hint);
    auto* descriptionTitle = new QLabel(QStringLiteral("任务说明"), m_form);
    descriptionTitle->setProperty("section", true);
    layout->addWidget(descriptionTitle);
    m_description = new MarkdownEditor(m_form);
    layout->addWidget(m_description);
    auto* checkTitle = new QLabel(QStringLiteral("子任务清单"), m_form);
    checkTitle->setProperty("section", true);
    layout->addWidget(checkTitle);
    m_checklist = new QListWidget(m_form);
    m_checklist->setObjectName(QStringLiteral("TaskChecklist"));
    m_checklist->setMinimumHeight(80);
    m_checklist->setMaximumHeight(160);
    layout->addWidget(m_checklist);
    auto* checkRow = new QHBoxLayout;
    m_checkInput = new QLineEdit(m_form);
    m_checkInput->setPlaceholderText(QStringLiteral("添加一项，按回车确认"));
    m_checkInput->setObjectName(QStringLiteral("ChecklistInput"));
    auto *addCheck = new QPushButton(QStringLiteral("添加"), m_form),
         *removeCheck = new QPushButton(QStringLiteral("移除"), m_form);
    checkRow->addWidget(m_checkInput, 1);
    checkRow->addWidget(addCheck);
    checkRow->addWidget(removeCheck);
    layout->addLayout(checkRow);
    auto addItem = [this]() {
        const QString text = m_checkInput->text().trimmed();
        if (text.isEmpty())
            return;
        auto* item = new QListWidgetItem(text, m_checklist);
        item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        m_checkInput->clear();
        dirty(QStringLiteral("checklist"));
    };
    connect(addCheck, &QPushButton::clicked, this, addItem);
    connect(m_checkInput, &QLineEdit::returnPressed, this, addItem);
    connect(removeCheck, &QPushButton::clicked, this, [this]() {
        if (m_checklist->currentRow() >= 0) {
            delete m_checklist->takeItem(m_checklist->currentRow());
            dirty(QStringLiteral("checklist"));
        }
    });
    connect(m_checklist, &QListWidget::itemChanged, this, [this]() { dirty(QStringLiteral("checklist")); });
    m_tags = new QLineEdit(m_form);
    m_tags->setPlaceholderText(QStringLiteral("标签，用逗号分隔，例如：工作, 本周"));
    layout->addWidget(m_tags);
    auto* appearance = new QFormLayout;
    m_color = new QComboBox(m_form);
    m_color->addItems({QStringLiteral("浅黄"),
                       QStringLiteral("浅蓝"),
                       QStringLiteral("浅绿"),
                       QStringLiteral("浅粉"),
                       QStringLiteral("浅紫"),
                       QStringLiteral("白色")});
    appearance->addRow(QStringLiteral("便签颜色"), m_color);
    m_layer = new QComboBox(m_form);
    m_layer->addItems(
        {QStringLiteral("在其他窗口下方"), QStringLiteral("普通窗口"), QStringLiteral("始终置顶")});
    appearance->addRow(QStringLiteral("便签层位"), m_layer);
    layout->addLayout(appearance);
    m_status = new QLabel(m_form);
    m_status->setObjectName(QStringLiteral("DetailStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto* footer = new QHBoxLayout;
    auto* save = new QPushButton(QStringLiteral("立即保存"), m_form);
    save->setObjectName(QStringLiteral("SaveTask"));
    m_restore = new QPushButton(QStringLiteral("恢复任务"), m_form);
    m_remove = new QPushButton(QStringLiteral("移到回收站"), m_form);
    footer->addWidget(save);
    footer->addStretch();
    footer->addWidget(m_restore);
    footer->addWidget(m_remove);
    layout->addLayout(footer);
    layout->addStretch();
    connect(save, &QPushButton::clicked, this, &TaskDetailWidget::flush);
    connect(m_title, &QLineEdit::textEdited, this, [this]() { dirty(QStringLiteral("title")); });
    connect(m_tags, &QLineEdit::textEdited, this, [this]() { dirty(QStringLiteral("tags")); });
    connect(m_description, &MarkdownEditor::contentChanged, this, [this]() {
        dirty(QStringLiteral("description"));
    });
    const auto combo = [this](QComboBox* box, const QString& field) {
        connect(box, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, field]() {
            dirty(field);
        });
    };
    combo(m_group, QStringLiteral("group"));
    combo(m_priority, QStringLiteral("priority"));
    combo(m_recurrence, QStringLiteral("recurrence"));
    combo(m_color, QStringLiteral("color"));
    combo(m_layer, QStringLiteral("layer"));
    connect(m_favorite, &QCheckBox::toggled, this, [this]() { dirty(QStringLiteral("favorite")); });
    connect(m_completed, &QCheckBox::clicked, this, [this](bool value) {
        if (!flush() || !m_store->setCompleted(m_id, value)) {
            const QSignalBlocker blocker(m_completed);
            m_completed->setChecked(m_store->task(m_id).completed);
        }
    });
    connect(m_pinned, &QCheckBox::clicked, this, [this](bool value) {
        if (!flush() || !m_store->setPinned(m_id, value)) {
            const QSignalBlocker blocker(m_pinned);
            m_pinned->setChecked(m_store->task(m_id).pinned);
        }
    });
    connect(m_restore, &QPushButton::clicked, this, [this]() { m_store->restoreTask(m_id); });
    connect(m_remove, &QPushButton::clicked, this, [this]() {
        if (!flush())
            return;
        const Task task = m_store->task(m_id);
        if (task.deleted) {
            if (QMessageBox::question(this,
                                      QStringLiteral("永久删除"),
                                      QStringLiteral("永久删除“%1”？此操作无法撤销。").arg(task.title),
                                      QMessageBox::Yes | QMessageBox::No,
                                      QMessageBox::No) != QMessageBox::Yes)
                return;
            if (m_store->deletePermanently(m_id)) {
                const auto id = m_id;
                setTask(0);
                emit taskRemoved(id);
            }
        } else
            m_store->trashTask(m_id);
    });
}
void TaskDetailWidget::refreshGroups() {
    const QSignalBlocker blocker(m_group);
    const qint64 selected = m_dirty.contains(QStringLiteral("group")) ? m_group->currentData().toLongLong()
                                                                      : m_store->task(m_id).groupId;
    m_group->clear();
    m_group->addItem(QStringLiteral("未分组"), qint64(0));
    for (const auto& group : m_store->groups())
        m_group->addItem(group.name, group.id);
    const int index = m_group->findData(selected);
    m_group->setCurrentIndex(index < 0 ? 0 : index);
}
bool TaskDetailWidget::setTask(qint64 id) {
    if (m_id == id && id != 0)
        return true;
    if (!flush())
        return false;
    m_id = id;
    m_dirty.clear();
    loadFields();
    return true;
}
void TaskDetailWidget::loadFields() {
    const Task task = m_store->task(m_id);
    const bool exists = task.id > 0;
    m_loading = true;
    m_empty->setVisible(!exists);
    m_form->parentWidget()->parentWidget()->setVisible(exists);
    if (!exists) {
        m_id = 0;
        m_loading = false;
        return;
    }
    m_title->setText(task.title);
    m_description->setContent(task.description, task.contentFormat);
    m_completed->setChecked(task.completed);
    m_pinned->setChecked(task.pinned);
    m_favorite->setChecked(task.favorite);
    refreshGroups();
    m_priority->setCurrentIndex(qBound(0, task.priority, 2));
    const auto setTime = [](QCheckBox* check, QDateTimeEdit* edit, const QDateTime& date) {
        check->setChecked(date.isValid());
        edit->setEnabled(date.isValid());
        edit->setDateTime(date.isValid() ? date.toLocalTime() : QDateTime::currentDateTime().addSecs(3600));
    };
    setTime(m_hasStart, m_start, task.startAt);
    setTime(m_hasDue, m_due, task.dueAt);
    setTime(m_hasReminder, m_reminder, task.reminderAt);
    m_recurrence->setCurrentIndex(qMax(0, m_recurrence->findData(task.recurrence)));
    m_tags->setText(task.tags.join(QStringLiteral(", ")));
    m_color->setCurrentIndex(colorToInt(task.color));
    m_layer->setCurrentIndex(layerToInt(task.layer));
    m_checklist->clear();
    for (const auto& check : task.checklist) {
        auto* item = new QListWidgetItem(check.text, m_checklist);
        item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
        item->setCheckState(check.done ? Qt::Checked : Qt::Unchecked);
    }
    m_status->setText(task.deleted ? QStringLiteral("任务在回收站中，恢复后可继续编辑。")
                                   : QStringLiteral("已保存，所有更改保存在本机。"));
    updateEnabled();
    m_loading = false;
}
void TaskDetailWidget::updateEnabled() {
    const bool enabled = !m_store->task(m_id).deleted;
    const QList<QWidget*> fields = {m_title,
                                    m_description,
                                    m_group,
                                    m_priority,
                                    m_favorite,
                                    m_completed,
                                    m_pinned,
                                    m_tags,
                                    m_recurrence,
                                    m_color,
                                    m_layer,
                                    m_hasStart,
                                    m_hasDue,
                                    m_hasReminder,
                                    m_checklist,
                                    m_checkInput};
    for (QWidget* widget : fields)
        widget->setEnabled(enabled);
    m_start->setEnabled(enabled && m_hasStart->isChecked());
    m_due->setEnabled(enabled && m_hasDue->isChecked());
    m_reminder->setEnabled(enabled && m_hasReminder->isChecked());
    for (auto* button : m_form->findChildren<QPushButton*>())
        if (button != m_restore && button != m_remove)
            button->setEnabled(enabled);
    m_restore->setVisible(!enabled);
    m_remove->setText(enabled ? QStringLiteral("移到回收站") : QStringLiteral("永久删除"));
}
void TaskDetailWidget::dirty(const QString& field) {
    if (m_loading || !m_id || m_store->task(m_id).deleted)
        return;
    m_dirty.insert(field);
    m_status->setText(QStringLiteral("正在编辑…"));
    m_saveTimer.start();
}
bool TaskDetailWidget::flush() {
    m_saveTimer.stop();
    if (m_dirty.isEmpty() || !m_id)
        return true;
    Task task = m_store->task(m_id);
    if (!task.id)
        return false;
    if (m_dirty.contains(QStringLiteral("title")))
        task.title = m_title->text().trimmed();
    if (m_dirty.contains(QStringLiteral("description"))) {
        task.description = m_description->content();
        task.contentFormat = ContentFormat::Markdown;
    }
    if (m_dirty.contains(QStringLiteral("group")))
        task.groupId = m_group->currentData().toLongLong();
    if (m_dirty.contains(QStringLiteral("priority")))
        task.priority = m_priority->currentIndex();
    if (m_dirty.contains(QStringLiteral("favorite")))
        task.favorite = m_favorite->isChecked();
    if (m_dirty.contains(QStringLiteral("start")))
        task.startAt = m_hasStart->isChecked() ? m_start->dateTime().toUTC() : QDateTime();
    if (m_dirty.contains(QStringLiteral("due")))
        task.dueAt = m_hasDue->isChecked() ? m_due->dateTime().toUTC() : QDateTime();
    if (m_dirty.contains(QStringLiteral("reminder"))) {
        const QDateTime date = m_hasReminder->isChecked() ? m_reminder->dateTime().toUTC() : QDateTime();
        if (task.reminderAt != date)
            task.notifiedAt = QDateTime();
        task.reminderAt = date;
    }
    if (m_dirty.contains(QStringLiteral("recurrence")))
        task.recurrence = m_recurrence->currentData().toString();
    if (m_dirty.contains(QStringLiteral("color")))
        task.color = colorFromInt(m_color->currentIndex());
    if (m_dirty.contains(QStringLiteral("layer")))
        task.layer = layerFromInt(m_layer->currentIndex());
    if (m_dirty.contains(QStringLiteral("tags"))) {
        task.tags =
            m_tags->text().split(QRegularExpression(QStringLiteral("[,，;；]")), QString::SkipEmptyParts);
        for (auto& tag : task.tags)
            tag = tag.trimmed();
        task.tags.removeDuplicates();
    }
    if (m_dirty.contains(QStringLiteral("checklist"))) {
        task.checklist.clear();
        for (int i = 0; i < m_checklist->count(); ++i) {
            const auto* item = m_checklist->item(i);
            ChecklistItem check;
            check.text = item->text().trimmed();
            check.done = item->checkState() == Qt::Checked;
            if (!check.text.isEmpty())
                task.checklist.append(check);
        }
    }
    if (task.title.isEmpty()) {
        m_status->setText(QStringLiteral("尚未保存：任务标题不能为空。"));
        return false;
    }
    if (task.startAt.isValid() && task.dueAt.isValid() && task.dueAt < task.startAt) {
        m_status->setText(QStringLiteral("尚未保存：截止时间不能早于开始时间。"));
        return false;
    }
    m_saving = true;
    const bool ok = m_store->saveTask(task);
    m_saving = false;
    if (ok) {
        m_dirty.clear();
        m_status->setText(
            QStringLiteral("已保存  %1").arg(QTime::currentTime().toString(QStringLiteral("HH:mm"))));
    } else
        m_status->setText(QStringLiteral("保存失败，编辑内容已保留。请修正后点击“立即保存”。"));
    return ok;
}
void TaskDetailWidget::focusTitle() {
    m_title->setFocus();
    m_title->selectAll();
}
}  // namespace kn
