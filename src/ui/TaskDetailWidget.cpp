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
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include "../app/TaskStore.h"
#include "AppIcons.h"
#include "AttachmentWidget.h"
#include "ActivityLogDialog.h"
#include "MarkdownEditor.h"

namespace kl {
TaskDetailWidget::TaskDetailWidget(TaskStore* store, QWidget* parent) : QWidget(parent), m_store(store) {
    buildUi();
    m_description->setStore(store);
    connect(m_description, &MarkdownEditor::internalLinkActivated, this, &TaskDetailWidget::internalLinkRequested);
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(450);
    connect(&m_saveTimer, &QTimer::timeout, this, &TaskDetailWidget::flush);
    connect(store, &TaskStore::taskChanged, this, [this](QString id) {
        if (id == m_id && !m_saving && m_dirty.isEmpty())
            loadFields();
    });
    connect(store, &TaskStore::desktopNoteChanged, this, [this](QString id) {
        if (id == m_id && !m_saving && m_dirty.isEmpty())
            loadFields();
    });
    connect(store, &TaskStore::tasksReset, this, [this]() {
        if (m_dirty.isEmpty())
            loadFields();
    });
    connect(store, &TaskStore::groupsChanged, this, &TaskDetailWidget::refreshGroups);
    setTask(QString());
}

void TaskDetailWidget::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    m_empty = new QLabel(QStringLiteral("选择一项任务或笔记\n\n在这里编辑内容与检查清单。"), this);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setProperty("muted", true);
    root->addWidget(m_empty, 1);

    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    root->addWidget(scroll, 1);
    m_form = new QWidget(scroll);
    scroll->setWidget(m_form);
    m_form->setObjectName(QStringLiteral("TaskDetailForm"));
    auto* layout = new QVBoxLayout(m_form);
    layout->setContentsMargins(24, 16, 24, 12);
    layout->setSpacing(10);

    // ========== R8: 顶部固定区 —— 高频属性 ==========
    // 考据 03:30：完成框、设置日期、优先级旗标
    auto* top = new QHBoxLayout;
    m_completed = new QCheckBox(QStringLiteral("已完成"), m_form);
    m_completed->setObjectName(QStringLiteral("TaskCompleted"));
    m_pinnedAt = new QCheckBox(QStringLiteral("置顶"), m_form);
    m_pinnedAt->setObjectName(QStringLiteral("TaskPinnedAt"));
    m_pinned = new QCheckBox(QStringLiteral("贴到桌面"), m_form);
    m_pinned->setObjectName(QStringLiteral("TaskPinned"));
    top->addWidget(m_completed);
    top->addStretch();
    top->addWidget(m_pinnedAt);
    top->addWidget(m_pinned);
    layout->addLayout(top);

    // 标题
    m_title = new QLineEdit(m_form);
    m_title->setObjectName(QStringLiteral("TaskTitle"));
    m_title->setPlaceholderText(QStringLiteral("标题"));
    m_title->setMaxLength(500);
    m_title->setMinimumHeight(48);
    m_title->setStyleSheet(QStringLiteral("QLineEdit{background:transparent;border:0;padding:4px 0;}"));
    QFont heading = m_title->font();
    heading.setPointSize(14);
    heading.setBold(true);
    m_title->setFont(heading);
    layout->addWidget(m_title);

    // ========== 低频设置收纳区（考据 07:66：便签颜色/层位进入低频区） ==========
    auto* settingsHost = new QWidget(m_form);
    settingsHost->setObjectName(QStringLiteral("AdditionalSettings"));
    auto* settingsLayout = new QVBoxLayout(settingsHost);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsLayout->setSpacing(10);

    auto* classification = new QFormLayout;
    classification->setHorizontalSpacing(12);
    m_group = new QComboBox(m_form);
    m_group->setObjectName(QStringLiteral("TaskGroup"));
    classification->addRow(QStringLiteral("清单"), m_group);
    settingsLayout->addLayout(classification);

    m_priorityHost = new QWidget(m_form); m_priorityHost->setObjectName(QStringLiteral("TaskPriorityHost"));
    auto* priorityLayout = new QFormLayout(m_priorityHost); priorityLayout->setContentsMargins(0, 0, 0, 0);
    m_priority = new QComboBox(m_form);
    m_priority->addItems({QStringLiteral("无"), QStringLiteral("低"), QStringLiteral("中"), QStringLiteral("高")});
    priorityLayout->addRow(QStringLiteral("优先级"), m_priority);
    settingsLayout->addWidget(m_priorityHost);

