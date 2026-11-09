#include "MarkdownEditor.h"
#include <QAbstractTextDocumentLayout>
#include <QDesktopServices>
#include <QFrame>
#include <QGridLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QStackedWidget>
#include <QTabBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtMath>
#include "../app/TaskStore.h"
#include "../domain/InternalLink.h"
#include "AppIcons.h"
#include "AppStyle.h"
#include "EntityPicker.h"
#include "Markdown.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {
struct FormatSpec {
    const char* label;
    const char* before;
    const char* after;
    int kind;
    const char* key;
    IconId icon;
};
// 顺序必须与 MarkdownEditor::FormatAction 一致。
const FormatSpec& formatSpec(MarkdownEditor::FormatAction action) {
    static const FormatSpec specs[] = {
        {"加粗", "**", "**", 0, "Ctrl+B", IconId::Bold},
        {"斜体", "*", "*", 1, "Ctrl+I", IconId::Italic},
        {"删除线", "~~", "~~", 2, "Ctrl+Shift+X", IconId::Strike},
        {"标题", "## ", "", 3, "", IconId::Heading},
        {"列表", "- ", "", 4, "", IconId::List},
        {"编号", "1. ", "", 5, "", IconId::OrderedList},
        {"块引用", "> ", "", 6, "", IconId::Quote},
        {"代码", "`", "`", 7, "", IconId::Code},
        {"清单", "- [ ] ", "", 8, "", IconId::Checklist},
        {"链接", "", "", 9, "", IconId::Link},
        {"实体引用", "", "", 10, "", IconId::Link},
        {"下划线", "<u>", "</u>", 11, "Ctrl+U", IconId::Underline},
        {"高亮", "", "", 12, "", IconId::Highlight},
        {"清除格式", "", "", 13, "", IconId::FormatClear},
    };
    const int index = static_cast<int>(action);
    if (index < 0 || index >= static_cast<int>(MarkdownEditor::FormatAction::Count)) return specs[0];
    return specs[index];
}

class SourceBlockData : public QTextBlockUserData {
public:
    QString source;
    // Each visible UTF-16 character maps back to its source range. Revealing a
    // rendered line must translate the selection past hidden Markdown wrappers.
    QVector<int> starts;
    QVector<int> ends;
    bool rendered = false;
};

SourceBlockData* sourceData(const QTextBlock& block) {
    return static_cast<SourceBlockData*>(block.userData());
}

constexpr int kSourceProperty = QTextFormat::UserProperty + 41;
constexpr int kRenderedTextProperty = QTextFormat::UserProperty + 42;
constexpr int kSourceStartsProperty = QTextFormat::UserProperty + 43;
constexpr int kSourceEndsProperty = QTextFormat::UserProperty + 44;

bool hasRenderedSource(const QTextBlock& block) {
    const QTextBlockFormat format = block.blockFormat();
    return format.hasProperty(kSourceProperty)
        && format.stringProperty(kRenderedTextProperty) == block.text();
}

class PresentationEdit {
public:
    explicit PresentationEdit(QTextDocument* document) : m_document(document), m_cursor(document) {
        m_pauseUndo = document->isUndoRedoEnabled() && !document->isUndoAvailable() && !document->isRedoAvailable();
        if (m_pauseUndo) document->setUndoRedoEnabled(false);
        else m_cursor.joinPreviousEditBlock();
    }
    ~PresentationEdit() {
        if (m_pauseUndo) m_document->setUndoRedoEnabled(true);
        else m_cursor.endEditBlock();
    }
private:
    QTextDocument* m_document;
    QTextCursor m_cursor;
    bool m_pauseUndo = false;
};

struct SavedPosition {
    int block = 0;
    int offset = 0;
};

SavedPosition savePosition(const QTextDocument* document, int position, bool selectionEnd = false) {
    const QTextBlock block = document->findBlock(position);
    SavedPosition result;
    result.block = qMax(0, block.blockNumber());
    result.offset = position - block.position();
    if (hasRenderedSource(block)) {
        const QVector<int> starts = block.blockFormat().property(kSourceStartsProperty).value<QVector<int>>();
        const QVector<int> ends = block.blockFormat().property(kSourceEndsProperty).value<QVector<int>>();
        if (!starts.isEmpty()) {
            const int visible = qBound(0, result.offset, starts.size());
            if (selectionEnd && visible == 0) result.offset = 0;
            else result.offset = visible == starts.size() || (selectionEnd && visible > 0)
                ? ends.at(qMax(0, visible - 1)) : starts.at(visible);
        }
    }
    return result;
}

int restorePosition(const QTextDocument* document, const SavedPosition& saved) {
    QTextBlock block = document->findBlockByNumber(qMin(saved.block, document->blockCount() - 1));
    if (!block.isValid()) block = document->lastBlock();
    int offset = saved.offset;
    if (hasRenderedSource(block)) {
        const QVector<int> starts = block.blockFormat().property(kSourceStartsProperty).value<QVector<int>>();
        const QVector<int> ends = block.blockFormat().property(kSourceEndsProperty).value<QVector<int>>();
        if (!starts.isEmpty()) {
            offset = starts.size();
            for (int i = 0; i < starts.size(); ++i) {
                if (saved.offset <= starts.at(i)) { offset = i; break; }
                if (saved.offset <= ends.at(i)) { offset = i + 1; break; }
            }
        }
    }
    return block.position() + qBound(0, offset, block.length() - 1);
}

bool isEscaped(const QString& text, int position) {
    int slashes = 0;
    for (int i = position - 1; i >= 0 && text.at(i) == QLatin1Char('\\'); --i) ++slashes;
    return slashes % 2 != 0;
}

int findToken(const QString& text, const QString& token, int from) {
    int position = from;
    while ((position = text.indexOf(token, position)) >= 0) {
        if (!isEscaped(text, position)) return position;
        position += token.size();
    }
    return -1;
}

int closingBracket(const QString& text, int from) {
    int depth = 0;
    for (int i = from; i < text.size(); ++i) {
        if (isEscaped(text, i)) continue;
        if (text.at(i) == QLatin1Char('[')) ++depth;
        else if (text.at(i) == QLatin1Char(']')) {
            if (depth == 0) return i;
            --depth;
        }
    }
    return -1;
}

