#include "MarkdownEditor.h"
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QEnterEvent>
#include <QFrame>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSet>
#include <QSignalBlocker>
#include <QShortcut>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtMath>
#include "../app/TaskStore.h"
#include "../domain/InternalLink.h"
#include "../domain/SourceChange.h"
#include "AppIcons.h"
#include "EditorPopups.h"
#include "EditorTextEdit.h"
#include "Markdown.h"
#include "MarkdownDocument.h"
#include "PopupMenu.h"
#include "design/FontManager.h"
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
        {"链接", "", "", 8, "", IconId::Link},
        {"关联任务/笔记", "", "", 9, "", IconId::RelatedEntity},
        {"下划线", "<u>", "</u>", 10, "Ctrl+U", IconId::Underline},
        {"高亮", "", "", 11, "", IconId::Highlight},
        {"清除格式", "", "", 12, "", IconId::FormatClear},
        {"水平分割线", "", "", 13, "", IconId::HorizontalRule},
    };
    const int index = static_cast<int>(action);
    if (index < 0 || index >= static_cast<int>(MarkdownEditor::FormatAction::Count)) return specs[0];
    return specs[index];
}

using CodeLine = MarkdownDocument::CodeContext;

CodeLine codeLine(const QStringList& lines, int line) { return MarkdownDocument::codeContext(lines, line); }
QString plainLine(const QString& source, CodeLine code, const QHash<QString, Entity>* targets) {
    return MarkdownDocument::plainLine(source, code, targets ? *targets : QHash<QString, Entity>());
}

bool isEscaped(const QString& text, int position) {
    int slashes = 0;
    for (int i = position - 1; i >= 0 && text.at(i) == QLatin1Char('\\'); --i) ++slashes;
    return slashes % 2 != 0;
}

class PresentationEdit {
public:
    explicit PresentationEdit(QTextDocument* document, bool userEdit = false) : m_document(document), m_cursor(document) {
        m_pauseUndo = !userEdit && document->isUndoRedoEnabled() && !document->isUndoAvailable() && !document->isRedoAvailable();
        if (m_pauseUndo) document->setUndoRedoEnabled(false);
        else if (userEdit) m_cursor.beginEditBlock();
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

struct SavedPosition { int block = 0; int offset = 0; };

SavedPosition savePosition(const QTextDocument* document, int position, bool selectionEnd = false) {
    const QTextBlock block = document->findBlock(position);
    SavedPosition result;
    result.block = qMax(0, block.blockNumber());
    const auto data = MarkdownDocument::readBlock(block);
    const int visible = qBound(0, position - block.position(), data.starts.size());
    if (data.starts.isEmpty()) result.offset = data.emptyOffset;
    else if (selectionEnd && visible == 0) result.offset = 0;
    else result.offset = visible == data.starts.size() || (selectionEnd && visible > 0)
        ? data.ends.at(qMax(0, visible - 1)) : data.starts.at(visible);
    return result;
}

int restorePosition(const QTextDocument* document, const SavedPosition& saved) {
    QTextBlock block = document->findBlockByNumber(qMin(saved.block, document->blockCount() - 1));
    if (!block.isValid()) block = document->lastBlock();
    const auto data = MarkdownDocument::readBlock(block);
    int offset = data.starts.size();
    for (int i = 0; i < data.starts.size(); ++i) {
        if (saved.offset <= data.starts.at(i)) { offset = i; break; }
        if (saved.offset <= data.ends.at(i)) {
            offset = i + 1;
            while (offset < data.starts.size() && data.starts.at(offset) == data.starts.at(i)
                   && data.ends.at(offset) == data.ends.at(i)) ++offset;
            break;
        }
    }
    return block.position() + qBound(0, offset, block.length() - 1);
}
}  // namespace

MarkdownEditor::MarkdownEditor(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("MarkdownEditor"));
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);

    for (int i = 0; i < static_cast<int>(FormatAction::Count); ++i) {
        const auto action = static_cast<FormatAction>(i);
        const QString key = formatShortcut(action);
        if (key.isEmpty()) continue;
        auto* shortcut = new QShortcut(QKeySequence(key), this);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, action]() { applyFormat(action); });
    }
    m_stack = new QStackedWidget(this); m_editor = new EditorTextEdit(this);
    m_editor->setObjectName(QStringLiteral("DescriptionEditor"));
    // External HTML is not imported; Markdown MIME uses the same restricted
    // local codec as document loading.
    m_editor->setAcceptRichText(false); m_editor->setMinimumHeight(150);
    m_editor->setFont(ds::qfont(ds::FontScene::Body));
    m_editor->setPlaceholderText(QStringLiteral("记录内容…"));
    m_editor->viewport()->setCursor(Qt::IBeamCursor);
    m_preview = new DocumentPreview(this); m_preview->setMinimumHeight(150);
    m_stack->addWidget(m_editor); m_stack->addWidget(m_preview); layout->addWidget(m_stack, 1);
    qApp->installEventFilter(this);
    auto* body = static_cast<EditorTextEdit*>(m_editor);
    body->selectedSource = [this]() { return MarkdownDocument::selectedMarkdown(m_editor->textCursor()); };
    body->insertMarkdown = [this](const QString& source) {
        QTextDocument fragment;
        MarkdownDocument::importMarkdown(&fragment, source, m_targets, m_themeOverride);
        QTextCursor cursor = m_editor->textCursor();
        cursor.insertFragment(QTextDocumentFragment(&fragment));
        m_editor->setTextCursor(cursor);
    };
    body->sourceCursorPosition = [this]() { return sourceSelection().second; };
    body->finishEdit = [this]() {
        if (!m_preeditActive) { synchronizeSource(); updateLineStates(); }
    };
    body->editCompleted = [this](bool continuation) { emit editFinished(continuation); };
    body->paragraphInserted = [this](const QTextBlockFormat& previous) { continueParagraph(previous); };
    body->presentationChanging = [this]() { return m_rendering || m_editingSource; };
    body->blockInsertionAllowed = [this](int block) { return blockInsertionAllowed(block); };
    connect(body, &EditorTextEdit::blockMenuRequested, this, &MarkdownEditor::showBlockMenu);
    connect(body, &EditorTextEdit::compositionChanged, this, [this](bool active) {
        m_preeditActive = active;
        if (active) closeHints();
        else { scheduleHints(); scheduleLineUpdate(); }
    });
    m_completion = new EditorCompletion(m_editor, this);
    connect(m_completion, &EditorCompletion::chosen, this, &MarkdownEditor::acceptHint);
    connect(m_completion, &EditorCompletion::dismissed, this, [this]() { closeHints(true); });
    connect(m_preview, &DocumentPreview::internalLinkActivated, this, &MarkdownEditor::internalLinkActivated);
    connect(m_editor, &QTextEdit::textChanged, this, [this]() {
        if (m_loading || m_rendering || m_editingSource) return;
        // 不在 QTextCursor 正在替换选区时重设光标，否则会破坏 Qt 的撤销分组。
        synchronizeSource();
        if (m_linkTools) m_linkTools->hide();
        refreshPreview(); emit contentChanged();
        scheduleHints();
        scheduleLineUpdate();
    });
    connect(m_editor, &QTextEdit::cursorPositionChanged, this, [this]() {
        if (m_loading || m_rendering || m_editingSource) return;
        scheduleHints();
    });
    connect(m_editor, &QTextEdit::selectionChanged, this, [this]() {
        if (!m_rendering && !m_editingSource) {
            emit selectionChanged(m_editor->textCursor().hasSelection());
            scheduleHints();
        }
    });
    connect(m_editor->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            this, [this](const QSizeF&) { updateContentHeight(); });
    connect(m_preview->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            this, [this](const QSizeF&) { updateContentHeight(); });
    connect(&FontManager::instance(), &FontManager::fontChanged,
            this, &MarkdownEditor::applyFontPreference);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this](ds::Theme) {
        if (m_loading || m_rendering || m_themeOverride != ds::Theme::Count) return;
        synchronizeSource(); updateLineStates(true); refreshPreview();
    });
    updateContentHeight();
}

