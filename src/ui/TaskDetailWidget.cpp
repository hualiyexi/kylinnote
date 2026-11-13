#include "TaskDetailWidget.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include "../app/TaskStore.h"
#include "AppIcons.h"
#include "AttachmentWidget.h"
#include "ActivityLogDialog.h"
#include "DatePickerPopup.h"
#include "DetailPopups.h"
#include "FormatToolbar.h"
#include "MarkdownEditor.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {
class InlineChecklistDelegate final : public QStyledItemDelegate {
public:
    explicit InlineChecklistDelegate(QObject* parent) : QStyledItemDelegate(parent) {}
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(28, size.height()));
        return size;
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem text(option);
        initStyleOption(&text, index);
        const bool checked = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
        text.features &= ~QStyleOptionViewItem::HasCheckIndicator;
        text.rect.adjust(22, 0, 0, 0);
        text.palette.setColor(QPalette::Text, ThemeManager::instance().color(
            checked ? ds::ColorToken::TextSecondary : ds::ColorToken::Text));
        QStyle* style = text.widget ? text.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &text, painter, text.widget);
        const QRect box = indicatorRect(option.rect);
        const QColor stroke = ThemeManager::instance().color(
            checked ? ds::ColorToken::TextSecondary : ds::ColorToken::TextTertiary);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(stroke, 1));
        painter->setBrush(checked ? QBrush(stroke) : QBrush(Qt::NoBrush));
        painter->drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), 2, 2);
        if (checked) AppIcons::paint(painter, IconId::Check, box.adjusted(1, 1, -1, -1),
                                    ThemeManager::instance().color(ds::ColorToken::Surface));
        painter->restore();
    }
    bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option,
                     const QModelIndex& index) override {
        if (!(index.flags() & Qt::ItemIsEnabled) || !(index.flags() & Qt::ItemIsUserCheckable)) return false;
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease
            || event->type() == QEvent::MouseButtonDblClick) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() != Qt::LeftButton || !indicatorRect(option.rect).adjusted(-4, -4, 4, 4).contains(mouse->pos()))
                return false;
            if (event->type() == QEvent::MouseButtonRelease)
                model->setData(index, index.data(Qt::CheckStateRole).toInt() == Qt::Checked
                                      ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
            return true;
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
private:
    static QRect indicatorRect(const QRect& row) { return QRect(row.left() + 2, row.center().y() - 6, 13, 13); }
};
} // namespace

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

void TaskDetailWidget::setOverlayMode(bool overlay) {
    m_overlayMode = overlay;
    // 覆盖模式把关闭放在提醒旁；停靠模式保留信息按钮后的常规位置。
    if (auto* header = findChild<QFrame*>(QStringLiteral("DetailHeader"))) {
        if (auto* layout = qobject_cast<QHBoxLayout*>(header->layout())) {
            layout->setContentsMargins(overlay ? 12 : 20, 6, overlay ? 10 : 16, 6);
            layout->removeWidget(m_closeButton);
            layout->insertWidget(overlay ? 2 : layout->count(), m_closeButton);
            layout->invalidate();
            layout->activate();
        }
    }
}

