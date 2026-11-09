#include "FormatToolbar.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QEvent>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QScrollBar>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "AppIcons.h"
#include "MarkdownEditor.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {
constexpr int kShadowMargin = 10;
constexpr int kIconSize = 18;
constexpr int kPanelWidth = 330;
constexpr int kPanelHeight = 36;
constexpr int kPanelButtonSize = 26;
constexpr int kCompactButtonSize = 22;
constexpr int kPaletteArrowWidth = 10;
constexpr int kCardPadding = 7;
constexpr int kPaneInset = 16;
constexpr int kPopupGap = 6;
constexpr int kBubbleButtonSize = 32;
constexpr int kBubbleHeight = 32;

struct HighlightSwatch {
    const char* key;
    const char* label;
    ds::ColorToken token;
};
const HighlightSwatch kSwatches[] = {
    {"yellow", "黄色", ds::ColorToken::HighlightYellow},
    {"green", "绿色", ds::ColorToken::HighlightGreen},
    {"blue", "蓝色", ds::ColorToken::HighlightBlue},
    {"pink", "粉色", ds::ColorToken::HighlightPink},
    {"purple", "紫色", ds::ColorToken::HighlightPurple},
};

QString tokenColor(ds::ColorToken token) {
    return ThemeManager::instance().color(token).name();
}

QString resolveTokens(QString qss) {
    struct Replacement { const char* key; ds::ColorToken token; };
    const Replacement replacements[] = {
        {"%SURFACE%", ds::ColorToken::Surface},
        {"%MENUBG%", ds::ColorToken::MenuBg},
        {"%MENUHOVER%", ds::ColorToken::MenuHover},
        {"%BORDER%", ds::ColorToken::Border},
        {"%DIVIDER%", ds::ColorToken::Divider},
        {"%TEXT%", ds::ColorToken::Text},
        {"%PRIMARY%", ds::ColorToken::Primary},
        {"%PRIMARYSOFT%", ds::ColorToken::PrimarySoft},
    };
    for (const auto& replacement : replacements)
        qss.replace(QLatin1String(replacement.key), tokenColor(replacement.token));
    return qss;
}

QToolButton* makeIconButton(IconId icon, const QString& label, const QString& name,
                            int size, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setObjectName(name);
    button->setIcon(AppIcons::icon(icon, ds::ColorToken::Text, kIconSize));
    button->setIconSize(QSize(kIconSize, kIconSize));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(true);
    button->setFixedSize(size, size);
    button->setFocusPolicy(Qt::NoFocus);
    button->setAccessibleName(label);
    button->setToolTip(label);
    button->setStatusTip(label);
    button->setProperty("iconId", static_cast<int>(icon));
    return button;
}

QToolButton* makeFormatButton(MarkdownEditor* editor, MarkdownEditor::FormatAction action,
                              int size, const QString& name, QWidget* parent) {
    const QString label = editor->formatLabel(action);
    const QString shortcut = editor->formatShortcut(action);
    auto* button = makeIconButton(editor->formatIcon(action), label, name, size, parent);
    if (!shortcut.isEmpty()) {
        const QString tip = QStringLiteral("%1 (%2)").arg(label, shortcut);
        button->setToolTip(tip);
        button->setStatusTip(tip);
    }
    button->setProperty("formatAction", static_cast<int>(action));
    return button;
}

void refreshButtonIcons(const QVector<QToolButton*>& buttons) {
    for (QToolButton* button : buttons) {
        const auto icon = static_cast<IconId>(button->property("iconId").toInt());
        button->setIcon(AppIcons::icon(icon, ds::ColorToken::Text, button->iconSize().width()));
    }
}

void tintShadow(QWidget* card) {
    auto* effect = card ? qobject_cast<QGraphicsDropShadowEffect*>(card->graphicsEffect()) : nullptr;
    if (!effect) return;
    QColor shadow = ThemeManager::instance().color(ds::ColorToken::Text);
    shadow.setAlpha(28);
    effect->setColor(shadow);
}

