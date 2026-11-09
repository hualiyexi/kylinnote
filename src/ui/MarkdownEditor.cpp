#include "MarkdownEditor.h"
#include <QDesktopServices>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QRegularExpression>
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
#include "../app/TaskStore.h"
#include "../domain/InternalLink.h"
#include "AppIcons.h"
#include "AppStyle.h"
#include "EntityPicker.h"
#include "Markdown.h"

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
    };
    const int index = static_cast<int>(action);
    if (index < 0 || index >= static_cast<int>(MarkdownEditor::FormatAction::Count)) return specs[0];
    return specs[index];
}

class SourceBlockData : public QTextBlockUserData {
public:
    QString source;
    bool rendered = false;
};

SourceBlockData* sourceData(const QTextBlock& block) {
    return static_cast<SourceBlockData*>(block.userData());
}

struct SavedPosition {
    int block = 0;
    int offset = 0;
    bool atEnd = false;
};

SavedPosition savePosition(const QTextDocument* document, int position) {
    const QTextBlock block = document->findBlock(position);
    SavedPosition result;
    result.block = qMax(0, block.blockNumber());
    result.offset = position - block.position();
    result.atEnd = position == block.position() + block.length() - 1;
    return result;
}

int restorePosition(const QTextDocument* document, const SavedPosition& saved) {
    QTextBlock block = document->findBlockByNumber(qMin(saved.block, document->blockCount() - 1));
    if (!block.isValid()) block = document->lastBlock();
    return block.position() + (saved.atEnd ? block.length() - 1 : qMin(saved.offset, block.length() - 1));
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

void insertInline(QTextCursor& cursor, const QString& text, const QTextCharFormat& base, int depth = 0) {
    if (depth > 16) { cursor.insertText(text, base); return; }
    QString literal;
    const auto flush = [&]() {
        if (!literal.isEmpty()) { cursor.insertText(literal, base); literal.clear(); }
    };
    int i = 0;
    while (i < text.size()) {
        if (text.at(i) == QLatin1Char('\\') && i + 1 < text.size()) {
            literal += text.at(i + 1); i += 2; continue;
        }
        if (text.at(i) == QLatin1Char('`')) {
            const int end = findToken(text, QStringLiteral("`"), i + 1);
            if (end > i + 1) {
                flush(); QTextCharFormat format = base;
                format.setFontFamily(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
                format.setBackground(QColor(QStringLiteral("#EEF1F5")));
                cursor.insertText(text.mid(i + 1, end - i - 1), format); i = end + 1; continue;
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
                insertInline(cursor, text.mid(i + delimiter.size(), end - i - delimiter.size()), format, depth + 1);
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
                    format.setForeground(QColor(target.startsWith(QStringLiteral("kl:")) ? QStringLiteral("#7357B8")
                                                                                           : QStringLiteral("#2768A8")));
                    format.setToolTip(target);
                    insertInline(cursor, text.mid(i + 1, bracket - i - 1), format, depth + 1);
                    i = parenthesis + 1; continue;
                }
            }
        }
        literal += text.at(i++);
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
    static const QRegularExpression inlineMarker(QStringLiteral("[\\\\`*_~\\[]"));
    return !block.match(source).hasMatch() && !inlineMarker.match(source).hasMatch();
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
    m_editor->setPlaceholderText(QStringLiteral("记录任务说明。当前行显示 Markdown，光标离开后自动排版。"));
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
                if (m_editor->hasFocus() || m_editingLine >= 0) m_editingLine = m_editor->textCursor().blockNumber();
                updateLineStates();
            });
        }
    });
    connect(m_editor, &QTextEdit::cursorPositionChanged, this, [this]() {
        if (m_loading || m_rendering) return;
        const int line = m_editor->textCursor().blockNumber();
        if (line == m_editingLine) return;
        synchronizeSource();
        m_editingLine = line;
        updateLineStates();
    });
    connect(m_editor, &QTextEdit::selectionChanged, this, [this]() {
        emit selectionChanged(m_editor->textCursor().hasSelection());
    });
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
}
QString MarkdownEditor::content() const { return m_sourceLines.join(QLatin1Char('\n')); }
void MarkdownEditor::setPreview(bool preview) {
    if (m_loading || m_rendering) { m_tabs->setCurrentIndex(preview ? 1 : 0); return; }
    synchronizeSource();
    m_editingLine = preview ? -1 : m_editor->textCursor().blockNumber();
    updateLineStates(); refreshPreview(); m_tabs->setCurrentIndex(preview ? 1 : 0);
    m_stack->setCurrentIndex(preview ? 1 : 0);
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
    m_rendering = false;
}

