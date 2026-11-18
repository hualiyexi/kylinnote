#include "FormatToolbar.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QScrollBar>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVariant>
#include <functional>

#include "AppIcons.h"
#include "MarkdownEditor.h"
#include "PopupMenu.h"
#include "PopupShadow.h"
#include "platform/SystemShadow.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {
using Action = MarkdownEditor::FormatAction;
using BlockStyle = MarkdownEditor::BlockStyle;
constexpr int kIconSize = 18;
constexpr int kPanelWidth = 304;
constexpr int kPanelHeight = 36;
constexpr int kPanelButtonSize = 26;
constexpr int kCompactButtonSize = 22;
constexpr int kPaletteArrowWidth = 10;
constexpr int kCardPadding = 7;
constexpr int kPaneInset = 16;
constexpr int kPopupGap = 6;
constexpr int kBubbleButtonSize = 24;
constexpr int kBubbleHeight = 32;
constexpr int kTextTypeWidth = 56;
constexpr int kMenuWidth = 194;
constexpr qreal kCardRadius = 10.0;

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
        {"%MENUHOVER%", ds::ColorToken::MenuHover},
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

QString formatTip(MarkdownEditor* editor, Action action) {
    const QString label = editor->formatLabel(action);
    const QString shortcut = editor->formatShortcut(action);
    return shortcut.isEmpty() ? label : QStringLiteral("%1 (%2)").arg(label, shortcut);
}

QToolButton* makeFormatButton(MarkdownEditor* editor, Action action, int size,
                              const QString& name, QWidget* parent) {
    auto* button = makeIconButton(editor->formatIcon(action), editor->formatLabel(action),
                                  name, size, parent);
    button->setToolTip(formatTip(editor, action));
    button->setStatusTip(button->toolTip());
    button->setProperty("formatAction", static_cast<int>(action));
    return button;
}

// 文本类型保留真实按钮的 hover/pressed 状态，文字与末尾下箭头只绘制一次。
class TextTypeButton final : public QToolButton {
public:
    explicit TextTypeButton(QWidget* parent) : QToolButton(parent) {
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(kTextTypeWidth, kBubbleButtonSize);
        setIconSize(QSize(kIconSize, kIconSize));
        setAccessibleName(QStringLiteral("文本类型"));
        setToolTip(accessibleName());
        setStatusTip(accessibleName());
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QStylePainter painter(this);
        QStyleOptionToolButton option;
        initStyleOption(&option);
        option.text.clear();
        option.icon = QIcon();
        option.features = QStyleOptionToolButton::None;
        painter.drawComplexControl(QStyle::CC_ToolButton, option);
        painter.setFont(font());
        painter.setPen(ThemeManager::instance().color(ds::ColorToken::Text));
        const QRect label(6, 0, width() - 22, height());
        painter.drawText(label, Qt::AlignVCenter | Qt::AlignLeft,
                         painter.fontMetrics().elidedText(text(), Qt::ElideRight, label.width()));
        AppIcons::paint(&painter, IconId::ChevronDown,
                        QRectF(width() - 15, (height() - 10) / 2, 10, 10),
                        ThemeManager::instance().color(ds::ColorToken::Text));
    }
};

void refreshButtonIcons(const QVector<QToolButton*>& buttons) {
    for (QToolButton* button : buttons) {
        if (!button->property("iconId").isValid()) continue;
        const auto icon = static_cast<IconId>(button->property("iconId").toInt());
        button->setIcon(AppIcons::icon(icon, ds::ColorToken::Text, button->iconSize().width()));
    }
}

void refreshMenuIcons(const QVector<QAction*>& actions) {
    for (QAction* action : actions) {
        if (!action->property("iconId").isValid()) continue;
        action->setIcon(AppIcons::icon(static_cast<IconId>(action->property("iconId").toInt()), kIconSize));
    }
}

