#include "NoteWidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include "AttachmentWidget.h"
#include "ColorTheme.h"
#include "Markdown.h"
#include "MarkdownEditor.h"
#include "NoteChecklist.h"
#include "ResizeGrip.h"
#include "TaskFilter.h"
#include "TitleBar.h"

namespace kn {

NoteWidget::NoteWidget(const Task& task, QWidget* parent) : QWidget(parent), m_task(task) {
    setObjectName(QStringLiteral("NoteWidget"));
    setAttribute(Qt::WA_StyledBackground, false);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAutoFillBackground(false);
    setMouseTracking(true);
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
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_titleBar = new TitleBar(this);
    root->addWidget(m_titleBar);

    auto* titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(18, 4, 18, 0);
    titleRow->setSpacing(10);
    m_complete = new QCheckBox(this);
    m_complete->setObjectName(QStringLiteral("NoteComplete"));
    m_complete->setToolTip(QStringLiteral("完成任务"));
    m_complete->setAccessibleName(QStringLiteral("完成任务"));
    m_complete->setFixedSize(20, 24);
    titleRow->addWidget(m_complete, 0, Qt::AlignTop);
    m_title = new QLabel(this);
    m_title->setTextFormat(Qt::PlainText);
    m_title->setWordWrap(true);
    m_title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    QFont font = m_title->font();
    font.setPointSize(12);
    font.setWeight(QFont::DemiBold);
    m_title->setFont(font);
    titleRow->addWidget(m_title, 1);
    root->addLayout(titleRow);

    m_body = new QStackedWidget(this);
    m_preview = new DocumentPreview(this);
    m_preview->setObjectName(QStringLiteral("NotePreview"));
    m_preview->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_preview->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    m_preview->viewport()->setAutoFillBackground(false);
    m_preview->setContentsMargins(18, 8, 18, 0);
    m_preview->setStyleSheet(QStringLiteral("QTextBrowser{padding:0;}"));
    m_preview->document()->setDocumentMargin(0);
    m_editor = new MarkdownEditor(this);
    m_editor->setCompact(true);
    m_editor->setMinimumHeight(0);
    m_editor->textEditor()->setMinimumHeight(0);
    m_body->addWidget(m_preview);
    m_body->addWidget(m_editor);
    root->addWidget(m_body, 1);

    m_checklist = new NoteChecklist(this);
    auto* listLayout = new QVBoxLayout;
    listLayout->setContentsMargins(18, 0, 18, 0);
    listLayout->setSpacing(0);
    listLayout->addWidget(m_checklist);
    root->addLayout(listLayout);

    m_attachments = new AttachmentWidget(nullptr, this);
    m_attachments->setCompact(true);
    root->addWidget(m_attachments);

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(18, 0, 16, 12);
    footer->setSpacing(8);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("NoteStatus"));
    m_schedule = new QLabel(this);
    m_schedule->setObjectName(QStringLiteral("NoteSchedule"));
    m_schedule->setTextFormat(Qt::PlainText);
    auto* text = new QVBoxLayout;
    text->setSpacing(2);
    text->addWidget(m_schedule);
    text->addWidget(m_status);
    footer->addLayout(text, 1);
    m_editToggle = new QPushButton(QStringLiteral("编辑"), this);
    m_editToggle->setObjectName(QStringLiteral("NoteEditButton"));
    m_editToggle->setCursor(Qt::PointingHandCursor);
    m_editToggle->setFixedHeight(28);
    footer->addWidget(m_editToggle, 0, Qt::AlignBottom);
    root->addLayout(footer);

    m_grip = new ResizeGrip(this);
    m_titleBar->setHovered(false);
    m_editToggle->setVisible(false);
    m_status->hide();
    m_schedule->hide();
    connect(m_editToggle, &QPushButton::clicked, this, [this]() {
        const bool editing = m_body->currentIndex() == 0;
        if (!editing && !flush())
            return;
        m_body->setCurrentIndex(editing ? 1 : 0);
        m_editToggle->setText(editing ? QStringLiteral("完成") : QStringLiteral("编辑"));
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
    connect(m_checklist, &NoteChecklist::itemToggled, this, [this](int index, bool done) {
        emit checklistToggled(id(), index, done);
    });
}

void NoteWidget::paintEvent(QPaintEvent*) {
    // 整张纸面只有一个绘制者。圆角内不使用多个不透明子背景，缩放时不留下旧帧。
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto spec = ColorTheme::spec(m_task.color);
    const QRectF paper = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(QColor(spec.border), 1));
    painter.setBrush(QColor(spec.body));
    painter.drawRoundedRect(paper, 7, 7);
}

void NoteWidget::updateView() {
    const QSignalBlocker blocker(m_complete);
    m_complete->setChecked(m_task.completed);
    m_title->setText(m_task.title);
    QString schedule = scheduleLabel(m_task);
    if (m_task.reminderAt.isValid())
        schedule += (schedule.isEmpty() ? QString() : QStringLiteral(" · ")) +
                    m_task.reminderAt.toLocalTime().toString(QStringLiteral("M月d日 HH:mm"));
    m_schedule->setText(schedule);
    m_schedule->setVisible(!schedule.isEmpty());
    m_preview->setHtml(Markdown::displayHtml(m_task));
    m_preview->setPlaceholderText(QStringLiteral("写下说明，或从详情添加图片与附件"));
    ColorTheme::apply(this, m_task.color);
    m_titleBar->setColorIndicator(m_task.color);
    m_checklist->setItems(m_task.checklist);
    m_checklist->setInkColor(QColor(ColorTheme::spec(m_task.color).text));
    m_checklist->setVisible(!m_task.checklist.isEmpty());
    setWindowTitle(m_task.title + QStringLiteral(" — KylinNote"));
}

void NoteWidget::setStore(TaskStore* store) {
    m_attachments->setStore(store);
    m_attachments->setTask(id());
}

void NoteWidget::refreshAttachments() {
    m_attachments->refresh();
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
    m_status->setText(failed ? QStringLiteral("保存失败，内容仍在") : QString());
    if (failed) {
        m_status->setStyleSheet(QStringLiteral("color:#A84640;"));
        m_saveTimer.start(SaveRetryMs);
    } else {
        m_status->setStyleSheet(QString());
        m_status->hide();
    }
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
        m_status->setText(QStringLiteral("保存中…"));
        m_status->show();
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
    m_editToggle->setText(QStringLiteral("完成"));
    m_editor->textEditor()->setFocus();
}

void NoteWidget::enterEvent(QEvent* event) {
    m_titleBar->setHovered(true);
    m_editToggle->setVisible(true);
    QWidget::enterEvent(event);
}

void NoteWidget::leaveEvent(QEvent* event) {
    m_titleBar->setHovered(m_body->currentIndex() != 0);
    m_editToggle->setVisible(m_body->currentIndex() != 0);
    QWidget::leaveEvent(event);
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
    // Qt 5/Windows 对无边框工具窗的 dirty-region 更新可能不完整。
    // 同步刷新完整子树，旧尺寸下的文本/按钮不能残留到新窗口表面。
    for (QWidget* child : findChildren<QWidget*>())
        child->update();
    update();
    if (isVisible())
        QApplication::sendPostedEvents(this, QEvent::UpdateRequest);
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