QIcon swatchIcon(ds::ColorToken token) {
    QPixmap pixmap(kIconSize * 2, kIconSize * 2);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(ThemeManager::instance().color(ds::ColorToken::Border));
    painter.setBrush(ThemeManager::instance().color(token));
    painter.drawRoundedRect(QRectF(1, 1, kIconSize - 2, kIconSize - 2), 3, 3);
    return QIcon(pixmap);
}

QIcon highlighterIcon(const QString& color) {
    ds::ColorToken token = ds::ColorToken::HighlightYellow;
    for (const auto& swatch : kSwatches)
        if (color == QLatin1String(swatch.key)) token = swatch.token;
    QPixmap pixmap(kIconSize * 2, kIconSize * 2);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    AppIcons::paint(&painter, IconId::Highlight, QRectF(0, 0, kIconSize, kIconSize - 4),
                    ThemeManager::instance().color(ds::ColorToken::Text));
    painter.setPen(Qt::NoPen);
    painter.setBrush(ThemeManager::instance().color(token));
    painter.drawRoundedRect(QRectF(1, kIconSize - 3, kIconSize - 2, 3), 1, 1);
    return QIcon(pixmap);
}

// 包括外层 QScrollArea 的 viewport；只检查 QTextEdit 自己会漏掉正文流的裁剪。
QRect visibleGlobalRect(QWidget* widget) {
    if (!widget) return QRect();
    QRect result(widget->mapToGlobal(QPoint()), widget->size());
    for (QWidget* current = widget; current; current = current->parentWidget()) {
        if (!current->isVisible() || current->isMinimized()) return QRect();
        result = result.intersected(QRect(current->mapToGlobal(QPoint()), current->size()));
        if (current->isWindow()) break;
    }
    return result;
}

QWidget* detailPane(MarkdownEditor* editor) {
    if (!editor) return nullptr;
    for (QWidget* widget = editor; widget; widget = widget->parentWidget()) {
        if (widget->objectName() == QLatin1String("TaskDetailHost") ||
            widget->objectName() == QLatin1String("DetailFrame")) return widget;
    }
    return editor->window();
}

QRect popupBounds(MarkdownEditor* editor) {
    QWidget* pane = detailPane(editor);
    QRect bounds = visibleGlobalRect(pane);
    if (bounds.isEmpty()) return QRect();
    QScreen* screen = QGuiApplication::screenAt(bounds.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) bounds = bounds.intersected(screen->availableGeometry());
    bounds.adjust(kPaneInset, kPaneInset, -kPaneInset, -kPaneInset);
    if (QWidget* footer = pane->findChild<QWidget*>(QStringLiteral("DetailFooter"))) {
        if (footer->isVisible())
            bounds.setBottom(qMin(bounds.bottom(), footer->mapToGlobal(QPoint()).y() - kPopupGap));
    }
    return bounds;
}

bool formatPopupForEditor(QWidget* popup, MarkdownEditor* editor) {
    if (!editor) return false;
    for (QWidget* widget = popup; widget; widget = widget->parentWidget()) {
        if (qobject_cast<FormatPanel*>(widget)) {
            QWidget* parent = widget->parentWidget();
            return parent && (parent == editor || parent->isAncestorOf(editor));
        }
    }
    return false;
}

void watchEditorAncestors(MarkdownEditor* editor, QObject* filter) {
    if (!editor || !editor->textEditor()) return;
    editor->textEditor()->viewport()->installEventFilter(filter);
    for (QWidget* widget = editor->textEditor(); widget; widget = widget->parentWidget())
        widget->installEventFilter(filter);
}

bool restoreCursor(MarkdownEditor* editor, const QTextCursor& cursor) {
    if (!editor || !editor->textEditor() || !editor->isVisible()) return false;
    if (!cursor.isNull() && cursor.document() == editor->textEditor()->document())
        editor->textEditor()->setTextCursor(cursor);
    return true;
}
}  // namespace

