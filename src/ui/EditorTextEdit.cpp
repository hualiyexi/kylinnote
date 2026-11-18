#include "EditorTextEdit.h"

#include <QAbstractTextDocumentLayout>
#include <QEvent>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QToolButton>
#include <memory>

#include "AppIcons.h"
#include "MarkdownDocument.h"
#include "PopupMenu.h"
#include "TextGeometry.h"
#include "design/ThemeManager.h"

namespace kl {
namespace {
constexpr int kGutterWidth = 24;
const char kMarkdownMime[] = "application/x-kylinlist-markdown";

QTextCharFormat inputFormat(QTextEdit* edit) {
    const QTextCursor cursor = edit->textCursor();
    QTextCharFormat format = edit->currentCharFormat();
    if (cursor.hasSelection()) {
        QTextCursor first(edit->document());
        first.setPosition(cursor.selectionStart() + 1);
        format = first.charFormat();
    }
    format.clearProperty(MarkdownDocument::EscapedProperty);
    if (!cursor.hasSelection() && !format.stringProperty(MarkdownDocument::LinkSourceProperty).isEmpty()) {
        QTextCursor next = cursor;
        const bool within = cursor.positionInBlock() < cursor.block().length() - 1
            && next.movePosition(QTextCursor::NextCharacter)
            && next.charFormat().property(MarkdownDocument::LinkTokenProperty) == format.property(MarkdownDocument::LinkTokenProperty)
            && next.charFormat().stringProperty(MarkdownDocument::LinkSourceProperty) == format.stringProperty(MarkdownDocument::LinkSourceProperty);
        if (!within) {
            format.setAnchor(false); format.setAnchorHref(QString());
            format.clearProperty(MarkdownDocument::LinkSourceProperty);
            format.clearProperty(MarkdownDocument::LinkLabelProperty);
            format.clearProperty(MarkdownDocument::LinkOuterStylesProperty);
            format.clearProperty(MarkdownDocument::LinkTokenProperty);
            format.clearProperty(MarkdownDocument::LinkOffsetProperty);
            format.setProperty(MarkdownDocument::InlineStylesProperty, cursor.block().charFormat().property(MarkdownDocument::InlineStylesProperty));
            format.setFontUnderline(false);
            format.setForeground(ThemeManager::instance().color(ds::ColorToken::Text));
        }
    }
    return format;
}

// Qt's lazy selection MIME overrides formats()/retrieveData(); setData() on that
// object does not advertise an added source format in Qt 5.12. Delegate its rich
// formats lazily and expose the source alongside them, rather than replacing it.
class SourceMimeData : public QMimeData {
public:
    SourceMimeData(QMimeData* native, const QString& source) : m_native(native), m_source(source.toUtf8()) {}
    QStringList formats() const override {
        QStringList values = m_native->formats();
        values.append(QLatin1String(kMarkdownMime));
        return values;
    }
    bool hasFormat(const QString& mime) const override {
        return mime == QLatin1String(kMarkdownMime) || m_native->hasFormat(mime);
    }
protected:
    QVariant retrieveData(const QString& mime, QVariant::Type) const override {
        return mime == QLatin1String(kMarkdownMime) ? QVariant(m_source) : QVariant(m_native->data(mime));
    }
private:
    std::unique_ptr<QMimeData> m_native;
    QByteArray m_source;
};
}

EditorTextEdit::EditorTextEdit(QWidget* parent) : QTextEdit(parent) {
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    m_gutter = new QWidget(this);
    m_gutter->setObjectName(QStringLiteral("EditorBlockGutter"));
    m_indicator = new QToolButton(m_gutter);
    m_indicator->setObjectName(QStringLiteral("EditorBlockInsert"));
    m_indicator->setAccessibleName(QStringLiteral("插入或转换此段落"));
    m_indicator->setToolTip(m_indicator->accessibleName());
    m_indicator->setAutoRaise(true);
    m_indicator->setFixedSize(22, 22);
    m_indicator->setIconSize(QSize(14, 14));
    m_indicator->setFocusPolicy(Qt::NoFocus);
    m_indicator->setCursor(Qt::PointingHandCursor);
    m_indicator->setStyleSheet(QStringLiteral(
        "QToolButton#EditorBlockInsert,QToolButton#EditorBlockInsert:hover,"
        "QToolButton#EditorBlockInsert:pressed,QToolButton#EditorBlockInsert:focus"
        "{border:0;padding:0;background:transparent;}"));
    const auto theme = [this]() {
        m_indicator->setIcon(AppIcons::icon(IconId::BlockInsert, ds::ColorToken::TextTertiary, 14));
        refreshBlockIndicator();
    };
    theme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [theme](ds::Theme) { theme(); });
    connect(m_indicator, &QToolButton::clicked, this, [this]() {
        if (!canInsertBlock()) return;
        emit blockMenuRequested(textCursor().blockNumber(), QRect(m_indicator->mapToGlobal(QPoint()), m_indicator->size()));
    });
    connect(this, &QTextEdit::cursorPositionChanged, this, &EditorTextEdit::refreshBlockIndicator);
    connect(this, &QTextEdit::selectionChanged, this, &EditorTextEdit::refreshBlockIndicator);
    connect(this, &QTextEdit::textChanged, this, &EditorTextEdit::refreshBlockIndicator);
    const auto breakTyping = [this]() {
        if (!m_keyEditing && (!presentationChanging || !presentationChanging())) m_typingEnd = -1;
    };
    connect(this, &QTextEdit::cursorPositionChanged, this, breakTyping);
    connect(this, &QTextEdit::selectionChanged, this, breakTyping);
    connect(document(), &QTextDocument::contentsChanged, this, [this, breakTyping]() {
        if (m_keyEditing) m_keyChanged = true;
        else breakTyping();
    });
    connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            this, [this](const QSizeF&) { refreshBlockIndicator(); });
    m_gutter->hide();
    selectedSource = []() { return QString(); };
}