void TaskDetailWidget::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_empty = new QLabel(this);
    m_empty->hide();
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setProperty("muted", true);
    root->addWidget(m_empty, 1);

    // 固定日期栏；标题和正文共用下面的文档滚动区。
    m_detailHost = new QWidget(this);
    m_detailHost->setObjectName(QStringLiteral("TaskDetailHost"));
    m_detailHost->setAttribute(Qt::WA_StyledBackground);
    auto* hostLayout = new QVBoxLayout(m_detailHost);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(0);
    root->addWidget(m_detailHost, 1);

    auto* header = new QFrame(m_detailHost);
    header->setObjectName(QStringLiteral("DetailHeader"));
    header->setFixedHeight(48);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(20, 6, 16, 6);
    headerLayout->setSpacing(2);

    m_dateChip = new QPushButton(header);
    m_dateChip->setObjectName(QStringLiteral("DateChip"));
    m_dateChip->setToolTip(QStringLiteral("设置日期、时间与提醒"));
    m_dateChip->setAccessibleName(QStringLiteral("设置日期"));
    m_dateChip->setCursor(Qt::PointingHandCursor);
    m_dateChip->setMinimumWidth(0);
    m_dateChip->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    connect(m_dateChip, &QPushButton::clicked, this, &TaskDetailWidget::openDatePicker);
    headerLayout->addWidget(m_dateChip);

    m_reminderButton = new QToolButton(header);
    m_reminderButton->setObjectName(QStringLiteral("DetailReminder"));
    m_reminderButton->setFixedSize(26, 26);
    m_reminderButton->setAccessibleName(QStringLiteral("查看提醒"));
    connect(m_reminderButton, &QToolButton::clicked, this, &TaskDetailWidget::openDatePicker);
    headerLayout->addWidget(m_reminderButton);
    headerLayout->addStretch(1);

    m_infoButton = new QToolButton(header);
    m_infoButton->setObjectName(QStringLiteral("DetailInfo"));
    m_infoButton->setFixedSize(28, 28);
    m_infoButton->setToolTip(QStringLiteral("信息"));
    m_infoButton->setAccessibleName(QStringLiteral("信息"));
    m_infoButton->setCursor(Qt::PointingHandCursor);
    connect(m_infoButton, &QToolButton::clicked, this, &TaskDetailWidget::showInfoPopup);
    headerLayout->addWidget(m_infoButton);

    m_closeButton = new QToolButton(header);
    m_closeButton->setObjectName(QStringLiteral("DetailClose"));
    m_closeButton->setFixedSize(28, 28);
    m_closeButton->setAutoRaise(true);
    m_closeButton->setToolTip(QStringLiteral("关闭详情"));
    m_closeButton->setAccessibleName(QStringLiteral("关闭详情"));
    connect(m_closeButton, &QToolButton::clicked, this, [this]() {
        if (finishEditing()) emit closeRequested();
    });
    headerLayout->addWidget(m_closeButton);

    // 保留值控件供字段级保存使用；入口统一放在更多菜单。
    m_completed = new QCheckBox(header);
    m_completed->setObjectName(QStringLiteral("TaskCompleted"));
    m_completed->hide();
    m_pinnedAt = new QCheckBox(header);
    m_pinnedAt->setObjectName(QStringLiteral("TaskPinnedAt"));
    m_pinnedAt->hide();
    m_pinned = new QCheckBox(header);
    m_pinned->setObjectName(QStringLiteral("TaskPinned"));
    m_pinned->hide();
    m_priority = new QComboBox(header);
    m_priority->setObjectName(QStringLiteral("TaskPriority"));
    m_priority->addItems({QStringLiteral("无优先级"), QStringLiteral("低优先级"),
                          QStringLiteral("中优先级"), QStringLiteral("高优先级")});
    m_priority->hide();

    // 所属清单：移到固定底栏左侧（t02/官方旧图），这里保留隐藏 combo 供业务与测试
    m_group = new QComboBox(header);
    m_group->setObjectName(QStringLiteral("TaskGroup"));
    m_group->hide();

    hostLayout->addWidget(header, 0);

    // ========== 正文滚动区（干净富文本 + 子项/检查清单/附件） ==========
    auto* scroll = new QScrollArea(m_detailHost);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setObjectName(QStringLiteral("DetailScroll"));
    m_form = new QWidget(scroll);
    scroll->setWidget(m_form);
    m_form->setObjectName(QStringLiteral("TaskDetailForm"));
    m_form->setAttribute(Qt::WA_StyledBackground);
    m_form->setAutoFillBackground(false);
    scroll->viewport()->setAutoFillBackground(false);
    auto* layout = new QVBoxLayout(m_form);
    layout->setContentsMargins(24, 12, 24, 20);
    layout->setSpacing(4);

    m_title = new QLineEdit(m_form);
    m_title->setObjectName(QStringLiteral("TaskTitle"));
    m_title->setPlaceholderText(QStringLiteral("标题"));
    m_title->setMaxLength(500);
    m_title->setFont(ds::qfont(ds::FontScene::DetailTitle));
    m_title->setFixedHeight(32);
    layout->addWidget(m_title);

    m_description = new MarkdownEditor(m_form);
    m_description->setFlowLayout(true);
    m_description->textEditor()->document()->setDocumentMargin(2);
    layout->addWidget(m_description);
    const auto followCaret = [this, scroll]() {
        QTimer::singleShot(0, this, [this, scroll]() {
            auto* editor = m_description->textEditor();
            if (!editor->hasFocus()) return;
            const QPoint caret = editor->viewport()->mapTo(m_form, editor->cursorRect().center());
            scroll->ensureVisible(caret.x(), caret.y(), 0, 24);
        });
    };
    connect(m_description->textEditor(), &QTextEdit::cursorPositionChanged, this, followCaret);
    connect(m_description, &MarkdownEditor::contentHeightChanged, this, followCaret);
    m_bubble = new SelectionBubble(m_description, m_description->textEditor());
    connect(m_description, &MarkdownEditor::checklistRequested, this, &TaskDetailWidget::addChecklistRow);
    connect(m_description, &MarkdownEditor::attachmentRequested, this, [this]() {
        if (finishEditing()) m_attachments->importFiles();
    });
    connect(m_description, &MarkdownEditor::expandRequested, this, [this]() {
        m_focusMode = !m_focusMode;
        emit focusModeRequested(m_focusMode);
        m_description->textEditor()->setFocus();
    });

    m_checklist = new QListWidget(m_form);
    m_checklist->setObjectName(QStringLiteral("TaskChecklist"));
    m_checklist->setItemDelegate(new InlineChecklistDelegate(m_checklist));
    m_checklist->setFrameShape(QFrame::NoFrame);
    m_checklist->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_checklist->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_checklist->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_checklist->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_checklist->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_checklist);
    m_checkInput = new QLineEdit(m_form);
    m_checkInput->setPlaceholderText(QStringLiteral("添加检查项，回车继续"));
    m_checkInput->setObjectName(QStringLiteral("ChecklistInput"));
    m_checkInput->hide();
    layout->addWidget(m_checkInput);
    auto* addCheck = new QPushButton(QStringLiteral("添加"), m_form);
    addCheck->setObjectName(QStringLiteral("AddChecklistItem"));
    addCheck->hide();
    auto addItem = [this]() {
        const QString text = m_checkInput->text().trimmed();
        if (text.isEmpty()) {
            m_checkInput->hide();
            m_description->textEditor()->setFocus();
            return;
        }
        auto* item = new QListWidgetItem(text, m_checklist);
        item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        m_checkInput->clear();
        updateChecklistHeight();
        dirty(QStringLiteral("checklist"));
    };
    connect(addCheck, &QPushButton::clicked, this, addItem);
    connect(m_checkInput, &QLineEdit::returnPressed, this, addItem);
    connect(m_checklist, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        QFont font = ds::qfont(ds::FontScene::Body);
        font.setStrikeOut(item->checkState() == Qt::Checked);
        {
            const QSignalBlocker blocker(m_checklist);
            item->setFont(font);
        }
        updateChecklistHeight();
        dirty(QStringLiteral("checklist"));
    });
    connect(m_checklist, &QListWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        auto* item = m_checklist->itemAt(point);
        if (!item || !m_checklist->isEnabled()) return;
        QMenu menu(this);
        auto* edit = menu.addAction(QStringLiteral("编辑检查项"));
        connect(edit, &QAction::triggered, this, [this, item]() { m_checklist->editItem(item); });
        auto* add = menu.addAction(QStringLiteral("添加检查项"));
        connect(add, &QAction::triggered, this, &TaskDetailWidget::addChecklistRow);
        menu.addSeparator();
        auto* remove = menu.addAction(AppIcons::icon(IconId::Delete), QStringLiteral("删除检查项"));
        connect(remove, &QAction::triggered, this, [this, item]() {
            delete m_checklist->takeItem(m_checklist->row(item));
            updateChecklistHeight();
            dirty(QStringLiteral("checklist"));
        });
        menu.exec(m_checklist->viewport()->mapToGlobal(point));
    });

    m_attachments = new AttachmentWidget(m_store, m_form);
    m_attachments->setInlineMode(true);
    layout->addWidget(m_attachments);

    // 低频字段收纳区（默认隐藏）：标签、便签外观、以及兼容旧 QDateTimeEdit 的隐藏容器
    m_settingsHost = new QWidget(m_form);
    m_settingsHost->setObjectName(QStringLiteral("AdditionalSettings"));
    auto* settingsLayout = new QVBoxLayout(m_settingsHost);
    settingsLayout->setContentsMargins(0, 6, 0, 0);
    settingsLayout->setSpacing(10);

    m_tags = new QLineEdit(m_settingsHost);
    m_tags->setPlaceholderText(QStringLiteral("标签，用逗号分隔，例如：工作, 本周"));
    settingsLayout->addWidget(m_tags);

    m_appearanceHost = new QWidget(m_settingsHost);
    m_appearanceHost->setObjectName(QStringLiteral("TaskAppearanceHost"));
    auto* appearance = new QFormLayout(m_appearanceHost);
    appearance->setContentsMargins(0, 0, 0, 0);
    m_color = new QComboBox(m_appearanceHost);
    m_color->addItems({QStringLiteral("浅黄"), QStringLiteral("浅蓝"), QStringLiteral("浅绿"),
                       QStringLiteral("浅粉"), QStringLiteral("浅紫"), QStringLiteral("白色")});
    appearance->addRow(QStringLiteral("便签颜色"), m_color);
    m_layer = new QComboBox(m_appearanceHost);
    m_layer->addItems({QStringLiteral("在其他窗口下方"), QStringLiteral("普通窗口"), QStringLiteral("始终置顶")});
    appearance->addRow(QStringLiteral("便签层位"), m_layer);
    settingsLayout->addWidget(m_appearanceHost);

    // 隐藏的兼容容器：三个 QDateTimeEdit / 复选 / 重复 / 优先级占位
    // 数据读写全部走 DatePickerPopup 浮层；这里仅为保留测试 objectName 与 flush() 兼容
    m_scheduleHost = new QWidget(m_settingsHost);
    m_scheduleHost->setObjectName(QStringLiteral("TaskScheduleHost"));
    m_scheduleHost->hide();
    auto* scheduleLayout = new QVBoxLayout(m_scheduleHost);
    scheduleLayout->setContentsMargins(0, 0, 0, 0);
    auto addTime = [this, scheduleLayout](const QString& label, const QString& name,
                                          QCheckBox*& check, QDateTimeEdit*& edit) {
        check = new QCheckBox(label, m_scheduleHost);
        check->setObjectName(name + QStringLiteral("Enabled"));
        const QDateTime defaultTime = toAppTime(appNow()).addSecs(3600);
        edit = new QDateTimeEdit(QDateTime(defaultTime.date(), defaultTime.time()), m_scheduleHost);
        edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
        edit->setCalendarPopup(true);
        edit->setObjectName(name);
        auto* row = new QHBoxLayout;
        row->addWidget(check);
        row->addWidget(edit, 1);
        scheduleLayout->addLayout(row);
        connect(check, &QCheckBox::toggled, this, [this, edit, name](bool enabled) {
            edit->setEnabled(enabled);
            dirty(name);
        });
        connect(edit, &QDateTimeEdit::dateTimeChanged, this, [this, name]() { dirty(name); });
    };
    addTime(QStringLiteral("开始"), QStringLiteral("start"), m_hasStart, m_start);
    addTime(QStringLiteral("截止"), QStringLiteral("due"), m_hasDue, m_due);
    addTime(QStringLiteral("提醒"), QStringLiteral("reminder"), m_hasReminder, m_reminder);
    m_recurrence = new QComboBox(m_scheduleHost);
    m_recurrence->addItem(QStringLiteral("不重复"), QStringLiteral("none"));
    m_recurrence->addItem(QStringLiteral("每天"), QStringLiteral("daily"));
    m_recurrence->addItem(QStringLiteral("每周"), QStringLiteral("weekly"));
    m_recurrence->addItem(QStringLiteral("每月"), QStringLiteral("monthly"));
    scheduleLayout->addWidget(m_recurrence);
    settingsLayout->addWidget(m_scheduleHost);

    m_priorityHost = new QWidget(m_settingsHost);
    m_priorityHost->setObjectName(QStringLiteral("TaskPriorityHost"));
    m_priorityHost->hide();

    layout->addWidget(m_settingsHost);
    m_settingsHost->hide();

    // 保存状态
    m_status = new QLabel(m_form);
    m_status->setObjectName(QStringLiteral("DetailStatus"));
    m_status->setWordWrap(true);
    m_status->hide();
    layout->addWidget(m_status);
    layout->addStretch(1);
    hostLayout->addWidget(scroll, 1);

    // ========== 固定底栏 48px（t02/官方旧图） ==========
    auto* footer = new QFrame(m_detailHost);
    footer->setObjectName(QStringLiteral("DetailFooter"));
    footer->setFixedHeight(48);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(16, 6, 16, 6);
    footerLayout->setSpacing(4);

    // 左：所属清单 chip（点击弹清单选择菜单）
    m_groupChip = new QToolButton(footer);
    m_groupChip->setObjectName(QStringLiteral("GroupChip"));
    m_groupChip->setIcon(AppIcons::icon(IconId::Inbox, 16));
    m_groupChip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_groupChip->setToolTip(QStringLiteral("所属清单"));
    m_groupChip->setAccessibleName(QStringLiteral("所属清单"));
    m_groupChip->setAutoRaise(true);
    m_groupChip->setMinimumWidth(0);
    m_groupChip->setMaximumWidth(180);
    m_groupChip->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_groupChip->setCursor(Qt::PointingHandCursor);
    connect(m_groupChip, &QToolButton::clicked, this, &TaskDetailWidget::showGroupMenu);
    footerLayout->addWidget(m_groupChip);
    footerLayout->addStretch(1);

    // 右：A(格式) / 评论 / ···
    m_formatButton = new QToolButton(footer);
    m_formatButton->setObjectName(QStringLiteral("FooterFormat"));
    m_formatButton->setIcon(AppIcons::icon(IconId::Format, 16));
    m_formatButton->setCheckable(true);
    m_formatButton->setFocusPolicy(Qt::NoFocus);
    m_formatButton->setToolTip(QStringLiteral("文本格式"));
    m_formatButton->setAccessibleName(QStringLiteral("文本格式"));
    m_formatButton->setAutoRaise(true);
    m_formatButton->setFixedSize(32, 32);
    m_formatButton->setCursor(Qt::PointingHandCursor);
    connect(m_formatButton, &QToolButton::clicked, this, &TaskDetailWidget::showFormatPanel);
    footerLayout->addWidget(m_formatButton);

    m_commentButton = new QToolButton(footer);
    m_commentButton->setObjectName(QStringLiteral("FooterComment"));
    m_commentButton->setIcon(AppIcons::icon(IconId::Comment, 16));
    m_commentButton->setToolTip(QStringLiteral("活动日志"));
    m_commentButton->setAccessibleName(QStringLiteral("活动日志"));
    m_commentButton->setAutoRaise(true);
    m_commentButton->setFixedSize(32, 32);
    m_commentButton->setCursor(Qt::PointingHandCursor);
    connect(m_commentButton, &QToolButton::clicked, this, [this]() {
        if (!finishEditing()) return;
        ActivityLogDialog dialog(m_store, m_id, this); dialog.exec();
    });
    footerLayout->addWidget(m_commentButton);

    m_moreButton = new QToolButton(footer);
    m_moreButton->setIcon(AppIcons::icon(IconId::More, 18));
    m_moreButton->setToolTip(QStringLiteral("更多操作"));
    m_moreButton->setAccessibleName(QStringLiteral("详情更多操作"));
    m_moreButton->setObjectName(QStringLiteral("DetailMore"));
    m_moreButton->setAutoRaise(true);
    m_moreButton->setFixedSize(32, 32);
    m_moreButton->setCursor(Qt::PointingHandCursor);
    connect(m_moreButton, &QToolButton::clicked, this, &TaskDetailWidget::showMoreMenu);
    footerLayout->addWidget(m_moreButton);
    hostLayout->addWidget(footer, 0);

    // 隐藏的兼容按钮：测试通过 objectName 查找
    auto* saveCompat = new QPushButton(m_detailHost);
    saveCompat->setObjectName(QStringLiteral("SaveTask"));
    saveCompat->hide();
    connect(saveCompat, &QPushButton::clicked, this, &TaskDetailWidget::flush);
    auto* settingsCompat = new QPushButton(m_detailHost);
    settingsCompat->setObjectName(QStringLiteral("AdditionalSettingsToggle"));
    settingsCompat->hide();
    connect(settingsCompat, &QPushButton::clicked, this, [this]() {
        setSettingsExpanded(m_settingsHost->isHidden());
    });
    auto* convertCompat = new QPushButton(m_detailHost);
    convertCompat->setObjectName(QStringLiteral("ConvertKind"));
    convertCompat->hide();
    connect(convertCompat, &QPushButton::clicked, this, &TaskDetailWidget::showMoreMenu);
    auto* activityCompat = new QPushButton(m_detailHost);
    activityCompat->setObjectName(QStringLiteral("ActivityLog"));
    activityCompat->hide();
    auto* restoreCompat = new QPushButton(m_detailHost);
    restoreCompat->setObjectName(QStringLiteral("RestoreTask"));
    restoreCompat->hide();
    connect(restoreCompat, &QPushButton::clicked, this, [this]() { m_store->restoreTask(m_id); });
    auto* trashCompat = new QPushButton(m_detailHost);
    trashCompat->setObjectName(QStringLiteral("TrashTask"));
    trashCompat->hide();

    // ========== 事件过滤器 ==========
    m_title->installEventFilter(this); m_description->textEditor()->installEventFilter(this);
    m_checklist->installEventFilter(this); m_checkInput->installEventFilter(this);

    // ========== 信号连接 ==========
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
    refreshIcons();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        refreshIcons();
    });
    auto* midnightTimer = new QTimer(this);
    midnightTimer->setInterval(60000);
    connect(midnightTimer, &QTimer::timeout, this, &TaskDetailWidget::updateDateChip);
    midnightTimer->start();
}
void TaskDetailWidget::showMoreMenu() {
    if (m_id.isEmpty()) return;
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("DetailMoreMenu"));
    const Entity entity = m_store->entity(m_id);

    if (entity.kind == EntityKind::Task) {
        auto* complete = menu.addAction(AppIcons::icon(IconId::Done),
            entity.completed ? QStringLiteral("标为未完成") : QStringLiteral("标为已完成"));
        complete->setObjectName(QStringLiteral("CmdToggleCompleted"));
        complete->setEnabled(!entity.deleted);
        connect(complete, &QAction::triggered, this, [this, entity]() {
            if (finishEditing()) m_store->setCompleted(m_id, !entity.completed);
        });
        auto* priority = menu.addMenu(AppIcons::icon(IconId::FlagNone), QStringLiteral("优先级"));
        const IconId flags[] = {IconId::FlagNone, IconId::FlagLow, IconId::FlagMedium, IconId::FlagHigh};
        const ds::ColorToken colors[] = {ds::ColorToken::TextSecondary, ds::ColorToken::PriorityLow,
                                        ds::ColorToken::PriorityMedium, ds::ColorToken::PriorityHigh};
        for (int i = 3; i >= 0; --i) {
            auto* action = priority->addAction(AppIcons::icon(flags[i], colors[i]), m_priority->itemText(i));
            action->setObjectName(QStringLiteral("CmdPriority%1").arg(i));
            action->setCheckable(true);
            action->setChecked(entity.priority == i);
            action->setEnabled(!entity.deleted);
            connect(action, &QAction::triggered, this, [this, i]() {
                if (!finishEditing()) return;
                Entity task = m_store->entity(m_id);
                task.priority = i;
                if (!m_store->saveTask(task)) setStatus(QStringLiteral("优先级保存失败。"), true);
            });
        }
        menu.addSeparator();
    }
    auto* addChecklist = menu.addAction(AppIcons::icon(IconId::Checklist), QStringLiteral("添加检查项"));
    addChecklist->setObjectName(QStringLiteral("AddChecklist"));
    addChecklist->setEnabled(!entity.deleted);
    connect(addChecklist, &QAction::triggered, this, &TaskDetailWidget::addChecklistRow);
    auto* addAttachment = menu.addAction(AppIcons::icon(IconId::Attachment), QStringLiteral("添加附件"));
    addAttachment->setObjectName(QStringLiteral("AddAttachment"));
    addAttachment->setEnabled(!entity.deleted);
    connect(addAttachment, &QAction::triggered, this, [this]() {
        if (finishEditing()) m_attachments->importFiles();
    });
    menu.addSeparator();

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
    menu.addSeparator();

    // 置顶 / 贴桌面（从顶栏收进更多菜单，对齐 g02 顶部只有完成框）
    auto* pin = menu.addAction(AppIcons::icon(IconId::Pin, 16),
        entity.pinnedAt.isValid() ? QStringLiteral("取消置顶") : QStringLiteral("置顶"));
    pin->setObjectName(QStringLiteral("CmdTogglePin"));
    connect(pin, &QAction::triggered, this, [this, entity]() {
        if (flush()) m_store->setPinnedAt(m_id, entity.pinnedAt.isValid() ? QDateTime() : appNow());
    });
    if (entity.kind == EntityKind::Task) {
        const bool open = m_store->desktopNote(m_id).open;
        auto* desktop = menu.addAction(AppIcons::icon(IconId::DesktopNote, 16),
            open ? QStringLiteral("关闭桌面便签") : QStringLiteral("贴到桌面"));
        desktop->setObjectName(QStringLiteral("CmdDesktopNote"));
        connect(desktop, &QAction::triggered, this, [this, open]() {
            if (!flush()) return;
            const Entity cur = m_store->entity(m_id);
            m_store->saveDesktopState(m_id, cur.color, cur.geometry, cur.zOrder, cur.layer, !open);
        });
    }
    menu.addSeparator();
    auto* moreSettings = menu.addAction(AppIcons::icon(IconId::Settings, 16),
        m_settingsHost->isHidden() ? QStringLiteral("显示标签与便签外观") : QStringLiteral("收起标签与便签外观"));
    connect(moreSettings, &QAction::triggered, this, [this]() {
        setSettingsExpanded(m_settingsHost->isHidden());
    });
    menu.addSeparator();
    auto* trash = menu.addAction(AppIcons::icon(IconId::Trash, 16),
        entity.deleted ? QStringLiteral("永久删除") : QStringLiteral("移到回收站"));
    connect(trash, &QAction::triggered, this, [this, entity]() {
        if (!flush()) return;
        if (entity.deleted) {
            if (QMessageBox::question(this, QStringLiteral("永久删除"),
                    QStringLiteral("永久删除“%1”？此操作无法撤销。").arg(entity.title),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) {
                if (m_store->deletePermanently(m_id)) { const auto id = m_id; setTask(QString()); emit taskRemoved(id); }
            }
        } else m_store->trashTask(m_id);
    });
    if (entity.deleted) {
        auto* restore = menu.addAction(AppIcons::icon(IconId::Restore, 16), QStringLiteral("恢复"));
        connect(restore, &QAction::triggered, this, [this]() { m_store->restoreTask(m_id); });
    }

    menu.exec(m_moreButton ? m_moreButton->mapToGlobal(QPoint(0, 0)) : QCursor::pos());
}

