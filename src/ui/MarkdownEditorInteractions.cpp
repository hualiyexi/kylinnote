#include "MarkdownEditor.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QMenu>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QScopedValueRollback>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTextEdit>
#include <QTimer>
#include <algorithm>

#include "../app/TaskStore.h"
#include "../domain/InternalLink.h"
#include "EditorPopups.h"
#include "EditorTextEdit.h"
#include "EntityPicker.h"
#include "MarkdownDocument.h"
#include "PopupMenu.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {
struct BlockSpec {
    MarkdownEditor::BlockStyle style;
    const char* label;
    const char* prefix;
    IconId icon;
};
const BlockSpec kBlocks[] = {
    {MarkdownEditor::BlockStyle::Paragraph, "正文", "", IconId::Paragraph},
    {MarkdownEditor::BlockStyle::Heading1, "一级标题", "# ", IconId::Heading1},
    {MarkdownEditor::BlockStyle::Heading2, "二级标题", "## ", IconId::Heading2},
    {MarkdownEditor::BlockStyle::Heading3, "三级标题", "### ", IconId::Heading3},
    {MarkdownEditor::BlockStyle::UnorderedList, "无序列表", "- ", IconId::List},
    {MarkdownEditor::BlockStyle::OrderedList, "有序列表", "1. ", IconId::OrderedList},
    {MarkdownEditor::BlockStyle::TaskList, "检查项", "- [ ] ", IconId::Checklist},
    {MarkdownEditor::BlockStyle::Quote, "引用", "> ", IconId::Quote},
    {MarkdownEditor::BlockStyle::HorizontalRule, "水平分割线", "---", IconId::HorizontalRule},
};
const BlockSpec& blockSpec(MarkdownEditor::BlockStyle style) {
    for (const BlockSpec& spec : kBlocks) if (spec.style == style) return spec;
    return kBlocks[0];
}
bool escaped(const QString& text, int position) {
    int slashes = 0;
    while (position > 0 && text.at(--position) == QLatin1Char('\\')) ++slashes;
    return slashes % 2 != 0;
}
QString literalText(const QString& text) {
    QString result;
    const QString punctuation = QStringLiteral("\\`*_{}[]<>#~|.!+-");
    for (QChar character : text) {
        if (punctuation.contains(character)) result += QLatin1Char('\\');
        result += character;
    }
    return result;
}
}

QVector<MarkdownEditor::BlockStyle> MarkdownEditor::blockStyles() const {
    QVector<BlockStyle> styles;
    for (const BlockSpec& spec : kBlocks) styles.append(spec.style);
    return styles;
}
QString MarkdownEditor::blockStyleLabel(BlockStyle style) const { return QString::fromUtf8(blockSpec(style).label); }
IconId MarkdownEditor::blockStyleIcon(BlockStyle style) const { return blockSpec(style).icon; }

void MarkdownEditor::applyBlockStyle(BlockStyle style) {
    if (!isEnabled() || m_editor->isReadOnly()) return;
    closeHints();
    const QTextCursor cursor = m_editor->textCursor();
    const int originalAnchor = cursor.anchor(), originalPosition = cursor.position();
    const int first = m_editor->document()->findBlock(cursor.selectionStart()).blockNumber();
    int last = m_editor->document()->findBlock(cursor.selectionEnd()).blockNumber();
    if (cursor.hasSelection() && cursor.selectionEnd() == m_editor->document()->findBlock(cursor.selectionEnd()).position())
        last = qMax(first, last - 1);
    static const QRegularExpression prefix(QStringLiteral(
        "^([ \\t]*)(?:#{1,6}[ \\t]+|[-+*][ \\t]+(?:\\[[ xX]\\][ \\t]+)?|[0-9]+[.)][ \\t]+|>[ \\t]?)"));
    int start = 0;
    for (int line = 0; line < first; ++line) start += m_sourceLines.at(line).size() + 1;
    if (style == BlockStyle::HorizontalRule) {
        const int end = start + m_sourceLines.at(first).size();
        replaceSource(end, 0, (m_sourceLines.at(first).isEmpty() ? QString() : QStringLiteral("\n")) + QStringLiteral("---\n"));
        return;
    }
    QStringList lines;
    int length = qMax(0, last - first);
    for (int line = first; line <= last; ++line) {
        QString text = m_sourceLines.at(line);
        length += text.size();
        const auto match = prefix.match(text);
        static const QRegularExpression indentationPattern(QStringLiteral("^[ \\t]*"));
        const QString indentation = match.hasMatch() ? match.captured(1) : indentationPattern.match(text).captured(0);
        text.remove(0, match.hasMatch() ? match.capturedLength() : indentation.size());
        QString marker = QString::fromLatin1(blockSpec(style).prefix);
        if (style == BlockStyle::TaskList && match.captured(0).contains(QRegularExpression(QStringLiteral("\\[[xX]\\]"))))
            marker = QStringLiteral("- [x] ");
        lines.append(indentation + marker + text);
    }
    replaceSource(start, length, lines.join(QLatin1Char('\n')));
    QTextCursor restored(m_editor->document());
    restored.setPosition(qMin(originalAnchor, m_editor->document()->characterCount() - 1));
    restored.setPosition(qMin(originalPosition, m_editor->document()->characterCount() - 1), QTextCursor::KeepAnchor);
    m_editor->setTextCursor(restored);
    m_editor->setFocus();
}