int closingParenthesis(const QString& text, int from) {
    int depth = 0;
    for (int i = from; i < text.size(); ++i) {
        if (isEscaped(text, i)) continue;
        if (text.at(i) == QLatin1Char('(')) ++depth;
        else if (text.at(i) == QLatin1Char(')')) {
            if (depth == 0) return i;
            --depth;
        }
    }
    return -1;
}

void insertMapped(QTextCursor& cursor, const QString& text, const QTextCharFormat& format,
                  SourceBlockData* data, int sourceStart, int sourceLength = -1) {
    cursor.insertText(text, format);
    if (!data) return;
    if (sourceLength < 0) sourceLength = text.size();
    for (int i = 0; i < text.size(); ++i) {
        data->starts.append(sourceStart + i * sourceLength / text.size());
        data->ends.append(sourceStart + (i + 1) * sourceLength / text.size());
    }
}

int closingStyleTag(const QString& text, const QString& opening, const QString& closing, int from) {
    int depth = 0;
    for (int i = from; i < text.size(); ++i) {
        if (isEscaped(text, i)) continue;
        if (text.midRef(i, opening.size()) == opening) { ++depth; i += opening.size() - 1; }
        else if (text.midRef(i, closing.size()) == closing) {
            if (depth == 0) return i;
            --depth; i += closing.size() - 1;
        }
    }
    return -1;
}

void insertInline(QTextCursor& cursor, const QString& text, const QTextCharFormat& base,
                  int depth = 0, SourceBlockData* data = nullptr, int sourceOffset = 0) {
    if (depth > 16) { insertMapped(cursor, text, base, data, sourceOffset); return; }
    QString literal;
    QVector<int> literalStarts, literalEnds;
    const auto flush = [&]() {
        if (literal.isEmpty()) return;
        cursor.insertText(literal, base);
        if (data) { data->starts += literalStarts; data->ends += literalEnds; }
        literal.clear(); literalStarts.clear(); literalEnds.clear();
    };
    int i = 0;
    while (i < text.size()) {
        if (text.at(i) == QLatin1Char('\\') && i + 1 < text.size()) {
            literal += text.at(i + 1);
            literalStarts.append(sourceOffset + i + 1); literalEnds.append(sourceOffset + i + 2);
            i += 2; continue;
        }
        if (text.at(i) == QLatin1Char('`')) {
            const int end = findToken(text, QStringLiteral("`"), i + 1);
            if (end > i + 1) {
                flush(); QTextCharFormat format = base;
                format.setFontFamily(ds::qfont(ds::FontScene::Mono).family());
                format.setBackground(ThemeManager::instance().color(ds::ColorToken::Field));
                insertMapped(cursor, text.mid(i + 1, end - i - 1), format, data, sourceOffset + i + 1);
                i = end + 1; continue;
            }
        }
        if (text.at(i) == QLatin1Char('<')) {
            QString opening, closing;
            QTextCharFormat format = base;
            if (text.midRef(i, 3) == QLatin1String("<u>")) {
                opening = QStringLiteral("<u>"); closing = QStringLiteral("</u>"); format.setFontUnderline(true);
            } else {
                static const QRegularExpression mark(QStringLiteral("^<mark data-color=\"(yellow|green|blue|pink|purple)\">"));
                const QRegularExpressionMatch match = mark.match(text.mid(i));
                if (match.hasMatch()) {
                    opening = match.captured(0); closing = QStringLiteral("</mark>");
                    format.setBackground(Markdown::highlightColor(match.captured(1)));
                }
            }
            if (!opening.isEmpty()) {
                const int end = closingStyleTag(text, closing == QLatin1String("</mark>") ? QStringLiteral("<mark ") : opening,
                                                closing, i + opening.size());
                if (end > i + opening.size()) {
                    flush();
                    insertInline(cursor, text.mid(i + opening.size(), end - i - opening.size()), format,
                                 depth + 1, data, sourceOffset + i + opening.size());
                    i = end + closing.size(); continue;
                }
            }
        }
        QString delimiter;
        enum { None, Bold, Italic, Strike } style = None;
        if ((text.mid(i, 2) == QStringLiteral("**")) || (text.mid(i, 2) == QStringLiteral("__"))) {
            delimiter = text.mid(i, 2); style = Bold;
        } else if (text.mid(i, 2) == QStringLiteral("~~")) {
            delimiter = QStringLiteral("~~"); style = Strike;
        } else if (text.at(i) == QLatin1Char('*') || text.at(i) == QLatin1Char('_')) {
            delimiter = text.at(i); style = Italic;
        }
        if (style != None) {
            const int end = findToken(text, delimiter, i + delimiter.size());
            if (end > i + delimiter.size()) {
                flush(); QTextCharFormat format = base;
                if (style == Bold) format.setFontWeight(QFont::Bold);
                if (style == Italic) format.setFontItalic(true);
                if (style == Strike) format.setFontStrikeOut(true);
                insertInline(cursor, text.mid(i + delimiter.size(), end - i - delimiter.size()), format,
                             depth + 1, data, sourceOffset + i + delimiter.size());
                i = end + delimiter.size(); continue;
            }
        }
        if (text.at(i) == QLatin1Char('[')) {
            const int bracket = closingBracket(text, i + 1);
            if (bracket > i + 1 && bracket + 1 < text.size() && text.at(bracket + 1) == QLatin1Char('(')) {
                const int parenthesis = closingParenthesis(text, bracket + 2);
                if (parenthesis > bracket + 2) {
                    flush(); QTextCharFormat format = base;
                    const QString target = text.mid(bracket + 2, parenthesis - bracket - 2).trimmed();
                    format.setAnchor(true); format.setAnchorHref(target); format.setFontUnderline(true);
                    format.setForeground(ThemeManager::instance().color(ds::ColorToken::Primary));
                    format.setToolTip(target);
                    insertInline(cursor, text.mid(i + 1, bracket - i - 1), format,
                                 depth + 1, data, sourceOffset + i + 1);
                    i = parenthesis + 1; continue;
                }
            }
        }
        literal += text.at(i);
        literalStarts.append(sourceOffset + i); literalEnds.append(sourceOffset + i + 1);
        ++i;
    }
    flush();
}

