#include "TaskDetailWidget.h"

#include <QApplication>
#include <QDebug>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QScrollBar>
#include <limits>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QTextEdit>
#include <QTimeZone>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <memory>
#include "../app/TaskStore.h"
#include "../app/EntityTextSession.h"
#include "../domain/ViewSpec.h"
#include "ActivityTimelineWidget.h"
#include "AppIcons.h"
#include "AttachmentWidget.h"
#include "DatePickerPopup.h"
#include "DetailPopups.h"
#include "EntityCommands.h"
#include "FormatToolbar.h"
#include "ListAppearance.h"
#include "MarkdownEditor.h"
#include "ModalDialog.h"
#include "PopupMenu.h"
#include "TaskDateController.h"
#include "TextGeometry.h"
#include "WorkspaceGeometry.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {
// Qt 5.12 QLineEdit 固定保留 2px 文字内缩；用原生 margin API 对齐真实文字原点。
constexpr int kLineEditTextInset = 2;
constexpr int kDetailGutter = 24;
constexpr int kDetailFooterHeight = 48;
constexpr int kMinimumBodyHeight = 120;
constexpr int kAttachmentHeaderHeight = 32;
constexpr int kAttachmentBottomMargin = 8;
constexpr int kAttachmentCardHeight = 56;
constexpr int kAttachmentCardSpacing = 6;
constexpr int kAttachmentVisibleCards = 3;

class CompletionCheckBox final : public QCheckBox {
public:
    explicit CompletionCheckBox(QWidget* parent) : QCheckBox(parent) {}
    bool hitButton(const QPoint& point) const override { return rect().contains(point); }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        // 网页 checkbox 的 17px 字形与 20px date-unselected 有相同的可见墨迹尺寸。
        const QRectF glyph((width() - 17.0) / 2.0, (height() - 17.0) / 2.0, 17.0, 17.0);
        const QColor color = ThemeManager::instance().color(
            isEnabled() ? ds::ColorToken::TextSecondary : ds::ColorToken::TextTertiary);
        const QString icon = isChecked() ? QStringLiteral("completed")
            : (underMouse() ? QStringLiteral("hover-checkbox") : QStringLiteral("checkbox"));
        AppIcons::paint(&painter, icon, glyph, color);
    }
};

class GroupChipButton final : public QToolButton {
public:
    explicit GroupChipButton(QWidget* parent) : QToolButton(parent) {}
    QSize sizeHint() const override {
        ensurePolished();
        const QFontMetrics metrics(font());
        return QSize(12 + iconSize().width() + 4 + metrics.horizontalAdvance(text()),
                     8 + qMax(iconSize().height(), metrics.height()));
    }
    QSize minimumSizeHint() const override { return QSize(0, sizeHint().height()); }
protected:
    void paintEvent(QPaintEvent*) override {
        QStyleOptionToolButton option;
        initStyleOption(&option);
        QStylePainter painter(this);
        QStyleOptionToolButton background = option;
        background.text.clear();
        background.icon = QIcon();
        painter.drawComplexControl(QStyle::CC_ToolButton, background);

        const QRect content = rect().adjusted(6, 4, -6, -4);
        if (content.isEmpty()) return;
        const qreal center = content.top() + content.height() / 2.0;
        QRect glyph(content.left(), qRound(center - iconSize().height() / 2.0),
                    qMin(iconSize().width(), content.width()), iconSize().height());
        QRect label = content.adjusted(iconSize().width() + 4, 0, 0, 0);
        glyph = QStyle::visualRect(layoutDirection(), rect(), glyph);
        label = QStyle::visualRect(layoutDirection(), rect(), label);
        icon().paint(&painter, glyph, Qt::AlignCenter,
                     isEnabled() ? QIcon::Normal : QIcon::Disabled, isChecked() ? QIcon::On : QIcon::Off);
        if (label.isEmpty()) return;
        const QString shown = fontMetrics().elidedText(text(), Qt::ElideRight, label.width());
        const QFontMetricsF metrics(font());
        const qreal x = layoutDirection() == Qt::RightToLeft
            ? label.right() + 1 - metrics.horizontalAdvance(shown) : label.left();
        painter.setFont(font());
        painter.setPen(option.palette.buttonText().color());
        painter.drawText(QPointF(x, textBaselineForCenter(metrics, center, shown)), shown);
    }
};

class DateChipButton final : public QPushButton {
public:
    explicit DateChipButton(QWidget* parent) : QPushButton(parent) {}
    QSize sizeHint() const override {
        ensurePolished();
        return QSize(8 + (icon().isNull() ? 0 : iconSize().width() + 4) + fontMetrics().horizontalAdvance(text()), 30);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QStyleOptionButton option;
        initStyleOption(&option);
        const QIcon glyph = option.icon;
        option.icon = QIcon();
        option.text.clear();
        QStylePainter painter(this);
        painter.drawControl(QStyle::CE_PushButton, option);
        const QRect content = rect().adjusted(4, 0, -4, 0);
        const qreal center = content.top() + content.height() / 2.0;
        const QRect iconRect(content.left(), qRound(center - iconSize().height() / 2.0),
                             iconSize().width(), iconSize().height());
        glyph.paint(&painter, iconRect, Qt::AlignCenter,
                    isEnabled() ? QIcon::Normal : QIcon::Disabled);
        const QRect label = content.adjusted(glyph.isNull() ? 0 : iconSize().width() + 4, 0, 0, 0);
        if (label.isEmpty()) return;
        painter.setFont(font());
        painter.setPen(option.palette.buttonText().color());
        const QFontMetricsF metrics(font());
        const QString shown = metrics.elidedText(text(), Qt::ElideRight, label.width());
        painter.drawText(QPointF(label.left(), textBaselineForCenter(metrics, center, shown)), shown);
    }
};

class AttachmentPane final : public QWidget {
public:
    explicit AttachmentPane(QWidget* parent) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }
    QSize sizeHint() const override { return QSize(QWidget::sizeHint().width(), m_height); }
    QSize minimumSizeHint() const override { return QSize(0, 0); }
    void setPreferredHeight(int height) {
        if (m_height == height && maximumHeight() == height) return;
        m_height = height;
        setMaximumHeight(height);
        updateGeometry();
    }
private:
    int m_height = 0;
};

QFrame* separator(QWidget* parent, const QString& name, int height) {
    auto* host = new QFrame(parent);
    host->setObjectName(name + QStringLiteral("Host"));
    host->setFixedSize(17, height);
    auto* layout = new QHBoxLayout(host);
    layout->setContentsMargins(8, 0, 8, 0);
    layout->setSpacing(0);
    auto* line = new QFrame(host);
    line->setObjectName(name);
    line->setFixedSize(1, height);
    layout->addWidget(line);
    return host;
}

QStringList localFilePaths(const QMimeData* mime) {
    QStringList paths;
    if (!mime || !mime->hasUrls()) return paths;
    for (const QUrl& url : mime->urls())
        if (url.isLocalFile() && !url.toLocalFile().isEmpty()) paths.append(url.toLocalFile());
    return paths;
}
}  // namespace

TaskDetailWidget::TaskDetailWidget(TaskStore* store, QWidget* parent) : QWidget(parent), m_store(store) {
    buildUi();
    m_description->setStore(store);
    connect(m_description, &MarkdownEditor::internalLinkActivated, this, [this](const QString& id) {
        if (finishEditing()) emit relatedDetailRequested(id);
    });
    connect(m_description, &MarkdownEditor::tagSelected, this, &TaskDetailWidget::attachSelectedTag);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget*) {
        scheduleTextSessionEnd();
    });
    connect(m_description, &MarkdownEditor::editFinished, this, [this](bool continuation) {
        if (!m_loading) publishBody(continuation, true);
    });
    connect(store, &TaskStore::taskChanged, this, [this](const QString& id) {
        if (id == m_id) refreshFromStore();
    });
    connect(store, &TaskStore::desktopNoteChanged, this, [this](const QString& id) {
        if (id == m_id) refreshFromStore();
    });
    connect(store, &TaskStore::tasksReset, this, &TaskDetailWidget::refreshFromStore);
    connect(store, &TaskStore::groupsChanged, this, &TaskDetailWidget::refreshGroups);
    connect(store, &QObject::destroyed, this, [this]() {
        const bool pending = hasPendingText();
        m_textSession.clear();
        m_title->setReadOnly(true);
        m_description->setEnabled(false);
        QWidget* controls[] = {m_completed, m_dateChip, m_groupChip, m_formatButton, m_moreButton,
                               m_infoButton, m_attachmentAdd, m_parentLink, m_parentArrow};
        for (QWidget* control : controls) control->setEnabled(false);
        m_attachments->setReadOnly(true);
        if (!pending) {
            const QString target = m_id;
            m_id.clear();
            m_description->setEntityId(QString());
            m_detailHost->hide();
            m_empty->show();
            if (!target.isEmpty()) emit targetUnavailable(target);
        } else setStatus(QStringLiteral("数据源已不可用，未保存的草稿已保留。请先复制草稿。"), true, true);
    });
    // QTextEdit、标题等子控件各自接收拖放；统一过滤到本详情宿主，不改变文本拖放。
    qApp->installEventFilter(this);
    setTask(QString());
}