MarkdownEditor::~MarkdownEditor() {
    m_loading = true;
    qApp->removeEventFilter(this);
    disconnect(qApp, nullptr, this, nullptr);
    disconnect(m_editor, nullptr, this, nullptr);
    disconnect(m_preview, nullptr, this, nullptr);
    disconnect(m_editor->document()->documentLayout(), nullptr, this, nullptr);
    disconnect(m_preview->document()->documentLayout(), nullptr, this, nullptr);
    disconnect(&FontManager::instance(), nullptr, this, nullptr);
    disconnect(&ThemeManager::instance(), nullptr, this, nullptr);
    if (m_store) disconnect(m_store, nullptr, this, nullptr);
    auto* body = static_cast<EditorTextEdit*>(m_editor);
    body->selectedSource = {};
    body->insertMarkdown = {};
    body->sourceCursorPosition = {};
    body->finishEdit = {};
    body->editCompleted = {};
    body->historyRequested = {};
    body->historyAvailable = {};
    body->paragraphInserted = {};
    body->presentationChanging = {};
    body->blockInsertionAllowed = {};
    closePopups();
    delete m_linkTools;
}

void MarkdownEditor::setStore(TaskStore* store) {
    if (m_store == store) return;
    closePopups(); ++m_contextGeneration;
    if (m_store) disconnect(m_store, nullptr, this, nullptr);
    m_store = store; m_preview->setStore(store);
    if (m_store) {
        connect(m_store, &TaskStore::taskChanged, this, [this](QString) { refreshTargets(); scheduleHints(); });
        connect(m_store, &TaskStore::tasksReset, this, [this]() {
            closePopups(); ++m_contextGeneration; refreshTargets();
        });
        connect(m_store, &QObject::destroyed, this, [this]() {
            closePopups(); ++m_contextGeneration; m_targets.clear(); refreshPreview();
        });
    }
    refreshTargets();
}
void MarkdownEditor::setEntityId(const QString& entityId) {
    if (m_entityId == entityId) return;
    closePopups(); ++m_contextGeneration; m_entityId = entityId;
}
void MarkdownEditor::refreshTargets() {
    const auto targets = m_store ? Markdown::entityMap(m_store->entities()) : Markdown::EntityMap();
    QSet<QString> changed;
    for (auto it = m_targets.constBegin(); it != m_targets.constEnd(); ++it) {
        const auto next = targets.constFind(it.key());
        if (next == targets.constEnd() || next->title != it->title || next->deleted != it->deleted)
            changed.insert(it.key());
    }
    for (auto it = targets.constBegin(); it != targets.constEnd(); ++it)
        if (!m_targets.contains(it.key())) changed.insert(it.key());
    m_targets = targets;
    bool affected = false;
    if (!m_loading && !m_rendering && !changed.isEmpty()) {
        // Only caption, tooltip or availability changes invalidate a reference.
        // An unrelated save must not add a presentation-only undo operation.
        for (QTextBlock block = m_editor->document()->begin(); block.isValid(); block = block.next()) {
            if (block.blockNumber() >= m_sourceLines.size()
                || codeLine(m_sourceLines, block.blockNumber()) != CodeLine::Normal) continue;
            for (const QString& id : internalTargets(m_sourceLines.value(block.blockNumber()))) {
                if (changed.contains(id)) {
                    affected = true;
                    break;
                }
            }
        }
        if (affected) {
            if (m_editingSource) QTimer::singleShot(0, this, [this]() { scheduleLineUpdate(); });
            else updateLineStates(true);
        }
    }
    refreshPreview();
}
void MarkdownEditor::setContent(const QString& content) {
    closePopups(); ++m_contextGeneration;
    m_sourceLines = content.split(QLatin1Char('\n'), QString::KeepEmptyParts);
    m_loading = true; renderDocument(); refreshPreview(); m_loading = false;
    updateContentHeight();
    static_cast<EditorTextEdit*>(m_editor)->refreshBlockIndicator();
}
void MarkdownEditor::applyLiveContent(const QString& source) {
    if (source == content() || m_preeditActive) return;
    const QPointer<MarkdownEditor> guard(this);
    const QString previousText = m_editor->toPlainText();
    const QTextCursor previous = m_editor->textCursor();
    const int anchor = previous.anchor(), position = previous.position();
    const int vertical = m_editor->verticalScrollBar()->value();
    const int horizontal = m_editor->horizontalScrollBar()->value();
    closePopups();
    ++m_contextGeneration;
    {
        const QSignalBlocker signalBlocker(m_editor);
        QScopedValueRollback<bool> editing(m_editingSource, true);
        QScopedValueRollback<bool> sizing(m_sizing, true);
        static_cast<EditorTextEdit*>(m_editor)->breakTyping();
        m_sourceLines = source.split(QLatin1Char('\n'), QString::KeepEmptyParts);
        updateLineStates();
        const SourceChange change = sourceChange(previousText, m_editor->toPlainText());
        const auto selection = sourceSelectionAfter(change, anchor, position);
        QTextCursor restored(m_editor->document());
        restored.setPosition(selection.first);
        restored.setPosition(selection.second, QTextCursor::KeepAnchor);
        m_editor->setTextCursor(restored);
        if (!restored.hasSelection()) {
            const auto data = MarkdownDocument::readBlock(restored.block());
            const int visible = restored.positionInBlock();
            const bool caption = visible > 0 && visible < data.starts.size()
                && data.starts.at(visible - 1) == data.starts.at(visible)
                && data.ends.at(visible - 1) == data.ends.at(visible);
            const int offset = MarkdownDocument::insertionOffset(restored.block(), visible, restored.charFormat());
            m_editor->setCurrentCharFormat(caption ? restored.charFormat()
                : MarkdownDocument::insertionFormat(restored.block(), offset));
        }
        refreshPreview();
        m_editor->verticalScrollBar()->setValue(vertical);
        m_editor->horizontalScrollBar()->setValue(horizontal);
    }
    updateContentHeight();
    if (!guard) return;
    static_cast<EditorTextEdit*>(m_editor)->refreshBlockIndicator();
    emit selectionChanged(m_editor->textCursor().hasSelection());
}