enum class CodeLine { Normal, Fence, Content };
CodeLine codeLine(const QStringList& lines, int line) {
    bool inside = false; QChar marker;
    for (int i = 0; i <= line && i < lines.size(); ++i) {
        const QString trimmed = lines.at(i).trimmed();
        const bool fence = trimmed.startsWith(QStringLiteral("```")) || trimmed.startsWith(QStringLiteral("~~~"));
        if (i == line) return fence ? CodeLine::Fence : (inside ? CodeLine::Content : CodeLine::Normal);
        if (fence) {
            const QChar current = trimmed.at(0);
            if (!inside) { inside = true; marker = current; }
            else if (current == marker) inside = false;
        }
    }
    return CodeLine::Normal;
}

bool plainSource(const QStringList& lines, int line) {
    if (codeLine(lines, line) != CodeLine::Normal) return false;
    const QString& source = lines.at(line);
    static const QRegularExpression block(QStringLiteral("^[ \\t]*(?:#{1,6}[ \\t]+|[-+*][ \\t]+|[0-9]+[.)][ \\t]+|>[ \\t]?)"));
    static const QRegularExpression inlineMarker(QStringLiteral("[\\\\`*_~\\[<]"));
    return !block.match(source).hasMatch() && !inlineMarker.match(source).hasMatch();
}

QString plainLine(const QString& source, CodeLine code = CodeLine::Normal) {
    if (code == CodeLine::Fence) return QString();
    if (code == CodeLine::Content) return source;
    static const QRegularExpression prefix(QStringLiteral(
        "^([ \\t]*)(?:#{1,6}[ \\t]+|[-+*][ \\t]+(?:\\[[ xX]\\][ \\t]+)?|[0-9]+[.)][ \\t]+|>[ \\t]?)"));
    QString text = source;
    const QRegularExpressionMatch match = prefix.match(text);
    if (match.hasMatch()) text.replace(0, match.capturedLength(), match.captured(1));
    QTextDocument document;
    QTextCursor cursor(&document);
    insertInline(cursor, text, QTextCharFormat());
    return document.toPlainText();
}
}  // namespace

MarkdownEditor::MarkdownEditor(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("MarkdownEditor"));
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);

    // 正文区保持干净：常驻工具条与编辑/预览 tab 不再占位。
    // 格式按钮寄存在屏幕外的宿主里，仍保留 objectName 与真实几何，
    // 因此兼容测试里的 QTest::mouseClick 依旧有效，外部面板也可复用动作。
    m_actionHost = new QWidget(this);
    m_actionHost->setObjectName(QStringLiteral("FormatActionHost"));
    m_actionHost->move(-10000, -10000);
    m_actionHost->resize(11 * 24, 24);

    // 保留 QFrame#EditorToolbar 与 moreFormats 结构（QSS 选择器与对象名兼容），但隐藏、不入布局。
    m_toolbar = new QFrame(this);
    m_toolbar->setObjectName(QStringLiteral("EditorToolbar"));
    auto* grid = new QGridLayout(m_toolbar); grid->setContentsMargins(0, 0, 0, 0); grid->setSpacing(0);
    auto* more = new QToolButton(m_toolbar);
    more->setObjectName(QStringLiteral("moreFormats")); more->setText(QStringLiteral("更多格式"));
    more->setIcon(AppIcons::icon(IconId::More)); more->setIconSize(QSize(16, 16));
    more->setToolButtonStyle(Qt::ToolButtonIconOnly); more->setAutoRaise(true);
    more->setToolTip(QStringLiteral("更多格式")); more->setAccessibleName(QStringLiteral("更多格式"));
    more->setFocusPolicy(Qt::NoFocus); more->setPopupMode(QToolButton::InstantPopup);
    auto* moreMenu = new QMenu(more); moreMenu->setObjectName(QStringLiteral("MoreFormatsMenu"));
    moreMenu->setToolTipsVisible(true); more->setMenu(moreMenu);
    grid->addWidget(more, 0, 0);

    // 9 个格式动作：objectName 仍为 format0..8，行为统一走 applyFormat()。
    for (int i = 0; i <= static_cast<int>(FormatAction::Checklist); ++i) {
        const FormatAction action = static_cast<FormatAction>(i);
        const FormatSpec& spec = formatSpec(action);
        const QString label = QString::fromUtf8(spec.label);
        const QString key = QString::fromLatin1(spec.key);
        auto* button = new QToolButton(m_actionHost);
        button->setObjectName(QStringLiteral("format%1").arg(i));
        button->setGeometry(i * 24, 0, 24, 24);
        button->setText(label);
        button->setIcon(AppIcons::icon(spec.icon)); button->setIconSize(QSize(16, 16));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly); button->setAutoRaise(true);
        button->setToolTip(key.isEmpty() ? label : QStringLiteral("%1 (%2)").arg(label, key));
        button->setAccessibleName(AppIcons::accessibleName(spec.icon));
        button->setFocusPolicy(Qt::NoFocus);
        connect(button, &QToolButton::clicked, this, [this, action]() { applyFormat(action); });
        if (!key.isEmpty()) {
            auto* shortcut = new QShortcut(QKeySequence(key), this);
            shortcut->setContext(Qt::WidgetWithChildrenShortcut);
            connect(shortcut, &QShortcut::activated, this, [this, action]() { applyFormat(action); });
        }
    }
    auto* link = new QToolButton(m_actionHost);
    link->setObjectName(QStringLiteral("insertLink")); link->setText(QStringLiteral("链接"));
    link->setGeometry(9 * 24, 0, 24, 24);
    link->setIcon(AppIcons::icon(IconId::Link)); link->setIconSize(QSize(16, 16));
    link->setToolButtonStyle(Qt::ToolButtonIconOnly); link->setAutoRaise(true);
    link->setToolTip(AppIcons::accessibleName(IconId::Link)); link->setAccessibleName(AppIcons::accessibleName(IconId::Link));
    link->setFocusPolicy(Qt::NoFocus);
    connect(link, &QToolButton::clicked, this, [this]() { applyFormat(FormatAction::Link); });

    m_referenceButton = new QToolButton(m_actionHost);
    m_referenceButton->setObjectName(QStringLiteral("insertEntityReference"));
    m_referenceButton->setText(QStringLiteral("实体引用"));
    m_referenceButton->setGeometry(10 * 24, 0, 24, 24);
    m_referenceButton->setIcon(AppIcons::icon(IconId::Link)); m_referenceButton->setIconSize(QSize(16, 16));
    m_referenceButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); m_referenceButton->setAutoRaise(true);
    m_referenceButton->setToolTip(QStringLiteral("实体引用")); m_referenceButton->setAccessibleName(QStringLiteral("实体引用"));
    m_referenceButton->setFocusPolicy(Qt::NoFocus); m_referenceButton->setEnabled(false);
    connect(m_referenceButton, &QToolButton::clicked, this, [this]() { applyFormat(FormatAction::EntityReference); });

    m_actionHost->show();
    m_toolbar->hide();

    // 默认只有一个行内 WYSIWYG 编辑区；旧预览页仅供 setPreview(true) 与兼容 API 使用。
    m_tabs = new QTabBar(this); m_tabs->addTab(QStringLiteral("编辑")); m_tabs->addTab(QStringLiteral("预览"));
    m_tabs->setExpanding(false); m_tabs->hide();
    m_stack = new QStackedWidget(this); m_editor = new QTextEdit(this);
    m_editor->setObjectName(QStringLiteral("DescriptionEditor"));
    // 富文本仅由本类按源码生成；外部粘贴仍强制纯文本，避免带入不可信 HTML。
    m_editor->setAcceptRichText(false); m_editor->setMinimumHeight(150);
    m_editor->setFont(ds::qfont(ds::FontScene::Body));
    m_editor->setPlaceholderText(QStringLiteral("记录内容…"));
    m_preview = new DocumentPreview(this); m_preview->setMinimumHeight(150);
    m_stack->addWidget(m_editor); m_stack->addWidget(m_preview); layout->addWidget(m_stack, 1);
    m_editor->installEventFilter(this); m_editor->viewport()->installEventFilter(this);
    connect(m_preview, &DocumentPreview::internalLinkActivated, this, &MarkdownEditor::internalLinkActivated);
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) { refreshPreview(); m_stack->setCurrentIndex(index); });
    connect(m_editor, &QTextEdit::textChanged, this, [this]() {
        if (m_loading || m_rendering) return;
        // 不在 QTextCursor 正在替换选区时重设光标，否则会破坏 Qt 的撤销分组。
        synchronizeSource();
        refreshPreview(); emit contentChanged();
        if (!m_lineUpdatePending) {
            m_lineUpdatePending = true;
            QTimer::singleShot(0, this, [this]() {
                m_lineUpdatePending = false;
                if (m_loading || m_rendering) return;
                synchronizeSource();
                m_editingLine = m_editor->hasFocus() ? m_editor->textCursor().blockNumber() : -1;
                updateLineStates();
            });
        }
    });
    connect(m_editor, &QTextEdit::cursorPositionChanged, this, [this]() {
        if (m_loading || m_rendering) return;
        const int line = m_editor->hasFocus() ? m_editor->textCursor().blockNumber() : -1;
        if (line == m_editingLine) return;
        synchronizeSource();
        m_editingLine = line;
        updateLineStates();
    });
    connect(m_editor, &QTextEdit::selectionChanged, this, [this]() {
        if (!m_rendering) emit selectionChanged(m_editor->textCursor().hasSelection());
    });
    auto* underline = new QShortcut(QKeySequence(formatShortcut(FormatAction::Underline)), this);
    underline->setContext(Qt::WidgetWithChildrenShortcut);
    connect(underline, &QShortcut::activated, this, [this]() { applyFormat(FormatAction::Underline); });
    connect(m_editor->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            this, [this](const QSizeF&) { updateContentHeight(); });
    connect(m_preview->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            this, [this](const QSizeF&) { updateContentHeight(); });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this](ds::Theme) {
        // Rebuild only presentation; the stored body and the selected source range stay intact.
        if (m_loading || m_rendering) return;
        synchronizeSource();
        for (QTextBlock block = m_editor->document()->begin(); block.isValid(); block = block.next()) {
            if (SourceBlockData* data = sourceData(block)) data->source.clear();
        }
        updateLineStates(); refreshPreview();
    });
    updateContentHeight();
}

