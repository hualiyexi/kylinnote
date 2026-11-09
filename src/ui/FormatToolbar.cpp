#include "FormatToolbar.h"

#include <QApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QScreen>
#include <QScrollBar>
#include <QSize>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QtGlobal>
#include <QVBoxLayout>
#include <QWidget>

#include "AppIcons.h"
#include "MarkdownEditor.h"
#include "design/ThemeManager.h"

namespace kl {
namespace {

// 阴影留出画布余量：QGraphicsDropShadowEffect 在子控件外扩绘制，
// 顶层窗口用 WA_TranslucentBackground 时才不会被裁掉。
constexpr int kShadowMargin = 14;
constexpr int kIconSize = 16;
constexpr int kPanelWidth = 260;
constexpr int kPanelButtonSize = 34;
constexpr int kBubbleButtonSize = 32;
constexpr int kBubbleHeight = 32;

QString tokenColor(ds::ColorToken token)
{
    return ThemeManager::instance().color(token).name();
}

// 把 QSS 中的 %TOKEN% 占位符换成当前主题语义色；颜色不落字面量。
QString resolveTokens(QString qss)
{
    struct Replacement {
        const char* key;
        ds::ColorToken token;
    };
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

QToolButton* makeFormatButton(MarkdownEditor* editor,
                              MarkdownEditor::FormatAction action,
                              int buttonSize,
                              const QString& objectName,
                              QWidget* parent)
{
    const QString label = editor->formatLabel(action);
    const QString shortcut = editor->formatShortcut(action);
    const QString tip = shortcut.isEmpty()
                            ? label
                            : QStringLiteral("%1 (%2)").arg(label).arg(shortcut);

    auto* button = new QToolButton(parent);
    button->setObjectName(objectName);
    button->setIcon(AppIcons::icon(editor->formatIcon(action), kIconSize));
    button->setIconSize(QSize(kIconSize, kIconSize));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(true);
    button->setFixedSize(buttonSize, buttonSize);
    button->setAccessibleName(label);
    button->setToolTip(tip);
    // Qt 5.12 的 QAction 没有 setAccessibleName；按钮以 toolTip/statusTip 提供等价提示。
    button->setStatusTip(tip);
    button->setProperty("formatAction", static_cast<int>(action));
    return button;
}

// 图标是随主题着色的 SVG，主题切换后必须重新取色。
void refreshButtonIcons(MarkdownEditor* editor, const QVector<QToolButton*>& buttons)
{
    if (!editor) return;
    for (QToolButton* button : buttons) {
        if (!button) continue;
        const auto action = static_cast<MarkdownEditor::FormatAction>(
            button->property("formatAction").toInt());
        button->setIcon(AppIcons::icon(editor->formatIcon(action), kIconSize));
    }
}

void tintShadow(QWidget* card)
{
    if (!card) return;
    auto* effect = qobject_cast<QGraphicsDropShadowEffect*>(card->graphicsEffect());
    if (!effect) return;
    QColor shadow = ThemeManager::instance().color(ds::ColorToken::Text);
    shadow.setAlpha(40);
    effect->setColor(shadow);
}

}  // namespace

// ---------------------------------------------------------------------------
// FormatPanel
// ---------------------------------------------------------------------------

FormatPanel::FormatPanel(MarkdownEditor* editor, QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint)
    , m_editor(editor)
{
    setObjectName(QStringLiteral("FormatPanel"));
    setWindowTitle(QStringLiteral("文本格式"));
    setAttribute(Qt::WA_TranslucentBackground);
    setFrameShape(QFrame::NoFrame);

    buildUi();
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [this](ds::Theme) { applyTheme(); });
}

void FormatPanel::buildUi()
{
    if (!m_editor) return;

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kShadowMargin, kShadowMargin, kShadowMargin, kShadowMargin);
    outer->setSpacing(0);

    m_card = new QWidget(this);
    m_card->setObjectName(QStringLiteral("FormatPanelCard"));
    m_card->setAttribute(Qt::WA_StyledBackground, true);
    m_card->setFixedWidth(kPanelWidth);
    auto* shadow = new QGraphicsDropShadowEffect(m_card);
    shadow->setBlurRadius(18);
    shadow->setOffset(0, 4);
    m_card->setGraphicsEffect(shadow);
    outer->addWidget(m_card);
    setFixedWidth(kPanelWidth + 2 * kShadowMargin);