FormatPanel::FormatPanel(MarkdownEditor* editor, QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), m_editor(editor) {
    setObjectName(QStringLiteral("FormatPanel"));
    setWindowTitle(QStringLiteral("文本格式"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setFrameShape(QFrame::NoFrame);
    setFont(ds::qfont(ds::FontScene::Base));
    buildUi();
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() { applyTheme(); });
    if (editor) {
        connect(editor, &MarkdownEditor::highlightColorChanged, this, [this]() { updateHighlight(); });
        connect(editor, &QObject::destroyed, this, &QWidget::hide);
        if (editor->textEditor()) {
            connect(editor->textEditor(), &QTextEdit::cursorPositionChanged, this, [this]() {
                if (isVisible() && !m_applying && m_editor)
                    m_selection = m_editor->textEditor()->textCursor();
            });
        }
        watchEditorAncestors(editor, this);
    }
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive) hide();
    });
}

void FormatPanel::buildUi() {
    if (!m_editor) return;
    using Action = MarkdownEditor::FormatAction;
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(kShadowMargin, kShadowMargin, kShadowMargin, kShadowMargin);
    outer->setSpacing(0);
    m_card = new QWidget(this);
    m_card->setObjectName(QStringLiteral("FormatPanelCard"));
    m_card->setAttribute(Qt::WA_StyledBackground);
    m_card->setFixedHeight(kPanelHeight);
    auto* shadow = new QGraphicsDropShadowEffect(m_card);
    shadow->setBlurRadius(12);
    shadow->setOffset(0, 1);
    m_card->setGraphicsEffect(shadow);
    outer->addWidget(m_card);
    auto* row = new QHBoxLayout(m_card);
    row->setContentsMargins(kCardPadding, 5, kCardPadding, 5);
    row->setSpacing(0);
    row->setAlignment(Qt::AlignCenter);

    m_overflow = new QMenu(this);
    m_overflow->setObjectName(QStringLiteral("FormatOverflowMenu"));
    m_palette = new QMenu(QStringLiteral("高亮颜色"), this);
    m_palette->setObjectName(QStringLiteral("FormatHighlightPalette"));
    auto* colors = new QActionGroup(m_palette);
    colors->setExclusive(true);
    for (const auto& swatch : kSwatches) {
        const QString key = QString::fromLatin1(swatch.key);
        const QString label = QString::fromUtf8(swatch.label);
        auto* action = m_palette->addAction(swatchIcon(swatch.token), label);
        action->setObjectName(QStringLiteral("Highlight_%1").arg(key));
        action->setData(key);
        action->setCheckable(true);
        action->setToolTip(label + QStringLiteral("高亮"));
        action->setStatusTip(action->toolTip());
        colors->addAction(action);
        m_swatches.append(action);
        connect(action, &QAction::triggered, this, [this, key]() {
            if (!beginAction()) return;
            m_editor->applyHighlight(key);
            finishAction();
        });
    }
    m_palette->setToolTipsVisible(true);

    const auto apply = [this](Action action) {
        if (!beginAction()) return;
        m_editor->applyFormat(action);
        finishAction(action == Action::Link || action == Action::EntityReference);
    };
    const auto addFormat = [this, row, apply](Action action, const QString& name) {
        auto* button = makeFormatButton(m_editor, action, kPanelButtonSize, name, m_card);
        row->addWidget(button);
        m_buttons.append(button);
        connect(button, &QToolButton::clicked, this, [apply, action]() { apply(action); });
        return button;
    };
    const auto addDivider = [this, row]() {
        auto* host = new QWidget(m_card);
        auto* layout = new QHBoxLayout(host);
        layout->setContentsMargins(4, 0, 4, 0);
        layout->setSpacing(0);
        auto* line = new QWidget(host);
        line->setObjectName(QStringLiteral("FormatPanelDivider"));
        line->setAttribute(Qt::WA_StyledBackground);
        line->setFixedSize(1, 18);
        layout->addWidget(line, 0, Qt::AlignVCenter);
        row->addWidget(host);
        m_dividers.append(host);
    };

    auto* expand = makeIconButton(IconId::Expand, QStringLiteral("展开编辑器"),
                                  QStringLiteral("FormatExpand"), kPanelButtonSize, m_card);
    row->addWidget(expand);
    m_buttons.append(expand);
    connect(expand, &QToolButton::clicked, this, [this]() {
        if (!beginAction(false)) return;
        QPointer<MarkdownEditor> editor = m_editor;
        finishAction(true);
        if (editor) emit editor->expandRequested();
    });
    auto* heading = addFormat(Action::Heading, QStringLiteral("FormatHeading"));
    auto* bold = addFormat(Action::Bold, QStringLiteral("FormatBold"));

    m_highlightGroup = new QWidget(m_card);
    auto* highlightLayout = new QHBoxLayout(m_highlightGroup);
    highlightLayout->setContentsMargins(0, 0, 0, 0);
    highlightLayout->setSpacing(0);
    m_highlight = makeFormatButton(m_editor, Action::Highlight, kPanelButtonSize,
                                   QStringLiteral("FormatHighlight"), m_highlightGroup);
    highlightLayout->addWidget(m_highlight);
    m_buttons.append(m_highlight);
    connect(m_highlight, &QToolButton::clicked, this, [apply]() { apply(Action::Highlight); });
    m_paletteButton = makeIconButton(IconId::ChevronDown, QStringLiteral("选择高亮颜色"),
                                     QStringLiteral("FormatHighlightColors"), kPanelButtonSize, m_highlightGroup);
    m_paletteButton->setIconSize(QSize(kPaletteArrowWidth, kPaletteArrowWidth));
    m_paletteButton->setMenu(m_palette);
    m_paletteButton->setPopupMode(QToolButton::InstantPopup);
    highlightLayout->addWidget(m_paletteButton);
    m_buttons.append(m_paletteButton);
    row->addWidget(m_highlightGroup);
    addDivider();

    auto* checklist = makeIconButton(IconId::Checklist, QStringLiteral("添加检查项"),
                                     QStringLiteral("FormatChecklist"), kPanelButtonSize, m_card);
    row->addWidget(checklist);
    m_buttons.append(checklist);
    connect(checklist, &QToolButton::clicked, this, [this]() {
        if (!beginAction()) return;
        QPointer<MarkdownEditor> editor = m_editor;
        finishAction(true);
        if (editor) emit editor->checklistRequested();
    });
    auto* bullets = addFormat(Action::List, QStringLiteral("FormatBullets"));
    auto* numbered = addFormat(Action::OrderedList, QStringLiteral("FormatNumbered"));
    addDivider();
    auto* italic = addFormat(Action::Italic, QStringLiteral("FormatItalic"));
    auto* underline = addFormat(Action::Underline, QStringLiteral("FormatUnderline"));
    auto* strike = addFormat(Action::Strike, QStringLiteral("FormatStrike"));
    auto* more = makeIconButton(IconId::More, QStringLiteral("更多格式"),
                                QStringLiteral("FormatMore"), kPanelButtonSize, m_card);
    more->setMenu(m_overflow);
    more->setPopupMode(QToolButton::InstantPopup);
    row->addWidget(more);
    m_buttons.append(more);

    // 缩窄时先收起尾部样式；始终优先保留结构化检查项和更多入口。
    const QVector<QWidget*> priority = {strike, underline, italic, numbered, bullets,
                                        heading, expand, m_highlightGroup, bold};
    for (QWidget* widget : priority) {
        QAction* action = nullptr;
        if (widget == m_highlightGroup) {
            action = m_overflow->addMenu(m_palette);
        } else {
            auto* button = qobject_cast<QToolButton*>(widget);
            action = m_overflow->addAction(button->icon(), button->accessibleName());
            action->setObjectName(QStringLiteral("Overflow%1").arg(button->objectName()));
            action->setToolTip(button->toolTip());
            action->setProperty("iconId", button->property("iconId"));
            m_menuActions.append(action);
            connect(action, &QAction::triggered, button, &QToolButton::click);
        }
        action->setVisible(false);
        m_overflowItems.append({widget, action});
    }
    m_overflowSeparator = m_overflow->addSeparator();
    m_overflowSeparator->setVisible(false);
    auto* attachment = m_overflow->addAction(AppIcons::icon(IconId::Attachment), QStringLiteral("添加图片或附件"));
    attachment->setObjectName(QStringLiteral("FormatAttachment"));
    attachment->setProperty("iconId", static_cast<int>(IconId::Attachment));
    m_menuActions.append(attachment);
    connect(attachment, &QAction::triggered, this, [this]() {
        if (!beginAction()) return;
        QPointer<MarkdownEditor> editor = m_editor;
        finishAction(true);
        if (editor) emit editor->attachmentRequested();
    });
    for (Action action : {Action::Link, Action::EntityReference, Action::Quote, Action::Code, Action::ClearFormat}) {
        if (action == Action::ClearFormat) m_overflow->addSeparator();
        auto* item = m_overflow->addAction(AppIcons::icon(m_editor->formatIcon(action)), m_editor->formatLabel(action));
        item->setObjectName(QStringLiteral("FormatOverflow_%1").arg(static_cast<int>(action)));
        item->setProperty("iconId", static_cast<int>(m_editor->formatIcon(action)));
        m_menuActions.append(item);
        connect(item, &QAction::triggered, this, [apply, action]() { apply(action); });
    }
    for (QMenu* menu : {m_palette, m_overflow}) {
        connect(menu, &QMenu::aboutToShow, this, [this]() {
            if (m_editor && !m_applying) m_selection = m_editor->textEditor()->textCursor();
        });
    }
    adaptToWidth(kPanelWidth);
}

