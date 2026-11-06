#include "NoteWidget.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMoveEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include "ColorTheme.h"
#include "Markdown.h"
#include "MarkdownEditor.h"
#include "ResizeGrip.h"
#include "TaskFilter.h"
#include "TitleBar.h"
namespace kn {
NoteWidget::NoteWidget(const Task& task, QWidget* parent) : QWidget(parent), m_task(task) {
    setObjectName(QStringLiteral("NoteWidget"));
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumSize(MinWidth, MinHeight);
    buildUi();
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(SaveDebounceMs);
    connect(&m_saveTimer, &QTimer::timeout, this, [this]() { emit saveRequested(this); });
    m_loading = true;
    if (task.geometry.isValid())
        setGeometry(task.geometry);
    else
        resize(DefaultWidth, DefaultHeight);
    m_editor->setContent(task.description, task.contentFormat);
    updateView();
    applyWindowFlags();
    m_loading = false;
}
void NoteWidget::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(1, 1, 1, 1);
    root->setSpacing(0);
    m_titleBar = new TitleBar(this);
    root->addWidget(m_titleBar);
    auto* titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(12, 12, 12, 2);
    titleRow->setSpacing(9);
    m_complete = new QCheckBox(this);
    m_complete->setToolTip(QStringLiteral("完成任务"));
    m_complete->setAccessibleName(QStringLiteral("完成任务"));
    titleRow->addWidget(m_complete, 0, Qt::AlignTop);
    m_title = new QLabel(this);
    m_title->setTextFormat(Qt::PlainText);
    m_title->setWordWrap(true);
    QFont f = m_title->font();
    f.setPointSize(12);
    f.setBold(true);
    m_title->setFont(f);
    titleRow->addWidget(m_title, 1);
    root->addLayout(titleRow);
    m_schedule = new QLabel(this);
    m_schedule->setObjectName(QStringLiteral("NoteSchedule"));
    m_schedule->setWordWrap(true);
    m_schedule->setContentsMargins(14, 4, 12, 7);
    root->addWidget(m_schedule);
    m_body = new QStackedWidget(this);
    m_preview = new DocumentPreview(this);
    m_editor = new MarkdownEditor(this);
    m_body->addWidget(m_preview);
    m_body->addWidget(m_editor);
    root->addWidget(m_body, 1);
    m_checklist = new QListWidget(this);
    m_checklist->setMaximumHeight(100);
    m_checklist->setObjectName(QStringLiteral("NoteChecklist"));
    root->addWidget(m_checklist);
    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(8, 3, 24, 6);
    m_status = new QLabel(QStringLiteral("本机已保存"), this);
    m_status->setObjectName(QStringLiteral("NoteStatus"));
    footer->addWidget(m_status, 1);
    m_editToggle = new QPushButton(QStringLiteral("编辑内容"), this);
    footer->addWidget(m_editToggle);
    root->addLayout(footer);
    m_grip = new ResizeGrip(this);
    connect(m_editToggle, &QPushButton::clicked, this, [this]() {
        const bool editing = m_body->currentIndex() == 0;
        if (!editing && !flush())
            return;
        m_body->setCurrentIndex(editing ? 1 : 0);
        m_editToggle->setText(editing ? QStringLiteral("收起编辑") : QStringLiteral("编辑内容"));
        if (editing) {
            m_editor->setPreview(false);
            m_editor->textEditor()->setFocus();
        }
    });
    connect(m_editor, &MarkdownEditor::contentChanged, this, [this]() {
        if (!m_loading) {
            m_descriptionDirty = true;
            scheduleSave();
        }
    });
    connect(m_titleBar, &TitleBar::closeClicked, this, [this]() { emit closeRequested(this); });
    connect(m_titleBar, &TitleBar::colorClicked, this, [this]() {
        m_task.color = ColorTheme::next(m_task.color);
        ColorTheme::apply(this, m_task.color);
        m_titleBar->setColorIndicator(m_task.color);
        scheduleSave();
    });
    connect(m_titleBar, &TitleBar::openClicked, this, [this]() {
        if (flush())
            emit taskOpenRequested(id());
    });
    connect(m_complete, &QCheckBox::clicked, this, [this](bool checked) {
        if (flush())
            emit completionRequested(id(), checked);
        else {
            const QSignalBlocker blocker(m_complete);
            m_complete->setChecked(m_task.completed);
        }
    });
    connect(m_checklist, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (!m_loading)
            emit checklistToggled(id(), m_checklist->row(item), item->checkState() == Qt::Checked);
    });
}
void NoteWidget::updateView() {
    const QSignalBlocker blocker(m_complete);
    m_complete->setChecked(m_task.completed);
    m_title->setText(m_task.title);
    QString schedule = scheduleLabel(m_task);
    if (m_task.reminderAt.isValid())
        schedule += QStringLiteral("\n提醒 ") +
                    m_task.reminderAt.toLocalTime().toString(QStringLiteral("M月d日 HH:mm"));
    m_schedule->setText(schedule);
    m_preview->setHtml(
        m_task.description.isEmpty()
            ? QStringLiteral("<p style=\"color:#64748B\">点击“编辑内容”，为这件事记下说明。</p>")
            : Markdown::displayHtml(m_task));
    ColorTheme::apply(this, m_task.color);
    m_titleBar->setColorIndicator(m_task.color);
    const QSignalBlocker listBlock(m_checklist);
    m_checklist->clear();
    for (const auto& check : m_task.checklist) {
        auto* item = new QListWidgetItem(check.text, m_checklist);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(check.done ? Qt::Checked : Qt::Unchecked);
    }
    m_checklist->setVisible(!m_task.checklist.isEmpty());
    setWindowTitle(m_task.title + QStringLiteral(" — KylinNote"));
}
void NoteWidget::applyTask(const Task& task) {
    const bool changedLayer = m_task.layer != task.layer;
    m_loading = true;
    const QString pendingDescription = m_descriptionDirty ? m_editor->content() : QString();
    m_task = task;
    if (!m_descriptionDirty && m_editor->content() != task.description)
        m_editor->setContent(task.description, task.contentFormat);
    if (m_descriptionDirty)
        m_task.description = pendingDescription;
    updateView();
    if (changedLayer)
        applyWindowFlags();
    m_loading = false;
}
Task NoteWidget::snapshot() const {
    Task task = m_task;
    task.geometry = geometry();
    if (m_descriptionDirty) {
        task.description = m_editor->content();
        task.contentFormat = ContentFormat::Markdown;
    }
    return task;
}
void NoteWidget::applySaved(const Task& task) {
    m_descriptionDirty = false;
    m_pending = false;
    applyTask(task);
    setSaveFailed(false);
}
void NoteWidget::setSaveFailed(bool failed) {
    m_saveFailed = failed;
    m_status->setText(failed ? QStringLiteral("保存失败，内容仍在，请重试") : QStringLiteral("本机已保存"));
    if (failed) {
        m_status->setStyleSheet(QStringLiteral("color:#A73530;"));
        m_saveTimer.start(SaveRetryMs);
    } else
        m_status->setStyleSheet(QString());
}
bool NoteWidget::flush() {
    m_saveTimer.stop();
    if (m_pending)
        emit saveRequested(this);
    return !m_saveFailed;
}
void NoteWidget::scheduleSave() {
    if (!m_loading) {
        m_pending = true;
        m_status->setText(QStringLiteral("正在保存…"));
        m_saveTimer.start(SaveDebounceMs);
    }
}
void NoteWidget::setLayer(NoteLayer layer) {
    m_task.layer = layer;
    applyWindowFlags();
    scheduleSave();
}
NoteLayer NoteWidget::cycleLayer() {
    setLayer(nextLayer(m_task.layer));
    return m_task.layer;
}
void NoteWidget::applyWindowFlags() {
    m_applyingFlags = true;
    const bool visible = isVisible();
    const QRect rect = geometry();
    Qt::WindowFlags flags = Qt::Tool | Qt::FramelessWindowHint;
    if (!m_temporary) {
        if (m_task.layer == NoteLayer::OnTop)
            flags |= Qt::WindowStaysOnTopHint;
        else if (m_task.layer == NoteLayer::OnDesktop)
            flags |= Qt::WindowStaysOnBottomHint;
    }
    setWindowFlags(flags);
    setGeometry(rect);
    if (visible)
        show();
    m_applyingFlags = false;
}
void NoteWidget::raiseTemporarily() {
    m_temporary = true;
    applyWindowFlags();
    show();
    raise();
    activateWindow();
}
void NoteWidget::focusEditor() {
    raiseTemporarily();
    m_body->setCurrentIndex(1);
    m_editToggle->setText(QStringLiteral("收起编辑"));
    m_editor->textEditor()->setFocus();
}
void NoteWidget::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    scheduleSave();
}
void NoteWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (m_grip) {
        m_grip->move(width() - m_grip->width(), height() - m_grip->height());
        m_grip->raise();
    }
    scheduleSave();
}
void NoteWidget::closeEvent(QCloseEvent* event) {
    event->ignore();
    emit closeRequested(this);
}
void NoteWidget::changeEvent(QEvent* event) {
    if (event->type() == QEvent::ActivationChange && isActiveWindow())
        emit activated(this);
    if (event->type() == QEvent::WindowDeactivate && !m_applyingFlags) {
        flush();
        if (m_temporary) {
            m_temporary = false;
            applyWindowFlags();
        }
    }
    QWidget::changeEvent(event);
}
}  // namespace kn
