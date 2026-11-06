#include "NoteEditor.h"

#include "../Common.h"

#include <QFont>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QWheelEvent>

namespace kn {

NoteEditor::NoteEditor(QWidget *parent)
    : QTextEdit(parent)
{
    setObjectName(QStringLiteral("NoteEditor"));
    setAcceptRichText(true);
    setAutoFormatting(QTextEdit::AutoNone);
    setLineWrapMode(QTextEdit::WidgetWidth);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setTabChangesFocus(false);
    setUndoRedoEnabled(true);

    // viewport 透明，让 #NoteEditor 的背景规则真正可见
    viewport()->setStyleSheet(QStringLiteral("background:transparent;"));

    QFont f(QStringLiteral("WenQuanYi Micro Hei"), DefaultFontPt);
    f.setStyleHint(QFont::SansSerif);
    document()->setDefaultFont(f);
}

void NoteEditor::wheelEvent(QWheelEvent *event)
{
    // Ctrl + 滚轮：调整字号（Qt 5.12 无内置，且需屏蔽 QTextEdit 自带的缩放）
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
        const int delta = event->angleDelta().y();
        if (delta != 0) {
            applyFontSizeDelta(delta > 0 ? 1 : -1);
            event->accept();
            return;
        }
    }
    QTextEdit::wheelEvent(event);
}

void NoteEditor::keyPressEvent(QKeyEvent *event)
{
    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool ctrl  = mods.testFlag(Qt::ControlModifier);
    const bool shift = mods.testFlag(Qt::ShiftModifier);

    // Ctrl+Shift+> / < ：字号增减。多数键盘上 > 与 . 同键，两种写法都接受
    if (ctrl && shift && (key == Qt::Key_Greater || key == Qt::Key_Period)) {
        applyFontSizeDelta(1);
        event->accept();
        return;
    }
    if (ctrl && shift && (key == Qt::Key_Less || key == Qt::Key_Comma)) {
        applyFontSizeDelta(-1);
        event->accept();
        return;
    }
    // Ctrl+T：删除线
    if (ctrl && !shift && key == Qt::Key_T) {
        toggleStrikeThrough();
        event->accept();
        return;
    }
    // Ctrl+Shift+L：列表样式循环（无 -> 实心圆 -> 空心圆 -> 方块 -> 无）
    if (ctrl && shift && key == Qt::Key_L) {
        cycleListStyle();
        event->accept();
        return;
    }

    // Ctrl+B/I/U 与 Ctrl+L/E/R 交给 QTextEdit 内置实现
    QTextEdit::keyPressEvent(event);
}

int NoteEditor::currentFontSize() const
{
    const qreal pt = textCursor().charFormat().fontPointSize();
    if (pt > 0) {
        return qRound(pt);
    }
    return qRound(document()->defaultFont().pointSizeF());
}

void NoteEditor::applyFontSizeDelta(int delta)
{
    QTextCursor cursor = textCursor();
    if (!cursor.hasSelection()) {
        // 无选区时作用于整篇，符合便笺的整体缩放预期
        cursor.select(QTextCursor::Document);
    }

    const int start = cursor.selectionStart();
    const int end   = cursor.selectionEnd();
    QTextDocument *doc = document();
    const qreal fallback = doc->defaultFont().pointSizeF() > 0
                               ? doc->defaultFont().pointSizeF()
                               : qreal(DefaultFontPt);

    QTextCursor worker(doc);
    worker.beginEditBlock();

    QTextBlock block = doc->findBlock(start);
    while (block.isValid() && block.position() < end) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || fragment.position() >= end) {
                continue;
            }
            QTextCharFormat src = fragment.charFormat();
            const qreal base = src.fontPointSize() > 0 ? src.fontPointSize() : fallback;
            const qreal next = qBound<qreal>(MinFontSize, base + delta, MaxFontSize);
            if (qFuzzyCompare(next, base)) {
                continue;
            }

            QTextCursor target(doc);
            target.setPosition(fragment.position());
            target.setPosition(qMin(end, fragment.position() + fragment.length()),
                               QTextCursor::KeepAnchor);
            QTextCharFormat fmt;
            fmt.setFontPointSize(next);
            target.mergeCharFormat(fmt);
        }
        block = block.next();
    }

    worker.endEditBlock();
    emit fontSizeChanged(currentFontSize());
}

void NoteEditor::increaseFontSize()
{
    applyFontSizeDelta(1);
}

void NoteEditor::decreaseFontSize()
{
    applyFontSizeDelta(-1);
}

void NoteEditor::toggleStrikeThrough()
{
    QTextCursor cursor = textCursor();
    if (!cursor.hasSelection()) {
        cursor.select(QTextCursor::BlockUnderCursor);
    }
    const bool enabled = cursor.charFormat().fontStrikeOut();

    QTextCharFormat fmt;
    fmt.setFontStrikeOut(!enabled);
    cursor.mergeCharFormat(fmt);
}

void NoteEditor::cycleListStyle()
{
    QTextCursor cursor = textCursor();
    QTextList *list = cursor.currentList();

    cursor.beginEditBlock();
    if (!list) {
        QTextListFormat fmt;
        fmt.setStyle(QTextListFormat::ListDisc);
        fmt.setIndent(1);
        cursor.createList(fmt);
        cursor.endEditBlock();
        return;
    }

    const QTextListFormat::Style current = list->format().style();
    QTextListFormat::Style next = QTextListFormat::ListDisc;
    switch (current) {
    case QTextListFormat::ListDisc:
        next = QTextListFormat::ListCircle;
        break;
    case QTextListFormat::ListCircle:
        next = QTextListFormat::ListSquare;
        break;
    default:
        next = QTextListFormat::ListDisc;
        break;
    }

    if (current == QTextListFormat::ListSquare) {
        removeListAtCursor();
    } else {
        QTextListFormat fmt = list->format();
        fmt.setStyle(next);
        list->setFormat(fmt);
    }
    cursor.endEditBlock();
}

void NoteEditor::removeListAtCursor()
{
    QTextCursor cursor = textCursor();
    QTextDocument *doc = document();
    const int start = cursor.selectionStart();
    const int end   = cursor.selectionEnd();

    QTextCursor worker(doc);
    worker.beginEditBlock();

    QTextBlock block = doc->findBlock(start);
    while (block.isValid() && block.position() < end) {
        QTextList *list = block.textList();
        if (list) {
            list->remove(block);
            QTextCursor fix(block);
            QTextBlockFormat blockFormat = fix.blockFormat();
            blockFormat.setIndent(0);
            fix.setBlockFormat(blockFormat);
        }
        block = block.next();
    }

    worker.endEditBlock();
}

} // namespace kn