EditorTextEdit::~EditorTextEdit() {
    disconnect(this, nullptr, this, nullptr);
    disconnect(document(), nullptr, this, nullptr);
    disconnect(document()->documentLayout(), nullptr, this, nullptr);
    disconnect(&ThemeManager::instance(), nullptr, this, nullptr);
}

void EditorTextEdit::setBlockGutterEnabled(bool enabled) {
    if (m_gutterEnabled == enabled) return;
    m_gutterEnabled = enabled;
    setViewportMargins(enabled ? kGutterWidth : 0, 0, 0, 0);
    m_gutter->setVisible(enabled);
    refreshBlockIndicator();
}

bool EditorTextEdit::canInsertBlock() const {
    const QTextCursor cursor = textCursor();
    const QTextBlock block = cursor.block();
    return m_gutterEnabled && isVisible() && hasFocus() && isEnabled() && !isReadOnly()
        && !m_composing && !cursor.hasSelection() && block.isValid() && block.text().isEmpty()
        && (!presentationChanging || !presentationChanging())
        && (!blockInsertionAllowed || blockInsertionAllowed(block.blockNumber()));
}

void EditorTextEdit::refreshBlockIndicator() {
    if (!m_gutterEnabled) return;
    m_gutter->setGeometry(frameWidth(), viewport()->y(), kGutterWidth, viewport()->height());
    if (!canInsertBlock()) {
        m_indicator->hide();
        return;
    }
    const QTextBlock block = textCursor().block();
    const QRect caret = cursorRect(QTextCursor(block));
    const QFontMetricsF metrics(block.charFormat().font().resolve(document()->defaultFont()));
    const QTextLayout* layout = block.layout();
    const QTextLine line = layout && layout->lineCount() ? layout->lineAt(0) : QTextLine();
    const qreal baseline = caret.top() + (line.isValid() ? line.ascent() : metrics.ascent());
    const qreal center = baseline + textContentCenterOffset(metrics);
    m_indicator->move(1, qRound(center - m_indicator->height() / 2.0));
    m_indicator->setVisible(caret.bottom() >= 0 && caret.top() < viewport()->height());
    m_gutter->raise();
}