void MarkdownEditor::showBlockMenu(int block, const QRect& anchor) {
    if (!blockInsertionAllowed(block) || !isEnabled() || m_editor->isReadOnly() || !m_editor->hasFocus()
        || m_editor->textCursor().hasSelection() || m_editor->textCursor().blockNumber() != block) return;
    closePopups();
    if (m_blockMenu) m_blockMenu->deleteLater();
    auto* menu = new PopupMenu(this);
    m_blockMenu = menu;
    menu->setObjectName(QStringLiteral("EditorBlockMenu"));
    menu->setProperty("entityMenu", true);
    menu->setProperty("markdownEditor", QVariant::fromValue<QObject*>(this));
    menu->setFont(ds::qfont(ds::FontScene::Menu));
    menu->setMinimumWidth(188 + 2 * PopupMenu::shadowMargin());
    const QString snapshot = content();
    const quint64 generation = m_contextGeneration;
    int lineStart = 0;
    for (int i = 0; i < block; ++i) lineStart += m_sourceLines.at(i).size() + 1;
    const int lineEnd = lineStart + m_sourceLines.at(block).size();
    const auto current = sourceSelection();
    const int insertion = current.first == current.second && current.second >= lineStart && current.second <= lineEnd
        ? current.second : lineEnd;
    const auto valid = [this, snapshot, generation]() {
        return isEnabled() && !m_editor->isReadOnly() && generation == m_contextGeneration && content() == snapshot;
    };
    for (BlockStyle style : blockStyles()) {
        auto* action = menu->addAction(AppIcons::icon(blockStyleIcon(style), 18), blockStyleLabel(style));
        action->setObjectName(QStringLiteral("InsertBlock_%1").arg(static_cast<int>(style)));
        connect(action, &QAction::triggered, this, [this, valid, lineStart, lineEnd, style]() {
            if (!valid()) return;
            selectSource(lineStart, lineEnd);
            applyBlockStyle(style);
        });
    }
    menu->addSeparator();
    const auto add = [this, menu](IconId icon, const QString& label, const QString& name) {
        QAction* action = menu->addAction(AppIcons::icon(icon, 18), label);
        action->setObjectName(name);
        return action;
    };
    connect(add(IconId::Attachment, QStringLiteral("附件"), QStringLiteral("InsertAttachment")), &QAction::triggered,
            this, [this, valid]() { if (valid()) emit attachmentRequested(); });
    connect(add(IconId::Tag, QStringLiteral("标签"), QStringLiteral("InsertTag")), &QAction::triggered,
            this, [this, valid, insertion]() { if (valid()) { selectSource(insertion, insertion); beginTag(); } });
    connect(add(IconId::RelatedEntity, QStringLiteral("关联任务/笔记"), QStringLiteral("InsertRelatedEntity")), &QAction::triggered,
            this, [this, valid, insertion]() { if (valid()) { selectSource(insertion, insertion); beginReference(); } });
    const QRect bounds = editorPopupBounds();
    if (bounds.isEmpty()) return;
    menu->setMaximumHeight(bounds.height() + 2 * PopupMenu::shadowMargin());
    const QSize card = menu->sizeHint() - QSize(2 * PopupMenu::shadowMargin(), 2 * PopupMenu::shadowMargin());
    int y = anchor.bottom() + 5;
    if (y + card.height() > bounds.bottom() + 1) y = qMax(bounds.top(), anchor.top() - card.height() - 4);
    const int x = qBound(bounds.left(), anchor.left(), qMax(bounds.left(), bounds.right() - card.width() + 1));
    menu->popup(QPoint(x, y));
}

