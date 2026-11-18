#include "RelatedDetailPopup.h"

#include <QApplication>
#include <QCloseEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include "../app/TaskStore.h"
#include "PopupShadow.h"
#include "TaskDetailWidget.h"
#include "WindowChrome.h"
#include "WorkspaceGeometry.h"
#include "platform/SystemShadow.h"
#include "design/ThemeManager.h"
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <windowsx.h>
#endif

namespace kl {
namespace {
constexpr int kCardWidth = 610;
constexpr int kCardHeight = 500;
constexpr int kCardRadius = 16;
constexpr int kMinimumContentWidth = 420;
constexpr int kMinimumContentHeight = 360;

class CardBorder final : public QWidget {
public:
    explicit CardBorder(QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("RelatedDetailBorder"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }
    void setRounded(bool rounded) { m_rounded = rounded; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(ThemeManager::instance().color(ds::ColorToken::Border));
        const QRectF bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        if (m_rounded)
            painter.drawRoundedRect(bounds, kCardRadius - 0.5, kCardRadius - 0.5);
        else
            painter.drawRect(bounds);
    }
private:
    bool m_rounded = true;
};

class DetailCard final : public QWidget {
public:
    explicit DetailCard(QWidget* parent) : QWidget(parent), m_border(new CardBorder(this)) {
        setObjectName(QStringLiteral("RelatedDetailCard"));
        setAttribute(Qt::WA_TranslucentBackground);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(PopupShadowMargin, PopupShadowMargin,
                                   PopupShadowMargin, PopupShadowMargin);
        layout->setSpacing(0);
    }
    QRect contentRect() const {
        const int margin = m_decorated ? PopupShadowMargin : 0;
        return rect().adjusted(margin, margin, -margin, -margin);
    }
    bool decorated() const { return m_decorated; }
    void setDecorated(bool decorated) {
        if (m_decorated == decorated) return;
        m_decorated = decorated;
        const int margin = decorated ? PopupShadowMargin : 0;
        layout()->setContentsMargins(margin, margin, margin, margin);
        m_border->setRounded(decorated);
        m_shadow = QImage();
        m_shadowColor = QColor();
        raiseBorder();
        update();
    }
    void raiseBorder() {
        m_border->setGeometry(contentRect());
        m_border->raise();
        m_border->update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        auto& theme = ThemeManager::instance();
        const QColor shadowColor = theme.color(ds::ColorToken::Shadow);
        if (m_decorated && (m_shadow.size() != size() || m_shadowColor != shadowColor)) {
            m_shadow = popupShadow(size(), QRectF(contentRect()), kCardRadius, shadowColor, 5);
            m_shadowColor = shadowColor;
        }
        QPainter painter(this);
        if (m_decorated) painter.drawImage(0, 0, m_shadow);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.color(ds::ColorToken::Surface));
        if (m_decorated)
            painter.drawRoundedRect(QRectF(contentRect()), kCardRadius, kCardRadius);
        else
            painter.drawRect(contentRect());
    }
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        raiseBorder();
    }
private:
    CardBorder* m_border;
    QImage m_shadow;
    QColor m_shadowColor;
    bool m_decorated = true;
};
}  // namespace

