#include "NoteWidget.h"

#include <QApplication>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QShortcut>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

#include "../app/AutostartSettings.h"
#include "../app/Branding.h"
#include "../app/TaskStore.h"
#include "AttachmentWidget.h"
#include "ColorTheme.h"
#include "MarkdownEditor.h"
#include "PopupMenu.h"
#include "PopupShadow.h"
#include "SettingsDialog.h"
#include "TaskDetailWidget.h"
#include "TitleBar.h"
#include "WindowChrome.h"
#include "design/DesignTokens.h"
#include "platform/SystemShadow.h"

namespace kl {
namespace {
constexpr int kPaperRadius = 7;
constexpr int kMinimumPaperWidth = 220;
constexpr int kMinimumPaperHeight = 180;

class NoteBorder final : public QWidget {
public:
    explicit NoteBorder(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("NoteBorder"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(ColorTheme::borderColor(), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                kPaperRadius - 0.5, kPaperRadius - 0.5);
    }
};

void popupAt(PopupMenu* menu, QWidget* anchor, bool above) {
    const QSize card = menu->sizeHint() - QSize(2 * PopupShadowMargin, 2 * PopupShadowMargin);
    const QRect target(anchor->mapToGlobal(QPoint()), anchor->size());
    QScreen* screen = QGuiApplication::screenAt(target.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry() : QRect(target.topLeft(), QSize(1920, 1080));
    int x = target.right() + 1 - card.width();
    int y = above ? target.top() - card.height() - 4 : target.bottom() + 5;
    if (above && y < available.top()) y = target.bottom() + 5;
    if (!above && y + card.height() > available.bottom() + 1) y = target.top() - card.height() - 4;
    x = qBound(available.left(), x, qMax(available.left(), available.right() + 1 - card.width()));
    y = qBound(available.top(), y, qMax(available.top(), available.bottom() + 1 - card.height()));
    menu->popup(QPoint(x, y));
}
} // namespace

QSize NoteWidget::minimumPaperSize() { return QSize(kMinimumPaperWidth, kMinimumPaperHeight); }

NoteWidget::NoteWidget(const Entity& task, QWidget* parent) : QWidget(parent), m_task(task) {
    m_loading = true;
    setObjectName(QStringLiteral("NoteWidget"));
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setMinimumSize(kMinimumPaperWidth + 2 * PopupShadowMargin,
                   kMinimumPaperHeight + 2 * PopupShadowMargin);
    buildUi();
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(SaveDebounceMs);
    connect(&m_saveTimer, &QTimer::timeout, this, &NoteWidget::flush);
    auto* save = new QShortcut(QKeySequence::Save, this);
    save->setContext(Qt::WidgetWithChildrenShortcut);
    connect(save, &QShortcut::activated, this, &NoteWidget::finishEditing);
    auto* create = new QShortcut(QKeySequence(QStringLiteral("Ctrl+N")), this);
    create->setContext(Qt::WidgetWithChildrenShortcut);
    connect(create, &QShortcut::activated, this, [this]() { if (finishEditing()) emit newNoteRequested(); });
    auto* complete = new QShortcut(QKeySequence(QStringLiteral("Ctrl+M")), this);
    complete->setContext(Qt::WidgetWithChildrenShortcut);
    connect(complete, &QShortcut::activated, this, [this]() {
        if (finishEditing()) emit completionRequested(id(), true);
    });
    if (task.geometry.isValid()) setGeometry(task.geometry);
    else resize(DefaultWidth + 2 * PopupShadowMargin, DefaultHeight + 2 * PopupShadowMargin);
    applyWindowFlags();
    updateView();
    syncChromeGeometry();
    m_observedGeometry = geometry();
    m_loading = false;
}

NoteWidget::~NoteWidget() {
    m_loading = true;
    m_saveTimer.stop();
    if (m_menu) {
        disconnect(m_menu, nullptr, this, nullptr);
        m_menu->hide();
    }
    if (m_detail) disconnect(m_detail, nullptr, this, nullptr);
}

void NoteWidget::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(PopupShadowMargin, PopupShadowMargin, PopupShadowMargin, PopupShadowMargin);
    root->setSpacing(0);
    m_paper = new QWidget(this);
    m_paper->setObjectName(QStringLiteral("NotePaper"));
    m_paper->setAutoFillBackground(false);
    auto* paper = new QVBoxLayout(m_paper);
    paper->setContentsMargins(0, 0, 0, 0);
    paper->setSpacing(0);
    root->addWidget(m_paper);
    m_titleBar = new TitleBar(m_paper);
    paper->addWidget(m_titleBar);
    m_status = new QLabel(m_paper);
    m_status->setObjectName(QStringLiteral("NoteStatus"));
    m_status->setWordWrap(true);
    m_status->setContentsMargins(14, 0, 14, 5);
    m_status->hide();
    paper->addWidget(m_status);
    m_border = new NoteBorder(this);
    const struct { const char* name; Qt::Edges edges; } definitions[] = {
        {"NoteResizeLeft", Qt::LeftEdge}, {"NoteResizeRight", Qt::RightEdge},
        {"NoteResizeTop", Qt::TopEdge}, {"NoteResizeBottom", Qt::BottomEdge},
        {"NoteResizeTopLeft", Qt::LeftEdge | Qt::TopEdge},
        {"NoteResizeTopRight", Qt::RightEdge | Qt::TopEdge},
        {"NoteResizeBottomLeft", Qt::LeftEdge | Qt::BottomEdge},
        {"NoteResizeBottomRight", Qt::RightEdge | Qt::BottomEdge}
    };
    for (const auto& definition : definitions)
        m_resizeAreas.append(new WindowResizeArea(this, definition.edges, QString::fromLatin1(definition.name)));
    m_titleBar->setMoveConstraint([this](const QPoint& raw) {
        if (!m_snapProvider) return raw;
        const QRect paper(raw + QPoint(PopupShadowMargin, PopupShadowMargin),
                          size() - QSize(2 * PopupShadowMargin, 2 * PopupShadowMargin));
        return m_snapProvider(paper).topLeft() - QPoint(PopupShadowMargin, PopupShadowMargin);
    });
    connect(m_titleBar, &TitleBar::closeClicked, this, [this]() { emit closeRequested(this); });
    connect(m_titleBar, &TitleBar::colorClicked, this, &NoteWidget::showColorMenu);
    connect(m_titleBar, &TitleBar::pinClicked, this, [this]() {
        setLayer(layer() == NoteLayer::OnTop ? NoteLayer::Normal : NoteLayer::OnTop);
    });
}

void NoteWidget::setStore(TaskStore* store) {
    if (m_store == store && m_detail) return;
    if (m_detail && !finishEditing()) return;
    delete m_detail;
    m_detail = nullptr;
    m_store = store;
    if (!store) return;
    m_detail = new TaskDetailWidget(store, m_paper);
    m_detail->setObjectName(QStringLiteral("NoteDetail"));
    m_detail->setDesktopMode(true);
    static_cast<QVBoxLayout*>(m_paper->layout())->insertWidget(1, m_detail, 1);
    m_detail->setTask(id());
    m_detail->setDesktopHovered(m_hovered);
    connect(m_detail, &TaskDetailWidget::desktopMoreRequested, this, &NoteWidget::showMoreMenu);
    connect(m_detail, &TaskDetailWidget::relatedDetailRequested, this, &NoteWidget::relatedDetailRequested);
    connect(m_detail, &TaskDetailWidget::internalLinkRequested, this, &NoteWidget::relatedDetailRequested);
    connect(m_detail, &TaskDetailWidget::draftStateChanged, this, [this]() {
        updateView();
        if (!m_store || !m_detail || m_detail->hasPendingText()) return;
        const Entity current = m_store->entity(id());
        if (current.id.isEmpty() || current.deleted || current.kind != EntityKind::Task
            || current.status != TaskStatus::Active || m_store->isListArchived(current.listId))
            QTimer::singleShot(0, this, [this]() { emit closeRequested(this); });
    });
    connect(m_detail, &TaskDetailWidget::targetUnavailable, this, [this](const QString&) {
        emit closeRequested(this);
    });
    updateView();
}

void NoteWidget::updateView() {
    if (m_appliedColor != colorToInt(m_task.color)) {
        m_appliedColor = colorToInt(m_task.color);
        ColorTheme::apply(this, m_task.color);
    }
    m_titleBar->setPinned(m_task.layer == NoteLayer::OnTop);
    const QString title = m_detail ? m_detail->titleDraft() : m_task.title;
    const QString caption = title + QStringLiteral(" — ") + Branding::displayName();
    if (windowTitle() != caption) setWindowTitle(caption);
    update();
}

Entity NoteWidget::snapshot() const {
    Entity task = m_task;
    task.geometry = geometry();
    if (m_detail) {
        task.title = m_detail->titleDraft();
        task.body = m_detail->bodyDraft();
    }
    return task;
}

void NoteWidget::applyTask(const Entity& task) {
    if (task.id != id()) return;
    const Entity pending = snapshot();
    const bool changedLayer = !m_desktopDirty && m_task.layer != task.layer;
    m_task = task;
    if (m_desktopDirty) {
        m_task.color = pending.color;
        m_task.layer = pending.layer;
        m_task.zOrder = pending.zOrder;
    }
    if (changedLayer) applyWindowFlags();
    updateView();
}

void NoteWidget::applySaved(const Entity& task) {
    if (task.id != id()) return;
    if (geometry() == task.geometry && m_task.color == task.color
        && m_task.layer == task.layer && m_task.zOrder == task.zOrder) m_desktopDirty = false;
    applyTask(task);
    setSaveFailed(false);
    if (m_desktopDirty) m_saveTimer.start();
}

void NoteWidget::setSaveFailed(bool failed) {
    m_saveFailed = failed;
    m_status->setText(failed ? QStringLiteral("便签位置或外观保存失败，修改已保留。按 Ctrl+S 重试。") : QString());
    m_status->setVisible(failed);
    if (failed) m_saveTimer.start(SaveRetryMs);
}

bool NoteWidget::saveFailed() const { return m_saveFailed || textRejected(); }
bool NoteWidget::descriptionDirty() const { return m_detail && m_detail->hasPendingText(); }
bool NoteWidget::isSaving() const { return m_saving || (m_detail && m_detail->isSavingText()); }
bool NoteWidget::textRejected() const { return m_detail && m_detail->textRejected(); }

bool NoteWidget::flush() {
    m_saveTimer.stop();
    if (m_loading || m_saving) return false;
    if (m_detail && !m_detail->flush()) return false;
    if (m_desktopDirty) {
        const QPointer<NoteWidget> guard(this);
        m_saving = true;
        emit saveRequested(this);
        if (!guard) return false;
        m_saving = false;
    }
    return !m_saveFailed && !m_desktopDirty;
}

bool NoteWidget::finishEditing() {
    if (m_detail && !m_detail->finishEditing()) return false;
    return flush();
}

void NoteWidget::setAutostart(AutostartSettings* autostart) { m_autostart = autostart; }

void NoteWidget::refreshAttachments() {
    if (m_detail) for (auto* attachments : m_detail->findChildren<AttachmentWidget*>()) attachments->refresh();
}

void NoteWidget::scheduleSave() {
    if (m_loading || m_applyingFlags) return;
    m_desktopDirty = true;
    m_saveTimer.start(SaveDebounceMs);
}

void NoteWidget::setLayer(NoteLayer layer) {
    m_temporary = false;
    m_task.layer = layer;
    applyWindowFlags();
    m_titleBar->setPinned(layer == NoteLayer::OnTop);
    scheduleSave();
}
NoteLayer NoteWidget::cycleLayer() { setLayer(nextLayer(layer())); return layer(); }

void NoteWidget::applyWindowFlags() {
    m_applyingFlags = true;
    const bool visible = isVisible();
    const QRect previous = geometry();
    Qt::WindowFlags flags = Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint;
    if (!m_temporary) {
        if (layer() == NoteLayer::OnTop) flags |= Qt::WindowStaysOnTopHint;
        else if (layer() == NoteLayer::OnDesktop) flags |= Qt::WindowStaysOnBottomHint;
    }
    if (windowFlags() != flags) setWindowFlags(flags);
    setGeometry(previous);
    disableNativeWindowShadows();
    if (visible) show();
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
    if (m_detail) m_detail->focusTitle();
}

QRect NoteWidget::paperGeometry() const {
    return geometry().adjusted(PopupShadowMargin, PopupShadowMargin, -PopupShadowMargin, -PopupShadowMargin);
}
void NoteWidget::setPaperGeometry(const QRect& paper) {
    if (paper.isValid()) setGeometry(paper.adjusted(-PopupShadowMargin, -PopupShadowMargin,
                                                   PopupShadowMargin, PopupShadowMargin));
}
void NoteWidget::setSnapProvider(std::function<QRect(const QRect&)> provider) { m_snapProvider = std::move(provider); }

void NoteWidget::setControlsHovered(bool hovered) {
    m_hovered = hovered;
    const bool shown = hovered || (m_menu && m_menu->isVisible());
    m_titleBar->setHovered(shown);
    if (m_detail) m_detail->setDesktopHovered(shown);
}
void NoteWidget::enterEvent(QEvent* event) { setControlsHovered(true); QWidget::enterEvent(event); }
void NoteWidget::leaveEvent(QEvent* event) { setControlsHovered(false); QWidget::leaveEvent(event); }

void NoteWidget::showColorMenu() {
    auto* menu = new PopupMenu(this);
    menu->setObjectName(QStringLiteral("NoteColorMenu"));
    menu->setThemeOverride(ColorTheme::paperTheme());
    menu->setAttribute(Qt::WA_DeleteOnClose);
    for (int index = 0; index < ColorCount; ++index) {
        const NoteColor color = colorFromInt(index);
        const auto spec = ColorTheme::spec(color);
        QPixmap swatch(14, 14);
        swatch.fill(QColor(spec.body));
        auto* action = menu->addAction(QIcon(swatch), QString::fromUtf8(spec.label), this, [this, color]() {
            m_task.color = color;
            updateView();
            scheduleSave();
        });
        action->setCheckable(true);
        action->setChecked(color == m_task.color);
    }
    m_menu = menu;
    connect(menu, &QMenu::aboutToHide, this, [this]() { m_menu.clear(); setControlsHovered(underMouse()); });
    popupAt(menu, m_titleBar->findChild<QToolButton*>(QStringLiteral("ColorButton")), false);
}

void NoteWidget::showMoreMenu() {
    if (!m_detail || !m_store || !finishEditing()) return;
    auto* menu = new PopupMenu(this);
    menu->setObjectName(QStringLiteral("NoteMoreMenu"));
    menu->setThemeOverride(ColorTheme::paperTheme());
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(QStringLiteral("新建"), this, [this]() { emit newNoteRequested(); })
        ->setShortcut(QKeySequence(QStringLiteral("Ctrl+N")));
    menu->addAction(QStringLiteral("完成"), this, [this]() { emit completionRequested(id(), true); })
        ->setShortcut(QKeySequence(QStringLiteral("Ctrl+M")));
    auto* arrange = menu->addMenu(QStringLiteral("排列"));
    arrange->addAction(QStringLiteral("横向排列"), this, [this]() { emit arrangeRequested(false); });
    arrange->addAction(QStringLiteral("纵向排列"), this, [this]() { emit arrangeRequested(true); });
    menu->addAction(QStringLiteral("打开主窗口"), this, [this]() { emit taskOpenRequested(id()); });
    auto* settings = menu->addMenu(QStringLiteral("设置"));
    for (NoteLayer value : {NoteLayer::Normal, NoteLayer::OnTop, NoteLayer::OnDesktop}) {
        auto* action = settings->addAction(layerLabel(value), this, [this, value]() { setLayer(value); });
        action->setCheckable(true);
        action->setChecked(value == layer());
    }
    settings->addSeparator();
    settings->addAction(QStringLiteral("应用设置…"), this, [this]() {
        QTimer::singleShot(0, this, [this]() {
            auto* dialog = new SettingsDialog(m_autostart, this);
            connect(dialog, &SettingsDialog::taskWidgetRequested, this, &NoteWidget::taskWidgetRequested);
            dialog->setWindowFlags(dialog->windowFlags() | Qt::Window);
            dialog->setWindowModality(Qt::ApplicationModal);
            dialog->setObjectName(QStringLiteral("SettingsDialog"));
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->open();
        });
    });
    menu->addSeparator();
    menu->addAction(QStringLiteral("删除"), this, [this]() {
        if (m_store && finishEditing()) m_store->trashTask(id());
    });
    m_menu = menu;
    connect(menu, &QMenu::aboutToHide, this, [this]() { m_menu.clear(); setControlsHovered(underMouse()); });
    popupAt(menu, m_detail->findChild<QToolButton*>(QStringLiteral("DetailMore")), true);
}

void NoteWidget::paintEvent(QPaintEvent*) {
    const QRect paper = rect().adjusted(PopupShadowMargin, PopupShadowMargin, -PopupShadowMargin, -PopupShadowMargin);
    if (m_shadow.isNull()) {
        const QColor shadow = ds::color(ds::ColorToken::Shadow, ColorTheme::paperTheme());
        m_shadow = popupShadow(QSize(128, 128), QRectF(24, 24, 80, 80), kPaperRadius, shadow, 5);
    }
    QPainter painter(this);
    const int source[] = {0, 52, 76, 128};
    const int columns[] = {0, 52, width() - 52, width()};
    const int rows[] = {0, 52, height() - 52, height()};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            painter.drawImage(QRect(columns[column], rows[row], columns[column + 1] - columns[column],
                                    rows[row + 1] - rows[row]), m_shadow,
                              QRect(source[column], source[row], source[column + 1] - source[column],
                                    source[row + 1] - source[row]));
        }
    }
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(ColorTheme::spec(m_task.color).body));
    painter.drawRoundedRect(QRectF(paper), kPaperRadius, kPaperRadius);
}