    auto* cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(10, 10, 10, 10);
    cardLayout->setSpacing(6);

    const QVector<MarkdownEditor::FormatAction> primary = m_editor->primaryFormats();
    const QVector<MarkdownEditor::FormatAction> all = m_editor->allFormats();

    // 高频一行。
    auto* primaryRow = new QHBoxLayout;
    primaryRow->setContentsMargins(0, 0, 0, 0);
    primaryRow->setSpacing(2);
    for (MarkdownEditor::FormatAction action : primary) {
        auto* button = makeFormatButton(m_editor, action, kPanelButtonSize,
                                        QStringLiteral("FormatActionButton"), m_card);
        primaryRow->addWidget(button);
        m_buttons.append(button);
        connect(button, &QToolButton::clicked, this, [this, action]() {
            if (m_editor) m_editor->applyFormat(action);
            hide();
        });
    }
    primaryRow->addStretch();
    cardLayout->addLayout(primaryRow);

    auto* divider = new QWidget(m_card);
    divider->setObjectName(QStringLiteral("FormatPanelDivider"));
    divider->setAttribute(Qt::WA_StyledBackground, true);
    divider->setFixedHeight(1);
    cardLayout->addWidget(divider);

    // 低频按 2 列网格排布。
    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(2);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    int index = 0;
    for (MarkdownEditor::FormatAction action : all) {
        if (primary.contains(action)) continue;
        auto* button = makeFormatButton(m_editor, action, kPanelButtonSize,
                                        QStringLiteral("FormatActionButton"), m_card);
        grid->addWidget(button, index / 2, index % 2, Qt::AlignLeft | Qt::AlignVCenter);
        ++index;
        m_buttons.append(button);
        connect(button, &QToolButton::clicked, this, [this, action]() {
            if (m_editor) m_editor->applyFormat(action);
            hide();
        });
    }
    cardLayout->addLayout(grid);
    layout()->activate();
    adjustSize();
}

void FormatPanel::applyTheme()
{
    if (!m_card) return;
    QString qss = QStringLiteral(R"QSS(
QWidget#FormatPanelCard {
    background-color: %SURFACE%;
    border: 1px solid %BORDER%;
    border-radius: 12px;
}
QToolButton#FormatActionButton {
    color: %TEXT%;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 6px;
    padding: 0;
}
QToolButton#FormatActionButton:hover { background-color: %MENUHOVER%; }
QToolButton#FormatActionButton:pressed { background-color: %PRIMARYSOFT%; }
QToolButton#FormatActionButton:focus { border-color: %PRIMARY%; }
QWidget#FormatPanelDivider { background-color: %DIVIDER%; border: 0; }
)QSS");
    m_card->setStyleSheet(resolveTokens(qss));
    refreshButtonIcons(m_editor, m_buttons);
    tintShadow(m_card);
    m_card->update();
}

void FormatPanel::popupAbove(QWidget* anchor)
{
    if (!anchor) return;
    layout()->activate();
    adjustSize();

    const QPoint anchorTop = anchor->mapToGlobal(QPoint(0, 0));
    const QPoint anchorBottom = anchor->mapToGlobal(QPoint(0, anchor->height()));
    QPoint position(anchorTop.x() + anchor->width() / 2 - width() / 2,
                    anchorTop.y() - height() - 6);

    QScreen* screen = QGuiApplication::screenAt(anchor->mapToGlobal(anchor->rect().center()));
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect area = screen->availableGeometry();
        position.setX(qMax(area.left(), qMin(position.x(), area.right() - width() + 1)));
        if (position.y() < area.top())
            position.setY(anchorBottom.y() + 6);
        position.setY(qMax(area.top(), qMin(position.y(), area.bottom() - height() + 1)));
    }

    move(position);
    show();
    raise();
    setFocus();
}