void MarkdownEditor::beginReference() {
    if (!m_store || !isEnabled() || m_editor->isReadOnly()) return;
    const auto selection = sourceSelection();
    const int start = qMin(selection.first, selection.second);
    const QString prefix = start > 0 && !content().at(start - 1).isSpace() ? QStringLiteral(" [[") : QStringLiteral("[[");
    replaceSource(start, qAbs(selection.second - selection.first), prefix);
    scheduleHints();
}
void MarkdownEditor::beginTag() {
    if (!m_store || !isEnabled() || m_editor->isReadOnly()) return;
    const auto selection = sourceSelection();
    const int start = qMin(selection.first, selection.second);
    const QString prefix = start > 0 && !content().at(start - 1).isSpace() ? QStringLiteral(" #") : QStringLiteral("#");
    replaceSource(start, qAbs(selection.second - selection.first), prefix);
    scheduleHints();
}

void MarkdownEditor::scheduleHints() {
    if (m_hintUpdatePending || m_acceptingHint || m_loading || m_rendering || m_editingSource) return;
    m_hintUpdatePending = true;
    QTimer::singleShot(0, this, [this]() { m_hintUpdatePending = false; updateHints(); });
}
void MarkdownEditor::closeHints(bool dismissed) {
    if (dismissed) { m_dismissedHintStart = m_hintStart; m_dismissedHint = m_hintText; }
    m_hintMode = 0; m_hintStart = m_hintEnd = -1; m_hintText.clear();
    if (m_completion) m_completion->hide();
}
void MarkdownEditor::updateHints() {
    if (m_loading || m_rendering || m_acceptingHint || m_preeditActive || !m_store || !isEnabled()
        || m_editor->isReadOnly() || !m_editor->isVisible() || m_stack->currentWidget() != m_editor
        || (!m_editor->hasFocus() && !ownsPopup(QApplication::activePopupWidget()))) { closeHints(); return; }
    QWidget* active = QApplication::activePopupWidget();
    if (active && active != m_completion->popup()) { closeHints(); return; }
    const auto selection = sourceSelection();
    if (selection.first != selection.second) { closeHints(); return; }
    const int caret = selection.second;
    const QString body = content();
    const int lineStart = caret > 0 ? body.lastIndexOf(QLatin1Char('\n'), caret - 1) + 1 : 0;
    const QString line = body.mid(lineStart, caret - lineStart);
    for (const MarkdownLink& link : markdownLinks(body))
        if (caret > link.start && caret < link.start + link.length) { closeHints(); return; }
    int mode = 0, local = line.lastIndexOf(QStringLiteral("[["));
    QString query;
    if (local >= 0 && !escaped(line, local)) {
        query = line.mid(local + 2);
        if (!query.contains(QStringLiteral("]]")) && !query.contains(QLatin1Char('|'))) mode = 1;
    }
    if (!mode) {
        local = line.lastIndexOf(QLatin1Char('#'));
        if (local >= 0 && !escaped(line, local) && (local == 0 || line.at(local - 1).isSpace())) {
            query = line.mid(local + 1);
            if (!query.contains(QRegularExpression(QStringLiteral("\\s")))) mode = 2;
        }
    }
    const int start = lineStart + local;
    if (!mode || !completionAllowedAt(start)) { closeHints(); return; }
    const QString token = body.mid(start, caret - start);
    if (m_dismissedHintStart == start && m_dismissedHint == token) { closeHints(); return; }
    m_hintMode = mode; m_hintStart = start; m_hintEnd = caret; m_hintText = token;
    QVector<EditorHint> hints;
    if (mode == 1) {
        for (const Entity& entity : matchingEntities(m_store, query))
            hints.append({entity.id, entity.title, entity.title, false});
        if (hints.isEmpty()) hints.append({QString(), QStringLiteral("没有匹配的任务或笔记"), QString(), false});
    } else {
        QStringList tags;
        QSet<QString> seen;
        bool exact = false;
        for (const Entity& entity : m_store->entities()) {
            if (entity.deleted) continue;
            for (const QString& tag : entity.tags) {
                const QString key = tag.trimmed().toCaseFolded();
                if (tag.compare(query.trimmed(), Qt::CaseInsensitive) == 0) exact = true;
                if (!seen.contains(key) && tag.contains(query.trimmed(), Qt::CaseInsensitive)) {
                    seen.insert(key); tags.append(tag);
                }
            }
        }
        std::sort(tags.begin(), tags.end(), [](const QString& a, const QString& b) { return QString::localeAwareCompare(a, b) < 0; });
        for (const QString& tag : tags) hints.append({tag, tag, tag, false});
        if (!exact && !query.trimmed().isEmpty())
            hints.append({query.trimmed(), QStringLiteral("创建“%1”").arg(query.trimmed()), QString(), true});
        if (hints.isEmpty()) hints.append({QString(), QStringLiteral("输入标签名称"), QString(), false});
    }
    const QRect caretRect = sourceAnchorRect(caret);
    QRect visible(m_editor->viewport()->mapToGlobal(QPoint()), m_editor->viewport()->size());
    for (QWidget* ancestor = m_editor->viewport()->parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
        visible = visible.intersected(QRect(ancestor->mapToGlobal(QPoint()), ancestor->size()));
        if (ancestor->isWindow()) break;
    }
    if (!visible.intersects(caretRect)) { closeHints(); return; }
    m_completion->showItems(hints, caretRect, editorPopupBounds(),
                            mode == 1 ? QStringLiteral("关联任务/笔记候选") : QStringLiteral("标签候选"));
}
void MarkdownEditor::acceptHint(const QString& value, bool) {
    if (!m_store || !isEnabled() || m_editor->isReadOnly() || m_hintMode == 0 || m_hintStart < 0
        || content().mid(m_hintStart, m_hintEnd - m_hintStart) != m_hintText) { closeHints(); return; }
    const int mode = m_hintMode, start = m_hintStart, length = m_hintEnd - m_hintStart;
    QString replacement;
    if (mode == 1) {
        const Entity target = m_store->entity(value);
        if (target.id.isEmpty() || target.deleted) { closeHints(); return; }
        replacement = entityReference(target.id, target.title);
    } else {
        if (value.trimmed().isEmpty()) { closeHints(); return; }
        replacement = QLatin1Char('#') + value.trimmed() + QLatin1Char(' ');
    }
    m_acceptingHint = true;
    closeHints();
    replaceSource(start, length, replacement);
    m_acceptingHint = false;
    if (mode == 2) emit tagSelected(value.trimmed());
}