void TaskDetailWidget::showFormatPanel() {
    if (!m_formatPanel) {
        m_formatPanel = new FormatPanel(m_description, this);
        m_formatPanel->installEventFilter(this);
    }
    if (m_formatPanel->isVisible()) {
        m_formatPanel->hide();
        return;
    }
    m_formatButton->setChecked(true);
    m_formatPanel->popupAbove(m_formatButton);
}

void TaskDetailWidget::showGroupMenu() {
    if (m_id.isEmpty()) return;
    if (!m_groupPopup) {
        m_groupPopup = new ListSelectPopup(this);
        connect(m_groupPopup, &ListSelectPopup::listSelected, this, [this](const QString& listId) {
            if (!finishEditing()) return;
            Entity task = m_store->entity(m_id);
            if (task.listId == listId) return;
            task.listId = listId;
            task.sectionId.clear();
            if (!m_store->saveTask(task)) {
                setStatus(QStringLiteral("移动失败，原清单及编辑内容已保留。"), true);
                return;
            }
            loadFields();
        });
    }
    m_groupPopup->setLists(m_store->lists(), m_store->folders(), m_store->entity(m_id).listId);
    m_groupPopup->popupAbove(m_groupChip);
}

void TaskDetailWidget::showInfoPopup() {
    if (m_id.isEmpty()) return;
    if (!m_infoPopup) m_infoPopup = new EntityInfoPopup(this);
    QStringList checklist;
    for (int i = 0; i < m_checklist->count(); ++i) checklist.append(m_checklist->item(i)->text());
    m_infoPopup->setDocument(m_store->entity(m_id), m_title->text(),
                            m_description->plainTextContent(), checklist);
    m_infoPopup->popupAt(m_infoButton, false, true);
}