void EditorTextEdit::focusInEvent(QFocusEvent* event) {
    QTextEdit::focusInEvent(event);
    refreshBlockIndicator();
}

void EditorTextEdit::focusOutEvent(QFocusEvent* event) {
    m_typingEnd = -1;
    QTextEdit::focusOutEvent(event);
    refreshBlockIndicator();
}

void EditorTextEdit::resizeEvent(QResizeEvent* event) {
    QTextEdit::resizeEvent(event);
    refreshBlockIndicator();
}

void EditorTextEdit::scrollContentsBy(int dx, int dy) {
    QTextEdit::scrollContentsBy(dx, dy);
    refreshBlockIndicator();
}

QRectF EditorTextEdit::blockFirstLineRect(const QTextBlock& block) const {
    if (!block.isValid()) return QRectF();
    const QRectF bounds = document()->documentLayout()->blockBoundingRect(block);
    const QTextLayout* layout = block.layout();
    if (!layout || !layout->lineCount()) return QRectF();
    const int xOffset = isRightToLeft()
        ? horizontalScrollBar()->maximum() - horizontalScrollBar()->value() : horizontalScrollBar()->value();
    // A cursor at an empty block also includes the IME preedit cursor offset.
    // Block decorations belong to the first layout line, not that moving caret.
    return layout->lineAt(0).rect().translated(bounds.topLeft() - QPointF(xOffset, verticalScrollBar()->value()));
}

QRect EditorTextEdit::checkBoxRect(const QTextBlock& block) const {
    if (MarkdownDocument::kind(block) != MarkdownDocument::Kind::TaskList) return QRect();
    const QRectF firstLine = blockFirstLineRect(block);
    if (firstLine.height() <= 0) return QRect();
    const QFontMetricsF metrics(block.charFormat().font().resolve(document()->defaultFont()));
    const qreal baseline = firstLine.top() + block.layout()->lineAt(0).ascent();
    return QRect(qRound(firstLine.left()) - 20, qRound(baseline + textContentCenterOffset(metrics) - 7), 14, 14);
}