TaskDetailWidget::~TaskDetailWidget() {
    if (qApp) {
        qApp->removeEventFilter(this);
        disconnect(qApp, nullptr, this, nullptr);
    }
}

void TaskDetailWidget::setOverlayMode(bool overlay) {
    m_overlayMode = overlay;
    auto* header = findChild<QFrame*>(QStringLiteral("DetailHeader"));
    if (header && header->layout())
        header->layout()->setContentsMargins(overlay ? 12 : 20,
            WorkspaceGeometry::DetailHeaderContentTopMargin, overlay ? 12 : 20,
            WorkspaceGeometry::DetailHeaderContentBottomMargin);
    m_closeButton->setVisible(overlay);
    m_headerSeparator->setVisible(overlay);
    updateEnabled();
    scheduleDetailLayout();
}

void TaskDetailWidget::setDesktopMode(bool desktop) {
    if (m_desktopMode == desktop) return;
    m_desktopMode = desktop;
    auto* header = findChild<QFrame*>(QStringLiteral("DetailHeader"));
    auto* footer = findChild<QFrame*>(QStringLiteral("DetailFooter"));
    auto* footerLayout = static_cast<QHBoxLayout*>(footer->layout());
    auto* titleHost = findChild<QWidget*>(QStringLiteral("TaskTitleHost"));
    const int gutter = desktop ? 14 : kDetailGutter;
    titleHost->layout()->setContentsMargins(gutter, 0, gutter, 0);
    m_form->layout()->setContentsMargins(0, desktop ? 0 : 12, 0, 0);
    m_title->setFont(ds::qfont(desktop ? ds::FontScene::ListSection : ds::FontScene::DetailTitle));
    m_title->setFixedHeight(desktop ? 26 : 32);
    m_description->setBlockControlsEnabled(!desktop);
    m_description->layout()->setContentsMargins(desktop ? gutter : 0, 0, gutter, 0);
    m_groupChip->setVisible(!desktop);
    m_formatButton->setVisible(!desktop);
    header->setVisible(!desktop);
    footer->setFixedHeight(desktop ? 26 : kDetailFooterHeight);
    footerLayout->setContentsMargins(desktop ? 10 : 16, desktop ? 0 : 6,
                                     desktop ? 8 : 16, desktop ? 3 : 6);
    if (desktop) {
        header->layout()->removeWidget(m_dateChip);
        m_dateChip->setParent(footer);
        footerLayout->insertWidget(0, m_dateChip);
    } else {
        footerLayout->removeWidget(m_dateChip);
        m_dateChip->setParent(header);
        static_cast<QHBoxLayout*>(header->layout())->insertWidget(3, m_dateChip);
    }
    m_dateChip->setFixedHeight(desktop ? 20 : 30);
    m_dateChip->setIconSize(desktop ? QSize() : QSize(20, 20));
    m_dateChip->setFont(ds::qfont(desktop ? ds::FontScene::Caption : ds::FontScene::Body));
    m_moreButton->setFixedSize(desktop ? QSize(20, 20) : QSize(30, 30));
    m_moreButton->setIconSize(desktop ? QSize(12, 12) : QSize(18, 18));
    QSizePolicy morePolicy = m_moreButton->sizePolicy();
    morePolicy.setRetainSizeWhenHidden(true);
    m_moreButton->setSizePolicy(morePolicy);
    m_attachmentTitle->parentWidget()->setVisible(!desktop);
    m_attachmentDivider->setVisible(!desktop);
    m_attachmentPane->layout()->setContentsMargins(gutter, 0, gutter, desktop ? 4 : kAttachmentBottomMargin);
    setDesktopHovered(m_desktopHovered);
    refreshIcons();
    updateEnabled();
}

void TaskDetailWidget::setDesktopHovered(bool hovered) {
    m_desktopHovered = hovered;
    if (m_desktopMode) m_moreButton->setVisible(hovered);
}