RelatedDetailPopup::RelatedDetailPopup(TaskStore* store)
    : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint),
      m_store(store), m_card(new DetailCard(this)) {
    setObjectName(QStringLiteral("RelatedDetailPopup"));
    setWindowTitle(QStringLiteral("关联详情"));
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(kMinimumContentWidth + 2 * PopupShadowMargin,
                   kMinimumContentHeight + WorkspaceGeometry::WindowControlHeight
                       + 2 * PopupShadowMargin);
    resize(kCardWidth + 2 * PopupShadowMargin,
           kCardHeight + WorkspaceGeometry::WindowControlHeight + 2 * PopupShadowMargin);

    auto* titleBar = new QFrame(m_card);
    titleBar->setObjectName(QStringLiteral("RelatedDetailTitleBar"));
    titleBar->setFixedHeight(WorkspaceGeometry::WindowControlHeight);
    auto* titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(0);
    auto* dragArea = new WindowDragArea(titleBar);
    dragArea->setObjectName(QStringLiteral("RelatedDetailDragArea"));
    dragArea->setFixedHeight(WorkspaceGeometry::WindowControlHeight);
    dragArea->setAccessibleName(QStringLiteral("拖动关联详情窗口"));
    titleLayout->addWidget(dragArea, 1);
    auto* controls = new WindowControls(titleBar);
    titleLayout->addWidget(controls);
    connect(controls, &WindowControls::windowOperationFinished, this, [this]() {
        QTimer::singleShot(0, this, [this]() {
            if (!isMinimized()) syncWindowDecoration();
        });
    });
    m_card->layout()->addWidget(titleBar);

    const struct { const char* name; Qt::Edges edges; } resizeAreas[] = {
        {"RelatedResizeLeft", Qt::LeftEdge}, {"RelatedResizeRight", Qt::RightEdge},
        {"RelatedResizeTop", Qt::TopEdge}, {"RelatedResizeBottom", Qt::BottomEdge},
        {"RelatedResizeTopLeft", Qt::LeftEdge | Qt::TopEdge},
        {"RelatedResizeTopRight", Qt::RightEdge | Qt::TopEdge},
        {"RelatedResizeBottomLeft", Qt::LeftEdge | Qt::BottomEdge},
        {"RelatedResizeBottomRight", Qt::RightEdge | Qt::BottomEdge}
    };
    for (const auto& definition : resizeAreas)
        m_resizeAreas.append(new WindowResizeArea(this, definition.edges,
                                                  QString::fromLatin1(definition.name)));

    disableNativeWindowShadows();
#ifdef Q_OS_WIN
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_THICKFRAME);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, &RelatedDetailPopup::applyTheme);
    connect(store, &QObject::destroyed, this, [this]() { clearDetail(false); });
    applyTheme();
    syncWindowDecoration();
    qApp->installEventFilter(this);
    hide();
}

RelatedDetailPopup::~RelatedDetailPopup() {
    if (qApp) qApp->removeEventFilter(this);
}

void RelatedDetailPopup::applyTheme() {
    const bool decorated = static_cast<DetailCard*>(m_card)->decorated();
    const int radius = decorated ? kCardRadius : 0;
    auto& theme = ThemeManager::instance();
    setStyleSheet(QStringLiteral(
        "QWidget#RelatedDetailPopup{background:transparent;}"
        "QFrame#RelatedDetailTitleBar{background:%1;border:0;"
        "border-top-left-radius:%2px;border-top-right-radius:%2px;}"
        "QWidget#RelatedDetailCard QWidget#TaskDetailHost{background:%1;border:0;"
        "border-bottom-left-radius:%2px;border-bottom-right-radius:%2px;}"
        "QWidget#RelatedDetailCard QFrame#DetailHeader{background:%1;"
        "border:0;border-bottom:1px solid %3;}"
        "QWidget#RelatedDetailCard QFrame#DetailFooter{background:%1;border:0;"
        "border-bottom-left-radius:%2px;border-bottom-right-radius:%2px;}")
        .arg(theme.color(ds::ColorToken::Surface).name())
        .arg(radius)
        .arg(theme.color(ds::ColorToken::Divider).name()));
    m_card->update();
    static_cast<DetailCard*>(m_card)->raiseBorder();
}

void RelatedDetailPopup::createDetail() {
    m_detail = new TaskDetailWidget(m_store, m_card);
    m_detail->setObjectName(QStringLiteral("RelatedTaskDetailWidget"));
    m_card->layout()->addWidget(m_detail);
    connect(m_detail, &TaskDetailWidget::relatedDetailRequested, this, &RelatedDetailPopup::openEntity);
    connect(m_detail, &TaskDetailWidget::internalLinkRequested, this, [this](const QString& id) {
        if (requestClose()) emit internalLinkRequested(id);
    });
    connect(m_detail, &TaskDetailWidget::closeRequested, this, &RelatedDetailPopup::requestClose);
    connect(m_detail, &TaskDetailWidget::targetUnavailable, this, [this]() {
        const quint64 generation = m_generation;
        QTimer::singleShot(0, this, [this, generation]() {
            if (generation == m_generation && m_detail && m_detail->entityId().isEmpty()) requestClose();
        });
    });
    connect(m_detail, &TaskDetailWidget::focusModeRequested, this, [this](bool expanded) {
        if (expanded) showMaximized();
        else showNormal();
    });
    static_cast<DetailCard*>(m_card)->raiseBorder();
}