void FormatPanel::focusOutEvent(QFocusEvent* event)
{
    QFrame::focusOutEvent(event);
    // 延迟一拍再判断：焦点可能只是在卡片内部按钮之间移动，或 clicked 尚未投递；
    // 立即 hide() 会吞掉这次点击。Qt::Popup 也会在窗口失活时自动关闭。
    QTimer::singleShot(0, this, [this]() {
        QWidget* focused = QApplication::focusWidget();
        if (focused && (focused == this || isAncestorOf(focused))) return;
        if (!isVisible()) return;
        hide();
    });
}

// ---------------------------------------------------------------------------
// SelectionBubble
// ---------------------------------------------------------------------------

SelectionBubble::SelectionBubble(MarkdownEditor* editor, QWidget* parent)
    : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint)
    , m_editor(editor)
{
    setObjectName(QStringLiteral("SelectionBubble"));
    setWindowTitle(QStringLiteral("选中文字格式"));
    setAttribute(Qt::WA_TranslucentBackground);
    // 不抢焦点：QTextEdit 的选区必须原样保留给 applyFormat()。
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFrameShape(QFrame::NoFrame);

    buildUi();
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [this](ds::Theme) { applyTheme(); });

    if (editor)
        connect(editor, &MarkdownEditor::selectionChanged,
                this, &SelectionBubble::onSelectionChanged);

    QTextEdit* textEdit = editor ? editor->textEditor() : nullptr;
    if (textEdit) {
        connect(textEdit, &QTextEdit::cursorPositionChanged, this, [this]() {
            if (isVisible()) reposition();
        });
        connect(textEdit, &QTextEdit::textChanged, this, [this]() {
            if (isVisible()) reposition();
        });
        connect(textEdit->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
            if (isVisible()) reposition();
        });
        // 跟随编辑器及其所有祖先窗口的 Move / Resize / Show。
        for (QWidget* widget = textEdit; widget; widget = widget->parentWidget())
            widget->installEventFilter(this);
    }
    hide();
}

void SelectionBubble::buildUi()
{
    if (!m_editor) return;

    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(kShadowMargin, kShadowMargin, kShadowMargin, kShadowMargin);
    outer->setSpacing(0);

    m_card = new QWidget(this);
    m_card->setObjectName(QStringLiteral("SelectionBubbleCard"));
    m_card->setAttribute(Qt::WA_StyledBackground, true);
    m_card->setFixedHeight(kBubbleHeight);
    auto* shadow = new QGraphicsDropShadowEffect(m_card);
    shadow->setBlurRadius(14);
    shadow->setOffset(0, 2);
    m_card->setGraphicsEffect(shadow);
    outer->addWidget(m_card);

    auto* row = new QHBoxLayout(m_card);
    row->setContentsMargins(4, 0, 4, 0);
    row->setSpacing(2);

    const QVector<MarkdownEditor::FormatAction> primary = m_editor->primaryFormats();
    for (MarkdownEditor::FormatAction action : primary) {
        auto* button = makeFormatButton(m_editor, action, kBubbleButtonSize,
                                        QStringLiteral("BubbleFormatButton"), m_card);
        button->setFocusPolicy(Qt::NoFocus);
        row->addWidget(button);
        m_buttons.append(button);
        connect(button, &QToolButton::clicked, this, [this, action]() {
            if (!m_editor) return;
            // applyFormat() 可能连带发出 selectionChanged / textChanged，
            // 短暂屏蔽，避免气泡刚隐藏又被重新弹出。
            m_suppressSelection = true;
            m_editor->applyFormat(action);
            hide();
            QTimer::singleShot(0, this, [this]() { m_suppressSelection = false; });
        });
    }

    auto* divider = new QWidget(m_card);
    divider->setObjectName(QStringLiteral("SelectionBubbleDivider"));
    divider->setAttribute(Qt::WA_StyledBackground, true);
    divider->setFixedSize(1, 18);
    row->addWidget(divider, 0, Qt::AlignVCenter);

    m_moreButton = new QToolButton(m_card);
    m_moreButton->setObjectName(QStringLiteral("BubbleMoreButton"));
    m_moreButton->setIcon(AppIcons::icon(IconId::More, kIconSize));
    m_moreButton->setIconSize(QSize(kIconSize, kIconSize));
    m_moreButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_moreButton->setAutoRaise(true);
    m_moreButton->setFixedSize(kBubbleButtonSize, kBubbleButtonSize);
    m_moreButton->setFocusPolicy(Qt::NoFocus);
    m_moreButton->setAccessibleName(QStringLiteral("更多格式"));
    m_moreButton->setToolTip(QStringLiteral("更多格式"));
    m_moreButton->setStatusTip(QStringLiteral("更多格式"));
    row->addWidget(m_moreButton);
    connect(m_moreButton, &QToolButton::clicked, this, &SelectionBubble::openMorePanel);

    layout()->activate();
    adjustSize();
}