void TaskDetailWidget::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    m_empty = new QLabel(this);
    root->addWidget(m_empty, 1);
    m_detailHost = new QWidget(this);
    m_detailHost->setObjectName(QStringLiteral("TaskDetailHost"));
    m_detailHost->setAttribute(Qt::WA_StyledBackground);
    m_detailHost->setAcceptDrops(true);
    auto* hostLayout = new QVBoxLayout(m_detailHost);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(0);
    root->addWidget(m_detailHost, 1);

    auto* header = new QFrame(m_detailHost);
    header->setObjectName(QStringLiteral("DetailHeader"));
    header->setFixedHeight(WorkspaceGeometry::DetailHeaderHeight);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(20, WorkspaceGeometry::DetailHeaderContentTopMargin,
                                     20, WorkspaceGeometry::DetailHeaderContentBottomMargin);
    headerLayout->setSpacing(2);
    m_closeButton = new QToolButton(header);
    m_closeButton->setObjectName(QStringLiteral("DetailClose"));
    m_closeButton->setFixedSize(28, 30);
    m_closeButton->setAutoRaise(true);
    m_closeButton->setCursor(Qt::PointingHandCursor);
    m_closeButton->setToolTip(QStringLiteral("关闭详情"));
    m_closeButton->setAccessibleName(QStringLiteral("关闭详情"));
    connect(m_closeButton, &QToolButton::clicked, this, [this]() {
        if (finishEditing()) emit closeRequested();
    });
    headerLayout->addWidget(m_closeButton, 0, Qt::AlignVCenter);
    m_closeButton->hide();
    m_completed = new CompletionCheckBox(header);
    m_completed->setObjectName(QStringLiteral("TaskCompleted"));
    m_completed->setFixedSize(30, 30);
    m_completed->setCursor(Qt::PointingHandCursor);
    m_completed->setToolTip(QStringLiteral("完成"));
    m_completed->setAccessibleName(QStringLiteral("完成"));
    headerLayout->addWidget(m_completed, 0, Qt::AlignVCenter);
    m_dateSeparator = separator(header, QStringLiteral("DetailDateSeparator"), 14);
    headerLayout->addWidget(m_dateSeparator, 0, Qt::AlignVCenter);
    m_dateChip = new DateChipButton(header);
    m_dateChip->setObjectName(QStringLiteral("DateChip"));
    m_dateChip->setToolTip(QStringLiteral("设置日期、时间与提醒"));
    m_dateChip->setCursor(Qt::PointingHandCursor);
    m_dateChip->setFixedHeight(30);
    m_dateChip->setIconSize(QSize(20, 20));
    m_dateChip->setMinimumWidth(0);
    m_dateChip->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    connect(m_dateChip, &QPushButton::clicked, this, &TaskDetailWidget::openDatePicker);
    headerLayout->addWidget(m_dateChip, 0, Qt::AlignVCenter);
    headerLayout->addStretch(1);
    m_headerSeparator = separator(header, QStringLiteral("DetailHeaderSeparator"), 20);
    headerLayout->addWidget(m_headerSeparator, 0, Qt::AlignVCenter);
    m_headerSeparator->hide();
    m_infoButton = new QToolButton(header);
    m_infoButton->setObjectName(QStringLiteral("DetailInfo"));
    m_infoButton->setFixedSize(28, 30);
    m_infoButton->setIconSize(QSize(16, 16));
    m_infoButton->setToolTip(QStringLiteral("信息"));
    m_infoButton->setAccessibleName(QStringLiteral("信息"));
    m_infoButton->setCursor(Qt::PointingHandCursor);
    connect(m_infoButton, &QToolButton::clicked, this, [this]() {
        const Entity entity = m_store ? m_store->entity(m_id) : Entity();
        if (entity.kind == EntityKind::Task) showPriorityMenu();
        else showInfoPopup();
    });
    headerLayout->addWidget(m_infoButton, 0, Qt::AlignVCenter);
    hostLayout->addWidget(header);

    m_scroll = new QScrollArea(m_detailHost);
    m_scroll->setObjectName(QStringLiteral("DetailScroll"));
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->viewport()->setAutoFillBackground(false);
    m_form = new QWidget(m_scroll);
    m_form->setObjectName(QStringLiteral("TaskDetailForm"));
    m_form->setAttribute(Qt::WA_StyledBackground);
    m_form->setAutoFillBackground(false);
    m_scroll->setWidget(m_form);
    auto* formLayout = new QVBoxLayout(m_form);
    formLayout->setContentsMargins(0, 12, 0, 0);
    formLayout->setSpacing(4);
    m_parentHost = new QWidget(m_form);
    m_parentHost->setObjectName(QStringLiteral("ParentTaskHost"));
    m_parentHost->setFixedHeight(26);
    auto* parentLayout = new QHBoxLayout(m_parentHost);
    parentLayout->setContentsMargins(kDetailGutter, 0, kDetailGutter, 4);
    parentLayout->setSpacing(0);
    m_parentLink = new QPushButton(m_parentHost);
    m_parentLink->setObjectName(QStringLiteral("ParentTaskLink"));
    m_parentLink->setFont(ds::qfont(ds::FontScene::Menu));
    m_parentLink->setFixedHeight(22);
    m_parentLink->setMaximumWidth(220);
    m_parentLink->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    m_parentLink->setFlat(true);
    m_parentLink->setCursor(Qt::PointingHandCursor);
    const auto openParent = [this]() {
        if (!m_parentId.isEmpty()) emit internalLinkRequested(m_parentId);
    };
    connect(m_parentLink, &QPushButton::clicked, this, openParent);
    parentLayout->addWidget(m_parentLink, 0, Qt::AlignLeft | Qt::AlignVCenter);
    m_parentArrow = new QToolButton(m_parentHost);
    m_parentArrow->setObjectName(QStringLiteral("ParentTaskArrow"));
    m_parentArrow->setFixedSize(12, 22);
    m_parentArrow->setIconSize(QSize(12, 12));
    m_parentArrow->setAutoRaise(true);
    m_parentArrow->setCursor(Qt::PointingHandCursor);
    m_parentArrow->setAccessibleName(QStringLiteral("打开父任务"));
    connect(m_parentArrow, &QToolButton::clicked, this, openParent);
    parentLayout->addWidget(m_parentArrow, 0, Qt::AlignLeft | Qt::AlignVCenter);
    parentLayout->addStretch(1);
    formLayout->addWidget(m_parentHost);
    auto* titleHost = new QWidget(m_form);
    titleHost->setObjectName(QStringLiteral("TaskTitleHost"));
    auto* titleLayout = new QHBoxLayout(titleHost);
    titleLayout->setContentsMargins(kDetailGutter, 0, kDetailGutter, 0);
    titleLayout->setSpacing(0);
    m_title = new QLineEdit(titleHost);
    m_title->setObjectName(QStringLiteral("TaskTitle"));
    m_title->setPlaceholderText(QStringLiteral("标题"));
    m_title->setMaxLength(std::numeric_limits<int>::max());
    m_title->setTextMargins(-kLineEditTextInset, 0, 0, 0);
    m_title->setFont(ds::qfont(ds::FontScene::DetailTitle));
    m_title->setFixedHeight(32);
    titleLayout->addWidget(m_title);
    formLayout->addWidget(titleHost);
    m_description = new MarkdownEditor(m_form);
    m_description->setFlowLayout(true);
    m_description->setFillAvailableHeight(true);
    m_description->setBlockControlsEnabled(true);
    m_description->textEditor()->document()->setDocumentMargin(0);
    // 唯一伸展项就是可编辑的真实 viewport；短正文也不留下不能命中的尾部弹簧。
    formLayout->addWidget(m_description, 1);
    const auto followCaret = [this]() {
        if (m_loading) return;
        QTimer::singleShot(0, this, [this]() {
            if (m_loading) return;
            auto* editor = m_description->textEditor();
            if (!editor->hasFocus()) return;
            const QPoint caret = editor->viewport()->mapTo(m_form, editor->cursorRect().center());
            m_scroll->ensureVisible(caret.x(), caret.y(), 0, 24);
        });
    };
    connect(m_description->textEditor(), &QTextEdit::cursorPositionChanged, this, followCaret);
    connect(m_description, &MarkdownEditor::contentHeightChanged, this, followCaret);
    m_bubble = new SelectionBubble(m_description, m_description->textEditor());
    connect(m_description, &MarkdownEditor::attachmentRequested, this, &TaskDetailWidget::importAttachments);
    connect(m_description, &MarkdownEditor::expandRequested, this, [this]() {
        m_focusMode = !m_focusMode;
        emit focusModeRequested(m_focusMode);
        m_description->textEditor()->setFocus();
    });
    m_activityHost = new QWidget(m_form);
    m_activityHost->setObjectName(QStringLiteral("DetailActivityHost"));
    auto* activityLayout = new QVBoxLayout(m_activityHost);
    activityLayout->setContentsMargins(kDetailGutter, 0, kDetailGutter, 0);
    activityLayout->setSpacing(0);
    m_activity = new ActivityTimelineWidget(m_store, m_activityHost);
    m_activity->hide();
    m_activityHost->hide();
    connect(m_activity, &ActivityTimelineWidget::closeRequested, this, [this]() {
        m_activity->hide();
        updateDetailLayout();
    });
    activityLayout->addWidget(m_activity);
    formLayout->addWidget(m_activityHost);
    hostLayout->addWidget(m_scroll, 1);

    // 校验/只读提示独立于正文滚动，不再挤进 editor 的自然文档高度。
    m_statusHost = new QWidget(m_detailHost);
    m_statusHost->setObjectName(QStringLiteral("DetailStatusHost"));
    auto* statusLayout = new QVBoxLayout(m_statusHost);
    statusLayout->setContentsMargins(kDetailGutter, 0, kDetailGutter, 0);
    statusLayout->setSpacing(0);
    m_status = new QLabel(m_statusHost);
    m_status->setObjectName(QStringLiteral("DetailStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    m_status->hide();
    statusLayout->addWidget(m_status);
    m_recoveryActions = new QWidget(m_statusHost);
    m_recoveryActions->setObjectName(QStringLiteral("DetailRecoveryActions"));
    auto* recoveryLayout = new QHBoxLayout(m_recoveryActions);
    recoveryLayout->setContentsMargins(0, 0, 0, 4);
    recoveryLayout->setSpacing(8);
    m_copyDraft = new QToolButton(m_recoveryActions);
    m_copyDraft->setObjectName(QStringLiteral("DetailCopyDraft"));
    m_copyDraft->setText(QStringLiteral("复制草稿"));
    m_copyDraft->setCursor(Qt::PointingHandCursor);
    connect(m_copyDraft, &QToolButton::clicked, this, &TaskDetailWidget::copyDraft);
    recoveryLayout->addWidget(m_copyDraft);
    m_reloadDraft = new QToolButton(m_recoveryActions);
    m_reloadDraft->setObjectName(QStringLiteral("DetailReloadDraft"));
    m_reloadDraft->setText(QStringLiteral("重新载入"));
    m_reloadDraft->setCursor(Qt::PointingHandCursor);
    connect(m_reloadDraft, &QToolButton::clicked, this, &TaskDetailWidget::reloadDraft);
    recoveryLayout->addWidget(m_reloadDraft);
    recoveryLayout->addStretch(1);
    m_recoveryActions->hide();
    statusLayout->addWidget(m_recoveryActions);
    m_statusHost->hide();
    hostLayout->addWidget(m_statusHost);

    // 附件是 footer 上方的固定底栏，仅卡片列表在溢出时单独滚动。
    m_attachmentPane = new AttachmentPane(m_detailHost);
    m_attachmentPane->setObjectName(QStringLiteral("DetailAttachmentPane"));
    m_attachmentPane->setAttribute(Qt::WA_StyledBackground);
    m_attachmentPane->setAcceptDrops(true);
    auto* attachmentLayout = new QVBoxLayout(m_attachmentPane);
    attachmentLayout->setSizeConstraint(QLayout::SetNoConstraint);
    attachmentLayout->setContentsMargins(kDetailGutter, 0, kDetailGutter, kAttachmentBottomMargin);
    attachmentLayout->setSpacing(0);
    m_attachmentDivider = new QFrame(m_attachmentPane);
    m_attachmentDivider->setObjectName(QStringLiteral("DetailAttachmentDivider"));
    m_attachmentDivider->setFixedHeight(1);
    m_attachmentDivider->setAutoFillBackground(true);
    attachmentLayout->addWidget(m_attachmentDivider);
    auto* attachmentHeader = new QWidget(m_attachmentPane);
    attachmentHeader->setObjectName(QStringLiteral("DetailAttachmentHeader"));
    attachmentHeader->setFixedHeight(kAttachmentHeaderHeight);
    auto* attachmentHeaderLayout = new QHBoxLayout(attachmentHeader);
    attachmentHeaderLayout->setContentsMargins(0, 0, 0, 0);
    attachmentHeaderLayout->setSpacing(6);
    m_attachmentTitle = new QLabel(QStringLiteral("附件"), attachmentHeader);
    m_attachmentTitle->setObjectName(QStringLiteral("DetailAttachmentTitle"));
    m_attachmentTitle->setFont(ds::qfont(ds::FontScene::Menu));
    attachmentHeaderLayout->addWidget(m_attachmentTitle);
    m_attachmentCount = new QLabel(attachmentHeader);
    m_attachmentCount->setObjectName(QStringLiteral("DetailAttachmentCount"));
    m_attachmentCount->setFont(ds::qfont(ds::FontScene::Caption));
    attachmentHeaderLayout->addWidget(m_attachmentCount);
    attachmentHeaderLayout->addStretch(1);
    m_attachmentAdd = new QToolButton(attachmentHeader);
    m_attachmentAdd->setObjectName(QStringLiteral("DetailAttachmentAdd"));
    m_attachmentAdd->setFixedSize(24, 24);
    m_attachmentAdd->setIconSize(QSize(16, 16));
    m_attachmentAdd->setAutoRaise(true);
    m_attachmentAdd->setCursor(Qt::PointingHandCursor);
    m_attachmentAdd->setToolTip(QStringLiteral("添加附件"));
    m_attachmentAdd->setAccessibleName(QStringLiteral("添加附件"));
    connect(m_attachmentAdd, &QToolButton::clicked, this, &TaskDetailWidget::importAttachments);
    attachmentHeaderLayout->addWidget(m_attachmentAdd, 0, Qt::AlignVCenter);
    attachmentLayout->addWidget(attachmentHeader);
    m_attachmentScroll = new QScrollArea(m_attachmentPane);
    m_attachmentScroll->setObjectName(QStringLiteral("DetailAttachmentScroll"));
    m_attachmentScroll->setFrameShape(QFrame::NoFrame);
    m_attachmentScroll->setWidgetResizable(true);
    m_attachmentScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_attachmentScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_attachmentScroll->setMinimumHeight(0);
    m_attachmentScroll->setAcceptDrops(true);
    m_attachmentScroll->viewport()->setAcceptDrops(true);
    m_attachmentScroll->viewport()->setAutoFillBackground(false);
    m_attachments = new AttachmentWidget(m_store, m_attachmentScroll);
    m_attachments->setInlineMode(true);
    m_attachmentScroll->setWidget(m_attachments);
    m_attachments->setAutoFillBackground(false);
    attachmentLayout->addWidget(m_attachmentScroll, 1);
    m_attachmentPane->hide();
    hostLayout->addWidget(m_attachmentPane);
    connect(m_attachments, &AttachmentWidget::contentChanged, this, &TaskDetailWidget::updateDetailLayout);
    connect(m_attachments, &AttachmentWidget::heightChanged, this, &TaskDetailWidget::updateDetailLayout);

    auto* footer = new QFrame(m_detailHost);
    footer->setObjectName(QStringLiteral("DetailFooter"));
    footer->setFixedHeight(kDetailFooterHeight);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(16, 6, 16, 6);
    footerLayout->setSpacing(4);
    m_groupChip = new GroupChipButton(footer);
    m_groupChip->setObjectName(QStringLiteral("GroupChip"));
    m_groupChip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_groupChip->setIconSize(QSize(18, 18));
    m_groupChip->setAccessibleName(QStringLiteral("所属清单"));
    m_groupChip->setAutoRaise(true);
    m_groupChip->setMinimumWidth(0);
    m_groupChip->setMaximumWidth(180);
    m_groupChip->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_groupChip->setCursor(Qt::PointingHandCursor);
    connect(m_groupChip, &QToolButton::clicked, this, &TaskDetailWidget::showGroupMenu);
    footerLayout->addWidget(m_groupChip);
    footerLayout->addStretch(1);
    const auto footerButton = [footer, footerLayout](const QString& name, const QString& label) {
        auto* button = new QToolButton(footer);
        button->setObjectName(name);
        button->setAccessibleName(label);
        button->setToolTip(label);
        button->setAutoRaise(true);
        button->setFixedSize(30, 30);
        button->setIconSize(QSize(18, 18));
        button->setCursor(Qt::PointingHandCursor);
        footerLayout->addWidget(button);
        return button;
    };
    m_formatButton = footerButton(QStringLiteral("FooterFormat"), QStringLiteral("文本格式"));
    m_formatButton->setCheckable(true);
    m_formatButton->setFocusPolicy(Qt::NoFocus);
    connect(m_formatButton, &QToolButton::clicked, this, &TaskDetailWidget::showFormatPanel);
    m_moreButton = footerButton(QStringLiteral("DetailMore"), QStringLiteral("详情更多操作"));
    connect(m_moreButton, &QToolButton::clicked, this, &TaskDetailWidget::showMoreMenu);
    hostLayout->addWidget(footer);
    m_dropHint = new QLabel(QStringLiteral("松开以添加附件"), m_detailHost);
    m_dropHint->setObjectName(QStringLiteral("AttachmentDropHint"));
    m_dropHint->setAlignment(Qt::AlignCenter);
    m_dropHint->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_dropHint->hide();

    m_commands = new EntityCommands(m_store, this);
    connect(m_commands, &EntityCommands::entityCreated, this, &TaskDetailWidget::internalLinkRequested);
    connect(m_commands, &EntityCommands::errorOccurred, this, [this](const QString& error) {
        setStatus(error, true);
    });
    connect(m_commands, &EntityCommands::commandExecuted, this,
            [this](EntityCommand command, const QString& id) {
        if (command == EntityCommand::Delete || command == EntityCommand::DeletePermanently) {
            emit taskRemoved(id);
            return;
        }
        if (id != m_id) return;
        if (command == EntityCommand::ViewActivity) showActivity();
        else if (command == EntityCommand::UploadAttachment) importAttachments();
    });
    connect(m_title, &QLineEdit::textEdited, this, [this]() { dirty(QStringLiteral("title")); });
    connect(m_description, &MarkdownEditor::contentChanged, this, [this]() {
        dirty(QStringLiteral("body"));
        updateDetailLayout();
    });
    connect(m_completed, &QCheckBox::clicked, this, [this](bool value) {
        if (!finishEditing() || !m_store->setCompleted(m_id, value)) {
            const QSignalBlocker blocker(m_completed);
            m_completed->setChecked(m_store->entity(m_id).completed);
        }
    });
    refreshIcons();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, &TaskDetailWidget::refreshIcons);
    auto* midnightTimer = new QTimer(this);
    midnightTimer->setInterval(60000);
    connect(midnightTimer, &QTimer::timeout, this, &TaskDetailWidget::updateDateChip);
    midnightTimer->start();
}