void EditorTextEdit::paintEvent(QPaintEvent* event) {
    QTextEdit::paintEvent(event);
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    const auto& theme = ThemeManager::instance();
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        const QRect firstLine = blockFirstLineRect(block).toRect();
        if (firstLine.height() <= 0 || firstLine.bottom() < 0 || firstLine.top() >= viewport()->height()) continue;
        const auto kind = MarkdownDocument::kind(block);
        if (kind == MarkdownDocument::Kind::HorizontalRule) {
            painter.setPen(theme.color(ds::ColorToken::Divider));
            painter.drawLine(0, firstLine.center().y(), viewport()->width(), firstLine.center().y());
        } else if (kind == MarkdownDocument::Kind::TaskList) {
            const QRectF box = QRectF(checkBoxRect(block)).adjusted(0.5, 0.5, -0.5, -0.5);
            const bool checked = block.blockFormat().boolProperty(MarkdownDocument::CheckedProperty);
            painter.setPen(QPen(theme.color(checked ? ds::ColorToken::Primary : ds::ColorToken::TextTertiary), 1));
            painter.setBrush(checked ? QBrush(theme.color(ds::ColorToken::Primary)) : Qt::NoBrush);
            painter.drawRoundedRect(box, 2, 2);
            if (checked) {
                QPainterPath check;
                check.moveTo(box.left() + 2.5, box.top() + 6.5);
                check.lineTo(box.left() + 5.2, box.top() + 9.2);
                check.lineTo(box.left() + 10.3, box.top() + 3.8);
                painter.setPen(QPen(theme.color(ds::ColorToken::OnPrimary), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.drawPath(check);
            }
            painter.setBrush(Qt::NoBrush);
        } else if (kind == MarkdownDocument::Kind::UnorderedList || kind == MarkdownDocument::Kind::OrderedList) {
            painter.setFont(block.charFormat().font().resolve(document()->defaultFont()));
            painter.setPen(theme.color(ds::ColorToken::TextSecondary));
            painter.drawText(QRect(0, firstLine.top(), qMax(0, firstLine.left() - 7), firstLine.height()), Qt::AlignRight | Qt::AlignVCenter,
                             block.blockFormat().stringProperty(MarkdownDocument::ListMarkerProperty));
        } else if (kind == MarkdownDocument::Kind::Quote) {
            painter.setPen(QPen(theme.color(ds::ColorToken::Divider), 2));
            const qreal height = document()->documentLayout()->blockBoundingRect(block).height();
            painter.drawLine(QPointF(firstLine.left() - 8, firstLine.top()), QPointF(firstLine.left() - 8, firstLine.top() + height));
        }
    }
}

void EditorTextEdit::changeEvent(QEvent* event) {
    QTextEdit::changeEvent(event);
    refreshBlockIndicator();
}

void EditorTextEdit::keyPressEvent(QKeyEvent* event) {
    if (historyRequested && (event->matches(QKeySequence::Undo) || event->matches(QKeySequence::Redo))) {
        breakTyping();
        if (!isReadOnly()) historyRequested(event->matches(QKeySequence::Redo));
        event->accept();
        return;
    }
    const bool ordinary = !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    const bool insert = ordinary && !event->text().isEmpty() && event->text().at(0).isPrint();
    const int kind = insert ? 1 : event->key() == Qt::Key_Backspace ? 2 : event->key() == Qt::Key_Delete ? 3 : 0;
    const bool modifies = kind || event->matches(QKeySequence::Paste) || event->matches(QKeySequence::Cut)
        || (ordinary && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter
                         || (event->key() == Qt::Key_Tab && !tabChangesFocus())));
    if (isReadOnly() || !modifies) {
        m_typingEnd = -1;
        QTextEdit::keyPressEvent(event);
        return;
    }
    const QPointer<EditorTextEdit> guard(this);
    const QPointer<QTextDocument> doc = document();
    QTextCursor group = textCursor();
    const int position = sourceCursorPosition ? sourceCursorPosition() : group.position();
    const bool continuation = kind && kind == m_typingKind && !group.hasSelection()
        && position == m_typingEnd && m_typingTimer.isValid() && m_typingTimer.elapsed() < 1000
        && (historyRequested || doc->isUndoAvailable());
    // Give real key edits an explicit Qt transaction. Presentation can then join
    // that edit instead of becoming a separate, text-neutral undo operation.
    if (continuation) group.joinPreviousEditBlock();
    else group.beginEditBlock();
    m_keyChanged = false;
    m_keyEditing = true;
    const QTextBlockFormat previous = group.blockFormat();
    const bool paragraph = ordinary && !(event->modifiers() & Qt::ShiftModifier) && !m_composing
        && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter);
    if (insert && group.hasSelection()) {
        QTextCursor replacement = textCursor();
        replacement.insertText(event->text(), inputFormat(this));
        setTextCursor(replacement);
        event->accept();
    } else {
        if (insert && !group.hasSelection()) setCurrentCharFormat(inputFormat(this));
        QTextEdit::keyPressEvent(event);
    }
    if (guard && paragraph && paragraphInserted) paragraphInserted(previous);
    if (guard && finishEdit) finishEdit();
    if (doc) group.endEditBlock();
    if (!guard) return;
    m_keyEditing = false;
    m_typingKind = kind;
    m_typingEnd = doc && m_keyChanged
        ? (sourceCursorPosition ? sourceCursorPosition() : textCursor().position()) : -1;
    m_typingTimer.start();
    if (m_keyChanged && editCompleted) editCompleted(continuation);
}