bool MarkdownEditor::insertEntityReference(const QString& entityId) {
    if (!m_store || !isEnabled() || m_editor->isReadOnly()) return false;
    const Entity target = m_store->entity(entityId);
    if (target.id.isEmpty() || target.deleted) return false;
    setPreview(false);
    const auto selection = sourceSelection();
    replaceSource(qMin(selection.first, selection.second), qAbs(selection.second - selection.first),
                  entityReference(target.id, target.title));
    return true;
}

bool MarkdownEditor::ownsPopup(const QWidget* popup) const {
    for (const QWidget* current = popup; current; current = current->parentWidget()) {
        if (current == this || current->property("markdownEditor").value<QObject*>() == this) return true;
    }
    return false;
}
void MarkdownEditor::closePopups() {
    closeHints();
    m_dismissedHintStart = -1; m_dismissedHint.clear();
    if (m_blockMenu) m_blockMenu->hide();
    if (m_linkTools) m_linkTools->hide();
    if (m_linkEditor) m_linkEditor->hide();
    for (QWidget* popup : QApplication::topLevelWidgets())
        if (popup != window() && ownsPopup(popup)) popup->hide();
    for (QWidget* popup : findChildren<QWidget*>())
        if (popup->property("markdownEditor").value<QObject*>() == this) popup->hide();
    m_hoverLink = LinkContext(); m_editedLink = LinkContext(); m_pressedLink = LinkContext();
    m_pressedCheckLine = -1;
}
QRect MarkdownEditor::editorPopupBounds() const {
    QWidget* pane = window();
    for (QWidget* current = parentWidget(); current; current = current->parentWidget()) {
        if (current->objectName() == QLatin1String("TaskDetailHost")) { pane = current; break; }
    }
    if (!pane || !pane->isVisible()) return QRect();
    QRect bounds(pane->mapToGlobal(QPoint()), pane->size());
    for (QWidget* current = pane->parentWidget(); current; current = current->parentWidget()) {
        bounds = bounds.intersected(QRect(current->mapToGlobal(QPoint()), current->size()));
        if (current->isWindow()) break;
    }
    if (QWidget* footer = pane->findChild<QWidget*>(QStringLiteral("DetailFooter")))
        if (footer->isVisible()) bounds.setBottom(footer->mapToGlobal(QPoint()).y() - 6);
    QScreen* screen = QGuiApplication::screenAt(bounds.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) bounds = bounds.intersected(screen->availableGeometry());
    return bounds.adjusted(12, 8, -12, -8);
}
QRect MarkdownEditor::sourceAnchorRect(int offset) const {
    QTextCursor cursor(m_editor->document());
    cursor.setPosition(documentPositionForSource(offset));
    const QRect local = m_editor->cursorRect(cursor);
    return QRect(m_editor->viewport()->mapToGlobal(local.topLeft()), local.size());
}