void TaskDetailWidget::showMoreMenu() {
    if (m_desktopMode) { emit desktopMoreRequested(); return; }
    if (m_id.isEmpty() || !finishEditing()) return;
    const Entity entity = m_store->entity(m_id);
    MenuContext context;
    context.kind = entity.kind;
    context.completed = entity.status == TaskStatus::Completed;
    context.abandoned = entity.status == TaskStatus::Abandoned;
    context.deleted = entity.deleted;
    context.pinned = entity.pinnedAt.isValid();
    std::unique_ptr<QMenu> menu(m_commands->buildDetailMoreMenu(context, m_id, this));
    menu->setObjectName(QStringLiteral("DetailMoreMenu"));
    const int margin = PopupMenu::shadowMargin();
    const QSize card = menu->sizeHint() - QSize(2 * margin, 2 * margin);
    const QPoint anchor = m_moreButton->mapToGlobal(QPoint(m_moreButton->width(), 0));
    menu->exec(anchor - QPoint(card.width(), card.height() + 4));
}

void TaskDetailWidget::showFormatPanel() {
    if (!m_formatPanel) m_formatPanel = new FormatPanel(m_description, this);
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
            if (!m_store->moveEntityToList(m_id, listId)) {
                setStatus(QStringLiteral("移动失败，原清单及编辑内容已保留。"), true);
                return;
            }
            loadFields();
        });
    }
    m_groupPopup->setLists(m_store->lists(), m_store->folders(), m_store->entity(m_id).listId);
    m_groupPopup->popupAbove(m_groupChip);
}