void MarkdownEditor::renderLine(int line, bool source) {
    QTextDocument* document = m_editor->document();
    QTextBlock block = document->findBlockByNumber(line);
    if (!block.isValid() || line < 0 || line >= m_sourceLines.size()) return;
    const QString& markdown = m_sourceLines.at(line);
    SourceBlockData* oldData = sourceData(block);
    if (oldData && oldData->source == markdown && oldData->rendered == !source) return;

    // 源码块被输入时绝不做无意义的替换；纯文本行切换状态也不污染撤销栈。
    if (block.text() == markdown && ((source && (!oldData || !oldData->rendered)) || plainSource(m_sourceLines, line))) {
        auto* data = new SourceBlockData; data->source = markdown; data->rendered = !source;
        block.setUserData(data); return;
    }

    QTextCursor cursor(block);
    cursor.setPosition(block.position());
    cursor.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    cursor.setBlockFormat(QTextBlockFormat());
    if (source) {
        cursor.insertText(markdown, QTextCharFormat());
    } else {
        QTextCharFormat base;
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
            base.setFontFamily(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
            blockFormat.setBackground(QColor(QStringLiteral("#F3F4F6")));
            blockFormat.setLeftMargin(8); blockFormat.setRightMargin(8);
            cursor.setBlockFormat(blockFormat); cursor.insertText(markdown, base);
        } else {
            const QRegularExpressionMatch headingMatch = headingMarker.match(markdown);
            const QRegularExpressionMatch checklistMatch = checklist.match(markdown);
            const QRegularExpressionMatch unorderedMatch = unordered.match(markdown);
            const QRegularExpressionMatch orderedMatch = ordered.match(markdown);
            const QRegularExpressionMatch quoteMatch = quote.match(markdown);
            if (headingMatch.hasMatch()) {
                static const qreal scales[] = {1.85, 1.60, 1.38, 1.20, 1.10, 1.00};
                const int level = qBound(1, headingMatch.captured(1).size(), 6);
                qreal size = document->defaultFont().pointSizeF(); if (size <= 0) size = 10.0;
                base.setFontPointSize(size * scales[level - 1]); base.setFontWeight(QFont::Bold);
                blockFormat.setTopMargin(level <= 2 ? 10 : 6); blockFormat.setBottomMargin(5);
                cursor.setBlockFormat(blockFormat); insertInline(cursor, headingMatch.captured(2), base);
            } else if (checklistMatch.hasMatch()) {
                blockFormat.setLeftMargin(20); blockFormat.setBottomMargin(2); cursor.setBlockFormat(blockFormat);
                cursor.insertText(QString(QChar(checklistMatch.captured(1).compare(QStringLiteral("x"), Qt::CaseInsensitive) == 0 ? 0x2611 : 0x2610))
                                  + QStringLiteral(" "), base);
                insertInline(cursor, checklistMatch.captured(2), base);
            } else if (unorderedMatch.hasMatch()) {
                blockFormat.setLeftMargin(20); blockFormat.setBottomMargin(2); cursor.setBlockFormat(blockFormat);
                cursor.insertText(QStringLiteral("• "), base); insertInline(cursor, unorderedMatch.captured(1), base);
            } else if (orderedMatch.hasMatch()) {
                blockFormat.setLeftMargin(20); blockFormat.setBottomMargin(2); cursor.setBlockFormat(blockFormat);
                cursor.insertText(orderedMatch.captured(1) + orderedMatch.captured(2) + QStringLiteral(" "), base);
                insertInline(cursor, orderedMatch.captured(3), base);
            } else if (quoteMatch.hasMatch()) {
                base.setFontItalic(true); base.setForeground(QColor(QStringLiteral("#5E6B7B")));
                blockFormat.setLeftMargin(12); blockFormat.setBackground(QColor(100, 116, 139, 18));
                blockFormat.setTopMargin(3); blockFormat.setBottomMargin(3); cursor.setBlockFormat(blockFormat);
                QTextCharFormat bar = base; bar.setForeground(QColor(QStringLiteral("#94A3B8")));
                cursor.insertText(QStringLiteral("▏ "), bar); insertInline(cursor, quoteMatch.captured(1), base);
            } else {
                cursor.setBlockFormat(blockFormat); insertInline(cursor, markdown, base);
            }
        }
    }
    block = document->findBlockByNumber(line);
    auto* data = new SourceBlockData; data->source = markdown; data->rendered = !source;
    block.setUserData(data);
}