void FormatPanel::applyTheme() {
    if (!m_card) return;
    setStyleSheet(resolveTokens(QStringLiteral(R"QSS(
QWidget#FormatPanelCard {
    background: %SURFACE%; border: 1px solid %BORDER%; border-radius: 12px;
}
QWidget#FormatPanelCard QToolButton {
    color: %TEXT%; background: transparent; border: 1px solid transparent;
    border-radius: 4px; padding: 0; margin: 0;
}
QWidget#FormatPanelCard QToolButton:hover { background: %MENUHOVER%; }
QWidget#FormatPanelCard QToolButton:pressed { background: %PRIMARYSOFT%; }
QWidget#FormatPanelCard QToolButton:focus { border-color: %PRIMARY%; }
QWidget#FormatPanelCard QToolButton::menu-indicator { image: none; width: 0; }
QWidget#FormatPanelDivider { background: %DIVIDER%; border: 0; }
QMenu#FormatHighlightPalette, QMenu#FormatOverflowMenu {
    color: %TEXT%; background: %MENUBG%; border: 1px solid %BORDER%; border-radius: 8px;
}
)QSS")));
    refreshButtonIcons(m_buttons);
    for (QAction* action : m_menuActions)
        action->setIcon(AppIcons::icon(static_cast<IconId>(action->property("iconId").toInt()), kIconSize));
    for (int i = 0; i < m_swatches.size(); ++i) m_swatches.at(i)->setIcon(swatchIcon(kSwatches[i].token));
    updateHighlight();
    tintShadow(m_card);
}