void TaskDetailWidget::showPriorityMenu() {
    if (!m_store || m_id.isEmpty() || !finishEditing()) return;
    const Entity entity = m_store->entity(m_id);
    if (entity.kind != EntityKind::Task || entity.deleted) return;
    std::unique_ptr<QMenu> menu(m_commands->buildPriorityMenu(m_id, this));
    const QPoint bottom = m_infoButton->mapToGlobal(QPoint(0, m_infoButton->height()));
    menu->exec(QPoint(bottom.x() + m_infoButton->width() - 164, bottom.y() + 4));
    refreshIcons();
}

void TaskDetailWidget::showInfoPopup() {
    if (m_id.isEmpty()) return;
    if (!m_infoPopup) m_infoPopup = new EntityInfoPopup(this);
    m_infoPopup->setDocument(m_store->entity(m_id), m_title->text(), m_description->plainTextContent());
    m_infoPopup->popupAt(m_infoButton, false, true);
}

void TaskDetailWidget::importAttachments() {
    const QPointer<TaskStore> store = m_store;
    const QString target = m_id;
    if (!store || target.isEmpty() || store->entity(target).id.isEmpty()
        || store->entity(target).deleted || !finishEditing()) return;
    if (!store || store != m_store || target != m_id || store->entity(target).id.isEmpty()
        || store->entity(target).deleted) return;
    m_attachments->importFiles();
}

bool TaskDetailWidget::showActivity() {
    if (m_id.isEmpty() || !finishEditing()) return false;
    m_activity->setEntityId(m_id);
    m_activity->refresh();
    m_activity->show();
    updateDetailLayout();
    QTimer::singleShot(0, this, [this]() { m_scroll->ensureWidgetVisible(m_activity, 0, 12); });
    return true;
}

void TaskDetailWidget::setStatus(const QString& text, bool error, bool recoverable) {
    // refreshIcons() 会把任务按钮提示恢复为“优先级”；这里必须使用同一个正常值，
    // 否则每次键入都无法通过幂等判断，再次刷新整页并嵌套触发 UIA/输入法事件。
    const bool task = m_infoButton->objectName() == QLatin1String("DetailPriority");
    const QString normalToolTip = task ? QStringLiteral("优先级") : QStringLiteral("信息");
    const QString toolTip = error ? text : normalToolTip;
    if (m_status->text() == text && m_status->isVisible() == error
        && m_recoveryActions->isVisible() == (error && recoverable)
        && m_statusHost->isVisible() == error
        && m_infoButton->toolTip() == toolTip)
        return;
    m_status->setText(text);
    m_status->setVisible(error);
    m_recoveryActions->setVisible(error && recoverable);
    m_statusHost->setVisible(error);
    m_infoButton->setToolTip(toolTip);
    // 状态条变化不需要重铺日期、清单、父任务和全部图标；那会在文本输入
    // 与 Windows 可达性同步回调之间形成递归。主题和实体命令仍显式刷新图标。
    updateDetailLayout();
    scheduleDetailLayout();
}

void TaskDetailWidget::refreshIcons() {
    const Entity entity = m_store ? m_store->entity(m_id) : Entity();
    const bool task = entity.kind == EntityKind::Task;
    m_infoButton->setObjectName(task ? QStringLiteral("DetailPriority") : QStringLiteral("DetailInfo"));
    m_infoButton->setAccessibleName(task ? QStringLiteral("设置优先级") : QStringLiteral("信息"));
    m_infoButton->setToolTip(m_statusHost->isVisible() ? m_status->text()
        : task ? QStringLiteral("优先级") : QStringLiteral("信息"));
    const IconId priorityIcon = entity.priority == 3 ? IconId::FlagHigh
        : entity.priority == 2 ? IconId::FlagMedium
        : entity.priority == 1 ? IconId::FlagLow : IconId::FlagNone;
    m_infoButton->setIcon(AppIcons::icon(task ? priorityIcon : IconId::Info, task ? 18 : 16));
    m_infoButton->style()->unpolish(m_infoButton);
    m_infoButton->style()->polish(m_infoButton);
    m_closeButton->setIcon(AppIcons::icon(IconId::Close, 15));
    m_formatButton->setIcon(AppIcons::icon(IconId::Format, 18));
    m_moreButton->setIcon(AppIcons::icon(IconId::MoreTitlebar, 18));
    m_attachmentAdd->setIcon(AppIcons::icon(IconId::Add, ds::ColorToken::TextSecondary, 16));
    auto& theme = ThemeManager::instance();
    QPalette dividerPalette = m_attachmentDivider->palette();
    dividerPalette.setColor(QPalette::Window, theme.color(ds::ColorToken::DetailDivider));
    m_attachmentDivider->setPalette(dividerPalette);
    QPalette titlePalette = m_attachmentTitle->palette();
    titlePalette.setColor(QPalette::WindowText, theme.color(ds::ColorToken::TextSecondary));
    m_attachmentTitle->setPalette(titlePalette);
    QPalette countPalette = m_attachmentCount->palette();
    countPalette.setColor(QPalette::WindowText, theme.color(ds::ColorToken::TextTertiary));
    m_attachmentCount->setPalette(countPalette);
    updateDateChip();
    refreshGroups();
    updateParentLink();
}

void TaskDetailWidget::updateDateChip() {
    if (m_id.isEmpty() || !m_store) return;
    const Entity entity = m_store->entity(m_id);
    const QDateTime timed = entity.hasScheduleRange ? entity.startAt : entity.dueAt;
    const QDate today = toAppTime(appNow()).date();
    const QDate date = effectiveDate(entity);
    const bool active = !entity.deleted
        && (entity.kind == EntityKind::Note || entity.status == TaskStatus::Active);
    const qint64 days = date.isValid() ? date.daysTo(today) : 0;
    const bool overdue = active && (days > 0 || (entity.dueAt.isValid() && entity.dueAt < appNow()));
    const bool scheduled = date.isValid();
    QString text = scheduled ? date.toString(QStringLiteral("yyyy/MM/dd")) : QStringLiteral("设置日期");
    if (entity.hasScheduleRange) {
        const QDate startDate = entity.allDayDate.isValid() ? entity.allDayDate
            : entity.startAt.toTimeZone(QTimeZone(entity.timeZone.toUtf8())).date();
        const QDate endDate = entity.allDayDate.isValid() ? entity.allDayEndDate
            : entity.dueAt.toTimeZone(QTimeZone(entity.timeZone.toUtf8())).date();
        const QString endFormat = endDate.year() == startDate.year()
            ? QStringLiteral("MM/dd") : QStringLiteral("yyyy/MM/dd");
        text = startDate.toString(QStringLiteral("yyyy/MM/dd")) + QStringLiteral(" – ")
            + endDate.toString(endFormat);
        if (!entity.allDayDate.isValid()) {
            const QTimeZone zone(entity.timeZone.toUtf8());
            text += QStringLiteral(" %1–%2").arg(entity.startAt.toTimeZone(zone).toString(QStringLiteral("HH:mm")),
                                                 entity.dueAt.toTimeZone(zone).toString(QStringLiteral("HH:mm")));
        }
    } else if (timed.isValid()) {
        text += timed.toTimeZone(QTimeZone(entity.timeZone.toUtf8())).toString(QStringLiteral(" HH:mm"));
    }
    if (active && days > 0 && entity.kind == EntityKind::Task)
        text += QStringLiteral(", 延期%1天").arg(days);
    if (m_desktopMode) {
        if (scheduled && !entity.hasScheduleRange && date.year() == today.year()) {
            text = date.toString(QStringLiteral("M月d日"));
            if (timed.isValid()) text += timed.toTimeZone(QTimeZone(entity.timeZone.toUtf8())).toString(QStringLiteral(" HH:mm"));
            if (days > 0 && active) text += QStringLiteral(", 延期%1天").arg(days);
            else if (days == 0) text += QStringLiteral(", 今天");
            else if (days == -1) text += QStringLiteral(", 明天");
        }
        m_dateChip->setIcon(QIcon());
    } else m_dateChip->setIcon(AppIcons::icon(IconId::DetailCalendar,
        !scheduled ? ds::ColorToken::TextSecondary
        : overdue ? ds::ColorToken::Overdue : ds::ColorToken::Info, 20));
    m_dateChip->setText(text);
    m_dateChip->setAccessibleName(text);
    m_dateChip->setProperty("overdue", overdue);
    m_dateChip->setProperty("hasSchedule", scheduled && !overdue);
    m_dateChip->style()->unpolish(m_dateChip);
    m_dateChip->style()->polish(m_dateChip);
}