void MarkdownEditor::setStore(TaskStore* store) {
    if (m_store == store) return;
    if (m_store) disconnect(m_store, nullptr, this, nullptr);
    m_store = store; m_preview->setStore(store); m_referenceButton->setEnabled(m_store != nullptr);
    if (m_store) {
        connect(m_store, &TaskStore::taskChanged, this, [this](QString) { refreshPreview(); });
        connect(m_store, &TaskStore::tasksReset, this, &MarkdownEditor::refreshPreview);
    }
    refreshPreview();
}
void MarkdownEditor::setContent(const QString& content) {
    m_sourceLines = content.split(QLatin1Char('\n'), QString::KeepEmptyParts);
    m_editingLine = -1; m_loading = true; renderDocument(); refreshPreview(); m_loading = false;
    updateContentHeight();
}
QString MarkdownEditor::content() const { return m_sourceLines.join(QLatin1Char('\n')); }
QString MarkdownEditor::plainTextContent() const {
    QStringList lines;
    for (int i = 0; i < m_sourceLines.size(); ++i) lines.append(plainLine(m_sourceLines.at(i), codeLine(m_sourceLines, i)));
    return lines.join(QLatin1Char('\n'));
}
void MarkdownEditor::setFlowLayout(bool enabled) {
    if (m_flowLayout == enabled) return;
    if (enabled) {
        m_scrollMinimumHeight = minimumHeight(); m_scrollMaximumHeight = maximumHeight();
        m_scrollDocumentMargin = m_editor->document()->documentMargin();
        m_scrollContentsMargins = m_editor->contentsMargins(); m_scrollSizePolicy = sizePolicy();
    }
    m_flowLayout = enabled;
    const Qt::ScrollBarPolicy policy = enabled ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded;
    for (QTextEdit* edit : {m_editor, static_cast<QTextEdit*>(m_preview)}) {
        edit->setVerticalScrollBarPolicy(policy); edit->setHorizontalScrollBarPolicy(policy);
        edit->setMinimumHeight(enabled ? 0 : 150); edit->setMaximumHeight(QWIDGETSIZE_MAX);
        edit->setContentsMargins(enabled ? QMargins() : m_scrollContentsMargins);
        edit->document()->setDocumentMargin(enabled ? 0 : m_scrollDocumentMargin);
        if (enabled) {
            edit->setFrameShape(QFrame::NoFrame);
            edit->setLineWrapMode(QTextEdit::WidgetWidth);
            edit->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            edit->verticalScrollBar()->setValue(0); edit->horizontalScrollBar()->setValue(0);
        }
    }
    if (enabled) setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    else {
        m_stack->setMinimumHeight(0); m_stack->setMaximumHeight(QWIDGETSIZE_MAX);
        setMinimumHeight(m_scrollMinimumHeight); setMaximumHeight(m_scrollMaximumHeight); setSizePolicy(m_scrollSizePolicy);
    }
    updateContentHeight();
}
void MarkdownEditor::updateContentHeight() {
    if (m_sizing || !m_editor || !m_stack) return;
    m_sizing = true;
    QTextEdit* edit = m_stack->currentWidget() == m_preview ? static_cast<QTextEdit*>(m_preview) : m_editor;
    const int frame = edit->frameWidth() * 2;
    const int height = qMax(edit->fontMetrics().height(), qCeil(edit->document()->documentLayout()->documentSize().height()))
        + frame + edit->contentsMargins().top() + edit->contentsMargins().bottom();
    const QMargins margins = layout()->contentsMargins();
    const int total = height + margins.top() + margins.bottom();
    if (m_flowLayout) {
        edit->setFixedHeight(height); m_stack->setFixedHeight(height); setFixedHeight(total);
        edit->verticalScrollBar()->setValue(0); edit->horizontalScrollBar()->setValue(0);
    }
    const bool changed = m_contentHeight != total;
    m_contentHeight = total; m_sizing = false;
    if (changed) emit contentHeightChanged(total);
}
void MarkdownEditor::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateContentHeight();
    QTimer::singleShot(0, this, [this]() { updateContentHeight(); });
}
void MarkdownEditor::setHighlightColor(const QString& color) {
    if (!Markdown::highlightColors().contains(color) || m_highlightColor == color) return;
    m_highlightColor = color; emit highlightColorChanged(color);
}
void MarkdownEditor::applyHighlight(const QString& color) {
    if (!Markdown::highlightColors().contains(color)) return;
    setHighlightColor(color); applyFormat(FormatAction::Highlight);
}
void MarkdownEditor::setPreview(bool preview) {
    if (m_loading || m_rendering) { m_tabs->setCurrentIndex(preview ? 1 : 0); return; }
    synchronizeSource();
    m_editingLine = preview ? -1 : m_editor->textCursor().blockNumber();
    updateLineStates(); refreshPreview(); m_tabs->setCurrentIndex(preview ? 1 : 0);
    m_stack->setCurrentIndex(preview ? 1 : 0);
    updateContentHeight();
}
void MarkdownEditor::setCompact(bool compact) {
    Q_UNUSED(compact);
    m_toolbar->hide(); m_tabs->hide();
    m_editor->setFrameShape(QFrame::NoFrame);
    m_editor->setContentsMargins(0, 0, 0, 0); m_editor->document()->setDocumentMargin(0);
    m_preview->setContentsMargins(0, 0, 0, 0); m_preview->document()->setDocumentMargin(0);
}
void MarkdownEditor::refreshPreview() {
    m_preview->setHtml(Markdown::toHtml(content(), m_store ? Markdown::entityMap(m_store->entities())
                                                           : Markdown::EntityMap()));
}