void FormatPanel::updateHighlight() {
    if (!m_editor || !m_highlight) return;
    const QString color = m_editor->highlightColor();
    m_highlight->setIcon(highlighterIcon(color));
    m_palette->menuAction()->setIcon(m_highlight->icon());
    for (QAction* swatch : m_swatches) {
        const bool current = swatch->data().toString() == color;
        swatch->setChecked(current);
        if (current) {
            const QString tip = QStringLiteral("高亮（%1）").arg(swatch->text());
            m_highlight->setToolTip(tip);
            m_highlight->setAccessibleName(tip);
            m_highlight->setStatusTip(tip);
        }
    }
}

void FormatPanel::adaptToWidth(int width) {
    if (!m_card) return;
    const int extent = width < kPanelWidth ? kCompactButtonSize : kPanelButtonSize;
    for (QToolButton* button : m_buttons) button->setFixedSize(extent, extent);
    m_paletteButton->setFixedSize(kPaletteArrowWidth, extent);
    m_highlightGroup->setFixedSize(extent + kPaletteArrowWidth, extent);
    for (const auto& item : m_overflowItems) {
        item.widget->show();
        item.action->setVisible(false);
    }
    for (QWidget* divider : m_dividers) divider->show();
    const auto updateDividers = [this]() {
        // 尾部三个样式全部进入菜单后，同时收掉相邻分隔线。
        bool trailing = false;
        for (int i = 0; i < 3; ++i) trailing |= !m_overflowItems.at(i).widget->isHidden();
        m_dividers.at(1)->setVisible(trailing);
        bool leading = false;
        for (int i : {5, 6, 7, 8}) leading |= !m_overflowItems.at(i).widget->isHidden();
        m_dividers.at(0)->setVisible(leading);
    };
    bool hasOverflow = false;
    for (const auto& item : m_overflowItems) {
        m_card->layout()->invalidate();
        if (m_card->layout()->minimumSize().width() <= width) break;
        item.widget->hide();
        item.action->setVisible(true);
        hasOverflow = true;
        updateDividers();
    }
    m_overflowSeparator->setVisible(hasOverflow);
    m_card->setFixedWidth(width);
    setFixedSize(width + 2 * kShadowMargin, kPanelHeight + 2 * kShadowMargin);
    layout()->activate();
}