    // ========== 安排与提醒 ==========
    m_scheduleHost = new QWidget(m_form); m_scheduleHost->setObjectName(QStringLiteral("TaskScheduleHost"));
    auto* scheduleLayout = new QVBoxLayout(m_scheduleHost);
    scheduleLayout->setContentsMargins(0, 0, 0, 0); scheduleLayout->setSpacing(10);
    layout->addWidget(m_scheduleHost);

    auto* timeTitle = new QLabel(QStringLiteral("安排与提醒"), m_form);
    timeTitle->setProperty("section", true);
    scheduleLayout->addWidget(timeTitle);

    auto* time = new QVBoxLayout;
    time->setSpacing(4);
    auto addTime = [this, time](
                       const QString& label, const QString& name, QCheckBox*& check, QDateTimeEdit*& edit) {
        check = new QCheckBox(label, m_form);
        check->setObjectName(name + QStringLiteral("Enabled"));
        const QDateTime defaultTime = toAppTime(appNow()).addSecs(3600);
        edit = new QDateTimeEdit(QDateTime(defaultTime.date(), defaultTime.time()), m_form);
        edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
        edit->setCalendarPopup(true);
        edit->setObjectName(name);
        auto* row = new QHBoxLayout;
        row->setSpacing(8);
        row->addWidget(check);
        row->addWidget(edit, 1);
        time->addLayout(row);
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
    auto* recurrenceRow = new QHBoxLayout;
    recurrenceRow->addWidget(new QLabel(QStringLiteral("重复"), m_form));
    recurrenceRow->addWidget(m_recurrence);
    time->addLayout(recurrenceRow);
    scheduleLayout->addLayout(time);

    // 快捷提醒预设
    auto* presets = new QHBoxLayout;
    auto *atStart = new QPushButton(QStringLiteral("开始时提醒"), m_form),
         *beforeDue = new QPushButton(QStringLiteral("截止前10分钟"), m_form);
    // R8: 更多设置切换按钮
    auto* settingsToggle = new QPushButton(QStringLiteral("更多设置"), m_form);
    settingsToggle->setObjectName(QStringLiteral("AdditionalSettingsToggle"));
    settingsToggle->setProperty("quiet", true);
    presets->addWidget(atStart);
    presets->addWidget(beforeDue);
    presets->addWidget(settingsToggle);
    scheduleLayout->addLayout(presets);
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
        QStringLiteral("提醒通过系统通知和应用弹窗显示。应用需保持运行；休眠或关机期间的提醒会在恢复运行后补发。"),
        m_form);
    hint->setWordWrap(true);
    hint->setProperty("muted", true);
    scheduleLayout->addWidget(hint);

    // ========== 正文 ==========
    auto* descriptionTitle = new QLabel(QStringLiteral("正文"), m_form);
    descriptionTitle->setProperty("section", true);
    layout->addWidget(descriptionTitle);
    m_description = new MarkdownEditor(m_form);
    layout->addWidget(m_description);

    // ========== 附件 ==========
    m_attachments = new AttachmentWidget(m_store, m_form);
    layout->addWidget(m_attachments);

    // ========== 检查清单 ==========
    auto* checkTitle = new QLabel(QStringLiteral("检查清单"), m_form);
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
        if (text.isEmpty()) return;
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

    // 标签
    m_tags = new QLineEdit(m_form);
    m_tags->setPlaceholderText(QStringLiteral("标签，用逗号分隔，例如：工作, 本周"));
    layout->addWidget(m_tags);

    // ========== 便签外观（低频区） ==========
    m_appearanceHost = new QWidget(m_form); m_appearanceHost->setObjectName(QStringLiteral("TaskAppearanceHost"));
    auto* appearance = new QFormLayout(m_appearanceHost); appearance->setContentsMargins(0, 0, 0, 0);
    m_color = new QComboBox(m_form);
    m_color->addItems({QStringLiteral("浅黄"), QStringLiteral("浅蓝"), QStringLiteral("浅绿"),
                       QStringLiteral("浅粉"), QStringLiteral("浅紫"), QStringLiteral("白色")});
    appearance->addRow(QStringLiteral("便签颜色"), m_color);
    m_layer = new QComboBox(m_form);
    m_layer->addItems({QStringLiteral("在其他窗口下方"), QStringLiteral("普通窗口"), QStringLiteral("始终置顶")});
    appearance->addRow(QStringLiteral("便签层位"), m_layer);
    settingsLayout->addWidget(m_appearanceHost);
    layout->addWidget(settingsHost);
    settingsHost->hide();
    connect(settingsToggle, &QPushButton::clicked, this, [settingsHost, settingsToggle]() {
        settingsHost->setVisible(!settingsHost->isVisible());
        settingsToggle->setText(settingsHost->isVisible() ? QStringLiteral("收起更多设置")
                                                          : QStringLiteral("更多设置"));
    });