void MarkdownEditor::renderDocument() {
    m_rendering = true;
    m_editor->setPlainText(m_sourceLines.join(QLatin1Char('\n')));
    for (int i = 0; i < m_sourceLines.size(); ++i) renderLine(i, false);
    QTextCursor cursor(m_editor->document()); cursor.movePosition(QTextCursor::Start);
    m_editor->setTextCursor(cursor);
    m_editor->document()->setModified(false);
    m_editor->document()->clearUndoRedoStacks();
    m_rendering = false;
}

void MarkdownEditor::renderLine(int line, bool source) {
    QTextDocument* document = m_editor->document();
    QTextBlock block = document->findBlockByNumber(line);
    if (!block.isValid() || line < 0 || line >= m_sourceLines.size()) return;
    const QString& markdown = m_sourceLines.at(line);
    QTextCharFormat body;
    body.setFont(ds::qfont(ds::FontScene::Body));
    body.setFontWeight(QFont::Normal);
    body.setFontItalic(false); body.setFontUnderline(false); body.setFontStrikeOut(false);
    SourceBlockData* oldData = sourceData(block);
    const bool renderedSource = hasRenderedSource(block);

    // Keep source/plain text in place, but explicitly clear styles inherited
    // from a preceding heading or formatted paragraph, including empty blocks.
    if (block.text() == markdown && (source || plainSource(m_sourceLines, line))) {
        QTextCursor reset(block);
        reset.setPosition(block.position());
        reset.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
        if (block.blockFormat() != QTextBlockFormat()) reset.setBlockFormat(QTextBlockFormat());
        if (block.charFormat() != body) reset.setBlockCharFormat(body);
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            if (it.fragment().charFormat() != body) { reset.setCharFormat(body); break; }
        }
        auto* data = new SourceBlockData; data->source = markdown; data->rendered = !source;
        block.setUserData(data); return;
    }
    if (oldData && oldData->source == markdown && oldData->rendered == !source
        && (source ? block.text() == markdown && !renderedSource
                   : renderedSource && block.blockFormat().stringProperty(kSourceProperty) == markdown)) return;

    QTextCursor cursor(block);
    cursor.setPosition(block.position());
    cursor.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    cursor.setBlockFormat(QTextBlockFormat());
    cursor.setBlockCharFormat(body);
    auto* data = new SourceBlockData; data->source = markdown; data->rendered = !source;
    if (source) {
        cursor.insertText(markdown, body);
    } else {
        QTextCharFormat base = body;
        QTextBlockFormat blockFormat;
        const CodeLine code = codeLine(m_sourceLines, line);
        static const QRegularExpression headingMarker(QStringLiteral("^(#{1,6})[ \\t]+(.*)$"));
        static const QRegularExpression checklist(QStringLiteral("^[ \\t]*[-+*][ \\t]+\\[([ xX])\\][ \\t]+(.*)$"));
        static const QRegularExpression unordered(QStringLiteral("^[ \\t]*[-+*][ \\t]+(.*)$"));
        static const QRegularExpression ordered(QStringLiteral("^[ \\t]*([0-9]+)([.)])[ \\t]+(.*)$"));
        static const QRegularExpression quote(QStringLiteral("^[ \\t]*>[ \\t]?(.*)$"));
        if (code == CodeLine::Fence) {
            // 围栏行保留为空白块，从而源码行号与文档块号始终一一对应。
        } else if (code == CodeLine::Content) {
            base.setFontFamily(ds::qfont(ds::FontScene::Mono).family());
            blockFormat.setBackground(ThemeManager::instance().color(ds::ColorToken::Field));
            blockFormat.setLeftMargin(8); blockFormat.setRightMargin(8);
            cursor.setBlockFormat(blockFormat); insertMapped(cursor, markdown, base, data, 0);
        } else {
            const QRegularExpressionMatch headingMatch = headingMarker.match(markdown);
            const QRegularExpressionMatch checklistMatch = checklist.match(markdown);
            const QRegularExpressionMatch unorderedMatch = unordered.match(markdown);
            const QRegularExpressionMatch orderedMatch = ordered.match(markdown);
            const QRegularExpressionMatch quoteMatch = quote.match(markdown);
            if (headingMatch.hasMatch()) {
                const int level = qBound(1, headingMatch.captured(1).size(), 6);
                const ds::TypeScale scale = level == 1 ? ds::TypeScale::Title : level == 2 ? ds::TypeScale::DetailTitle
                    : level == 3 ? ds::TypeScale::Subtitle : ds::TypeScale::Body;
                base.setFontPointSize(ds::pointSize(scale)); base.setFontWeight(QFont::Bold);
                blockFormat.setTopMargin(level <= 2 ? 10 : 6); blockFormat.setBottomMargin(5);
                cursor.setBlockFormat(blockFormat);
                insertInline(cursor, headingMatch.captured(2), base, 0, data, headingMatch.capturedStart(2));
            } else if (checklistMatch.hasMatch()) {
                blockFormat.setLeftMargin(20); blockFormat.setBottomMargin(2); cursor.setBlockFormat(blockFormat);
                insertMapped(cursor, QString(QChar(checklistMatch.captured(1).compare(QStringLiteral("x"), Qt::CaseInsensitive) == 0 ? 0x2611 : 0x2610))
                                  + QStringLiteral(" "), base, data, 0, checklistMatch.capturedStart(2));
                insertInline(cursor, checklistMatch.captured(2), base, 0, data, checklistMatch.capturedStart(2));
            } else if (unorderedMatch.hasMatch()) {
                blockFormat.setLeftMargin(20); blockFormat.setBottomMargin(2); cursor.setBlockFormat(blockFormat);
                insertMapped(cursor, QStringLiteral("• "), base, data, 0, unorderedMatch.capturedStart(1));
                insertInline(cursor, unorderedMatch.captured(1), base, 0, data, unorderedMatch.capturedStart(1));
            } else if (orderedMatch.hasMatch()) {
                blockFormat.setLeftMargin(20); blockFormat.setBottomMargin(2); cursor.setBlockFormat(blockFormat);
                insertMapped(cursor, orderedMatch.captured(1) + orderedMatch.captured(2) + QStringLiteral(" "), base,
                             data, 0, orderedMatch.capturedStart(3));
                insertInline(cursor, orderedMatch.captured(3), base, 0, data, orderedMatch.capturedStart(3));
            } else if (quoteMatch.hasMatch()) {
                base.setFontItalic(true); base.setForeground(ThemeManager::instance().color(ds::ColorToken::TextSecondary));
                blockFormat.setLeftMargin(12); blockFormat.setBackground(ThemeManager::instance().color(ds::ColorToken::Field));
                blockFormat.setTopMargin(3); blockFormat.setBottomMargin(3); cursor.setBlockFormat(blockFormat);
                QTextCharFormat bar = base; bar.setForeground(ThemeManager::instance().color(ds::ColorToken::TextTertiary));
                insertMapped(cursor, QStringLiteral("▏ "), bar, data, 0, quoteMatch.capturedStart(1));
                insertInline(cursor, quoteMatch.captured(1), base, 0, data, quoteMatch.capturedStart(1));
            } else {
                cursor.setBlockFormat(blockFormat); insertInline(cursor, markdown, base, 0, data);
            }
        }
    }
    block = document->findBlockByNumber(line);
    if (!source) {
        // QTextBlockUserData is not restored by Qt undo. Keep the source snapshot
        // and selection map in undoable block properties as well.
        QTextBlockFormat format = block.blockFormat();
        format.setProperty(kSourceProperty, markdown);
        format.setProperty(kRenderedTextProperty, block.text());
        format.setProperty(kSourceStartsProperty, QVariant::fromValue(data->starts));
        format.setProperty(kSourceEndsProperty, QVariant::fromValue(data->ends));
        QTextCursor(block).setBlockFormat(format);
    }
    block.setUserData(data);
}