MarkdownEditor::LinkContext MarkdownEditor::linkAt(const QPoint& position) const {
    LinkContext result;
    const QString href = m_editor->anchorAt(position);
    if (href.isEmpty()) return result;
    QTextCursor hit = m_editor->cursorForPosition(position);
    const QTextBlock block = hit.block();
    const QRect caret = m_editor->cursorRect(hit);
    int character = hit.positionInBlock();
    const bool previousSide = block.textDirection() == Qt::RightToLeft ? position.x() > caret.right() : position.x() < caret.left();
    if (character > 0 && (character == block.length() - 1 || position.y() < caret.top() || previousSide)) --character;
    if (character >= block.length() - 1) return result;
    hit.setPosition(block.position() + character + 1);
    const QTextCharFormat format = hit.charFormat();
    result.source = format.stringProperty(MarkdownDocument::LinkSourceProperty);
    if (result.source.isEmpty() || format.anchorHref() != href) return LinkContext();
    result.start = format.intProperty(MarkdownDocument::LinkOffsetProperty);
    for (int line = 0; line < block.blockNumber(); ++line) result.start += m_sourceLines.at(line).size() + 1;
    result.length = result.source.size();
    result.target = href;
    const auto links = markdownLinks(result.source);
    if (!links.isEmpty()) result.label = links.first().label;
    if (result.label.isEmpty()) result.label = m_targets.value(href).title;
    result.generation = m_contextGeneration;
    return linkContextValid(result) ? result : LinkContext();
}
bool MarkdownEditor::linkContextValid(const LinkContext& context) const {
    return context.start >= 0 && context.generation == m_contextGeneration
        && context.start + context.length <= content().size()
        && content().mid(context.start, context.length) == context.source;
}
void MarkdownEditor::updateLinkHover(const QPoint& position) {
    if (m_editor->textCursor().hasSelection() || (m_linkEditor && m_linkEditor->isVisible())) return;
    const QTextBlock block = m_editor->cursorForPosition(position).block();
    if (static_cast<EditorTextEdit*>(m_editor)->checkBoxRect(block).contains(position)) {
        m_editor->viewport()->setCursor(isEnabled() && !m_editor->isReadOnly() ? Qt::PointingHandCursor : Qt::IBeamCursor);
        if (m_linkTools) m_linkTools->scheduleHide();
        return;
    }
    const LinkContext link = linkAt(position);
    if (link.start < 0) {
        m_editor->viewport()->setCursor(Qt::IBeamCursor);
        if (m_linkTools) m_linkTools->scheduleHide();
        return;
    }
    m_editor->viewport()->setCursor(Qt::PointingHandCursor);
    if (!m_linkTools) {
        m_linkTools = new LinkTools(window());
        m_linkTools->setProperty("markdownEditor", QVariant::fromValue<QObject*>(this));
        connect(m_linkTools, &LinkTools::editRequested, this, [this]() {
            if (linkContextValid(m_hoverLink)) openLinkEditor(m_hoverLink);
        });
        connect(m_linkTools, &LinkTools::copyRequested, this, [this]() {
            if (!linkContextValid(m_hoverLink)) return;
            const QString id = internalLinkId(m_hoverLink.target);
            QApplication::clipboard()->setText(id.isEmpty() ? m_hoverLink.target : entityReference(id, m_hoverLink.label));
            m_linkTools->hide();
        });
    }
    if (m_linkTools->parentWidget() != window()) m_linkTools->setParent(window());
    const bool same = m_hoverLink.start == link.start && m_linkTools->isVisible();
    m_hoverLink = link;
    if (!same) m_linkTools->showAt(sourceAnchorRect(link.start), editorPopupBounds());
}
void MarkdownEditor::insertLink() {
    const auto selection = sourceSelection();
    const int start = qMin(selection.first, selection.second);
    for (const MarkdownLink& link : markdownLinks(content())) {
        if (start < link.start || start > link.start + link.length) continue;
        LinkContext context;
        context.start = link.start; context.length = link.length; context.target = link.target;
        context.label = link.label.isEmpty() ? m_targets.value(link.target).title : link.label;
        context.source = content().mid(context.start, context.length); context.generation = m_contextGeneration;
        openLinkEditor(context); return;
    }
    LinkContext context;
    context.start = start; context.length = qAbs(selection.second - selection.first);
    context.source = content().mid(context.start, context.length);
    context.label = m_editor->textCursor().selectedText();
    context.target = QStringLiteral("https://"); context.generation = m_contextGeneration;
    openLinkEditor(context);
}
void MarkdownEditor::openLinkEditor(const LinkContext& link) {
    if (!isEnabled() || m_editor->isReadOnly() || !linkContextValid(link)) return;
    closeHints();
    if (m_linkTools) m_linkTools->hide();
    m_editedLink = link;
    if (!m_linkEditor) {
        m_linkEditor = new LinkEditorPopup(this);
        m_linkEditor->setProperty("markdownEditor", QVariant::fromValue<QObject*>(this));
        connect(m_linkEditor, &LinkEditorPopup::submitted, this, &MarkdownEditor::applyLinkEdit);
        connect(m_linkEditor, &QDialog::finished, this, [this]() {
            if (linkContextValid(m_editedLink) && isVisible() && isEnabled()) m_editor->setFocus();
        });
    }
    const bool existing = !markdownLinks(link.source).isEmpty();
    m_linkEditor->openFor(link.label, link.target, existing, sourceAnchorRect(link.start), editorPopupBounds());
}
void MarkdownEditor::applyLinkEdit(const QString& label, const QString& target, bool remove) {
    if (!isEnabled() || m_editor->isReadOnly() || !linkContextValid(m_editedLink)) return;
    QString replacement;
    if (remove) replacement = literalText(label);
    else {
        if (!isSafeLinkTarget(target)) return;
        const QString id = internalLinkId(target);
        if (!id.isEmpty() && (!m_store || m_store->entity(id).id.isEmpty() || m_store->entity(id).deleted)) return;
        const QString display = label.isEmpty() ? (id.isEmpty() ? target : m_store->entity(id).title) : label;
        replacement = markdownLink(target, display);
        if (replacement.isEmpty()) return;
    }
    replaceSource(m_editedLink.start, m_editedLink.length, replacement);
}

} // namespace kl