void TaskDetailWidget::updateParentLink() {
    if (!m_store || !m_parentHost) return;
    const Entity current = m_store->entity(m_id);
    const Entity parent = m_store->entity(current.parentId);
    m_parentId = parent.id;
    m_parentHost->setVisible(!m_desktopMode && !m_parentId.isEmpty());
    if (m_parentId.isEmpty()) {
        m_parentLink->clearFocus();
        m_parentLink->setText(QString());
        m_parentArrow->setIcon(QIcon());
        return;
    }
    m_parentLink->setToolTip(parent.title);
    m_parentLink->setAccessibleName(QStringLiteral("父任务：") + parent.title);
    m_parentLink->setText(m_parentLink->fontMetrics().elidedText(parent.title, Qt::ElideRight, 220));
    m_parentArrow->setIcon(AppIcons::icon(QStringLiteral("chevron-right"), ds::ColorToken::TextTertiary, 12));
}

void TaskDetailWidget::openDatePicker() {
    if (m_id.isEmpty() || !finishEditing()) return;
    if (!m_dateController) {
        m_dateController = new TaskDateController(m_store, this);
        connect(m_dateController, &TaskDateController::saved, this, [this](const QString& id) {
            if (m_id == id) loadFields();
        });
        connect(m_dateController, &TaskDateController::saveFailed, this,
                [this](const QString& message) { setStatus(message, true); });
    }
    m_dateController->open(m_id, QRect(m_dateChip->mapToGlobal(QPoint()), m_dateChip->size()));
}

void TaskDetailWidget::refreshGroups() {
    if (!m_store || !m_groupChip) return;
    const QString selected = m_store->entity(m_id).listId;
    TaskList selectedList;
    for (const TaskList& list : m_store->lists()) if (list.id == selected) selectedList = list;
    m_groupChip->setText(selectedList.name.isEmpty() ? QStringLiteral("收集箱") : selectedList.name);
    if (selectedList.name.isEmpty()) selectedList.inbox = true;
    m_groupChip->setIcon(ListAppearance::icon(selectedList, 18,
        ThemeManager::instance().color(ds::ColorToken::TextSecondary)));
    m_groupChip->setToolTip(QStringLiteral("所属清单：%1").arg(m_groupChip->text()));
}

bool TaskDetailWidget::setTask(QString id) {
    if (m_id == id && !id.isEmpty()) return true;
    if (!finishEditing()) return false;
    m_description->textEditor()->clearFocus();
    if (m_infoPopup) m_infoPopup->hide();
    if (m_groupPopup) m_groupPopup->hide();
    if (m_formatPanel) m_formatPanel->hide();
    for (auto* picker : findChildren<DatePickerPopup*>()) picker->reject();
    m_activity->hide();
    m_activity->setEntityId(id);
    setDropHintVisible(false);
    m_id = id;
    m_dirty.clear();
    m_bodyPublishPending = false;
    m_bodyConflict = false;
    m_pendingBodyChanges.clear();
    bindTextSession();
    return loadFields(true);
}

bool TaskDetailWidget::loadFields(bool resetEditor) {
    if (m_loading || m_saving) return false;
    const QString target = m_id;
    const Entity task = m_store ? m_store->entity(target) : Entity();
    const bool exists = !task.id.isEmpty();
    if (exists && !task.deleted && !m_textSession) {
        bindTextSession();
        resetEditor = true;
    }
    m_loading = true;
    m_empty->setVisible(!exists);
    m_detailHost->setVisible(exists);
    m_remoteRefreshPending = false;
    if (!exists) {
        if (hasPendingText()) {
            m_detailHost->show();
            m_empty->hide();
            m_loading = false;
            updateEnabled();
            refreshTextState();
            return false;
        }
        const QPointer<TaskDetailWidget> guard(this);
        if (m_textSession && !m_textSession->finishEditing()) {
            if (!guard) return false;
            m_loading = false;
            refreshTextState();
            return false;
        }
        if (!guard) return false;
        if (m_textSession) disconnect(m_textSession, nullptr, this, nullptr);
        m_textSession.clear();
        m_liveBody.clear();
        m_lastSessionBody.clear();
        m_pendingBodyChanges.clear();
        m_bodyPublishPending = false;
        m_dirty.clear();
        m_id.clear();
        m_title->clear();
        m_description->setEntityId(QString());
        m_description->setContent(QString());
        m_attachments->setTask(QString());
        m_activity->setEntityId(QString());
        m_activity->hide();
        refreshIcons();
        setStatus(QString());
        m_loading = false;
        if (!target.isEmpty()) emit targetUnavailable(target);
        return true;
    }
    m_description->setEntityId(task.id);
    if (!m_textSession && m_title->text() != task.title) m_title->setText(task.title);
    if (resetEditor) {
        m_liveBody = m_textSession ? m_textSession->body() : task.body;
        m_lastSessionBody = m_liveBody;
        m_description->setContent(m_liveBody);
    }
    m_attachments->setTask(m_id);
    m_completed->setChecked(task.completed);
    refreshIcons();
    m_loading = false;
    refreshLiveText();
    updateEnabled();
    refreshTextState();
    return true;
}

void TaskDetailWidget::refreshFromStore() {
    m_remoteRefreshPending = true;
    if (!m_loading && !m_saving) loadFields();
}

void TaskDetailWidget::bindTextSession() {
    if (m_textSession) disconnect(m_textSession, nullptr, this, nullptr);
    const Entity task = m_store ? m_store->entity(m_id) : Entity();
    m_textSession = !task.id.isEmpty() && !task.deleted ? m_store->textSession(m_id) : nullptr;
    m_description->setSharedHistory(
        [this](bool redo) {
            if (!m_textSession || !publishBody()) return;
            if (redo) m_textSession->redoBody(this);
            else m_textSession->undoBody(this);
        },
        [this](bool redo) {
            return m_textSession && (redo ? m_textSession->canRedoBody() : m_textSession->canUndoBody());
        });
    if (!m_textSession) return;
    connect(m_textSession, &EntityTextSession::textChanged, this, &TaskDetailWidget::refreshLiveText);
    connect(m_textSession, &EntityTextSession::stateChanged, this, &TaskDetailWidget::refreshTextState);
}

void TaskDetailWidget::refreshLiveText(QObject* origin) {
    if (!m_textSession || m_loading) return;
    const QPointer<TaskDetailWidget> guard(this);
    const QString title = m_textSession->title();
    const QString body = m_textSession->body();
    const SourceChange bodyChange = sourceChange(m_lastSessionBody, body);
    m_lastSessionBody = body;
    const bool composing = m_description->isComposing();
    const bool deferBody = (origin != this && (composing || m_bodyPublishPending || m_bodyConflict))
        || (composing && body != m_description->content());
    if (deferBody && !bodyChange.isEmpty()) m_pendingBodyChanges.append(bodyChange);
    const bool loading = m_loading;
    m_loading = true;
    if (m_title->text() != title) {
        const SourceChange change = sourceChange(m_title->text(), title);
        const int position = m_title->cursorPosition();
        const int selected = m_title->selectionStart();
        const int length = m_title->selectedText().size();
        const QSignalBlocker signalBlocker(m_title);
        m_title->setText(title);
        if (selected >= 0) {
            const int anchor = position == selected ? selected + length : selected;
            const auto selection = sourceSelectionAfter(change, anchor, position);
            m_title->setSelection(selection.first, selection.second - selection.first);
        } else m_title->setCursorPosition(sourcePositionAfter(change, position));
    }
    if (!deferBody) {
        const int scroll = m_scroll->verticalScrollBar()->value();
        m_description->applyLiveContent(body);
        if (!guard) return;
        m_scroll->verticalScrollBar()->setValue(scroll);
        m_liveBody = body;
        m_pendingBodyChanges.clear();
    }
    m_loading = loading;
    emit draftStateChanged();
}