void MarkdownEditor::updateLineStates() {
    if (m_loading || m_rendering || m_editor->document()->isRedoAvailable()) return;
    m_rendering = true;
    const QTextCursor current = m_editor->textCursor();
    const SavedPosition anchor = savePosition(m_editor->document(), current.anchor(),
                                             current.hasSelection() && current.anchor() == current.selectionEnd());
    const SavedPosition position = savePosition(m_editor->document(), current.position(),
                                               current.hasSelection() && current.position() == current.selectionEnd());
    {
        // Presentation joins the last actual edit instead of adding a separate
        // undo step. Never re-render after undo while Qt still owns a redo chain.
        PresentationEdit edit(m_editor->document());
        for (int i = 0; i < m_sourceLines.size(); ++i) renderLine(i, i == m_editingLine);
        QTextCursor restored(m_editor->document());
        restored.setPosition(restorePosition(m_editor->document(), anchor));
        restored.setPosition(restorePosition(m_editor->document(), position), QTextCursor::KeepAnchor);
        m_editor->setTextCursor(restored);
    }
    m_rendering = false;
    updateContentHeight();
}

void MarkdownEditor::synchronizeSource() {
    QStringList result;
    for (QTextBlock block = m_editor->document()->begin(); block != m_editor->document()->end(); block = block.next()) {
        result.append(hasRenderedSource(block) ? block.blockFormat().stringProperty(kSourceProperty) : block.text());
    }
    m_sourceLines = result;
}