void TaskDetailWidget::addChecklistRow() {
    if (m_id.isEmpty() || m_store->entity(m_id).deleted) return;
    m_checkInput->show();
    m_checkInput->setFocus();
    if (auto* scroll = findChild<QScrollArea*>(QStringLiteral("DetailScroll")))
        scroll->ensureWidgetVisible(m_checkInput);
}

void TaskDetailWidget::updateChecklistHeight() {
    const int count = m_checklist->count();
    m_checklist->setVisible(count > 0);
    if (!count) {
        m_checklist->setFixedHeight(0);
        return;
    }
    m_checklist->doItemsLayout();
    int height = 0;
    for (int i = 0; i < count; ++i) height += qMax(28, m_checklist->sizeHintForRow(i));
    m_checklist->setFixedHeight(height + 2);
}

void TaskDetailWidget::setStatus(const QString& text, bool error) {
    m_status->setText(text);
    m_status->setVisible(error);
    m_infoButton->setToolTip(error ? text : QStringLiteral("信息"));
}

void TaskDetailWidget::refreshIcons() {
    m_infoButton->setIcon(AppIcons::icon(IconId::Info, 16));
    m_closeButton->setIcon(AppIcons::icon(IconId::Close, 15));
    m_reminderButton->setIcon(AppIcons::icon(IconId::Reminder, 14));
    m_formatButton->setIcon(AppIcons::icon(IconId::Format, 16));
    m_commentButton->setIcon(AppIcons::icon(IconId::Comment, 16));
    m_moreButton->setIcon(AppIcons::icon(IconId::More, 16));
    updateDateChip();
    refreshGroups();
}