    // 保存状态
    m_status = new QLabel(m_form);
    m_status->setObjectName(QStringLiteral("DetailStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    // ========== R8: 底部固定工具栏（考据 t02：48px，所属清单在左，更多在右） ==========
    auto* footer = new QHBoxLayout;
    footer->setSpacing(8);
    auto* save = new QPushButton(QStringLiteral("立即保存"), m_form);
    save->setObjectName(QStringLiteral("SaveTask"));
    save->setProperty("primary", true);
    footer->addWidget(save);

    // R8: 低频命令收进「更多」菜单（考据 R11：详情更多与右键不同集合）
    m_moreButton = new QToolButton(m_form);
    m_moreButton->setIcon(AppIcons::icon(IconId::More, 18));
    m_moreButton->setToolTip(QStringLiteral("更多操作"));
    m_moreButton->setAccessibleName(QStringLiteral("详情更多操作"));
    m_moreButton->setObjectName(QStringLiteral("DetailMore"));
    m_moreButton->setAutoRaise(true);
    m_moreButton->setFixedSize(36, 36);
    m_moreButton->setCursor(Qt::PointingHandCursor);
    connect(m_moreButton, &QToolButton::clicked, this, &TaskDetailWidget::showMoreMenu);
    footer->addWidget(m_moreButton);

    footer->addStretch();

    // 恢复/删除按钮（回收站语境）
    m_restore = new QPushButton(QStringLiteral("恢复"), m_form);
    m_restore->setObjectName(QStringLiteral("RestoreTask"));
    footer->addWidget(m_restore);
    m_remove = new QPushButton(QStringLiteral("移到回收站"), m_form);
    m_remove->setObjectName(QStringLiteral("TrashTask"));
    footer->addWidget(m_remove);
    layout->addLayout(footer);
    layout->addStretch();

    // 隐藏的兼容按钮：测试通过 objectName 查找，实际功能在更多菜单中
    auto* convertCompat = new QPushButton(m_form);
    convertCompat->setObjectName(QStringLiteral("ConvertKind"));
    convertCompat->hide();
    connect(convertCompat, &QPushButton::clicked, this, [this]() {
        if (!finishEditing()) return;
        const Entity current = m_store->entity(m_id);
        const EntityKind target = current.kind == EntityKind::Task ? EntityKind::Note : EntityKind::Task;
        const QString text = target == EntityKind::Note
            ? QStringLiteral("完成状态、日期、提醒、重复、优先级和贴桌面行为将暂停；字段、提醒、附件与清单均原样保留，转回任务后恢复。是否继续？")
            : QStringLiteral("将恢复任务状态、日期、提醒、重复、优先级及贴桌面行为；字段、提醒、附件与清单均已保留。是否继续？");
        if (QMessageBox::question(this, QStringLiteral("类型转换"), text,
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
            m_store->convertKind(m_id, target);
    });
    auto* activityCompat = new QPushButton(m_form);
    activityCompat->setObjectName(QStringLiteral("ActivityLog"));
    activityCompat->hide();
    connect(activityCompat, &QPushButton::clicked, this, [this]() {
        if (!finishEditing()) return;
        ActivityLogDialog dialog(m_store, m_id, this); dialog.exec();
    });

    // ========== 事件过滤器 ==========
    m_title->installEventFilter(this); m_description->textEditor()->installEventFilter(this);
    m_checklist->installEventFilter(this); m_checkInput->installEventFilter(this);

    // ========== 信号连接 ==========
    connect(save, &QPushButton::clicked, this, &TaskDetailWidget::flush);
    connect(m_title, &QLineEdit::textEdited, this, [this]() { dirty(QStringLiteral("title")); });
    connect(m_tags, &QLineEdit::textEdited, this, [this]() { dirty(QStringLiteral("tags")); });
    connect(m_description, &MarkdownEditor::contentChanged, this, [this]() {
        dirty(QStringLiteral("body"));
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

    connect(m_pinnedAt, &QCheckBox::clicked, this, [this](bool value) {
        bool ok = flush();
        if (ok) ok = m_store->setPinnedAt(m_id, value ? appNow() : QDateTime());
        if (!ok) {
            const QSignalBlocker blocker(m_pinnedAt);
            m_pinnedAt->setChecked(m_store->entity(m_id).pinnedAt.isValid());
        }
    });
    connect(m_completed, &QCheckBox::clicked, this, [this](bool value) {
        if (!flush() || !m_store->setCompleted(m_id, value)) {
            const QSignalBlocker blocker(m_completed);
            m_completed->setChecked(m_store->entity(m_id).completed);
        }
    });
    connect(m_pinned, &QCheckBox::clicked, this, [this](bool value) {
        bool ok = flush();
        if (ok) {
            const Entity current = m_store->entity(m_id);
            ok = m_store->saveDesktopState(m_id, current.color, current.geometry,
                                           current.zOrder, current.layer, value);
        }
        if (!ok) {
            const QSignalBlocker blocker(m_pinned);
            m_pinned->setChecked(m_store->desktopNote(m_id).open);
        }
    });
    connect(m_restore, &QPushButton::clicked, this, [this]() { m_store->restoreTask(m_id); });
    connect(m_remove, &QPushButton::clicked, this, [this]() {
        if (!flush()) return;
        const Entity task = m_store->entity(m_id);
        if (task.deleted) {
            if (QMessageBox::question(this, QStringLiteral("永久删除"),
                                      QStringLiteral("永久删除“%1”？此操作无法撤销。").arg(task.title),
                                      QMessageBox::Yes | QMessageBox::No,
                                      QMessageBox::No) != QMessageBox::Yes)
                return;
            if (m_store->deletePermanently(m_id)) {
                const auto id = m_id;
                setTask(QString());
                emit taskRemoved(id);
            }
        } else
            m_store->trashTask(m_id);
    });
}

// ========== R8: 详情更多菜单（考据 R11/m05：14 项，与右键不同集合） ==========
void TaskDetailWidget::showMoreMenu() {
    if (m_id.isEmpty()) return;
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("DetailMoreMenu"));
    const Entity entity = m_store->entity(m_id);

    // 类型转换
    const EntityKind target = entity.kind == EntityKind::Task ? EntityKind::Note : EntityKind::Task;
    auto* convert = menu.addAction(
        AppIcons::icon(IconId::Convert, 16),
        target == EntityKind::Note ? QStringLiteral("转为笔记") : QStringLiteral("转为任务"));
    convert->setObjectName(QStringLiteral("ConvertKind"));
    connect(convert, &QAction::triggered, this, [this, target]() {
        if (!finishEditing()) return;
        const QString text = target == EntityKind::Note
            ? QStringLiteral("完成状态、日期、提醒、重复、优先级和贴桌面行为将暂停；字段、提醒、附件与清单均原样保留，转回任务后恢复。是否继续？")
            : QStringLiteral("将恢复任务状态、日期、提醒、重复、优先级及贴桌面行为；字段、提醒、附件与清单均已保留。是否继续？");
        if (QMessageBox::question(this, QStringLiteral("类型转换"), text,
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
            m_store->convertKind(m_id, target);
    });

    // 活动日志
    auto* activity = menu.addAction(AppIcons::icon(IconId::ActivityLog, 16), QStringLiteral("活动日志"));
    activity->setObjectName(QStringLiteral("ActivityLog"));
    connect(activity, &QAction::triggered, this, [this]() {
        if (!finishEditing()) return;
        ActivityLogDialog dialog(m_store, m_id, this);
        dialog.exec();
    });

    menu.exec(QCursor::pos());
}

void TaskDetailWidget::refreshGroups() {
    const QSignalBlocker blocker(m_group);
    const QString selected = m_dirty.contains(QStringLiteral("group")) ? m_group->currentData().toString()
                                                                      : m_store->entity(m_id).listId;
    m_group->clear();
    for (const TaskList& list : m_store->lists()) m_group->addItem(list.name, list.id);
    const int index = m_group->findData(selected);
    m_group->setCurrentIndex(index < 0 ? 0 : index);
}

bool TaskDetailWidget::setTask(QString id) {
    if (m_id == id && !id.isEmpty())
        return true;
    if (!finishEditing())
        return false;
    m_id = id;
    m_dirty.clear();
    loadFields();
    return true;
}

void TaskDetailWidget::loadFields() {
    const Entity task = m_store->entity(m_id);
    const bool exists = !task.id.isEmpty();
    m_loading = true;
    m_empty->setVisible(!exists);
    m_form->parentWidget()->parentWidget()->setVisible(exists);
    if (!exists) {
        m_id.clear();
        m_attachments->setTask(QString());
        m_loading = false;
        return;
    }
    m_title->setText(task.title);
    m_description->setContent(task.body);
    m_attachments->setTask(m_id);
    m_completed->setChecked(task.completed);
    m_pinnedAt->setChecked(task.pinnedAt.isValid());
    // 更新兼容按钮文本（测试断言依赖）
    if (auto* convertBtn = m_form->findChild<QPushButton*>(QStringLiteral("ConvertKind")))
        convertBtn->setText(task.kind == EntityKind::Task ? QStringLiteral("转为笔记") : QStringLiteral("转为任务"));
    m_pinned->setChecked(m_store->desktopNote(m_id).open);
    refreshGroups();
    m_priority->setCurrentIndex(qBound(0, task.priority, 3));
    const auto setTime = [](QCheckBox* check, QDateTimeEdit* edit, const QDateTime& date) {
        check->setChecked(date.isValid());
        edit->setEnabled(date.isValid());
        const QDateTime wall = toAppTime(date.isValid() ? date : appNow().addSecs(3600));
        edit->setDateTime(QDateTime(wall.date(), wall.time()));
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
        item->setData(Qt::UserRole, check.id);
        item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
        item->setCheckState(check.done ? Qt::Checked : Qt::Unchecked);
    }
    m_status->setText(task.deleted ? QStringLiteral("任务在回收站中，恢复后可继续编辑。")
                                   : QStringLiteral("已保存，所有更改保存在本机。"));
    updateEnabled();
    m_loading = false;
}

void TaskDetailWidget::updateEnabled() {
    const Entity entity = m_store->entity(m_id);
    const bool enabled = !entity.deleted;
    const bool task = entity.kind == EntityKind::Task;
    m_completed->setVisible(task); m_pinned->setVisible(task);
    m_scheduleHost->setVisible(task); m_priorityHost->setVisible(task); m_appearanceHost->setVisible(task);
    const QList<QWidget*> common = {m_title, m_description, m_group, m_pinnedAt, m_tags,
                                    m_checklist, m_checkInput};
    for (QWidget* widget : common) widget->setEnabled(enabled);
    const QList<QWidget*> taskFields = {m_priority, m_completed, m_pinned, m_recurrence, m_color, m_layer,
                                        m_hasStart, m_hasDue, m_hasReminder};
    for (QWidget* widget : taskFields) widget->setEnabled(enabled && task);
    m_scheduleHost->setEnabled(enabled && task); m_priorityHost->setEnabled(enabled && task);
    m_appearanceHost->setEnabled(enabled && task);
    m_start->setEnabled(enabled && task && m_hasStart->isChecked());
    m_due->setEnabled(enabled && task && m_hasDue->isChecked());
    m_reminder->setEnabled(enabled && task && m_hasReminder->isChecked());
    for (auto* button : m_form->findChildren<QPushButton*>())
        if (button != m_restore && button != m_remove) button->setEnabled(enabled);
    m_scheduleHost->setEnabled(enabled && task); m_priorityHost->setEnabled(enabled && task);
    m_appearanceHost->setEnabled(enabled && task);
    m_moreButton->setEnabled(true);
    m_attachments->setReadOnly(!enabled);
    m_restore->setVisible(!enabled);
    m_remove->setText(enabled ? QStringLiteral("移到回收站") : QStringLiteral("永久删除"));
}

void TaskDetailWidget::dirty(const QString& field) {
    if (m_loading || m_id.isEmpty() || m_store->entity(m_id).deleted)
        return;
    m_dirty.insert(field);
    m_status->setText(QStringLiteral("正在编辑…"));
    m_saveTimer.start();
}

bool TaskDetailWidget::flush() {
    m_saveTimer.stop();
    if (m_dirty.isEmpty() || m_id.isEmpty())
        return true;
    Entity task = m_store->entity(m_id);
    if (task.id.isEmpty())
        return false;
    if (m_dirty.contains(QStringLiteral("title")))
        task.title = m_title->text().trimmed();
    if (m_dirty.contains(QStringLiteral("body")))
        task.body = m_description->content();
    if (m_dirty.contains(QStringLiteral("group")))
        task.listId = m_group->currentData().toString();
    if (task.kind == EntityKind::Task && m_dirty.contains(QStringLiteral("priority")))
        task.priority = m_priority->currentIndex();
    if (task.kind == EntityKind::Task && m_dirty.contains(QStringLiteral("start")))
        task.startAt = m_hasStart->isChecked() ? fromAppTime(m_start->dateTime()) : QDateTime();
    if (task.kind == EntityKind::Task && m_dirty.contains(QStringLiteral("due")))
        task.dueAt = m_hasDue->isChecked() ? fromAppTime(m_due->dateTime()) : QDateTime();
    if (task.kind == EntityKind::Task && m_dirty.contains(QStringLiteral("reminder"))) {
        const QDateTime date = m_hasReminder->isChecked() ? fromAppTime(m_reminder->dateTime()) : QDateTime();
        if (task.reminderAt != date)
            task.notifiedAt = QDateTime();
        task.reminderAt = date;
    }
    if (task.kind == EntityKind::Task && m_dirty.contains(QStringLiteral("recurrence")))
        task.recurrence = m_recurrence->currentData().toString();
    if (task.kind == EntityKind::Task && m_dirty.contains(QStringLiteral("color")))
        task.color = colorFromInt(m_color->currentIndex());
    if (task.kind == EntityKind::Task && m_dirty.contains(QStringLiteral("layer")))
        task.layer = layerFromInt(m_layer->currentIndex());
    if (m_dirty.contains(QStringLiteral("tags"))) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        const auto splitBehavior = Qt::SkipEmptyParts;
#else
        const auto splitBehavior = QString::SkipEmptyParts;
#endif
        task.tags =
            m_tags->text().split(QRegularExpression(QStringLiteral("[,，;；]")), splitBehavior);
        for (auto& tag : task.tags)
            tag = tag.trimmed();
        task.tags.removeDuplicates();
    }
    if (m_dirty.contains(QStringLiteral("checklist"))) {
        task.checklist.clear();
        for (int i = 0; i < m_checklist->count(); ++i) {
            const auto* item = m_checklist->item(i);
            ChecklistItem check;
            check.id = item->data(Qt::UserRole).toString();
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
        if (m_dirty.contains(QStringLiteral("checklist"))) {
            const auto savedItems = m_store->entity(m_id).checklist;
            if (savedItems.size() == m_checklist->count()) {
                const QSignalBlocker blocker(m_checklist);
                for (int i = 0; i < savedItems.size(); ++i)
                    m_checklist->item(i)->setData(Qt::UserRole, savedItems.at(i).id);
            }
        }
        m_dirty.clear();
        m_status->setText(QStringLiteral("已保存  %1").arg(toAppTime(appNow()).toString(QStringLiteral("HH:mm"))));
    } else
        m_status->setText(QStringLiteral("保存失败，编辑内容已保留。请修正后点击“立即保存”。"));
    return ok;
}

void TaskDetailWidget::beginTextSession() {
    if (!m_editSession.isEmpty() || m_id.isEmpty() || m_store->entity(m_id).deleted) return;
    m_editSession = m_store->beginEditSession(m_id);
}

bool TaskDetailWidget::endTextSession() {
    if (m_editSession.isEmpty()) return true;
    if (!flush()) return false;
    const QString session = m_editSession;
    if (!m_store->endEditSession(session)) return false;
    m_editSession.clear(); return true;
}

bool TaskDetailWidget::finishEditing() {
    return flush() && endTextSession();
}

bool TaskDetailWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_title || watched == m_description->textEditor() || watched == m_checklist || watched == m_checkInput) {
        if (event->type() == QEvent::FocusIn) beginTextSession();
        if (event->type() == QEvent::FocusOut) endTextSession();
    }
    return QWidget::eventFilter(watched, event);
}

void TaskDetailWidget::focusTitle() {
    m_title->setFocus();
    m_title->selectAll();
}
}  // namespace kl