void MarkdownEditor::revealLines(int first, int last) {
    if (m_loading || m_rendering) return;
    synchronizeSource();
    first = qBound(0, first, m_sourceLines.size() - 1);
    last = qBound(first, last, m_sourceLines.size() - 1);
    const QTextCursor current = m_editor->textCursor();
    const SavedPosition anchor = savePosition(m_editor->document(), current.anchor(),
                                             current.hasSelection() && current.anchor() == current.selectionEnd());
    const SavedPosition position = savePosition(m_editor->document(), current.position(),
                                               current.hasSelection() && current.position() == current.selectionEnd());
    m_rendering = true;
    {
        PresentationEdit edit(m_editor->document());
        for (int i = first; i <= last; ++i) renderLine(i, true);
        m_editingLine = current.blockNumber();
        QTextCursor restored(m_editor->document());
        restored.setPosition(restorePosition(m_editor->document(), anchor));
        restored.setPosition(restorePosition(m_editor->document(), position), QTextCursor::KeepAnchor);
        m_editor->setTextCursor(restored);
    }
    m_rendering = false;
    updateContentHeight();
}

void MarkdownEditor::revealSelection() {
    const QTextCursor cursor = m_editor->textCursor();
    const int first = m_editor->document()->findBlock(cursor.selectionStart()).blockNumber();
    const int last = m_editor->document()->findBlock(cursor.selectionEnd()).blockNumber();
    revealLines(first, last);
}