void TaskDetailWidget::updateDateChip() {
    if (m_id.isEmpty()) return;
    const Entity entity = m_store->entity(m_id);
    const QDateTime effective = entity.dueAt.isValid() ? entity.dueAt : entity.startAt;
    const QDate today = toAppTime(appNow()).date();
    const QDate date = entity.dueAt.isValid() ? toAppTime(entity.dueAt).date()
                      : entity.allDayDate.isValid() ? entity.allDayDate
                      : toAppTime(effective).date();
    const bool active = entity.kind == EntityKind::Task && entity.status == TaskStatus::Active;
    const qint64 days = date.isValid() ? date.daysTo(today) : 0;
    const bool overdue = active && (days > 0 || (entity.dueAt.isValid() && entity.dueAt < appNow()));
    QString text = date.isValid() ? date.toString(QStringLiteral("yyyy/MM/dd")) : QStringLiteral("设置日期");
    if (effective.isValid() && (!entity.allDayDate.isValid() || entity.dueAt.isValid())
        && toAppTime(effective).time() != QTime(0, 0))
        text += toAppTime(effective).toString(QStringLiteral(" HH:mm"));
    if (active && days > 0) text += QStringLiteral(", 延期%1天").arg(days);
    m_dateChip->setText(text);
    m_dateChip->setAccessibleName(text);
    m_dateChip->setProperty("overdue", overdue);
    m_dateChip->setIcon(AppIcons::icon(IconId::Calendar,
        overdue ? ds::ColorToken::Overdue : ds::ColorToken::TextSecondary, 16));
    m_dateChip->style()->unpolish(m_dateChip);
    m_dateChip->style()->polish(m_dateChip);
    m_reminderButton->setVisible(entity.kind == EntityKind::Task && !entity.reminders.isEmpty());
    QStringList reminders;
    for (const TaskReminder& reminder : entity.reminders)
        reminders.append(toAppTime(reminder.triggerAt).toString(QStringLiteral("yyyy/MM/dd HH:mm")));
    m_reminderButton->setToolTip(QStringLiteral("提醒\n") + reminders.join(QLatin1Char('\n')));
}