bool TaskDetailWidget::publishBody(bool continuation, bool committed) {
    if (m_loading) return true;
    if (!m_textSession) return !m_bodyPublishPending;
    if (m_description->isComposing() && !committed) return true;
    m_bodyPublishPending = false;
    SourceChange local = sourceChange(m_liveBody, m_description->content());
    for (const SourceChange& remote : m_pendingBodyChanges) {
        if (!rebaseSourceChange(&local, remote)) {
            m_bodyConflict = true;
            setStatus(QStringLiteral("输入期间同一段内容发生了重叠修改，草稿已保留。可复制草稿，或重新载入。"), true, true);
            return false;
        }
    }
    const QString next = local.isEmpty() ? m_textSession->body() : applySourceChange(m_textSession->body(), local);
    const QPointer<TaskDetailWidget> guard(this);
    const QPointer<EntityTextSession> session = m_textSession;
    const QString target = m_id;
    const QString previousBody = m_liveBody;
    m_bodyConflict = false;
    m_liveBody = next;
    const bool ok = session->updateBody(next, this, continuation);
    if (!guard || !session || session != m_textSession || target != m_id) return false;
    if (ok) {
        m_dirty.remove(QStringLiteral("unpublished"));
        m_pendingBodyChanges.clear();
        if (m_description->isComposing() && session->body() != m_description->content()) {
            m_liveBody = m_description->content();
            m_lastSessionBody = session->body();
            m_pendingBodyChanges.append(sourceChange(m_liveBody, m_lastSessionBody));
        } else refreshLiveText(this);
        if (!guard) return false;
        refreshTextState();
    } else {
        m_liveBody = previousBody;
        m_dirty.insert(QStringLiteral("unpublished"));
        m_bodyConflict = true;
        setStatus(QStringLiteral("修改尚未保存，草稿已保留。请按 Ctrl+S 重试，或先复制草稿。"), true, true);
    }
    return ok;
}

void TaskDetailWidget::refreshTextState() {
    if (m_loading) return;
    const Entity task = m_store ? m_store->entity(m_id) : Entity();
    if (!m_textSession) {
        if (task.deleted) setStatus(QStringLiteral("内容在回收站中，恢复后可继续编辑。"), true);
        return;
    }
    if (m_textSession->isDirty()) m_dirty.insert(QStringLiteral("shared"));
    else m_dirty.remove(QStringLiteral("shared"));
    if (m_bodyConflict) return;
    const QString error = m_textSession->lastError();
    if (!error.isEmpty()) setStatus(error, true, true);
    else if (task.deleted || task.id.isEmpty()) {
        setStatus(hasPendingText()
            ? QStringLiteral("目标已移入回收站或被删除，草稿已保留。可复制草稿，或重新载入。")
            : QStringLiteral("内容在回收站中，恢复后可继续编辑。"), true, hasPendingText());
    } else setStatus(QString());
    updateEnabled();
    emit draftStateChanged();
}

bool TaskDetailWidget::hasPendingText() const {
    return m_bodyPublishPending || m_bodyConflict || !m_dirty.isEmpty() || (m_textSession && m_textSession->isDirty());
}
bool TaskDetailWidget::isSavingText() const { return m_saving || (m_textSession && m_textSession->isSaving()); }
bool TaskDetailWidget::textRejected() const {
    return m_bodyConflict || (m_textSession && !m_textSession->lastError().isEmpty());
}
QString TaskDetailWidget::titleDraft() const { return m_title->text(); }
QString TaskDetailWidget::bodyDraft() const { return m_description->content(); }

void TaskDetailWidget::updateEnabled() {
    if (!m_store) return;
    const Entity entity = m_store->entity(m_id);
    const bool enabled = !entity.id.isEmpty() && !entity.deleted;
    const bool task = entity.kind == EntityKind::Task;
    m_completed->setVisible(task && !m_desktopMode);
    m_dateSeparator->setVisible(enabled && !m_desktopMode);
    m_dateChip->setVisible(enabled);
    m_dateChip->setEnabled(enabled);
    m_completed->setEnabled(enabled && task);
    m_title->setReadOnly(!enabled);
    m_description->setEnabled(enabled);
    m_groupChip->setEnabled(enabled);
    m_formatButton->setEnabled(enabled);
    m_moreButton->setEnabled(true);
    m_attachmentAdd->setEnabled(enabled);
    m_attachments->setReadOnly(!enabled);
    updateParentLink();
    updateDetailLayout();
}

void TaskDetailWidget::updateDetailLayout() {
    if (!m_store || !m_attachmentPane || !m_attachments || !m_activity || !m_status) return;
    m_activityHost->setVisible(!m_activity->isHidden());
    const int count = m_id.isEmpty() ? 0 : m_store->attachments(m_id).size();
    m_attachmentCount->setText(QString::number(count));
    m_attachmentPane->setVisible(count > 0);
    if (count == 0) return;

    // 上限来自宿主可用高度，而不是已扣除本 pane 的正文 viewport；否则会反复缩放。
    const int statusHeight = m_statusHost->isHidden() ? 0 : m_statusHost->height();
    const int available = qMax(0, m_detailHost->height()
        - (m_desktopMode ? 0 : WorkspaceGeometry::DetailHeaderHeight)
        - (m_desktopMode ? 26 : kDetailFooterHeight) - statusHeight);
    const auto* formLayout = static_cast<QVBoxLayout*>(m_form->layout());
    const int bodyChrome = formLayout->contentsMargins().top() + formLayout->contentsMargins().bottom()
        + m_title->height() + formLayout->spacing()
        + (m_parentHost->isHidden() ? 0 : m_parentHost->height() + formLayout->spacing());
    const int budget = qMin(available / (m_desktopMode ? 2 : 3),
        qMax(0, available - bodyChrome - (m_desktopMode ? 48 : kMinimumBodyHeight)));
    const int cardLimit = kAttachmentVisibleCards * kAttachmentCardHeight
        + (kAttachmentVisibleCards - 1) * kAttachmentCardSpacing;
    const int cardsHeight = qMin(qMax(0, m_attachments->sizeHint().height()), cardLimit);
    const int natural = (m_desktopMode ? 0 : 1 + kAttachmentHeaderHeight)
        + cardsHeight + (m_desktopMode ? 4 : kAttachmentBottomMargin);
    const int height = qMin(natural, budget);
    // 不把当前预算设成 minimumHeight，否则旧 pane 高度会阻止宿主一次缩小/重新停靠。
    static_cast<AttachmentPane*>(m_attachmentPane)->setPreferredHeight(height);
}

void TaskDetailWidget::scheduleDetailLayout() {
    if (m_layoutPending) return;
    m_layoutPending = true;
    QTimer::singleShot(0, this, [this]() {
        m_layoutPending = false;
        updateDetailLayout();
    });
}

void TaskDetailWidget::attachSelectedTag(const QString& name) {
    const QString tag = name.trimmed();
    const QPointer<TaskStore> store = m_store;
    const QString target = m_id;
    if (tag.isEmpty() || m_loading || m_saving || !store || target.isEmpty()
        || !m_description->isEnabled()) return;
    const Entity before = store->entity(target);
    if (before.id != target || before.deleted || !flush()) return;
    if (!store || store != m_store || target != m_id || !m_description->isEnabled()) return;

    // flush 可触发同步 store 信号：必须从最新实体合并，不能覆盖同时增添的标签。
    Entity current = store->entity(target);
    if (current.id != target || current.deleted) {
        loadFields();
        return;
    }
    for (const QString& existing : current.tags)
        if (existing.trimmed().compare(tag, Qt::CaseInsensitive) == 0) return;
    current.tags.append(tag);
    m_saving = true;
    const bool ok = store->saveTask(current);
    m_saving = false;
    if (!store || store != m_store || target != m_id) return;
    if (!ok) setStatus(QStringLiteral("标签保存失败，正文和已有标签已保留。请重试。"), true);
}

void TaskDetailWidget::dirty(const QString& field) {
    if (m_loading || !m_store || m_id.isEmpty() || !m_textSession) return;
    if (field == QStringLiteral("title")) {
        m_textSession->updateTitle(m_title->text(), this, true);
        return;
    }
    if (m_bodyPublishPending) return;
    m_bodyPublishPending = true;
    QTimer::singleShot(0, this, [this]() {
        if (m_bodyPublishPending && !m_loading) publishBody();
    });
}