bool FormatPanel::beginAction(bool editing) {
    if (!m_editor || !m_editor->textEditor() || (editing && m_editor->textEditor()->isReadOnly())) return false;
    m_applying = true;
    if (!restoreCursor(m_editor, m_selection)) {
        m_applying = false;
        hide();
        return false;
    }
    return true;
}

void FormatPanel::finishAction(bool closePanel) {
    if (m_editor) m_selection = m_editor->textEditor()->textCursor();
    m_applying = false;
    emit formatApplied();
    if (closePanel) hide();
}

void FormatPanel::popupAbove(QWidget* anchor) {
    if (!anchor || !m_editor || !m_editor->textEditor()) return;
    if (m_anchor && m_anchor != anchor) m_anchor->removeEventFilter(this);
    m_anchor = anchor;
    m_anchor->installEventFilter(this);
    m_anchorGeometry = QRect(anchor->mapToGlobal(QPoint()), anchor->size());
    setAttribute(Qt::WA_NoMouseReplay);
    m_selection = m_editor->textEditor()->textCursor();
    if (!updatePosition()) return;
    show();
    raise();
}

bool FormatPanel::updatePosition() {
    if (!m_card || !m_anchor || !m_editor || !m_editor->isVisible()) return false;
    if (!visibleGlobalRect(m_anchor).isEmpty()) {
        m_anchorGeometry = QRect(m_anchor->mapToGlobal(QPoint()), m_anchor->size());
    } else if (m_anchor->window()->objectName() != QLatin1String("SelectionBubble")) {
        return false;
    }
    const QRect bounds = popupBounds(m_editor);
    if (bounds.width() < 2 * kCompactButtonSize + 2 * kCardPadding || bounds.height() < kPanelHeight) return false;
    adaptToWidth(qMin(kPanelWidth, bounds.width()));
    const QRect anchor = m_anchorGeometry;
    int x = anchor.center().x() - m_card->width() / 2;
    x = qBound(bounds.left(), x, bounds.right() - m_card->width() + 1);
    int y = anchor.top() - kPopupGap - kPanelHeight;
    if (y < bounds.top()) y = anchor.bottom() + kPopupGap;
    y = qBound(bounds.top(), y, bounds.bottom() - kPanelHeight + 1);
    move(x - kShadowMargin, y - kShadowMargin);
    return true;
}

void FormatPanel::scheduleReposition() {
    if (m_repositionPending) return;
    m_repositionPending = true;
    QTimer::singleShot(0, this, [this]() {
        m_repositionPending = false;
        if (isVisible() && !updatePosition()) hide();
    });
}

bool FormatPanel::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::Hide:
        // 应用格式后气泡可以收起，格式岛仍保留在原位置供连续操作。
        if (watched == m_anchor.data() && m_anchor &&
            m_anchor->window()->objectName() == QLatin1String("SelectionBubble")) break;
        hide();
        break;
    case QEvent::Close:
        hide();
        break;
    case QEvent::WindowDeactivate:
        QTimer::singleShot(0, this, [this]() {
            if (m_editor && !m_editor->window()->isActiveWindow() &&
                !formatPopupForEditor(QApplication::activePopupWidget(), m_editor)) hide();
        });
        break;
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::Show:
        if (isVisible()) scheduleReposition();
        break;
    case QEvent::ParentChange:
        watchEditorAncestors(m_editor, this);
        if (isVisible()) scheduleReposition();
        break;
    default:
        break;
    }
    return QFrame::eventFilter(watched, event);
}