bool MarkdownEditor::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_editor && !m_loading && !m_rendering) {
        if (event->type() == QEvent::FocusIn) {
            synchronizeSource(); m_editingLine = m_editor->textCursor().blockNumber(); updateLineStates();
        } else if (event->type() == QEvent::FocusOut) {
            synchronizeSource(); m_editingLine = -1; updateLineStates();
        } else if (event->type() == QEvent::InputMethod) {
            revealSelection();
        } else if (event->type() == QEvent::KeyPress) {
            const QKeyEvent* key = static_cast<QKeyEvent*>(event);
            if (key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo)
                || key->matches(QKeySequence::Copy) || key->matches(QKeySequence::SelectAll))
                return QWidget::eventFilter(watched, event);
            const QTextCursor cursor = m_editor->textCursor();
            int first = m_editor->document()->findBlock(cursor.selectionStart()).blockNumber();
            int last = m_editor->document()->findBlock(cursor.selectionEnd()).blockNumber();
            const bool modifies = key->matches(QKeySequence::Paste) || key->matches(QKeySequence::Cut)
                                  || key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete
                                  || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter
                                  || !key->text().isEmpty();
            if (key->key() == Qt::Key_Backspace && !cursor.hasSelection()
                && cursor.positionInBlock() == 0 && cursor.blockNumber() > 0) --first;
            if (key->key() == Qt::Key_Delete && !cursor.hasSelection()
                && cursor.positionInBlock() == cursor.block().length() - 1
                && cursor.blockNumber() + 1 < m_editor->document()->blockCount()) ++last;
            if (modifies) revealLines(first, last);
        }
    }
    if (watched == m_editor->viewport() && event->type() == QEvent::MouseButtonPress) {
        const QMouseEvent* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            const QTextCursor cursor = m_editor->cursorForPosition(mouse->pos());
            const QString href = cursor.charFormat().anchorHref();
            const QUrl url(href);
            if (url.scheme().toLower() == QLatin1String("kl") && isValidLinkTarget(href)) {
                emit internalLinkActivated(href.mid(3)); return true;
            }
            if (QStringList({QStringLiteral("https"), QStringLiteral("http"), QStringLiteral("mailto")})
                    .contains(url.scheme().toLower())) {
                QDesktopServices::openUrl(url); return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MarkdownEditor::applyFormat(FormatAction action) {
    setPreview(false);
    if (action == FormatAction::Link) { insertLink(); return; }
    if (action == FormatAction::EntityReference) { chooseEntityReference(); return; }
    if (action == FormatAction::Highlight) {
        applyInline(QStringLiteral("<mark data-color=\"%1\">").arg(m_highlightColor), QStringLiteral("</mark>"), 12); return;
    }
    if (action == FormatAction::ClearFormat) { clearFormatting(); return; }
    if (action == FormatAction::Count) return;
    const FormatSpec& spec = formatSpec(action);
    if ((spec.kind >= 3 && spec.kind <= 6) || spec.kind == 8)
        applyBlock(QString::fromLatin1(spec.before), spec.kind);
    else
        applyInline(QString::fromLatin1(spec.before), QString::fromLatin1(spec.after), spec.kind);
}
QString MarkdownEditor::formatLabel(FormatAction action) const {
    return QString::fromUtf8(formatSpec(action).label);
}
IconId MarkdownEditor::formatIcon(FormatAction action) const {
    return formatSpec(action).icon;
}
QString MarkdownEditor::formatShortcut(FormatAction action) const {
    return QString::fromLatin1(formatSpec(action).key);
}
QVector<MarkdownEditor::FormatAction> MarkdownEditor::primaryFormats() const {
    return {FormatAction::Bold, FormatAction::Italic, FormatAction::Strike, FormatAction::Link};
}
QVector<MarkdownEditor::FormatAction> MarkdownEditor::allFormats() const {
    QVector<FormatAction> actions;
    for (int i = 0; i < static_cast<int>(FormatAction::Count); ++i)
        actions.append(static_cast<FormatAction>(i));
    return actions;
}

void MarkdownEditor::applyInline(const QString& before, const QString& after, int kind) {
    revealSelection();
    QTextCursor cursor = m_editor->textCursor();
    const QString document = m_editor->toPlainText();
    QString selected = cursor.hasSelection() ? cursor.selectedText() : QStringLiteral("文字");
    selected.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    int start = cursor.selectionStart(), end = cursor.selectionEnd();
    bool remove = false;
    if (cursor.hasSelection()) {
        QString existingBefore = before;
        if (kind == 12) {
            static const QRegularExpression mark(QStringLiteral("<mark data-color=\"(?:yellow|green|blue|pink|purple)\">$"));
            const QRegularExpressionMatch match = mark.match(document.left(start));
            if (match.hasMatch()) existingBefore = match.captured(0);
        }
        if (start >= existingBefore.size() && document.mid(start - existingBefore.size(), existingBefore.size()) == existingBefore
            && document.mid(end, after.size()) == after) {
            start -= existingBefore.size(); end += after.size();
            remove = kind != 12;
        } else if (selected.startsWith(before) && selected.endsWith(after) && selected.size() >= before.size() + after.size()) {
            selected = selected.mid(before.size(), selected.size() - before.size() - after.size());
            remove = kind != 12;
        } else if (kind == 12) {
            static const QRegularExpression marked(QStringLiteral("^<mark data-color=\"(?:yellow|green|blue|pink|purple)\">([\\s\\S]*)</mark>$"));
            const QRegularExpressionMatch match = marked.match(selected);
            if (match.hasMatch()) selected = match.captured(1);
        }
    }
    QString replacement = selected;
    if (!remove) {
        QStringList lines = selected.split(QLatin1Char('\n'), QString::KeepEmptyParts);
        for (QString& line : lines) line = before + line + after;
        replacement = lines.join(QLatin1Char('\n'));
    }
    cursor.setPosition(start); cursor.setPosition(end, QTextCursor::KeepAnchor);
    cursor.beginEditBlock(); cursor.insertText(replacement, QTextCharFormat()); cursor.endEditBlock();
    synchronizeSource();
    const int selectionStart = start + (remove ? 0 : before.size());
    const int selectionEnd = start + replacement.size() - (remove ? 0 : after.size());
    cursor.setPosition(selectionStart); cursor.setPosition(qMax(selectionStart, selectionEnd), QTextCursor::KeepAnchor);
    m_editingLine = cursor.blockNumber(); m_editor->setTextCursor(cursor); updateLineStates(); m_editor->setFocus();
}

void MarkdownEditor::clearFormatting() {
    revealSelection();
    QTextCursor cursor = m_editor->textCursor();
    if (!cursor.hasSelection()) cursor.select(QTextCursor::WordUnderCursor);
    if (!cursor.hasSelection()) return;
    int start = cursor.selectionStart(), end = cursor.selectionEnd();
    const QString document = m_editor->toPlainText();
    QVector<QPair<QString, QString>> wrappers = {
        {QStringLiteral("**"), QStringLiteral("**")}, {QStringLiteral("__"), QStringLiteral("__")},
        {QStringLiteral("~~"), QStringLiteral("~~")}, {QStringLiteral("*"), QStringLiteral("*")},
        {QStringLiteral("_"), QStringLiteral("_")}, {QStringLiteral("`"), QStringLiteral("`")},
        {QStringLiteral("<u>"), QStringLiteral("</u>")},
    };
    for (const QString& color : Markdown::highlightColors())
        wrappers.append(qMakePair(QStringLiteral("<mark data-color=\"%1\">").arg(color), QStringLiteral("</mark>")));
    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (const auto& wrapper : wrappers) {
            if (start >= wrapper.first.size() && document.mid(start - wrapper.first.size(), wrapper.first.size()) == wrapper.first
                && document.mid(end, wrapper.second.size()) == wrapper.second) {
                start -= wrapper.first.size(); end += wrapper.second.size(); expanded = true; break;
            }
        }
    }
    cursor.setPosition(start); cursor.setPosition(end, QTextCursor::KeepAnchor);
    QStringList lines = cursor.selectedText().split(QChar::ParagraphSeparator, QString::KeepEmptyParts);
    for (QString& line : lines) line = plainLine(line);
    const QString replacement = lines.join(QLatin1Char('\n'));
    cursor.beginEditBlock(); cursor.insertText(replacement, QTextCharFormat()); cursor.endEditBlock();
    synchronizeSource(); cursor.setPosition(start); cursor.setPosition(start + replacement.size(), QTextCursor::KeepAnchor);
    m_editingLine = cursor.blockNumber(); m_editor->setTextCursor(cursor); updateLineStates(); m_editor->setFocus();
}
void MarkdownEditor::applyBlock(const QString& prefix, int kind) {
    revealSelection(); QTextCursor cursor = m_editor->textCursor(); cursor.beginEditBlock(); Q_UNUSED(kind);
    const int end = cursor.selectionEnd(); int inserted = 0; cursor.setPosition(cursor.selectionStart());
    cursor.movePosition(QTextCursor::StartOfBlock);
    do {
        cursor.insertText(prefix); inserted += prefix.size();
        if (!cursor.movePosition(QTextCursor::NextBlock)) break;
    } while (cursor.position() <= end + inserted);
    cursor.endEditBlock(); synchronizeSource(); m_editingLine = cursor.blockNumber();
    m_editor->setTextCursor(cursor); updateLineStates(); m_editor->setFocus();
}
void MarkdownEditor::insertLink() {
    revealSelection(); bool ok = false;
    const QString link = QInputDialog::getText(this, QStringLiteral("插入链接"), QStringLiteral("地址（http、https 或 mailto）："),
                                               QLineEdit::Normal, QStringLiteral("https://"), &ok);
    if (!ok || link.trimmed().isEmpty()) return;
    const QUrl url(link.trimmed());
    if (!QStringList({QStringLiteral("https"), QStringLiteral("http"), QStringLiteral("mailto")}).contains(url.scheme().toLower())) return;
    revealSelection(); QTextCursor cursor = m_editor->textCursor(); QString label = cursor.selectedText(); if (label.isEmpty()) label = QStringLiteral("链接文字");
    cursor.insertText(QStringLiteral("[%1](%2)").arg(label, url.toString())); m_editor->setTextCursor(cursor); m_editor->setFocus();
}
void MarkdownEditor::chooseEntityReference() {
    if (!m_store) return;
    EntityPickerDialog dialog(m_store, this);
    if (dialog.exec() == QDialog::Accepted) insertEntityReference(dialog.selectedEntity().id);
}
bool MarkdownEditor::insertEntityReference(const QString& entityId) {
    if (!m_store) return false;
    const Entity target = m_store->entity(entityId); if (target.id.isEmpty()) return false;
    setPreview(false); revealSelection(); QString label = target.title;
    label.replace(QLatin1Char('\\'), QStringLiteral("\\\\")); label.replace(QLatin1Char('['), QStringLiteral("\\["));
    label.replace(QLatin1Char(']'), QStringLiteral("\\]"));
    QTextCursor cursor = m_editor->textCursor(); cursor.insertText(QStringLiteral("[%1](kl:%2)").arg(label, target.id));
    m_editor->setTextCursor(cursor); m_editor->setFocus(); return true;
}
} // namespace kl