bool TaskDetailWidget::flush() {
    if (m_loading || m_saving || (m_textSession && m_textSession->isSaving())
        || !publishBody() || m_bodyConflict) return false;
    if (!m_textSession) return !hasPendingText();
    const QPointer<TaskDetailWidget> guard(this);
    const QPointer<EntityTextSession> session = m_textSession;
    const QString target = m_id;
    m_saving = true;
    const bool ok = session->flush();
    if (!guard) return false;
    m_saving = false;
    if (!session || session != m_textSession || target != m_id) return false;
    refreshTextState();
    return guard && ok && !hasPendingText();
}

void TaskDetailWidget::beginTextSession() {
    // The entity owns one shared session; focus changes do not fork its draft.
}

void TaskDetailWidget::scheduleTextSessionEnd() {
    if (!m_textSession || m_sessionEndPending) return;
    m_sessionEndPending = true;
    QTimer::singleShot(0, this, [this]() {
        m_sessionEndPending = false;
        QWidget* focus = QApplication::focusWidget();
        auto* editor = m_description->textEditor();
        if (focus && focus->isVisible()
            && (focus == m_title || focus == editor || editor->isAncestorOf(focus))) return;
        if (m_description->ownsPopup(QApplication::activePopupWidget())
            || (focus && focus->isVisible() && m_description->ownsPopup(focus->window()))) return;
        endTextSession();
    });
}

bool TaskDetailWidget::endTextSession() {
    if (!flush()) return false;
    if (!m_textSession) return true;
    const QPointer<TaskDetailWidget> guard(this);
    m_saving = true;
    const bool ok = m_textSession->finishEditing();
    if (!guard) return false;
    m_saving = false;
    refreshTextState();
    return ok;
}

bool TaskDetailWidget::cancelTextSession() {
    return !m_textSession || m_textSession->reload();
}

void TaskDetailWidget::copyDraft() {
    QApplication::clipboard()->setText(m_title->text() + QStringLiteral("\n\n") + m_description->content());
}

void TaskDetailWidget::reloadDraft() {
    if (m_loading || m_saving) return;
    const QPointer<TaskDetailWidget> guard(this);
    const QPointer<TaskStore> store = m_store;
    const QString target = m_id;
    if (!ConfirmDialog::prompt(QStringLiteral("重新载入内容"),
        QStringLiteral("这会丢弃本窗口尚未保存的修改，并载入当前已保存的内容。需要保留时，请先复制草稿。"),
        QStringLiteral("丢弃并重新载入"), true, this)) return;
    if (!guard || store != m_store || target != m_id) return;
    if (!cancelTextSession()) {
        if (guard) setStatus(QStringLiteral("保存编辑历史失败，草稿未丢弃。请先复制草稿并重试。"), true, true);
        return;
    }
    if (!guard || store != m_store || target != m_id) return;
    m_bodyPublishPending = false;
    m_bodyConflict = false;
    m_pendingBodyChanges.clear();
    m_dirty.clear();
    loadFields(true);
}

bool TaskDetailWidget::finishEditing() { return flush() && endTextSession(); }

bool TaskDetailWidget::canDismissWithEscape() const {
    if (m_description->isComposing()) return false;
    for (QWidget* popup : QApplication::topLevelWidgets())
        if (popup->isVisible() && (isAncestorOf(popup) || m_description->ownsPopup(popup))) return false;
    return true;
}

void TaskDetailWidget::setDropHintVisible(bool visible) {
    m_dropHint->setVisible(visible);
    if (!visible) return;
    m_dropHint->setGeometry(12, WorkspaceGeometry::DetailHeaderHeight + 8,
        qMax(0, m_detailHost->width() - 24),
        qMax(0, m_detailHost->height() - WorkspaceGeometry::DetailHeaderHeight - 64));
    m_dropHint->raise();
}

bool TaskDetailWidget::handleFileDrop(QObject* watched, QEvent* event) {
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget || (widget != m_detailHost && !m_detailHost->isAncestorOf(widget))
        || widget->window() != window()) return false;
    if (event->type() == QEvent::Resize && widget == m_detailHost && m_dropHint->isVisible())
        setDropHintVisible(true);
    if (event->type() == QEvent::DragLeave) {
        setDropHintVisible(false);
        return false;
    }
    if (event->type() != QEvent::DragEnter && event->type() != QEvent::DragMove
        && event->type() != QEvent::Drop) return false;
    auto* drop = static_cast<QDropEvent*>(event);
    const QStringList paths = localFilePaths(drop->mimeData());
    if (paths.isEmpty()) return false;
    const bool writable = m_store && !m_id.isEmpty() && !m_store->entity(m_id).id.isEmpty()
        && !m_store->entity(m_id).deleted && (drop->possibleActions() & Qt::CopyAction);
    if (!writable) {
        setDropHintVisible(false);
        drop->ignore();
        return true;
    }
    if (event->type() == QEvent::Drop) {
        setDropHintVisible(false);
        const QString target = m_id;
        if (!finishEditing() || target != m_id || m_attachments->importPaths(paths) == 0) {
            drop->ignore();
            return true;
        }
    } else setDropHintVisible(true);
    drop->setDropAction(Qt::CopyAction);
    drop->accept();
    return true;
}

bool TaskDetailWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_title && event->type() == QEvent::ContextMenu && m_textSession) {
        PopupMenu menu(m_title);
        menu.setObjectName(QStringLiteral("SharedTitleContextMenu"));
        menu.addAction(QStringLiteral("撤销"), this, [this]() { if (m_textSession) m_textSession->undoTitle(this); })
            ->setEnabled(!m_title->isReadOnly() && m_textSession->canUndoTitle());
        menu.addAction(QStringLiteral("重做"), this, [this]() { if (m_textSession) m_textSession->redoTitle(this); })
            ->setEnabled(!m_title->isReadOnly() && m_textSession->canRedoTitle());
        menu.addSeparator();
        menu.addAction(QStringLiteral("剪切"), m_title, &QLineEdit::cut)
            ->setEnabled(!m_title->isReadOnly() && m_title->hasSelectedText());
        menu.addAction(QStringLiteral("复制"), m_title, &QLineEdit::copy)->setEnabled(m_title->hasSelectedText());
        menu.addAction(QStringLiteral("粘贴"), m_title, &QLineEdit::paste)->setEnabled(!m_title->isReadOnly());
        menu.addSeparator();
        menu.addAction(QStringLiteral("全选"), m_title, &QLineEdit::selectAll);
        menu.exec(static_cast<QContextMenuEvent*>(event)->globalPos());
        return true;
    }
    if (handleFileDrop(watched, event)) return true;
    if (watched == this || watched == m_detailHost || watched == m_form
        || watched == m_scroll->viewport() || watched == m_statusHost
        || watched == m_activity || watched == m_parentHost) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show
            || event->type() == QEvent::Hide || event->type() == QEvent::ParentChange
            || event->type() == QEvent::LayoutRequest) scheduleDetailLayout();
    }
    if (watched == m_formatPanel && event->type() == QEvent::Hide) m_formatButton->setChecked(false);
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget && (event->type() == QEvent::FocusOut || event->type() == QEvent::Hide
                   || event->type() == QEvent::Close)
        && m_description->ownsPopup(widget->window())) scheduleTextSessionEnd();
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (watched == m_title && m_textSession
            && (key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo))) {
            if (!m_title->isReadOnly()) {
                if (key->matches(QKeySequence::Redo)) m_textSession->redoTitle(this);
                else m_textSession->undoTitle(this);
            }
            return true;
        }
        if (watched == m_title && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)) {
            m_description->textEditor()->setFocus();
            auto cursor = m_description->textEditor()->textCursor();
            cursor.movePosition(QTextCursor::Start);
            m_description->textEditor()->setTextCursor(cursor);
            return true;
        }
        if (watched == m_description->textEditor() && key->key() == Qt::Key_Escape
            && !m_description->isComposing() && !QApplication::activePopupWidget()
            && m_bubble->isVisible()) {
            m_bubble->hide();
            return true;
        }
        if (watched == m_description->textEditor() && m_focusMode && key->key() == Qt::Key_Escape
            && canDismissWithEscape()) {
            m_focusMode = false;
            emit focusModeRequested(false);
            return true;
        }
    }
    if (watched == m_title || watched == m_description->textEditor()) {
        if (event->type() == QEvent::FocusIn) beginTextSession();
        if (event->type() == QEvent::FocusOut) scheduleTextSessionEnd();
    }
    return QWidget::eventFilter(watched, event);
}

void TaskDetailWidget::focusTitle() {
    m_title->setFocus();
    m_title->selectAll();
    m_scroll->ensureWidgetVisible(m_title, 0, 4);
}
}  // namespace kl