void SelectionBubble::applyTheme()
{
    if (!m_card) return;
    QString qss = QStringLiteral(R"QSS(
QWidget#SelectionBubbleCard {
    background-color: %MENUBG%;
    border: 1px solid %BORDER%;
    border-radius: 8px;
}
QToolButton#BubbleFormatButton, QToolButton#BubbleMoreButton {
    color: %TEXT%;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 6px;
    padding: 0;
}
QToolButton#BubbleFormatButton:hover, QToolButton#BubbleMoreButton:hover {
    background-color: %MENUHOVER%;
}
QToolButton#BubbleFormatButton:pressed, QToolButton#BubbleMoreButton:pressed {
    background-color: %PRIMARYSOFT%;
}
QWidget#SelectionBubbleDivider { background-color: %DIVIDER%; border: 0; }
)QSS");
    m_card->setStyleSheet(resolveTokens(qss));
    refreshButtonIcons(m_editor, m_buttons);
    if (m_moreButton)
        m_moreButton->setIcon(AppIcons::icon(IconId::More, kIconSize));
    tintShadow(m_card);
    m_card->update();
}

void SelectionBubble::onSelectionChanged(bool hasSelection)
{
    if (m_suppressSelection) return;
    if (!hasSelection) {
        hide();
        return;
    }
    if (!updatePosition()) {
        hide();
        return;
    }
    show();
    raise();
}

void SelectionBubble::reposition()
{
    if (isVisible() && !updatePosition())
        hide();
}

bool SelectionBubble::updatePosition()
{
    if (!m_editor || !m_editor->textEditor()) return false;
    QTextEdit* textEdit = m_editor->textEditor();

    const QTextCursor cursor = textEdit->textCursor();
    if (!cursor.hasSelection()) return false;

    // cursorRect() 是视口坐标；用选区首尾并集覆盖多行选区。
    QTextCursor endCursor = cursor;
    endCursor.setPosition(cursor.selectionEnd());
    const QRect selectionRect = textEdit->cursorRect(cursor)
                                    .united(textEdit->cursorRect(endCursor));
    // 选区滚出视口时不再跟随，避免气泡停在屏幕边缘的旧位置。
    if (!textEdit->viewport()->rect().intersects(selectionRect))
        return false;

    const QPoint topLeft = textEdit->viewport()->mapToGlobal(selectionRect.topLeft());
    const QPoint bottomLeft = textEdit->viewport()->mapToGlobal(selectionRect.bottomLeft());

    layout()->activate();
    adjustSize();
    QPoint position(topLeft.x(), topLeft.y() - height() - 6);

    QScreen* screen = QGuiApplication::screenAt(topLeft);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect area = screen->availableGeometry();
        if (position.y() < area.top())
            position.setY(bottomLeft.y() + 6);
        position.setX(qMax(area.left(), qMin(position.x(), area.right() - width() + 1)));
        position.setY(qMax(area.top(), qMin(position.y(), area.bottom() - height() + 1)));
    }
    move(position);
    return true;
}

bool SelectionBubble::eventFilter(QObject* watched, QEvent* event)
{
    if (isVisible()) {
        switch (event->type()) {
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::Show:
            reposition();
            break;
        default:
            break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void SelectionBubble::openMorePanel()
{
    if (!m_moreButton) return;
    if (!m_panel)
        m_panel = new FormatPanel(m_editor, this);
    m_panel->popupAbove(m_moreButton);
}

}  // namespace kl