void TaskDetailWidget::setSettingsExpanded(bool expanded) {
    m_settingsHost->setVisible(expanded);
    updateEnabled();
}

void TaskDetailWidget::openDatePicker() {
    if (m_id.isEmpty()) return;
    if (!flush()) return;
    const Entity entity = m_store->entity(m_id);
    DatePickerPopup* picker = new DatePickerPopup(this);
    const QDateTime initialDate = entity.dueAt.isValid() ? entity.dueAt
        : entity.allDayDate.isValid() ? fromAppTime(QDateTime(entity.allDayDate, QTime(0, 0)))
                                     : entity.startAt;
    picker->setDateTime(initialDate);
    picker->setHasDate(initialDate.isValid());
    picker->setReminder(entity.reminderAt);
    picker->setRecurrence(entity.recurrence);

    // 浮层是临时操作层（01:11）：确定 / 清除 / 取消都在弹层内完成，
    // 关闭后按结果一次性写回，详情区不常驻任何日期编辑控件。
    const auto apply = [this, picker]() {
        Entity updated = m_store->entity(m_id);
        const DatePickerPopup& state = *picker;
        const bool clear = state.cleared();  // const 引用才解析到 getter（与信号同名）
        if (!clear && picker->hasDate()) {
            if (updated.allDayDate.isValid() && !updated.dueAt.isValid()
                && toAppTime(picker->dateTime()).time() == QTime(0, 0)) {
                updated.allDayDate = toAppTime(picker->dateTime()).date();
            } else {
                updated.dueAt = picker->dateTime();
                updated.allDayDate = QDate();
            }
            updated.startAt = QDateTime();
            updated.reminderAt = picker->reminder();
            updated.recurrence = picker->recurrence();
        } else {
            updated.dueAt = QDateTime();
            updated.startAt = QDateTime();
            updated.allDayDate = QDate();
            updated.reminderAt = QDateTime();
            updated.recurrence = QStringLiteral("none");
        }
        if (!m_store->saveTask(updated)) {
            setStatus(QStringLiteral("日期保存失败，编辑内容已保留。"), true);
            return;
        }
        loadFields();
    };
    connect(picker, &QDialog::finished, this, [this, picker, apply](int result) {
        // 确定(Accepted) 或 清除(Rejected 且 cleared()) 都写回；单纯取消不写。
        const DatePickerPopup& state = *picker;
        if (result == QDialog::Accepted || state.cleared()) apply();
    });
    connect(picker, &QDialog::finished, picker, &QObject::deleteLater);
    picker->popupBelow(m_dateChip);
}