void FormatPanel::hideEvent(QHideEvent* event) {
    if (m_palette) m_palette->hide();
    if (m_overflow) m_overflow->hide();
    QFrame::hideEvent(event);
}

void FormatPanel::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        hide();
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

void FormatPanel::mousePressEvent(QMouseEvent* event) {
    // 点击原 A 入口时关闭弹窗，避免重放鼠标事件再次打开它。
    if (m_anchor && visibleGlobalRect(m_anchor).contains(event->globalPos()))
        setAttribute(Qt::WA_NoMouseReplay);
    QFrame::mousePressEvent(event);
}

SelectionBubble::SelectionBubble(MarkdownEditor* editor, QWidget* parent)
    : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint), m_editor(editor) {
    setObjectName(QStringLiteral("SelectionBubble"));
    setWindowTitle(QStringLiteral("选中文字格式"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setFrameShape(QFrame::NoFrame);
    setFont(ds::qfont(ds::FontScene::Base));
    buildUi();
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() { applyTheme(); });
    if (editor) {
        connect(editor, &MarkdownEditor::selectionChanged, this, &SelectionBubble::onSelectionChanged);
        connect(editor, &QObject::destroyed, this, [this]() { dismiss(); });
    }
    if (editor && editor->textEditor()) {
        QTextEdit* edit = editor->textEditor();
        connect(edit, &QTextEdit::cursorPositionChanged, this, [this]() { if (isVisible()) reposition(); });
        connect(edit, &QTextEdit::textChanged, this, [this]() { if (isVisible()) reposition(); });
        connect(edit->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() { if (isVisible()) reposition(); });
        connect(edit->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this]() { if (isVisible()) reposition(); });
        watchEditorAncestors(editor, this);
    }
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive) dismiss();
    });
    hide();
}

void SelectionBubble::buildUi() {
    if (!m_editor) return;
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(kShadowMargin, kShadowMargin, kShadowMargin, kShadowMargin);
    outer->setSpacing(0);
    m_card = new QWidget(this);
    m_card->setObjectName(QStringLiteral("SelectionBubbleCard"));
    m_card->setAttribute(Qt::WA_StyledBackground);
    m_card->setFixedHeight(kBubbleHeight);
    auto* shadow = new QGraphicsDropShadowEffect(m_card);
    shadow->setBlurRadius(12);
    shadow->setOffset(0, 1);
    m_card->setGraphicsEffect(shadow);
    outer->addWidget(m_card);
    auto* row = new QHBoxLayout(m_card);
    row->setContentsMargins(4, 0, 4, 0);
    row->setSpacing(2);
    for (MarkdownEditor::FormatAction action : m_editor->primaryFormats()) {
        auto* button = makeFormatButton(m_editor, action, kBubbleButtonSize,
                                        QStringLiteral("BubbleFormatButton"), m_card);
        row->addWidget(button);
        m_buttons.append(button);
        connect(button, &QToolButton::clicked, this, [this, action]() {
            if (!m_editor || m_editor->textEditor()->isReadOnly()) return;
            m_suppressSelection = true;
            if (restoreCursor(m_editor, m_selection)) m_editor->applyFormat(action);
            hide();
            QTimer::singleShot(0, this, [this]() { m_suppressSelection = false; });
        });
    }
    auto* divider = new QWidget(m_card);
    divider->setObjectName(QStringLiteral("SelectionBubbleDivider"));
    divider->setAttribute(Qt::WA_StyledBackground);
    divider->setFixedSize(1, 18);
    row->addWidget(divider, 0, Qt::AlignVCenter);
    m_moreButton = makeIconButton(IconId::More, QStringLiteral("更多格式"),
                                  QStringLiteral("BubbleMoreButton"), kBubbleButtonSize, m_card);
    row->addWidget(m_moreButton);
    m_buttons.append(m_moreButton);
    connect(m_moreButton, &QToolButton::clicked, this, &SelectionBubble::openMorePanel);
    layout()->activate();
    adjustSize();
}