void MarkdownEditor::updateLineStates() {
    if (m_loading || m_rendering) return;
    m_rendering = true;
    const QTextCursor current = m_editor->textCursor();
    const SavedPosition anchor = savePosition(m_editor->document(), current.anchor());
    const SavedPosition position = savePosition(m_editor->document(), current.position());
    for (int i = 0; i < m_sourceLines.size(); ++i) renderLine(i, i == m_editingLine);
    QTextCursor restored(m_editor->document());
    restored.setPosition(restorePosition(m_editor->document(), anchor));
    restored.setPosition(restorePosition(m_editor->document(), position), QTextCursor::KeepAnchor);
    m_editor->setTextCursor(restored);
    m_rendering = false;
}

void MarkdownEditor::synchronizeSource() {
    QStringList result;
    for (QTextBlock block = m_editor->document()->begin(); block != m_editor->document()->end(); block = block.next()) {
        SourceBlockData* data = sourceData(block);
        if (data && data->rendered && block.blockNumber() != m_editingLine) result.append(data->source);
        else result.append(block.text());
    }
    m_sourceLines = result;
}

void MarkdownEditor::revealLines(int first, int last) {
    if (m_loading || m_rendering) return;
    synchronizeSource();
    first = qBound(0, first, m_sourceLines.size() - 1);
    last = qBound(first, last, m_sourceLines.size() - 1);
    const QTextCursor current = m_editor->textCursor();
    const SavedPosition anchor = savePosition(m_editor->document(), current.anchor());
    const SavedPosition position = savePosition(m_editor->document(), current.position());
    m_rendering = true;
    for (int i = first; i <= last; ++i) renderLine(i, true);
    m_editingLine = current.blockNumber();
    QTextCursor restored(m_editor->document());
    restored.setPosition(restorePosition(m_editor->document(), anchor));
    restored.setPosition(restorePosition(m_editor->document(), position), QTextCursor::KeepAnchor);
    m_editor->setTextCursor(restored);
    m_rendering = false;
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
        } else if (event->type() == QEvent::KeyPress) {
            const QKeyEvent* key = static_cast<QKeyEvent*>(event);
            const QTextCursor cursor = m_editor->textCursor();
            const int first = m_editor->document()->findBlock(cursor.selectionStart()).blockNumber();
            const int last = m_editor->document()->findBlock(cursor.selectionEnd()).blockNumber();
            const bool modifies = key->matches(QKeySequence::Paste) || key->matches(QKeySequence::Cut)
                                  || key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete
                                  || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter
                                  || !key->text().isEmpty();
            if (modifies && first != last) revealLines(first, last);
            else if (key->key() == Qt::Key_Backspace && !cursor.hasSelection()
                     && cursor.positionInBlock() == 0 && cursor.blockNumber() > 0)
                revealLines(cursor.blockNumber() - 1, cursor.blockNumber());
            else if (key->key() == Qt::Key_Delete && !cursor.hasSelection()
                     && cursor.positionInBlock() == cursor.block().length() - 1
                     && cursor.blockNumber() + 1 < m_editor->document()->blockCount())
                revealLines(cursor.blockNumber(), cursor.blockNumber() + 1);
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
    revealSelection(); Q_UNUSED(kind);
    QTextCursor cursor = m_editor->textCursor();
    const QString selected = cursor.hasSelection() ? cursor.selectedText() : QStringLiteral("文字");
    const int start = cursor.selectionStart(); cursor.beginEditBlock(); cursor.insertText(before + selected + after); cursor.endEditBlock();
    synchronizeSource();
    cursor.setPosition(start + before.size()); cursor.setPosition(start + before.size() + selected.size(), QTextCursor::KeepAnchor);
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