void TaskDetailWidget::refreshGroups() {
    const QSignalBlocker blocker(m_group);
    const QString selected = m_dirty.contains(QStringLiteral("group")) ? m_group->currentData().toString()
                                                                      : m_store->entity(m_id).listId;
    m_group->clear();
    for (const TaskList& list : m_store->lists()) m_group->addItem(list.name, list.id);
    const int index = m_group->findData(selected);
    m_group->setCurrentIndex(index < 0 ? 0 : index);
    // 底栏左侧清单 chip 显示当前归属
    if (m_groupChip) {
        QString name;
        IconId icon = IconId::Inbox;
        for (const TaskList& list : m_store->lists()) {
            if (list.id != selected) continue;
            name = list.name;
            icon = list.inbox ? IconId::Inbox : (list.kind == EntityKind::Note ? IconId::Note : IconId::TaskList);
        }
        m_groupChip->setText(name.isEmpty() ? QStringLiteral("收集箱") : name);
        m_groupChip->setIcon(AppIcons::icon(icon, 16));
        m_groupChip->setToolTip(QStringLiteral("所属清单：%1").arg(m_groupChip->text()));
    }
}

bool TaskDetailWidget::setTask(QString id) {
    if (m_id == id && !id.isEmpty())
        return true;
    if (!finishEditing())
        return false;
    m_description->textEditor()->clearFocus();
    if (m_infoPopup) m_infoPopup->hide();
    if (m_groupPopup) m_groupPopup->hide();
    if (m_formatPanel) m_formatPanel->hide();
    for (auto* picker : findChildren<DatePickerPopup*>()) picker->reject();
    m_id = id;
    m_dirty.clear();
    loadFields(true);
    return true;
}