void EditorTextEdit::inputMethodEvent(QInputMethodEvent* event) {
    m_typingEnd = -1;
    m_composing = !event->preeditString().isEmpty();
    emit compositionChanged(m_composing);
    const bool commits = !event->commitString().isEmpty() || event->replacementLength() > 0;
    QTextCursor group = textCursor();
    const QTextCharFormat committedFormat = inputFormat(this);
    if (commits) group.beginEditBlock();
    if (commits && !group.hasSelection()) setCurrentCharFormat(committedFormat);
    QTextEdit::inputMethodEvent(event);
    if (commits && !event->commitString().isEmpty()) {
        QTextCursor inserted = textCursor();
        const int end = inserted.position();
        inserted.setPosition(qMax(0, end - event->commitString().size()), QTextCursor::KeepAnchor);
        inserted.setCharFormat(committedFormat);
    }
    if (!m_composing && finishEdit) finishEdit();
    if (commits) group.endEditBlock();
    const QPointer<EditorTextEdit> guard(this);
    if ((commits || !m_composing) && editCompleted) editCompleted(false);
    if (guard) refreshBlockIndicator();
}

void EditorTextEdit::contextMenuEvent(QContextMenuEvent* event) {
    if (!historyRequested) {
        QTextEdit::contextMenuEvent(event);
        return;
    }
    PopupMenu menu(this);
    menu.setObjectName(QStringLiteral("SharedTextContextMenu"));
    auto* undo = menu.addAction(QStringLiteral("撤销"), this, [this]() { historyRequested(false); });
    undo->setShortcut(QKeySequence::Undo);
    undo->setEnabled(!isReadOnly() && historyAvailable && historyAvailable(false));
    auto* redo = menu.addAction(QStringLiteral("重做"), this, [this]() { historyRequested(true); });
    redo->setShortcut(QKeySequence::Redo);
    redo->setEnabled(!isReadOnly() && historyAvailable && historyAvailable(true));
    menu.addSeparator();
    menu.addAction(QStringLiteral("剪切"), this, &QTextEdit::cut)
        ->setEnabled(!isReadOnly() && textCursor().hasSelection());
    menu.addAction(QStringLiteral("复制"), this, &QTextEdit::copy)->setEnabled(textCursor().hasSelection());
    menu.addAction(QStringLiteral("粘贴"), this, &QTextEdit::paste)->setEnabled(!isReadOnly() && canPaste());
    menu.addAction(QStringLiteral("删除"), this, [this]() {
        QTextCursor cursor = textCursor();
        cursor.removeSelectedText();
    })->setEnabled(!isReadOnly() && textCursor().hasSelection());
    menu.addSeparator();
    menu.addAction(QStringLiteral("全选"), this, &QTextEdit::selectAll);
    menu.exec(event->globalPos());
}

QMimeData* EditorTextEdit::createMimeDataFromSelection() const {
    QMimeData* mime = QTextEdit::createMimeDataFromSelection();
    if (selectedSource && textCursor().hasSelection()) return new SourceMimeData(mime, selectedSource());
    return mime;
}

void EditorTextEdit::insertFromMimeData(const QMimeData* source) {
    if (isReadOnly() || !source) return;
    QTextCursor group = textCursor();
    group.beginEditBlock();
    if (source->hasFormat(QLatin1String(kMarkdownMime))) {
        const QString markdown = QString::fromUtf8(source->data(QLatin1String(kMarkdownMime)));
        if (insertMarkdown) insertMarkdown(markdown);
        else insertPlainText(markdown);
    } else {
        QTextEdit::insertFromMimeData(source);
    }
    if (finishEdit) finishEdit();
    group.endEditBlock();
    if (editCompleted && !m_keyEditing) editCompleted(false);
}

} // namespace kl