void SelectionBubble::applyTheme() {
    if (!m_card) return;
    m_card->setStyleSheet(resolveTokens(QStringLiteral(R"QSS(
QWidget#SelectionBubbleCard {
    background: %MENUBG%; border: 1px solid %BORDER%; border-radius: 8px;
}
QToolButton#BubbleFormatButton, QToolButton#BubbleMoreButton {
    color: %TEXT%; background: transparent; border: 1px solid transparent;
    border-radius: 4px; padding: 0;
}
QToolButton#BubbleFormatButton:hover, QToolButton#BubbleMoreButton:hover { background: %MENUHOVER%; }
QToolButton#BubbleFormatButton:pressed, QToolButton#BubbleMoreButton:pressed { background: %PRIMARYSOFT%; }
QWidget#SelectionBubbleDivider { background: %DIVIDER%; border: 0; }
)QSS")));
    refreshButtonIcons(m_buttons);
    tintShadow(m_card);
}

void SelectionBubble::onSelectionChanged(bool hasSelection) {
    if (m_suppressSelection) return;
    if (formatPopupForEditor(QApplication::activePopupWidget(), m_editor)) return;
    if (!hasSelection || !updatePosition()) {
        hide();
        return;
    }
    show();
    raise();
}

void SelectionBubble::reposition() {
    if (isVisible() && !updatePosition()) hide();
}

bool SelectionBubble::updatePosition() {
    if (!m_editor || !m_editor->textEditor()) return false;
    QTextEdit* edit = m_editor->textEditor();
    if (!edit->window()->isActiveWindow() &&
        !formatPopupForEditor(QApplication::activePopupWidget(), m_editor)) return false;
    const QRect viewport = visibleGlobalRect(edit->viewport());
    if (viewport.isEmpty()) return false;
    const QTextCursor cursor = edit->textCursor();
    if (!cursor.hasSelection()) return false;
    QTextCursor start = cursor;
    QTextCursor end = cursor;
    start.setPosition(cursor.selectionStart());
    end.setPosition(cursor.selectionEnd());
    const QRect selection = edit->cursorRect(start).united(edit->cursorRect(end));
    const QRect visibleSelection = QRect(edit->viewport()->mapToGlobal(selection.topLeft()), selection.size())
        .intersected(viewport);
    if (visibleSelection.isEmpty()) return false;
    m_selection = cursor;
    layout()->activate();
    adjustSize();
    const QRect bounds = popupBounds(m_editor);
    if (bounds.width() < m_card->width() || bounds.height() < m_card->height()) return false;
    int x = qBound(bounds.left(), visibleSelection.left(), bounds.right() - m_card->width() + 1);
    int y = visibleSelection.top() - m_card->height() - kPopupGap;
    if (y < bounds.top()) y = visibleSelection.bottom() + kPopupGap;
    y = qBound(bounds.top(), y, bounds.bottom() - m_card->height() + 1);
    move(x - kShadowMargin, y - kShadowMargin);
    return true;
}

void SelectionBubble::dismiss() {
    hide();
    if (m_panel) m_panel->hide();
}

bool SelectionBubble::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::Hide:
    case QEvent::Close:
        dismiss();
        break;
    case QEvent::WindowDeactivate:
        QTimer::singleShot(0, this, [this]() {
            if (m_editor && !m_editor->window()->isActiveWindow() &&
                !formatPopupForEditor(QApplication::activePopupWidget(), m_editor)) dismiss();
        });
        break;
    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::Show:
        if (isVisible()) reposition();
        break;
    case QEvent::ParentChange:
        watchEditorAncestors(m_editor, this);
        reposition();
        break;
    default:
        break;
    }
    return QFrame::eventFilter(watched, event);
}

void SelectionBubble::openMorePanel() {
    if (!m_moreButton || !m_editor) return;
    if (!m_panel) {
        m_panel = new FormatPanel(m_editor, m_editor);
        connect(m_panel, &FormatPanel::formatApplied, this, [this]() {
            m_suppressSelection = true;
            hide();
            QTimer::singleShot(0, this, [this]() { m_suppressSelection = false; });
        });
    }
    m_panel->popupAbove(m_moreButton);
}

}  // namespace kl