bool RelatedDetailPopup::openEntity(const QString& id) {
    if (m_closing || !isEnabled() || !m_store || id.isEmpty() || m_store->entity(id).id.isEmpty()) return false;
    if (m_detail && isVisible() && m_detail->entityId() == id) {
        if (isMinimized()) setWindowState(windowState() & ~Qt::WindowMinimized);
        show();
        raise();
        activateWindow();
        return true;
    }
    const QPointer<RelatedDetailPopup> guard(this);
    if (!m_detail) createDetail();
    if (!m_detail->setTask(id) || !guard) return false;
    if (!m_store || !m_detail || m_detail->entityId() != id) return false;
    ++m_generation;
    if (!isVisible()) {
        m_previousFocus = QApplication::focusWidget();
        positionInitially();
    }
    show();
    if (isMinimized()) setWindowState(windowState() & ~Qt::WindowMinimized);
    raise();
    activateWindow();
    setFocus(Qt::ActiveWindowFocusReason);
    syncWindowDecoration();
    return true;
}

QString RelatedDetailPopup::entityId() const { return m_detail ? m_detail->entityId() : QString(); }
bool RelatedDetailPopup::finishEditing() { return !m_detail || m_detail->finishEditing(); }

bool RelatedDetailPopup::requestClose() {
    if (m_closing) return false;
    const QPointer<RelatedDetailPopup> guard(this);
    m_closing = true;
    if (!finishEditing() || !guard) {
        if (guard) m_closing = false;
        return false;
    }
    if (m_detail && !m_detail->setTask(QString())) {
        if (guard) m_closing = false;
        return false;
    }
    if (!guard) return false;
    clearDetail(true);
    m_closing = false;
    return true;
}

void RelatedDetailPopup::clearDetail(bool restoreFocus) {
    ++m_generation;
    hide();
    if (m_detail) {
        m_card->layout()->removeWidget(m_detail);
        m_detail->hide();
        m_detail->deleteLater();
        m_detail.clear();
    }
    const QPointer<QWidget> previous = m_previousFocus;
    m_previousFocus.clear();
    if (restoreFocus && previous && previous->isVisible() && previous->isEnabled())
        previous->setFocus(Qt::ActiveWindowFocusReason);
}

void RelatedDetailPopup::clearAfterRestore() { clearDetail(false); }

void RelatedDetailPopup::positionInitially() {
    if (m_positioned) return;
    QScreen* screen = m_anchorBounds.isValid()
        ? QGuiApplication::screenAt(m_anchorBounds.center()) : nullptr;
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect available = screen->availableGeometry();
        resize(size().boundedTo(available.size()));
        if (m_anchorBounds.isValid()) move(m_anchorBounds.center() - rect().center());
        const int nextX = qBound(available.left(), x(), qMax(available.left(), available.right() - width() + 1));
        const int nextY = qBound(available.top(), y(), qMax(available.top(), available.bottom() - height() + 1));
        move(nextX, nextY);
    }
    m_positioned = true;
}

void RelatedDetailPopup::syncWindowDecoration() {
    auto* card = static_cast<DetailCard*>(m_card);
    card->setGeometry(rect());
    const bool decorated = !isMaximized() && !isFullScreen();
    card->setDecorated(decorated);
    applyTheme();
    const int inset = decorated ? PopupShadowMargin : 0;
    for (WindowResizeArea* area : m_resizeAreas) {
        area->setVisible(decorated);
        if (!decorated) continue;
        const QString name = area->objectName();
        if (name == QLatin1String("RelatedResizeLeft"))
            area->setGeometry(inset - 6, inset, 12, height() - 2 * inset);
        else if (name == QLatin1String("RelatedResizeRight"))
            area->setGeometry(width() - inset - 6, inset, 12, height() - 2 * inset);
        else if (name == QLatin1String("RelatedResizeTop"))
            area->setGeometry(inset, inset - 6, width() - 2 * inset, 12);
        else if (name == QLatin1String("RelatedResizeBottom"))
            area->setGeometry(inset, height() - inset - 6, width() - 2 * inset, 12);
        else if (name == QLatin1String("RelatedResizeTopLeft"))
            area->setGeometry(inset - 6, inset - 6, 12, 12);
        else if (name == QLatin1String("RelatedResizeTopRight"))
            area->setGeometry(width() - inset - 6, inset - 6, 12, 12);
        else if (name == QLatin1String("RelatedResizeBottomLeft"))
            area->setGeometry(inset - 6, height() - inset - 6, 12, 12);
        else
            area->setGeometry(width() - inset - 6, height() - inset - 6, 12, 12);
        area->raise();
    }
    card->raiseBorder();
}