void paintSurface(QWidget* surface, QWidget* card, QImage& shadow, QColor& cachedColor,
                  ds::ColorToken background) {
    if (!card) return;
    auto& theme = ThemeManager::instance();
    const QColor shadowColor = theme.color(ds::ColorToken::Shadow);
    if (shadow.size() != surface->size() || cachedColor != shadowColor) {
        shadow = popupShadow(surface->size(), QRectF(card->geometry()), kCardRadius, shadowColor);
        cachedColor = shadowColor;
    }
    QPainter painter(surface);
    painter.drawImage(0, 0, shadow);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(QRectF(card->geometry()).adjusted(0.5, 0.5, -0.5, -0.5),
                        kCardRadius, kCardRadius);
    painter.fillPath(path, theme.color(background));
    painter.setPen(QPen(theme.color(ds::ColorToken::Border), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
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

bool editorOwnsPopup(MarkdownEditor* editor) {
    return editor && editor->ownsPopup(QApplication::activePopupWidget());
}

void watchEditorAncestors(MarkdownEditor* editor, QObject* filter) {
    if (!editor || !editor->textEditor()) return;
    editor->textEditor()->viewport()->installEventFilter(filter);
    for (QWidget* widget = editor->textEditor(); widget; widget = widget->parentWidget())
        widget->installEventFilter(filter);
}

PopupMenu* newFormatMenu(MarkdownEditor* editor, const QString& name, QWidget* parent) {
    auto* menu = new PopupMenu(parent);
    menu->setObjectName(name);
    menu->setProperty("entityMenu", true);
    menu->setProperty("formatMenu", true);
    menu->setProperty("markdownEditor", QVariant::fromValue<QObject*>(editor));
    menu->setFont(ds::qfont(ds::FontScene::Menu));
    menu->setToolTipsVisible(false);
    menu->setSeparatorsCollapsible(false);
    menu->setMinimumWidth(kMenuWidth + 2 * PopupShadowMargin);
    // 只给有界格式菜单启用 QMenu 自身的滚动，不改变共享菜单的默认观感。
    menu->setStyleSheet(menu->styleSheet() + QStringLiteral(
        "QMenu[formatMenu=\"true\"]{menu-scrollable:1;}"));
    return menu;
}

void limitMenu(PopupMenu* menu, const QRect& bounds) {
    if (!menu || bounds.isEmpty()) return;
    menu->setMinimumWidth(qMin(kMenuWidth, bounds.width()) + 2 * PopupShadowMargin);
    menu->setMaximumWidth(bounds.width() + 2 * PopupShadowMargin);
    menu->setMaximumHeight(bounds.height() + 2 * PopupShadowMargin);
    menu->ensurePolished();
}

void clampMenu(PopupMenu* menu, const QRect& bounds) {
    if (!menu || !menu->isVisible() || bounds.isEmpty()) return;
    menu->resize(qMin(menu->width(), bounds.width() + 2 * PopupShadowMargin),
                 qMin(menu->height(), bounds.height() + 2 * PopupShadowMargin));
    const int width = menu->width() - 2 * PopupShadowMargin;
    const int height = menu->height() - 2 * PopupShadowMargin;
    const int x = qBound(bounds.left(), menu->x() + PopupShadowMargin,
                         qMax(bounds.left(), bounds.right() - width + 1));
    const int y = qBound(bounds.top(), menu->y() + PopupShadowMargin,
                         qMax(bounds.top(), bounds.bottom() - height + 1));
    menu->move(x - PopupShadowMargin, y - PopupShadowMargin);
}

bool popupMenuAbove(PopupMenu* menu, QWidget* anchor, MarkdownEditor* editor) {
    const QRect bounds = popupBounds(editor);
    if (!menu || !anchor || bounds.isEmpty() || visibleGlobalRect(anchor).isEmpty()) return false;
    limitMenu(menu, bounds);
    const QSize hint = menu->sizeHint().boundedTo(
        QSize(bounds.width() + 2 * PopupShadowMargin, bounds.height() + 2 * PopupShadowMargin));
    const int width = hint.width() - 2 * PopupShadowMargin;
    const int height = hint.height() - 2 * PopupShadowMargin;
    const QRect entry(anchor->mapToGlobal(QPoint()), anchor->size());
    const int x = qBound(bounds.left(), entry.left(), qMax(bounds.left(), bounds.right() - width + 1));
    int y = entry.top() - kPopupGap - height;
    if (y < bounds.top()) y = entry.bottom() + 1 + kPopupGap;
    y = qBound(bounds.top(), y, qMax(bounds.top(), bounds.bottom() - height + 1));
    // PopupMenu 自身把窗口平移一个阴影边距，使请求点指向可见卡片。
    menu->popup(QPoint(x, y));
    clampMenu(menu, bounds);
    return menu->isVisible();
}

QAction* addFormatAction(PopupMenu* menu, MarkdownEditor* editor, Action action,
                         const QString& name, const std::function<void(Action)>& apply,
                         QVector<QAction*>& themedActions) {
    auto* item = menu->addAction(AppIcons::icon(editor->formatIcon(action), kIconSize),
                                  editor->formatLabel(action));
    item->setObjectName(name);
    item->setProperty("formatAction", static_cast<int>(action));
    item->setProperty("iconId", static_cast<int>(editor->formatIcon(action)));
    item->setProperty("trailingText", editor->formatShortcut(action));
    item->setToolTip(formatTip(editor, action));
    item->setStatusTip(item->toolTip());
    themedActions.append(item);
    QObject::connect(item, &QAction::triggered, menu, [apply, action]() { apply(action); });
    return item;
}

void addBlockStyles(PopupMenu* menu, MarkdownEditor* editor, const QString& prefix,
                     const std::function<void(BlockStyle)>& apply, QVector<QAction*>& themedActions) {
    menu->setTitle(QStringLiteral("文本类型"));
    menu->setIcon(AppIcons::icon(editor->blockStyleIcon(BlockStyle::Paragraph), kIconSize));
    menu->menuAction()->setProperty("iconId", static_cast<int>(editor->blockStyleIcon(BlockStyle::Paragraph)));
    themedActions.append(menu->menuAction());
    for (BlockStyle style : editor->blockStyles()) {
        auto* item = menu->addAction(AppIcons::icon(editor->blockStyleIcon(style), kIconSize),
                                      editor->blockStyleLabel(style));
        item->setObjectName(prefix + QString::number(static_cast<int>(style)));
        item->setProperty("blockStyle", static_cast<int>(style));
        item->setProperty("iconId", static_cast<int>(editor->blockStyleIcon(style)));
        item->setToolTip(item->text());
        themedActions.append(item);
        QObject::connect(item, &QAction::triggered, menu, [apply, style]() { apply(style); });
    }
}

void addCoreActions(PopupMenu* menu, MarkdownEditor* editor, const QString& prefix,
                     const std::function<void(Action)>& apply, const std::function<void()>& attach,
                     QVector<QAction*>& themedActions) {
    auto* attachment = menu->addAction(AppIcons::icon(IconId::Attachment, kIconSize),
                                        QStringLiteral("添加图片或附件"));
    attachment->setObjectName(prefix + QStringLiteral("Attachment"));
    attachment->setProperty("iconId", static_cast<int>(IconId::Attachment));
    attachment->setToolTip(attachment->text());
    themedActions.append(attachment);
    QObject::connect(attachment, &QAction::triggered, menu, attach);
    for (Action action : {Action::Link, Action::EntityReference, Action::Quote, Action::Code, Action::ClearFormat}) {
        if (action == Action::ClearFormat) {
            auto* divider = menu->addSeparator();
            divider->setObjectName(prefix + QStringLiteral("ClearDivider"));
            divider->setToolTip(QStringLiteral("命令分组"));
        }
        addFormatAction(menu, editor, action,
                        prefix + QStringLiteral("Overflow_%1").arg(static_cast<int>(action)), apply, themedActions);
    }
}
}  // namespace

void FormatSelection::capture(MarkdownEditor* editor) {
    valid = editor && editor->textEditor();
    if (!valid) return;
    offsets = editor->sourceSelection();
    generation = editor->contextGeneration();
}

bool FormatSelection::current(MarkdownEditor* editor) const {
    return valid && editor && editor->textEditor() && generation == editor->contextGeneration();
}

bool FormatSelection::restore(MarkdownEditor* editor) const {
    if (!current(editor) || !editor->isVisible()) return false;
    editor->selectSource(offsets.first, offsets.second);
    return current(editor);
}

FormatPanel::FormatPanel(MarkdownEditor* editor, QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), m_editor(editor) {
    setObjectName(QStringLiteral("FormatPanel"));
    setWindowTitle(QStringLiteral("文本格式"));
    setProperty("markdownEditor", QVariant::fromValue<QObject*>(editor));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setFrameShape(QFrame::NoFrame);
    setFont(ds::qfont(ds::FontScene::Base));
    disableNativeWindowShadows();
    buildUi();
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() { applyTheme(); });
    if (editor) {
        connect(editor, &MarkdownEditor::highlightColorChanged, this, [this]() { updateHighlight(); });
        connect(editor, &QObject::destroyed, this, &QWidget::hide);
        if (editor->textEditor()) {
            connect(editor->textEditor(), &QTextEdit::cursorPositionChanged, this, [this]() {
                if (!isVisible() || m_applying) return;
                if (!m_selection.current(m_editor)) { m_selection.clear(); hide(); }
            });
            connect(editor->textEditor(), &QTextEdit::textChanged, this, [this]() {
                if (m_selection.valid && !m_selection.current(m_editor)) { m_selection.clear(); hide(); }
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
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(PopupShadowMargin, PopupShadowMargin, PopupShadowMargin, PopupShadowMargin);
    outer->setSpacing(0);
    m_card = new QWidget(this);
    m_card->setObjectName(QStringLiteral("FormatPanelCard"));
    m_card->setFixedHeight(kPanelHeight);
    outer->addWidget(m_card);
    auto* row = new QHBoxLayout(m_card);
    row->setContentsMargins(kCardPadding, 5, kCardPadding, 5);
    row->setSpacing(0);
    row->setAlignment(Qt::AlignCenter);
    m_overflow = newFormatMenu(m_editor, QStringLiteral("FormatOverflowMenu"), this);
    m_palette = newFormatMenu(m_editor, QStringLiteral("FormatHighlightPalette"), this);
    m_palette->setTitle(QStringLiteral("高亮颜色"));
    m_blockStyles = newFormatMenu(m_editor, QStringLiteral("FormatBlockStyles"), this);

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
    const auto apply = [this](Action action) {
        if (!beginAction()) return;
        m_editor->applyFormat(action);
        finishAction(action == Action::Link || action == Action::EntityReference);
    };
    addBlockStyles(m_blockStyles, m_editor, QStringLiteral("FormatBlock_"), [this](BlockStyle style) {
        if (!beginAction()) return;
        m_editor->applyBlockStyle(style);
        finishAction();
    }, m_menuActions);
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
    auto* heading = makeIconButton(IconId::Heading, QStringLiteral("文本类型"),
                                   QStringLiteral("FormatHeading"), kPanelButtonSize, m_card);
    row->addWidget(heading);
    m_buttons.append(heading);
    connect(heading, &QToolButton::clicked, this, [this, heading]() { openMenu(m_blockStyles, heading); });
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
    connect(m_paletteButton, &QToolButton::clicked, this, [this]() { openMenu(m_palette, m_paletteButton); });
    highlightLayout->addWidget(m_paletteButton);
    m_buttons.append(m_paletteButton);
    row->addWidget(m_highlightGroup);
    addDivider();
    auto* bullets = addFormat(Action::List, QStringLiteral("FormatBullets"));
    auto* numbered = addFormat(Action::OrderedList, QStringLiteral("FormatNumbered"));
    addDivider();
    auto* italic = addFormat(Action::Italic, QStringLiteral("FormatItalic"));
    auto* underline = addFormat(Action::Underline, QStringLiteral("FormatUnderline"));
    auto* strike = addFormat(Action::Strike, QStringLiteral("FormatStrike"));
    auto* more = makeIconButton(IconId::More, QStringLiteral("更多格式"),
                                QStringLiteral("FormatMore"), kPanelButtonSize, m_card);
    row->addWidget(more);
    m_buttons.append(more);
    connect(more, &QToolButton::clicked, this, [this, more]() { openMenu(m_overflow, more); });

    // 动作在菜单中的顺序与横向行一致；收起优先级独立，始终保留更多入口。
    const QVector<QWidget*> order = {expand, heading, bold, m_highlightGroup, bullets, numbered, italic, underline, strike};
    QVector<QAction*> collapsed;
    for (QWidget* widget : order) {
        QAction* action = nullptr;
        if (widget == m_highlightGroup) action = m_overflow->addMenu(m_palette);
        else if (widget == heading) action = m_overflow->addMenu(m_blockStyles);
        else {
            auto* button = qobject_cast<QToolButton*>(widget);
            action = m_overflow->addAction(button->icon(), button->accessibleName());
            action->setObjectName(QStringLiteral("Overflow%1").arg(button->objectName()));
            action->setToolTip(button->toolTip());
            action->setProperty("iconId", button->property("iconId"));
            if (button->property("formatAction").isValid()) {
                action->setProperty("formatAction", button->property("formatAction"));
                action->setProperty("trailingText", m_editor->formatShortcut(
                    static_cast<Action>(button->property("formatAction").toInt())));
            }
            m_menuActions.append(action);
            connect(action, &QAction::triggered, button, &QToolButton::click);
        }
        action->setVisible(false);
        collapsed.append(action);
    }
    for (int index : {8, 7, 6, 5, 4, 1, 0, 3, 2}) m_overflowItems.append({order.at(index), collapsed.at(index)});
    m_overflowSeparator = m_overflow->addSeparator();
    m_overflowSeparator->setObjectName(QStringLiteral("FormatCollapsedDivider"));
    m_overflowSeparator->setVisible(false);
    addCoreActions(m_overflow, m_editor, QStringLiteral("Format"), apply, [this]() {
        if (!beginAction()) return;
        QPointer<MarkdownEditor> editor = m_editor;
        finishAction(true);
        if (editor) emit editor->attachmentRequested();
    }, m_menuActions);
    for (PopupMenu* menu : {m_palette, m_overflow, m_blockStyles}) wireMenu(menu);
    adaptToWidth(kPanelWidth);
}

void FormatPanel::applyTheme() {
    if (!m_card) return;
    setStyleSheet(resolveTokens(QStringLiteral(R"QSS(
QWidget#FormatPanelCard { background: transparent; border: 0; }
QWidget#FormatPanelCard QToolButton {
    color: %TEXT%; background: transparent; border: 1px solid transparent;
    border-radius: 4px; padding: 0; margin: 0;
}
QWidget#FormatPanelCard QToolButton:hover { background: %MENUHOVER%; }
QWidget#FormatPanelCard QToolButton:pressed { background: %PRIMARYSOFT%; }
QWidget#FormatPanelCard QToolButton:focus { border-color: transparent; background: %MENUHOVER%; }
QWidget#FormatPanelDivider { background: %DIVIDER%; border: 0; }
)QSS")));
    refreshButtonIcons(m_buttons);
    refreshMenuIcons(m_menuActions);
    for (int i = 0; i < m_swatches.size(); ++i) m_swatches.at(i)->setIcon(swatchIcon(kSwatches[i].token));
    updateHighlight();
    m_shadow = QImage();
    update();
}

void FormatPanel::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    paintSurface(this, m_card, m_shadow, m_shadowColor, ds::ColorToken::Surface);
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
        bool trailing = false;
        for (int i = 0; i < 3; ++i) trailing |= !m_overflowItems.at(i).widget->isHidden();
        m_dividers.at(1)->setVisible(trailing);
        bool leading = false;
        for (int i : {5, 6, 7, 8}) leading |= !m_overflowItems.at(i).widget->isHidden();
        bool lists = false;
        for (int i : {3, 4}) lists |= !m_overflowItems.at(i).widget->isHidden();
        m_dividers.at(0)->setVisible(leading && (lists || trailing));
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
    setFixedSize(width + 2 * PopupShadowMargin, kPanelHeight + 2 * PopupShadowMargin);
    layout()->activate();
    update();
}

bool FormatPanel::menusActive() const {
    return m_menuActive || (m_palette && m_palette->isVisible()) ||
        (m_overflow && m_overflow->isVisible()) || (m_blockStyles && m_blockStyles->isVisible());
}

void FormatPanel::wireMenu(PopupMenu* menu) {
    connect(menu, &QMenu::aboutToShow, this, [this, menu]() {
        m_menuActive = true;
        limitMenu(menu, popupBounds(m_editor));
        QTimer::singleShot(0, this, [this, menu]() {
            if (!m_selection.current(m_editor)) { m_selection.clear(); hide(); menu->hide(); return; }
            clampMenu(menu, popupBounds(m_editor));
        });
    });
    connect(menu, &QMenu::aboutToHide, this, [this]() {
        QTimer::singleShot(0, this, [this]() {
            m_menuActive = (m_palette && m_palette->isVisible()) ||
                (m_overflow && m_overflow->isVisible()) || (m_blockStyles && m_blockStyles->isVisible());
            if (m_selection.valid && !m_selection.current(m_editor)) { m_selection.clear(); hide(); }
            else if (isVisible() && !m_menuActive) scheduleReposition();
        });
    });
}

void FormatPanel::openMenu(PopupMenu* menu, QWidget* anchor) {
    if (!m_selection.current(m_editor)) { m_selection.clear(); hide(); return; }
    m_menuActive = true;
    if (!popupMenuAbove(menu, anchor, m_editor)) m_menuActive = false;
}

bool FormatPanel::beginAction(bool editing) {
    if (!m_editor || !m_editor->textEditor() ||
        (editing && m_editor->textEditor()->isReadOnly())) return false;
    m_applying = true;
    if (!m_selection.restore(m_editor)) {
        m_applying = false;
        m_selection.clear();
        hide();
        return false;
    }
    return true;
}

void FormatPanel::finishAction(bool closePanel) {
    const bool sameContext = m_selection.current(m_editor);
    if (sameContext) m_selection.capture(m_editor);
    else m_selection.clear();
    m_applying = false;
    if (sameContext) emit formatApplied();
    if (closePanel || !sameContext) hide();
}

void FormatPanel::popupAbove(QWidget* anchor) {
    if (!anchor || !m_editor || !m_editor->textEditor()) return;
    if (m_anchor && m_anchor != anchor) m_anchor->removeEventFilter(this);
    m_anchor = anchor;
    m_anchor->installEventFilter(this);
    m_anchorGeometry = QRect(anchor->mapToGlobal(QPoint()), anchor->size());
    setAttribute(Qt::WA_NoMouseReplay);
    m_selection.capture(m_editor);
    if (!updatePosition()) { hide(); return; }
    show();
    raise();
}

bool FormatPanel::updatePosition() {
    if (!m_card || !m_anchor || !m_selection.current(m_editor) ||
        !m_editor->isVisible() || visibleGlobalRect(m_anchor).isEmpty()) return false;
    m_anchorGeometry = QRect(m_anchor->mapToGlobal(QPoint()), m_anchor->size());
    const QRect bounds = popupBounds(m_editor);
    if (bounds.width() < kCompactButtonSize + 2 * kCardPadding || bounds.height() < kPanelHeight) return false;
    adaptToWidth(qMin(kPanelWidth, bounds.width()));
    const QRect anchor = m_anchorGeometry;
    const int x = qBound(bounds.left(), anchor.center().x() - m_card->width() / 2,
                         bounds.right() - m_card->width() + 1);
    int y = anchor.top() - kPopupGap - kPanelHeight;
    if (y < bounds.top()) y = anchor.bottom() + 1 + kPopupGap;
    y = qBound(bounds.top(), y, bounds.bottom() - kPanelHeight + 1);
    move(x - PopupShadowMargin, y - PopupShadowMargin);
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
    case QEvent::Close:
        hide();
        break;
    case QEvent::WindowDeactivate:
        QTimer::singleShot(0, this, [this]() {
            if (m_editor && !m_editor->window()->isActiveWindow() && !menusActive() && !editorOwnsPopup(m_editor)) hide();
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
    if (m_blockStyles) m_blockStyles->hide();
    m_menuActive = false;
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
    setProperty("markdownEditor", QVariant::fromValue<QObject*>(editor));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setFrameShape(QFrame::NoFrame);
    setFont(ds::qfont(ds::FontScene::Menu));
    disableNativeWindowShadows();
    buildUi();
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() { applyTheme(); });
    if (editor) {
        connect(editor, &MarkdownEditor::selectionChanged, this, &SelectionBubble::onSelectionChanged);
        connect(editor, &QObject::destroyed, this, [this]() { dismiss(); });
        connect(editor, &MarkdownEditor::highlightColorChanged, this, [this]() { applyTheme(); });
    }
    if (editor && editor->textEditor()) {
        QTextEdit* edit = editor->textEditor();
        connect(edit, &QTextEdit::cursorPositionChanged, this, [this]() { if (isVisible()) scheduleReposition(); });
        connect(edit, &QTextEdit::textChanged, this, [this]() {
            if (m_selection.valid && !m_selection.current(m_editor)) dismiss();
            else if (isVisible() && !m_applying) scheduleReposition();
        });
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
    outer->setContentsMargins(PopupShadowMargin, PopupShadowMargin, PopupShadowMargin, PopupShadowMargin);
    outer->setSpacing(0);
    m_card = new QWidget(this);
    m_card->setObjectName(QStringLiteral("SelectionBubbleCard"));
    m_card->setFixedHeight(kBubbleHeight);
    outer->addWidget(m_card);
    auto* row = new QHBoxLayout(m_card);
    // 32px 卡片 = 4px 上边距 + 24px 实际按钮 + 4px 下边距；18px 字形上下各 7px。
    row->setContentsMargins(4, 4, 4, 4);
    row->setSpacing(2);
    m_overflow = newFormatMenu(m_editor, QStringLiteral("SelectionOverflowMenu"), this);
    m_blockStyles = newFormatMenu(m_editor, QStringLiteral("SelectionBlockStyles"), this);
    const auto apply = [this](Action action) {
        if (!beginAction()) return;
        m_editor->applyFormat(action);
        finishAction();
    };
    addBlockStyles(m_blockStyles, m_editor, QStringLiteral("BubbleBlock_"), [this](BlockStyle style) {
        if (!beginAction()) return;
        m_editor->applyBlockStyle(style);
        finishAction();
    }, m_menuActions);
    m_textType = new TextTypeButton(m_card);
    m_textType->setObjectName(QStringLiteral("BubbleTextType"));
    m_textType->setText(m_editor->blockStyleLabel(BlockStyle::Paragraph));
    row->addWidget(m_textType);
    m_buttons.append(m_textType);
    connect(m_textType, &QToolButton::clicked, this, [this]() { openMenu(m_blockStyles, m_textType); });
    m_typeOverflow = m_overflow->addMenu(m_blockStyles);
    m_typeOverflow->setVisible(false);

    for (Action action : m_editor->primaryFormats()) {
        auto* button = makeFormatButton(m_editor, action, kBubbleButtonSize,
                                        QStringLiteral("BubbleFormatButton"), m_card);
        row->addWidget(button);
        m_buttons.append(button);
        connect(button, &QToolButton::clicked, this, [apply, action]() { apply(action); });
        // Link / Code 已在固定菜单中，不为窄宽度生成重复项。
        QAction* item = nullptr;
        if (action != Action::Link && action != Action::Code) {
            item = addFormatAction(m_overflow, m_editor, action,
                                    QStringLiteral("BubbleCollapsed_%1").arg(static_cast<int>(action)), apply, m_menuActions);
            item->setVisible(false);
        }
        m_overflowItems.prepend({button, item});
    }
    m_overflowSeparator = m_overflow->addSeparator();
    m_overflowSeparator->setObjectName(QStringLiteral("BubbleCollapsedDivider"));
    m_overflowSeparator->setVisible(false);
    addCoreActions(m_overflow, m_editor, QStringLiteral("Bubble"), apply, [this]() {
        if (!beginAction()) return;
        QPointer<MarkdownEditor> editor = m_editor;
        finishAction();
        if (editor) emit editor->attachmentRequested();
    }, m_menuActions);
    m_divider = new QWidget(m_card);
    m_divider->setObjectName(QStringLiteral("SelectionBubbleDivider"));
    m_divider->setAttribute(Qt::WA_StyledBackground);
    m_divider->setFixedSize(1, 18);
    row->addWidget(m_divider, 0, Qt::AlignVCenter);
    m_moreButton = makeIconButton(IconId::More, QStringLiteral("更多格式"),
                                  QStringLiteral("BubbleMoreButton"), kBubbleButtonSize, m_card);
    row->addWidget(m_moreButton);
    m_buttons.append(m_moreButton);
    connect(m_moreButton, &QToolButton::clicked, this, &SelectionBubble::openMoreMenu);
    for (PopupMenu* menu : {m_overflow, m_blockStyles}) wireMenu(menu);
    adaptToWidth(kPanelWidth);
}

void SelectionBubble::applyTheme() {
    if (!m_card) return;
    setStyleSheet(resolveTokens(QStringLiteral(R"QSS(
QWidget#SelectionBubbleCard { background: transparent; border: 0; }
QWidget#SelectionBubbleCard QToolButton {
    color: %TEXT%; background: transparent; border: 1px solid transparent;
    border-radius: 4px; padding: 0; margin: 0;
}
QWidget#SelectionBubbleCard QToolButton:hover { background: %MENUHOVER%; }
QWidget#SelectionBubbleCard QToolButton:pressed { background: %PRIMARYSOFT%; }
QWidget#SelectionBubbleCard QToolButton:focus { border-color: transparent; background: %MENUHOVER%; }
QWidget#SelectionBubbleDivider { background: %DIVIDER%; border: 0; }
)QSS")));
    refreshButtonIcons(m_buttons);
    refreshMenuIcons(m_menuActions);
    if (m_editor) {
        for (QToolButton* button : m_buttons)
            if (button->property("formatAction").isValid() &&
                static_cast<Action>(button->property("formatAction").toInt()) == Action::Highlight)
                button->setIcon(highlighterIcon(m_editor->highlightColor()));
    }
    m_shadow = QImage();
    m_textType->update();
    update();
}

void SelectionBubble::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    paintSurface(this, m_card, m_shadow, m_shadowColor, ds::ColorToken::MenuBg);
}

void SelectionBubble::adaptToWidth(int width) {
    if (!m_card) return;
    m_textType->show();
    m_typeOverflow->setVisible(false);
    m_divider->show();
    for (const auto& item : m_overflowItems) {
        item.widget->show();
        if (item.action) item.action->setVisible(false);
    }
    bool hasCollapsed = false;
    for (const auto& item : m_overflowItems) {
        m_card->layout()->invalidate();
        if (m_card->layout()->minimumSize().width() <= width) break;
        item.widget->hide();
        if (item.action) { item.action->setVisible(true); hasCollapsed = true; }
    }
    m_card->layout()->invalidate();
    if (m_card->layout()->minimumSize().width() > width) {
        m_textType->hide();
        m_typeOverflow->setVisible(true);
        hasCollapsed = true;
        m_divider->hide();
    }
    m_overflowSeparator->setVisible(hasCollapsed);
    m_card->layout()->invalidate();
    const int fittedWidth = qMin(width, m_card->layout()->minimumSize().width());
    m_card->setFixedWidth(fittedWidth);
    setFixedSize(fittedWidth + 2 * PopupShadowMargin, kBubbleHeight + 2 * PopupShadowMargin);
    layout()->activate();
    update();
}

bool SelectionBubble::menusActive() const {
    return m_menuActive || (m_overflow && m_overflow->isVisible()) || (m_blockStyles && m_blockStyles->isVisible());
}

void SelectionBubble::wireMenu(PopupMenu* menu) {
    connect(menu, &QMenu::aboutToShow, this, [this, menu]() {
        m_menuActive = true;
        limitMenu(menu, popupBounds(m_editor));
        QTimer::singleShot(0, this, [this, menu]() {
            if (!m_selection.current(m_editor)) { dismiss(); menu->hide(); return; }
            clampMenu(menu, popupBounds(m_editor));
        });
    });
    connect(menu, &QMenu::aboutToHide, this, [this]() {
        QTimer::singleShot(0, this, [this]() {
            m_menuActive = (m_overflow && m_overflow->isVisible()) || (m_blockStyles && m_blockStyles->isVisible());
            if (m_selection.valid && !m_selection.current(m_editor)) dismiss();
            else if (isVisible() && !m_applying && !m_menuActive) reposition();
        });
    });
}

void SelectionBubble::openMenu(PopupMenu* menu, QWidget* anchor) {
    if (!m_selection.current(m_editor)) { dismiss(); return; }
    m_menuActive = true;
    if (!popupMenuAbove(menu, anchor, m_editor)) m_menuActive = false;
}

void SelectionBubble::openMoreMenu() {
    if (m_moreButton) openMenu(m_overflow, m_moreButton);
}

bool SelectionBubble::beginAction(bool editing) {
    if (!m_editor || !m_editor->textEditor() ||
        (editing && m_editor->textEditor()->isReadOnly())) return false;
    m_applying = true;
    m_suppressSelection = true;
    if (!m_selection.restore(m_editor)) {
        m_applying = false;
        m_suppressSelection = false;
        dismiss();
        return false;
    }
    return true;
}

void SelectionBubble::finishAction() {
    if (m_selection.current(m_editor)) m_selection.capture(m_editor);
    else m_selection.clear();
    hide();
    m_applying = false;
    QTimer::singleShot(0, this, [this]() { m_suppressSelection = false; });
}

void SelectionBubble::onSelectionChanged(bool hasSelection) {
    if (m_suppressSelection || m_applying) return;
    if (menusActive() || editorOwnsPopup(m_editor)) {
        if (m_selection.valid && !m_selection.current(m_editor)) dismiss();
        return;
    }
    if (!hasSelection || !m_editor || !m_editor->textEditor()->hasFocus()) { dismiss(); return; }
    m_selection.capture(m_editor);
    if (!updatePosition()) { dismiss(); return; }
    show();
    raise();
}

void SelectionBubble::scheduleReposition() {
    if (m_repositionPending) return;
    m_repositionPending = true;
    QTimer::singleShot(0, this, [this]() {
        m_repositionPending = false;
        reposition();
    });
}

void SelectionBubble::reposition() {
    if (m_applying || m_suppressSelection) return;
    if (isVisible() && !updatePosition()) dismiss();
}

bool SelectionBubble::updatePosition() {
    if (!m_selection.current(m_editor)) return false;
    QTextEdit* edit = m_editor->textEditor();
    if ((!edit->window()->isActiveWindow() || !edit->hasFocus()) && !menusActive() && !editorOwnsPopup(m_editor)) return false;
    const QRect viewport = visibleGlobalRect(edit->viewport());
    if (viewport.isEmpty()) return false;
    const QTextCursor cursor = edit->textCursor();
    // 自有菜单的焦点交接期间不拿短暂的呈现游标覆盖源码选区。
    if (!cursor.hasSelection()) return menusActive();
    QTextCursor start = cursor;
    QTextCursor end = cursor;
    start.setPosition(cursor.selectionStart());
    end.setPosition(cursor.selectionEnd());
    const QRect selection = edit->cursorRect(start).united(edit->cursorRect(end));
    const QRect visibleSelection = QRect(edit->viewport()->mapToGlobal(selection.topLeft()), selection.size()).intersected(viewport);
    if (visibleSelection.isEmpty()) return false;
    const QRect bounds = popupBounds(m_editor);
    if (bounds.width() < kBubbleButtonSize + 8 || bounds.height() < kBubbleHeight) return false;
    adaptToWidth(bounds.width());
    const int x = qBound(bounds.left(), visibleSelection.left(), bounds.right() - m_card->width() + 1);
    int y = visibleSelection.top() - m_card->height() - kPopupGap;
    if (y < bounds.top()) y = visibleSelection.bottom() + 1 + kPopupGap;
    y = qBound(bounds.top(), y, bounds.bottom() - m_card->height() + 1);
    move(x - PopupShadowMargin, y - PopupShadowMargin);
    for (PopupMenu* menu : {m_overflow, m_blockStyles}) clampMenu(menu, bounds);
    return true;
}

void SelectionBubble::dismiss() {
    hide();
    if (m_overflow) m_overflow->hide();
    if (m_blockStyles) m_blockStyles->hide();
    if (!m_applying) m_selection.clear();
}

void SelectionBubble::hideEvent(QHideEvent* event) {
    if (m_overflow) m_overflow->hide();
    if (m_blockStyles) m_blockStyles->hide();
    m_menuActive = false;
    QFrame::hideEvent(event);
}

bool SelectionBubble::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::Hide:
    case QEvent::Close:
        dismiss();
        break;
    case QEvent::FocusIn:
        if (m_editor && watched == m_editor->textEditor())
            QTimer::singleShot(0, this, [this]() {
                if (m_editor && !m_suppressSelection) onSelectionChanged(m_editor->textEditor()->textCursor().hasSelection());
            });
        break;
    case QEvent::FocusOut:
    case QEvent::WindowDeactivate:
        QTimer::singleShot(0, this, [this]() {
            if (m_editor && !m_applying && !menusActive() && !editorOwnsPopup(m_editor) &&
                (!m_editor->window()->isActiveWindow() || !m_editor->textEditor()->hasFocus())) dismiss();
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

}  // namespace kl