void NoteWidget::syncChromeGeometry() {
    if (!m_border || m_resizeAreas.size() != 8) return;
    const QRect paper = rect().adjusted(PopupShadowMargin, PopupShadowMargin, -PopupShadowMargin, -PopupShadowMargin);
    const int left = paper.left(), top = paper.top(), right = paper.right(), bottom = paper.bottom();
    const QRect areas[] = {
        QRect(left - 3, top + 7, 6, qMax(1, paper.height() - 14)),
        QRect(right - 2, top + 7, 6, qMax(1, paper.height() - 14)),
        QRect(left + 7, top - 3, qMax(1, paper.width() - 14), 6),
        QRect(left + 7, bottom - 2, qMax(1, paper.width() - 14), 6),
        QRect(left - 3, top - 3, 10, 10), QRect(right - 6, top - 3, 10, 10),
        QRect(left - 3, bottom - 6, 10, 10), QRect(right - 6, bottom - 6, 10, 10)
    };
    for (int index = 0; index < m_resizeAreas.size(); ++index) {
        m_resizeAreas.at(index)->setGeometry(areas[index]);
        m_resizeAreas.at(index)->raise();
    }
    m_border->setGeometry(paper);
    m_border->raise();
}

void NoteWidget::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);
    const bool changed = geometry() != m_observedGeometry;
    m_observedGeometry = geometry();
    if (changed) scheduleSave();
}
void NoteWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    syncChromeGeometry();
    const bool changed = geometry() != m_observedGeometry;
    m_observedGeometry = geometry();
    if (changed) scheduleSave();
    update();
}
void NoteWidget::closeEvent(QCloseEvent* event) { event->ignore(); emit closeRequested(this); }
void NoteWidget::changeEvent(QEvent* event) {
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) emit activated(this);
    if (event->type() == QEvent::WindowDeactivate && !m_applyingFlags && !m_loading) {
        flush();
        if (m_temporary) { m_temporary = false; applyWindowFlags(); }
    }
    QWidget::changeEvent(event);
}

} // namespace kl