void RelatedDetailPopup::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    syncWindowDecoration();
}

void RelatedDetailPopup::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange && !isMinimized()) {
        syncWindowDecoration();
        QTimer::singleShot(0, this, [this]() {
            if (!isMinimized()) syncWindowDecoration();
        });
    }
}

void RelatedDetailPopup::closeEvent(QCloseEvent* event) {
    if (requestClose()) event->accept();
    else event->ignore();
}

bool RelatedDetailPopup::nativeEvent(const QByteArray& eventType, void* message, long* result) {
#ifdef Q_OS_WIN
    if (eventType == QByteArrayLiteral("windows_generic_MSG")) {
        auto* msg = static_cast<MSG*>(message);
        if (msg->message == WM_NCCALCSIZE) {
            *result = 0;
            return true;
        }
        if (msg->message == WM_NCHITTEST && !isMaximized() && !isFullScreen()) {
            RECT frame;
            GetWindowRect(msg->hwnd, &frame);
            const int x = GET_X_LPARAM(msg->lParam) - frame.left;
            const int y = GET_Y_LPARAM(msg->lParam) - frame.top;
            const int w = frame.right - frame.left;
            const int h = frame.bottom - frame.top;
            const int border = qRound(6 * devicePixelRatioF());
            const int inset = qRound(PopupShadowMargin * devicePixelRatioF());
            const bool left = x < border || qAbs(x - inset) < border;
            const bool right = x >= w - border || qAbs(x - (w - inset)) < border;
            const bool top = y < border || qAbs(y - inset) < border;
            const bool bottom = y >= h - border || qAbs(y - (h - inset)) < border;
            if (top && left) *result = HTTOPLEFT;
            else if (top && right) *result = HTTOPRIGHT;
            else if (bottom && left) *result = HTBOTTOMLEFT;
            else if (bottom && right) *result = HTBOTTOMRIGHT;
            else if (left) *result = HTLEFT;
            else if (right) *result = HTRIGHT;
            else if (top) *result = HTTOP;
            else if (bottom) *result = HTBOTTOM;
            else return false;
            return true;
        }
    }
#endif
    return QWidget::nativeEvent(eventType, message, result);
}

bool RelatedDetailPopup::eventFilter(QObject* watched, QEvent* event) {
    if (!isVisible() || !isEnabled() || !m_detail) return QWidget::eventFilter(watched, event);
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget || widget->window() != window()) return QWidget::eventFilter(watched, event);
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool escape = key->key() == Qt::Key_Escape;
        const bool save = key->matches(QKeySequence::Save);
        if ((escape || save) && !QApplication::activePopupWidget()) {
            if (event->type() == QEvent::ShortcutOverride) {
                key->accept();
                return true;
            }
            if (save) {
                m_detail->finishEditing();
                return true;
            }
            if (m_detail->canDismissWithEscape()) {
                requestClose();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool RelatedDetailPopup::focusNextPrevChild(bool next) {
    QWidget* start = QApplication::focusWidget();
    if (!start) start = this;
    QWidget* candidate = start;
    do {
        candidate = next ? candidate->nextInFocusChain() : candidate->previousInFocusChain();
        if (candidate != this && isAncestorOf(candidate) && candidate->isVisible() && candidate->isEnabled()
            && (candidate->focusPolicy() & Qt::TabFocus)) {
            candidate->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
            return true;
        }
    } while (candidate != start);
    setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
    return true;
}
}  // namespace kl
