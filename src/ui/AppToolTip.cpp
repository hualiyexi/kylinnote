#include "AppToolTip.h"

#include <QAbstractItemView>
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>
#include "design/ThemeManager.h"

namespace kl {
namespace AppToolTip {
namespace {

constexpr int kNoWrapCharacters = 30;
constexpr int kMaxWidth = 248;

int plainTextLength(const QString& text) {
    if (!Qt::mightBeRichText(text)) return text.size();
    QTextDocument document;
    document.setHtml(text);
    return document.toPlainText().size();
}

class Bubble final : public QWidget {
public:
    explicit Bubble()
        : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint) {
        setObjectName(QStringLiteral("AppToolTip"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 3, 8, 3);
        layout->setSpacing(0);
        m_label = new QLabel(this);
        m_label->setObjectName(QStringLiteral("AppToolTipText"));
        m_label->setMinimumHeight(18);
        m_label->setMaximumWidth(248);
        m_label->setWordWrap(true);
        m_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_label->setAttribute(Qt::WA_TransparentForMouseEvents);
        QFont font = m_label->font();
        font.setPixelSize(13);
        m_label->setFont(font);
        layout->addWidget(m_label);
        applyTheme();
    }

    void setText(const QString& text) {
        // 30 字以内保持单行；超过后才启用换行和最大宽度。
        const bool wrap = plainTextLength(text) > kNoWrapCharacters;
        m_label->setWordWrap(wrap);
        m_label->setMaximumWidth(wrap ? kMaxWidth : QWIDGETSIZE_MAX);
        m_label->setTextFormat(Qt::mightBeRichText(text) ? Qt::RichText : Qt::PlainText);
        m_label->setText(text);
        adjustSize();
    }

    void applyTheme() {
        QPalette palette = m_label->palette();
        palette.setColor(QPalette::WindowText,
                         ThemeManager::instance().color(ds::ColorToken::TooltipText));
        m_label->setPalette(palette);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ThemeManager::instance().color(ds::ColorToken::TooltipBg));
        painter.drawRoundedRect(QRectF(rect()), 6.0, 6.0);
    }

private:
    QLabel* m_label = nullptr;
};

bool textFits(const QString& text, const QFontMetrics& metrics, int width) {
    return width <= 0 || metrics.horizontalAdvance(text) <= width;
}

// 可见且完整的同名文字不再弹一次；图标按钮和被省略的文字仍然保留提示。
bool isRedundant(QWidget* widget, const QString& text) {
    if (auto* label = qobject_cast<QLabel*>(widget)) {
        return label->text() == text
            && textFits(label->text(), label->fontMetrics(), label->contentsRect().width());
    }
    if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
        if (button->text() != text) return false;
        if (auto* toolButton = qobject_cast<QToolButton*>(widget)) {
            if (toolButton->toolButtonStyle() == Qt::ToolButtonIconOnly) return false;
        }
        return textFits(text, button->fontMetrics(),
                        button->contentsRect().width() - button->iconSize().width());
    }
    if (auto* combo = qobject_cast<QComboBox*>(widget)) {
        return combo->currentText() == text
            && textFits(text, combo->fontMetrics(), combo->contentsRect().width() - 24);
    }
    if (auto* edit = qobject_cast<QLineEdit*>(widget)) {
        return edit->text() == text
            && textFits(text, edit->fontMetrics(), edit->contentsRect().width());
    }
    return false;
}

class Controller final : public QObject {
public:
    explicit Controller(QApplication* app) : QObject(app), m_app(app) {
        m_bubble = new Bubble;
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this]() { hideText(); });
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
                this, [this]() { if (m_bubble) m_bubble->applyTheme(); });
        m_app->installEventFilter(this);
    }

    QWidget* window() const { return m_bubble; }
    bool isVisible() const { return m_bubble && m_bubble->isVisible(); }

    void showText(const QPoint& globalPos, const QString& text, QWidget* context,
                  const QRect& target, int msecShowTime = -1) {
        if (!m_bubble || text.isEmpty()) {
            hideText();
            return;
        }
        m_context = context;
        m_target = target;
        m_bubble->setText(text);
        QRect anchor;
        if (context && target.isValid() && !target.isEmpty()) {
            anchor = QRect(context->mapToGlobal(target.topLeft()), target.size());
        }
        const QSize size = m_bubble->sizeHint();
        m_bubble->resize(size);
        QPoint position = globalPos + QPoint(0, 12);
        if (anchor.isValid()) {
            position = QPoint(anchor.center().x() - size.width() / 2,
                              anchor.top() - size.height() - 6);
        }
        QScreen* screen = QGuiApplication::screenAt(globalPos);
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (screen) {
            const QRect area = screen->availableGeometry();
            if (position.y() + size.height() > area.bottom())
                position.setY(anchor.isValid() ? anchor.top() - size.height() - 6
                                               : globalPos.y() - size.height() - 12);
            if (anchor.isValid() && position.y() < area.top())
                position.setY(anchor.bottom() + 6);
            position.setX(qBound(area.left(), position.x(),
                                 qMax(area.left(), area.right() - size.width() + 1)));
            position.setY(qBound(area.top(), position.y(),
                                 qMax(area.top(), area.bottom() - size.height() + 1)));
        }
        m_bubble->move(position);
        m_bubble->show();
        m_bubble->raise();
        if (msecShowTime != 0) m_timer.start(msecShowTime < 0 ? 5000 : msecShowTime);
        else m_timer.stop();
    }

    void hideText() {
        m_timer.stop();
        if (m_bubble) m_bubble->hide();
        m_context.clear();
        m_target = QRect();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (m_bubble && watched == m_bubble) return false;
        const auto type = event->type();
        if (type == QEvent::ToolTip) {
            auto* widget = qobject_cast<QWidget*>(watched);
            auto* help = static_cast<QHelpEvent*>(event);
            if (!widget) return false;
            auto* view = qobject_cast<QAbstractItemView*>(widget->parentWidget());
            if (view && widget == view->viewport()) {
                const QModelIndex index = view->indexAt(help->pos());
                if (!index.isValid()) {
                    hideText();
                    return true;
                }
                // 这两个委托有分区命中与省略判断，不能绕过它们的 helpEvent。
                if (view->objectName() == QLatin1String("TaskList")
                    || view->objectName() == QLatin1String("Navigation")) return false;
                const QString text = index.data(Qt::ToolTipRole).toString();
                const QRect target = view->visualRect(index);
                if (text.isEmpty()
                    || (text == index.data(Qt::DisplayRole).toString()
                        && textFits(text, view->fontMetrics(), target.width()))) {
                    hideText();
                    return true;
                }
                showText(help->globalPos(), text, widget, target);
                return true;
            }
            if (auto* menu = qobject_cast<QMenu*>(widget)) {
                QAction* action = menu->activeAction();
                const QString text = action ? action->toolTip() : QString();
                if (!action || text.isEmpty() || text == action->text()) {
                    hideText();
                    return true;
                }
                showText(help->globalPos(), text, menu, menu->actionGeometry(action));
                return true;
            }
            const QString text = widget->toolTip();
            if (text.isEmpty()) return false;
            if (isRedundant(widget, text)) {
                hideText();
                return true;
            }
            showText(help->globalPos(), text, widget, widget->rect());
            return true;
        }
        if (!isVisible()) return false;
        if (type == QEvent::MouseButtonPress || type == QEvent::KeyPress
            || type == QEvent::Wheel || type == QEvent::WindowDeactivate
            || type == QEvent::ApplicationDeactivate) {
            hideText();
            return false;
        }
        if (m_context && watched == m_context) {
            if (type == QEvent::Leave || type == QEvent::Hide || type == QEvent::Destroy) {
                hideText();
                return false;
            }
            if (type == QEvent::MouseMove && m_target.isValid() && !m_target.isEmpty()) {
                auto* mouse = static_cast<QMouseEvent*>(event);
                if (!m_target.contains(mouse->pos())) hideText();
            }
        }
        return false;
    }

private:
    QApplication* m_app = nullptr;
    QPointer<Bubble> m_bubble;
    QPointer<QWidget> m_context;
    QRect m_target;
    QTimer m_timer;
};

Controller& controller() {
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    Q_ASSERT(app);
    static Controller* instance = new Controller(app);
    return *instance;
}

}  // namespace

void install(QApplication& app) {
    Q_UNUSED(app);
    controller();
}

void showText(const QPoint& globalPos, const QString& text, QWidget* context,
              const QRect& target, int msecShowTime) {
    controller().showText(globalPos, text, context, target, msecShowTime);
}

void hideText() { controller().hideText(); }
bool isVisible() { return controller().isVisible(); }
QWidget* window() { return controller().window(); }

}  // namespace AppToolTip
}  // namespace kl