void TaskDetailWidget::loadFields(bool resetEditor) {
    const Entity task = m_store->entity(m_id);
    const bool exists = !task.id.isEmpty();
    m_loading = true;
    m_empty->setVisible(!exists);
    m_detailHost->setVisible(exists);
    if (!exists) {
        m_id.clear();
        m_attachments->setTask(QString());
        m_loading = false;
        return;
    }
    if (m_title->text() != task.title) m_title->setText(task.title);
    if (resetEditor || m_description->content() != task.body) m_description->setContent(task.body);
    m_attachments->setTask(m_id);
    m_completed->setChecked(task.completed);
    m_pinnedAt->setChecked(task.pinnedAt.isValid());
    // 更新兼容按钮文本（测试断言依赖）
    if (auto* convertBtn = findChild<QPushButton*>(QStringLiteral("ConvertKind")))
        convertBtn->setText(task.kind == EntityKind::Task ? QStringLiteral("转为笔记") : QStringLiteral("转为任务"));
    updateDateChip();
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
    m_checkInput->clear();
    m_checkInput->hide();
    for (const auto& check : task.checklist) {
        auto* item = new QListWidgetItem(check.text, m_checklist);
        item->setData(Qt::UserRole, check.id);
        item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
        item->setCheckState(check.done ? Qt::Checked : Qt::Unchecked);
        QFont font = ds::qfont(ds::FontScene::Body);
        font.setStrikeOut(check.done);
        item->setFont(font);
    }
    updateChecklistHeight();
    setStatus(task.deleted ? QStringLiteral("内容在回收站中，恢复后可继续编辑。") : QString(), task.deleted);
    updateEnabled();
    m_loading = false;
}

void TaskDetailWidget::updateEnabled() {
    const Entity entity = m_store->entity(m_id);
    const bool enabled = !entity.deleted;
    const bool task = entity.kind == EntityKind::Task;
    m_completed->hide();
    m_priority->hide();
    m_dateChip->setVisible(task);
    m_dateChip->setEnabled(enabled && task);
    m_scheduleHost->hide();       // 日期/时间/提醒/重复走浮层，兼容容器永不显示
    m_priorityHost->hide();
    m_appearanceHost->setVisible(task && !m_settingsHost->isHidden());
    const QList<QWidget*> common = {m_title, m_description, m_tags, m_checklist, m_checkInput};
    for (QWidget* widget : common) widget->setEnabled(enabled);
    const QList<QWidget*> taskFields = {m_priority, m_completed, m_recurrence, m_color, m_layer,
                                        m_hasStart, m_hasDue, m_hasReminder};
    for (QWidget* widget : taskFields) widget->setEnabled(enabled && task);
    m_reminderButton->setEnabled(enabled && task);
    m_groupChip->setEnabled(enabled);
    m_formatButton->setEnabled(enabled);
    m_commentButton->setEnabled(true);
    m_moreButton->setEnabled(true);
    m_attachments->setReadOnly(!enabled);
}

void TaskDetailWidget::dirty(const QString& field) {
    if (m_loading || m_id.isEmpty() || m_store->entity(m_id).deleted)
        return;
    m_dirty.insert(field);
    setStatus(QStringLiteral("正在编辑…"));
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
    if (m_dirty.contains(QStringLiteral("group"))) {
        const QString listId = m_group->currentData().toString();
        if (task.listId != listId) task.sectionId.clear();
        task.listId = listId;
    }
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
        setStatus(QStringLiteral("尚未保存：标题不能为空。"), true);
        return false;
    }
    if (task.startAt.isValid() && task.dueAt.isValid() && task.dueAt < task.startAt) {
        setStatus(QStringLiteral("尚未保存：截止时间不能早于开始时间。"), true);
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
        setStatus(QStringLiteral("已保存"));
        updateDateChip();
        refreshGroups();
    } else
        setStatus(QStringLiteral("保存失败，编辑内容已保留。修正后按 Ctrl+S 重试。"), true);
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
    if (watched == m_formatPanel && event->type() == QEvent::Hide)
        m_formatButton->setChecked(false);
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (watched == m_title && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)) {
            m_description->textEditor()->setFocus();
            auto cursor = m_description->textEditor()->textCursor();
            cursor.movePosition(QTextCursor::Start);
            m_description->textEditor()->setTextCursor(cursor);
            return true;
        }
        if (watched == m_checkInput && key->key() == Qt::Key_Escape) {
            m_checkInput->clear();
            m_checkInput->hide();
            m_description->textEditor()->setFocus();
            return true;
        }
        if (watched == m_checklist && key->key() == Qt::Key_Delete && m_checklist->isEnabled()) {
            delete m_checklist->takeItem(m_checklist->currentRow());
            updateChecklistHeight();
            dirty(QStringLiteral("checklist"));
            return true;
        }
        if (watched == m_description->textEditor() && m_focusMode && key->key() == Qt::Key_Escape) {
            m_focusMode = false;
            emit focusModeRequested(false);
            return true;
        }
    }
    if (watched == m_title || watched == m_description->textEditor() || watched == m_checklist || watched == m_checkInput) {
        if (event->type() == QEvent::FocusIn) beginTextSession();
        if (event->type() == QEvent::FocusOut) {
            endTextSession();
            if (watched == m_checkInput && m_checkInput->text().isEmpty()) m_checkInput->hide();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void TaskDetailWidget::focusTitle() {
    m_title->setFocus();
    m_title->selectAll();
}

bool TaskDetailWidget::hasDraftInput(QString* what) const {
    // isHidden() 反映控件自身显隐：检查项输入框默认隐藏，仅添加检查项时显示。
    if (m_checkInput && !m_checkInput->isHidden() && !m_checkInput->text().trimmed().isEmpty()) {
        if (what) *what = QStringLiteral("检查项输入框中有未提交内容");
        return true;
    }
    return false;
}
}  // namespace kl