void MarkdownEditor::setSharedHistory(std::function<void(bool)> requested,
                                      std::function<bool(bool)> available) {
    auto* body = static_cast<EditorTextEdit*>(m_editor);
    body->breakTyping();
    body->historyRequested = std::move(requested);
    body->historyAvailable = std::move(available);
    body->document()->setUndoRedoEnabled(!body->historyRequested);
}

QString MarkdownEditor::content() const { return m_sourceLines.join(QLatin1Char('\n')); }
void MarkdownEditor::applyFontPreference(const QString& family) {
    if (family.isEmpty()) return;
    synchronizeSource();
    QFont font = m_editor->document()->defaultFont(); font.setFamily(family);
    m_editor->document()->setDefaultFont(font);
    updateLineStates(true); refreshPreview(); updateContentHeight();
}
QString MarkdownEditor::plainTextContent() const {
    QStringList lines;
    const auto contexts = MarkdownDocument::codeContexts(m_sourceLines);
    for (int i = 0; i < m_sourceLines.size(); ++i) lines.append(plainLine(m_sourceLines.at(i), contexts.at(i), &m_targets));
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
void MarkdownEditor::setExpandWhenEmpty(bool enabled) {
    if (m_expandWhenEmpty == enabled) return;
    m_expandWhenEmpty = enabled;
    updateContentHeight();
}
void MarkdownEditor::setFillAvailableHeight(bool enabled) {
    if (m_fillAvailableHeight == enabled) return;
    m_fillAvailableHeight = enabled;
    updateContentHeight();
}
void MarkdownEditor::setBlockControlsEnabled(bool enabled) {
    if (m_blockControlsEnabled == enabled) return;
    m_blockControlsEnabled = enabled;
    layout()->setContentsMargins(0, 0, enabled ? 24 : 0, 0);
    static_cast<EditorTextEdit*>(m_editor)->setBlockGutterEnabled(enabled);
    m_preview->setContentsMargins(enabled ? 24 : 0, 0, 0, 0);
    if (!enabled && m_blockMenu) m_blockMenu->hide();
    updateContentHeight();
}
void MarkdownEditor::setThemeOverride(ds::Theme theme) {
    if (m_themeOverride == theme) return;
    m_themeOverride = theme;
    static_cast<EditorTextEdit*>(m_editor)->setThemeOverride(theme);
    if (!m_sourceLines.isEmpty()) updateLineStates(true);
}

void MarkdownEditor::updateContentHeight() {
    if (m_sizing || m_rendering || !m_editor || !m_stack) return;
    m_sizing = true;
    QTextEdit* edit = m_stack->currentWidget() == m_preview ? static_cast<QTextEdit*>(m_preview) : m_editor;
    const bool empty = m_flowLayout && edit == m_editor && edit->document()->isEmpty();
    const int minimum = empty ? edit->fontMetrics().lineSpacing() * 3 : edit->fontMetrics().height();
    const int frame = edit->frameWidth() * 2;
    const int height = qMax(minimum, qCeil(edit->document()->documentLayout()->documentSize().height()))
        + frame + edit->contentsMargins().top() + edit->contentsMargins().bottom();
    const QMargins margins = layout()->contentsMargins();
    const int total = height + margins.top() + margins.bottom();
    if (m_flowLayout) {
        const bool expand = m_fillAvailableHeight || (empty && m_expandWhenEmpty);
        edit->setMinimumHeight(height);
        edit->setMaximumHeight(expand ? QWIDGETSIZE_MAX : height);
        m_stack->setMinimumHeight(height);
        m_stack->setMaximumHeight(expand ? QWIDGETSIZE_MAX : height);
        setMinimumHeight(total);
        setMaximumHeight(expand ? QWIDGETSIZE_MAX : total);
        setSizePolicy(QSizePolicy::Expanding, expand ? QSizePolicy::Expanding : QSizePolicy::Fixed);
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
    if (m_loading || m_rendering || m_stack->currentIndex() == (preview ? 1 : 0)) return;
    closePopups();
    synchronizeSource();
    updateLineStates(); refreshPreview();
    m_stack->setCurrentIndex(preview ? 1 : 0);
    updateContentHeight();
}
void MarkdownEditor::setCompact(bool compact) {
    if (compact) setBlockControlsEnabled(false);
    m_editor->setFrameShape(QFrame::NoFrame);
    m_editor->setContentsMargins(0, 0, 0, 0); m_editor->document()->setDocumentMargin(0);
    m_preview->setContentsMargins(0, 0, 0, 0); m_preview->document()->setDocumentMargin(0);
}
void MarkdownEditor::refreshPreview() {
    m_preview->setHtml(Markdown::toHtml(content(), m_targets));
}

void MarkdownEditor::renderDocument() {
    m_rendering = true;
    MarkdownDocument::importMarkdown(m_editor->document(), m_sourceLines.join(QLatin1Char('\n')),
                                     m_targets, m_themeOverride);
    QTextCursor cursor(m_editor->document()); cursor.movePosition(QTextCursor::Start);
    m_editor->setTextCursor(cursor);
    m_editor->document()->setModified(false);
    m_editor->document()->clearUndoRedoStacks();
    m_rendering = false;
}

void MarkdownEditor::scheduleLineUpdate() {
    if (m_lineUpdatePending || m_loading || m_rendering || m_editingSource || m_preeditActive) return;
    m_lineUpdatePending = true;
    QTimer::singleShot(0, this, [this]() {
        m_lineUpdatePending = false;
        if (m_loading || m_rendering || m_editingSource || m_preeditActive) return;
        synchronizeSource(); updateLineStates();
    });
}

bool MarkdownEditor::blockInsertionAllowed(int block) const {
    return m_blockControlsEnabled && !m_loading && !m_rendering && !m_editingSource && !m_preeditActive
        && block >= 0 && block < m_sourceLines.size() && m_sourceLines.at(block).isEmpty()
        && codeLine(m_sourceLines, block) == CodeLine::Normal;
}

void MarkdownEditor::sourceEditFinished() {
    updateLineStates(); refreshPreview(); updateContentHeight();
    emit contentChanged();
    emit selectionChanged(m_editor->textCursor().hasSelection());
    scheduleHints();
    emit editFinished(false);
}

void MarkdownEditor::updateLineStates(bool force) {
    if (m_loading || m_rendering || m_preeditActive || (m_editor->document()->isRedoAvailable() && !m_editingSource)) return;
    QTextDocument* document = m_editor->document();
    const auto contexts = MarkdownDocument::codeContexts(m_sourceLines);
    bool changed = document->blockCount() != m_sourceLines.size();
    for (QTextBlock block = document->begin(); !changed && block.isValid(); block = block.next()) {
        const auto format = block.blockFormat();
        changed = force || !MarkdownDocument::hasSnapshot(block)
            || format.stringProperty(MarkdownDocument::SourceProperty) != m_sourceLines.value(block.blockNumber())
            || format.intProperty(MarkdownDocument::ContextProperty) != static_cast<int>(contexts.at(block.blockNumber()));
    }
    if (!changed) return;
    m_rendering = true;
    // Projection changes are not cursor navigation. In particular a checkbox
    // click must not ask a flow-layout host to scroll to the unchanged caret.
    const QSignalBlocker editorSignals(m_editor);
    const QTextCursor current = m_editor->textCursor();
    const QTextBlock anchorBlock = document->findBlock(current.anchor());
    const QString anchorText = anchorBlock.text(), positionText = current.block().text();
    const int anchorVisible = current.anchor() - anchorBlock.position(), positionVisible = current.positionInBlock();
    SavedPosition anchor = savePosition(document, current.anchor(), current.hasSelection() && current.anchor() == current.selectionEnd());
    SavedPosition position = savePosition(document, current.position(), current.hasSelection() && current.position() == current.selectionEnd());
    if (!current.hasSelection()) {
        position.offset = MarkdownDocument::insertionOffset(current.block(), current.positionInBlock(), m_editor->currentCharFormat());
        anchor = position;
    }
    const int scroll = m_editor->verticalScrollBar()->value();
    {
        PresentationEdit edit(document, m_editingSource);
        QTextCursor tail(document);
        while (document->blockCount() < m_sourceLines.size()) { tail.movePosition(QTextCursor::End); tail.insertBlock(); }
        while (document->blockCount() > m_sourceLines.size()) {
            tail.movePosition(QTextCursor::End); tail.movePosition(QTextCursor::StartOfBlock, QTextCursor::KeepAnchor);
            tail.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor); tail.removeSelectedText();
        }
        for (int i = 0; i < m_sourceLines.size(); ++i)
            MarkdownDocument::renderBlock(document->findBlockByNumber(i), m_sourceLines.at(i),
                                          contexts.at(i), m_targets, force, m_themeOverride);
        const auto restore = [document](const SavedPosition& saved, const QString& previousText, int visible) {
            const QTextBlock block = document->findBlockByNumber(qMin(saved.block, document->blockCount() - 1));
            return block.text() == previousText ? block.position() + qMin(visible, block.length() - 1)
                                                : restorePosition(document, saved);
        };
        QTextCursor restored(document);
        restored.setPosition(restore(anchor, anchorText, anchorVisible));
        restored.setPosition(restore(position, positionText, positionVisible), QTextCursor::KeepAnchor);
        m_editor->setTextCursor(restored);
        if (!restored.hasSelection()) {
            QTextCharFormat input = MarkdownDocument::insertionFormat(restored.block(), position.offset);
            const auto data = MarkdownDocument::readBlock(restored.block());
            const int visible = restored.positionInBlock();
            // Link captions map atomically for source commands, but native typing
            // inside a caption must keep its visible caret and character format.
            if (restored.block().text() == positionText && visible > 0 && visible < data.starts.size()
                && data.starts.at(visible - 1) == data.starts.at(visible) && data.ends.at(visible - 1) == data.ends.at(visible))
                input = restored.charFormat();
            m_editor->setCurrentCharFormat(input);
        }
        m_editor->verticalScrollBar()->setValue(scroll);
    }
    m_rendering = false;
    updateContentHeight();
    static_cast<EditorTextEdit*>(m_editor)->refreshBlockIndicator();
}

void MarkdownEditor::synchronizeSource() {
    QStringList result;
    for (QTextBlock block = m_editor->document()->begin(); block.isValid(); block = block.next())
        result.append(MarkdownDocument::readBlock(block).source);
    m_sourceLines = result;
    static_cast<EditorTextEdit*>(m_editor)->refreshBlockIndicator();
}

void MarkdownEditor::continueParagraph(const QTextBlockFormat& previous) {
    using namespace MarkdownDocument;
    const Kind previousKind = static_cast<Kind>(previous.intProperty(KindProperty));
    QString prefix;
    if (previousKind == Kind::UnorderedList || previousKind == Kind::OrderedList
        || previousKind == Kind::TaskList || previousKind == Kind::Quote) prefix = previous.stringProperty(PrefixProperty);
    if (previousKind == Kind::TaskList) {
        static const QRegularExpression state(QStringLiteral("\\[[xX]\\]"));
        prefix.replace(state, QStringLiteral("[ ]"));
    } else if (previousKind == Kind::OrderedList) {
        static const QRegularExpression number(QStringLiteral("[0-9]+"));
        const auto match = number.match(prefix);
        if (match.hasMatch()) prefix.replace(match.capturedStart(), match.capturedLength(), QString::number(match.captured(0).toInt() + 1));
    }
    QScopedValueRollback<bool> rendering(m_rendering, true);
    QTextCursor cursor = m_editor->textCursor();
    QTextBlockFormat format;
    format.setProperty(PrefixProperty, prefix);
    format.setProperty(KindProperty, static_cast<int>(prefix.isEmpty() ? Kind::Paragraph : previousKind));
    cursor.setBlockFormat(format);
}

bool MarkdownEditor::eventFilter(QObject* watched, QEvent* event) {
    auto* widget = qobject_cast<QWidget*>(watched);
    const bool ancestor = widget && (widget == this || widget->isAncestorOf(this));
    if (ancestor && (event->type() == QEvent::Hide || event->type() == QEvent::Close
                    || event->type() == QEvent::ParentChange
                    || (event->type() == QEvent::EnabledChange && !isEnabled()))) closePopups();
    if (ancestor && event->type() == QEvent::WindowDeactivate) {
        QTimer::singleShot(0, this, [this]() {
            if (!window()->isActiveWindow() && !ownsPopup(QApplication::activePopupWidget())) closePopups();
        });
    }
    if (ancestor && (event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
        if (m_linkTools) m_linkTools->hide();
        if (m_linkEditor && m_linkEditor->isVisible()) m_linkEditor->hide();
        if (m_completion && m_completion->isVisible()) scheduleHints();
    }
    if (watched == m_editor && event->type() == QEvent::ReadOnlyChange && m_editor->isReadOnly()) closePopups();
    if (watched == m_editor && event->type() == QEvent::ShortcutOverride
        && m_linkTools && m_linkTools->isVisible()
        && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        event->accept();
        return true;
    }
    if (watched == m_editor && event->type() == QEvent::ShortcutOverride
        && (m_preeditActive || (m_completion && m_completion->isVisible()))) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter
            || key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) {
            key->accept(); return true;
        }
    }
    if (watched == m_editor && !m_loading && !m_rendering && !m_editingSource) {
        if (event->type() == QEvent::FocusIn) {
            scheduleHints();
        } else if (event->type() == QEvent::FocusOut) {
            QTimer::singleShot(0, this, [this]() {
                if (!m_editor->hasFocus() && !ownsPopup(QApplication::activePopupWidget())) closeHints();
            });
        } else if (event->type() == QEvent::InputMethod) {
            m_preeditActive = !static_cast<QInputMethodEvent*>(event)->preeditString().isEmpty();
            if (m_preeditActive) closeHints();
        } else if (event->type() == QEvent::KeyPress) {
            const auto* key = static_cast<QKeyEvent*>(event);
            if (key->key() == Qt::Key_Escape && !m_preeditActive && m_linkTools && m_linkTools->isVisible()) {
                m_linkTools->hide();
                return true;
            }
            if (m_completion->isVisible() && !m_preeditActive) {
                if (key->key() == Qt::Key_Escape) { closeHints(true); return true; }
                if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter
                    || key->key() == Qt::Key_Up || key->key() == Qt::Key_Down)
                    return QWidget::eventFilter(watched, event);
            }
            if (m_editor->isReadOnly() || key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo)
                || key->matches(QKeySequence::Copy) || key->matches(QKeySequence::SelectAll))
                return QWidget::eventFilter(watched, event);
            const QTextCursor cursor = m_editor->textCursor();
            if (!cursor.hasSelection() && (key->key() == Qt::Key_Backspace || key->key() == Qt::Key_Delete)) {
                for (const MarkdownLink& link : markdownLinks(content())) {
                    const int begin = documentPositionForSource(link.start);
                    const int end = documentPositionForSource(link.start + link.length);
                    if ((key->key() == Qt::Key_Backspace && cursor.position() == end)
                        || (key->key() == Qt::Key_Delete && cursor.position() == begin)) {
                        replaceSource(link.start, link.length, QString()); return true;
                    }
                }
            }
            if (!m_preeditActive && !cursor.hasSelection() && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
                const auto kind = MarkdownDocument::kind(cursor.block());
                const bool prefixed = kind == MarkdownDocument::Kind::Heading || kind == MarkdownDocument::Kind::UnorderedList
                    || kind == MarkdownDocument::Kind::OrderedList || kind == MarkdownDocument::Kind::TaskList || kind == MarkdownDocument::Kind::Quote;
                const bool backspace = key->key() == Qt::Key_Backspace && cursor.positionInBlock() == 0;
                const bool emptyEnter = (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && cursor.block().text().isEmpty();
                if (prefixed && (backspace || emptyEnter)) { applyBlockStyle(BlockStyle::Paragraph); return true; }
            }
        }
    }
    if (watched == m_editor->viewport()) {
        if (event->type() == QEvent::Enter) {
            updateLinkHover(static_cast<QEnterEvent*>(event)->pos());
        } else if (event->type() == QEvent::Leave) {
            if (m_linkTools) m_linkTools->scheduleHide();
        } else if (event->type() == QEvent::MouseMove) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->buttons() & Qt::LeftButton) {
                if ((mouse->pos() - m_linkPressPosition).manhattanLength() > QApplication::startDragDistance()) m_pressedLink = LinkContext();
                if (m_linkTools) m_linkTools->hide();
            } else updateLinkHover(mouse->pos());
        } else if (event->type() == QEvent::MouseButtonPress) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            m_pressedLink = LinkContext();
            if (mouse->button() == Qt::LeftButton) {
                const QTextCursor cursor = m_editor->cursorForPosition(mouse->pos());
                const int line = cursor.blockNumber();
                if (isEnabled() && !m_editor->isReadOnly() && !m_preeditActive && line < m_sourceLines.size()
                    && MarkdownDocument::kind(cursor.block()) == MarkdownDocument::Kind::TaskList
                    && static_cast<EditorTextEdit*>(m_editor)->checkBoxRect(cursor.block()).contains(mouse->pos())) {
                    m_pressedCheckLine = line;
                    return true;
                }
                m_pressedLink = linkAt(mouse->pos());
                m_linkPressPosition = mouse->pos();
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            if (m_pressedCheckLine >= 0) {
                const int line = m_pressedCheckLine; m_pressedCheckLine = -1;
                const QTextBlock block = m_editor->document()->findBlockByNumber(line);
                if (mouse->button() == Qt::LeftButton && block.isValid()
                    && static_cast<EditorTextEdit*>(m_editor)->checkBoxRect(block).contains(mouse->pos())) {
                    static const QRegularExpression task(QStringLiteral("^[ \\t]*[-+*][ \\t]+\\[([ xX])\\]"));
                    const auto match = task.match(m_sourceLines.value(line));
                    if (match.hasMatch()) {
                        int offset = match.capturedStart(1);
                        for (int i = 0; i < line; ++i) offset += m_sourceLines.at(i).size() + 1;
                        replaceSource(offset, 1, match.captured(1) == QLatin1String(" ") ? QStringLiteral("x") : QStringLiteral(" "), true);
                    }
                }
                return true;
            }
            const LinkContext pressed = m_pressedLink;
            m_pressedLink = LinkContext();
            if (mouse->button() == Qt::LeftButton && mouse->modifiers() == Qt::NoModifier
                && !m_editor->textCursor().hasSelection() && linkContextValid(pressed)
                && (mouse->pos() - m_linkPressPosition).manhattanLength() <= QApplication::startDragDistance()) {
                const QString id = internalLinkId(pressed.target);
                if (!id.isEmpty()) { closePopups(); emit internalLinkActivated(id); return true; }
                if (isSafeLinkTarget(pressed.target)) { QDesktopServices::openUrl(QUrl(pressed.target)); return true; }
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MarkdownEditor::applyFormat(FormatAction action) {
    if (!isEnabled() || m_editor->isReadOnly()) return;
    setPreview(false);
    if (action == FormatAction::Link) { insertLink(); return; }
    if (action == FormatAction::EntityReference) { beginReference(); return; }
    if (action == FormatAction::Heading) { applyBlockStyle(BlockStyle::Heading2); return; }
    if (action == FormatAction::List) { applyBlockStyle(BlockStyle::UnorderedList); return; }
    if (action == FormatAction::OrderedList) { applyBlockStyle(BlockStyle::OrderedList); return; }
    if (action == FormatAction::Quote) { applyBlockStyle(BlockStyle::Quote); return; }
    if (action == FormatAction::HorizontalRule) { applyBlockStyle(BlockStyle::HorizontalRule); return; }
    if (action == FormatAction::Highlight) {
        applyInline(QStringLiteral("<mark data-color=\"%1\">").arg(m_highlightColor), QStringLiteral("</mark>"), 11); return;
    }
    if (action == FormatAction::ClearFormat) { clearFormatting(); return; }
    if (action == FormatAction::Count) return;
    const FormatSpec& spec = formatSpec(action);
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
    return {FormatAction::Bold, FormatAction::Italic, FormatAction::Strike, FormatAction::Underline,
            FormatAction::Highlight, FormatAction::Code, FormatAction::Link};
}
QVector<MarkdownEditor::FormatAction> MarkdownEditor::allFormats() const {
    QVector<FormatAction> actions;
    for (int i = 0; i < static_cast<int>(FormatAction::Count); ++i)
        actions.append(static_cast<FormatAction>(i));
    return actions;
}

void MarkdownEditor::applyInline(const QString& before, const QString& after, int kind) {
    const auto selection = sourceSelection();
    int start = qMin(selection.first, selection.second), end = qMax(selection.first, selection.second);
    const QString document = content();
    QString selected = start != end ? document.mid(start, end - start) : QStringLiteral("文字");
    bool remove = false;
    if (start != end) {
        QString existingBefore = before;
        if (kind == 11) {
            static const QRegularExpression mark(QStringLiteral("<mark data-color=\"(?:yellow|green|blue|pink|purple)\">$"));
            const auto match = mark.match(document.left(start));
            if (match.hasMatch()) existingBefore = match.captured(0);
        }
        if (start >= existingBefore.size() && document.mid(start - existingBefore.size(), existingBefore.size()) == existingBefore
            && document.mid(end, after.size()) == after) {
            start -= existingBefore.size(); end += after.size(); remove = kind != 11;
        } else if (selected.startsWith(before) && selected.endsWith(after) && selected.size() >= before.size() + after.size()) {
            selected = selected.mid(before.size(), selected.size() - before.size() - after.size()); remove = kind != 11;
        } else if (kind == 11) {
            static const QRegularExpression marked(QStringLiteral("^<mark data-color=\"(?:yellow|green|blue|pink|purple)\">([\\s\\S]*)</mark>$"));
            const auto match = marked.match(selected);
            if (match.hasMatch()) selected = match.captured(1);
        }
    }
    QString replacement = selected;
    if (!remove) {
        QStringList lines = selected.split(QLatin1Char('\n'), QString::KeepEmptyParts);
        for (QString& line : lines) line = before + line + after;
        replacement = lines.join(QLatin1Char('\n'));
    }
    replaceSource(start, end - start, replacement);
    const int from = start + (remove ? 0 : before.size());
    selectSource(from, qMax(from, start + replacement.size() - (remove ? 0 : after.size())));
    m_editor->setFocus();
}

void MarkdownEditor::clearFormatting() {
    QTextCursor cursor = m_editor->textCursor();
    if (!cursor.hasSelection()) { cursor.select(QTextCursor::WordUnderCursor); m_editor->setTextCursor(cursor); }
    if (!cursor.hasSelection()) return;
    const auto selection = sourceSelection();
    int start = qMin(selection.first, selection.second), end = qMax(selection.first, selection.second);
    const QString document = content();
    QVector<QPair<QString, QString>> wrappers = {
        {QStringLiteral("**"), QStringLiteral("**")}, {QStringLiteral("__"), QStringLiteral("__")},
        {QStringLiteral("~~"), QStringLiteral("~~")}, {QStringLiteral("*"), QStringLiteral("*")},
        {QStringLiteral("_"), QStringLiteral("_")}, {QStringLiteral("`"), QStringLiteral("`")},
        {QStringLiteral("<u>"), QStringLiteral("</u>")}};
    for (const QString& color : Markdown::highlightColors()) wrappers.append(qMakePair(QStringLiteral("<mark data-color=\"%1\">").arg(color), QStringLiteral("</mark>")));
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
    QStringList lines = document.mid(start, end - start).split(QLatin1Char('\n'), QString::KeepEmptyParts);
    for (QString& line : lines) line = plainLine(line, CodeLine::Normal, &m_targets);
    const QString replacement = lines.join(QLatin1Char('\n'));
    replaceSource(start, end - start, replacement);
    selectSource(start, start + replacement.size());
    m_editor->setFocus();
}
int MarkdownEditor::sourceOffsetForPosition(int position, bool selectionEnd) const {
    SavedPosition saved = savePosition(m_editor->document(), position, selectionEnd);
    const QTextCursor current = m_editor->textCursor();
    if (!current.hasSelection() && position == current.position())
        saved.offset = MarkdownDocument::insertionOffset(current.block(), current.positionInBlock(), m_editor->currentCharFormat());
    int result = saved.offset;
    for (int line = 0; line < saved.block && line < m_sourceLines.size(); ++line)
        result += m_sourceLines.at(line).size() + 1;
    return qBound(0, result, content().size());
}

int MarkdownEditor::documentPositionForSource(int offset) const {
    offset = qBound(0, offset, content().size());
    SavedPosition saved;
    while (saved.block + 1 < m_sourceLines.size() && offset > m_sourceLines.at(saved.block).size()) {
        offset -= m_sourceLines.at(saved.block).size() + 1;
        ++saved.block;
    }
    saved.offset = offset;
    return restorePosition(m_editor->document(), saved);
}

QPair<int, int> MarkdownEditor::sourceSelection() const {
    const QTextCursor cursor = m_editor->textCursor();
    return qMakePair(sourceOffsetForPosition(cursor.anchor(), cursor.hasSelection() && cursor.anchor() == cursor.selectionEnd()),
                     sourceOffsetForPosition(cursor.position(), cursor.hasSelection() && cursor.position() == cursor.selectionEnd()));
}

QString MarkdownEditor::selectedSource() const {
    const auto selection = sourceSelection();
    return content().mid(qMin(selection.first, selection.second), qAbs(selection.second - selection.first));
}

void MarkdownEditor::selectSource(int anchor, int position) {
    QTextCursor cursor(m_editor->document());
    cursor.setPosition(documentPositionForSource(anchor));
    cursor.setPosition(documentPositionForSource(position), QTextCursor::KeepAnchor);
    m_editor->setTextCursor(cursor);
    if (anchor == position) {
        int local = qBound(0, position, content().size());
        for (int line = 0; line < cursor.blockNumber(); ++line) local -= m_sourceLines.at(line).size() + 1;
        m_editor->setCurrentCharFormat(MarkdownDocument::insertionFormat(cursor.block(), local));
    }
}

void MarkdownEditor::replaceSource(int start, int length, const QString& replacement, bool preserveSelection) {
    const QString before = content();
    if (!isEnabled() || m_editor->isReadOnly() || start < 0 || length < 0 || start + length > before.size()) return;
    const QString next = before.left(start) + replacement + before.mid(start + length);
    if (next == before) return;
    static_cast<EditorTextEdit*>(m_editor)->breakTyping();
    {
        QScopedValueRollback<bool> editing(m_editingSource, true);
        QTextCursor group(m_editor->document()); group.beginEditBlock();
        m_sourceLines = next.split(QLatin1Char('\n'), QString::KeepEmptyParts);
        updateLineStates();
        if (!preserveSelection) selectSource(start + replacement.size(), start + replacement.size());
        group.endEditBlock();
    }
    sourceEditFinished();
    if (!preserveSelection) m_editor->setFocus();
}

bool MarkdownEditor::completionAllowedAt(int offset) const {
    if (markdownBlockCodeAt(content(), offset)) return false;
    int line = 0;
    while (line + 1 < m_sourceLines.size() && offset > m_sourceLines.at(line).size()) {
        offset -= m_sourceLines.at(line).size() + 1; ++line;
    }
    if (line >= m_sourceLines.size() || codeLine(m_sourceLines, line) != CodeLine::Normal) return false;
    const QString prefix = m_sourceLines.at(line).left(offset);
    int delimiter = 0;
    for (int i = 0; i < prefix.size(); ++i) {
        if (prefix.at(i) != QLatin1Char('`') || isEscaped(prefix, i)) continue;
        int count = 1;
        while (i + count < prefix.size() && prefix.at(i + count) == QLatin1Char('`')) ++count;
        if (delimiter == 0) delimiter = count;
        else if (delimiter == count) delimiter = 0;
        i += count - 1;
    }
    return delimiter == 0;
}
} // namespace kl
