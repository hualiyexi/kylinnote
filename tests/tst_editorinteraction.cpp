#include <cstdio>
#include <functional>
#include <memory>

#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QElapsedTimer>
#include <QEnterEvent>
#include <QFontMetricsF>
#include <QFrame>
#include <QGuiApplication>
#include <QImage>
#include <QInputMethodEvent>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextLayout>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtTest>

#include "app/TaskStore.h"
#include "db/Database.h"
#include "domain/InternalLink.h"
#include "platform/SystemShadow.h"
#include "ui/AppStyle.h"
#include "ui/EditorTextEdit.h"
#include "ui/Markdown.h"
#include "ui/MarkdownEditor.h"
#include "ui/PopupShadow.h"
#include "ui/TaskDetailWidget.h"
#include "ui/design/ThemeManager.h"

using namespace kl;

namespace {
using BlockStyle = MarkdownEditor::BlockStyle;
constexpr int ValueRole = Qt::UserRole + 1;
constexpr int CreateRole = Qt::UserRole + 2;
const char MarkdownMime[] = "application/x-kylinlist-markdown";

bool waitUntil(const std::function<bool()>& predicate, int timeout = 1800) {
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        QApplication::processEvents();
        if (predicate()) return true;
        QTest::qWait(5);
    } while (elapsed.elapsed() < timeout);
    return predicate();
}

bool expose(QWidget& window) {
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) return false;
    const QRect available = screen->availableGeometry();
    window.resize(qMin(window.width(), available.width() - 40),
                  qMin(window.height(), available.height() - 60));
    window.move(available.topLeft() + QPoint(20, 20));
    window.show();
    window.activateWindow();
    return QTest::qWaitForWindowExposed(&window) && QTest::qWaitForWindowActive(&window);
}

// QTest does not perform native activation/hit testing for us. Check the actual
// deepest child first; the offscreen/minimal plugins lack desktop widgetAt only.
bool clickActual(QWidget* target, QPoint point = QPoint(-1, -1)) {
    if (!target || !target->isVisible() || !target->isEnabled()) return false;
    if (point.x() < 0) point = target->rect().center();
    if (!target->rect().contains(point)) return false;
    QWidget* window = target->window();
    if (!window->isActiveWindow() && window->windowType() != Qt::Popup) {
        window->activateWindow();
        if (!QTest::qWaitForWindowActive(window)) return false;
    }
    const QPoint global = target->mapToGlobal(point);
    QWidget* hit = window->childAt(window->mapFromGlobal(global));
    if (!hit && target == window) hit = window;
    if (hit != target && (!hit || !target->isAncestorOf(hit))) return false;
    const QString platform = QGuiApplication::platformName();
    if (platform != QLatin1String("offscreen") && platform != QLatin1String("minimal")) {
        QWidget* nativeHit = QApplication::widgetAt(global);
        if (nativeHit != hit) return false;
    }
    QTest::mouseClick(hit, Qt::LeftButton, Qt::NoModifier, hit->mapFromGlobal(global));
    return true;
}

// Wait for delivery, not merely QCursor::pos(): Windows may report Enter first,
// and native moves can arrive after QTest::mouseMove has returned.
class PointerArrival : public QObject {
public:
    PointerArrival(QWidget* target, const QPoint& global) : m_target(target), m_global(global) {
        qApp->installEventFilter(this);
    }
    ~PointerArrival() override { qApp->removeEventFilter(this); }
    bool arrived = false;
protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        auto* widget = qobject_cast<QWidget*>(watched);
        if (!widget || (widget != m_target && !m_target->isAncestorOf(widget))) return false;
        if (event->type() == QEvent::MouseMove)
            arrived |= static_cast<QMouseEvent*>(event)->globalPos() == m_global;
        else if (event->type() == QEvent::Enter)
            arrived |= static_cast<QEnterEvent*>(event)->globalPos() == m_global;
        return false;
    }
private:
    QWidget* m_target;
    QPoint m_global;
};

bool hoverActual(QWidget* target, const QPoint& point) {
    if (!target || !target->isVisible() || !target->rect().contains(point)) return false;
    PointerArrival arrival(target, target->mapToGlobal(point));
    QTest::mouseMove(target, point, 1);
    if (waitUntil([&arrival]() { return arrival.arrived; })) return true;
    qWarning() << "Native hover did not reach the target" << target->objectName() << point;
    return false;
}

bool focusBody(MarkdownEditor& editor) {
    QTextEdit* body = editor.textEditor();
    QWidget* viewport = body->viewport();
    if (!body->isVisible() || viewport->width() < 60 || viewport->height() < 40) return false;
    // The blank tail is a real body hit, not an anchor or a synthetic focus call.
    if (!clickActual(viewport, QPoint(viewport->width() - 12, viewport->height() - 12))) return false;
    return waitUntil([body]() { return body->hasFocus(); });
}

// Alpha-mode ink bounds for in-memory renders of transparent-background widgets.
QRect inkBounds(const QImage& source) {
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    QRect bounds;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y).alpha() > 40) bounds = bounds.united(QRect(x, y, 1, 1));
    return bounds;
}

QToolButton* insertButton(MarkdownEditor& editor) {
    return editor.textEditor()->findChild<QToolButton*>(QStringLiteral("EditorBlockInsert"));
}

// 空行插入钮只以光标为目标：可见即“当前光标行可插入”，与悬停无关。
bool insertIndicatorVisible(MarkdownEditor& editor) {
    QToolButton* button = insertButton(editor);
    return button && button->isVisible() && button->isEnabled();
}

// 用真实键盘把聚焦正文的光标移到目标逻辑行（Ctrl+Home 后逐行 Down）。
bool keyboardToLine(MarkdownEditor& editor, int line) {
    QTextEdit* body = editor.textEditor();
    if (!body->hasFocus()) return false;
    QTest::keyClick(body, Qt::Key_Home, Qt::ControlModifier);
    for (int step = 0; step < line; ++step) QTest::keyClick(body, Qt::Key_Down);
    return waitUntil([body, line]() { return body->textCursor().blockNumber() == line; });
}

// QTest's ASCII key helpers cannot type Chinese/surrogate pairs. Use the same
// committed-text event path as an IME, only on a visible, genuinely focused edit.
bool commitText(QWidget* edit, const QString& text) {
    if (!edit || !edit->isVisible() || !edit->hasFocus()) return false;
    QInputMethodEvent event;
    event.setCommitString(text);
    QApplication::sendEvent(edit, &event);
    QApplication::processEvents();
    return true;
}

bool replaceField(QLineEdit* edit, const QString& text) {
    if (!clickActual(edit) || !waitUntil([edit]() { return edit->hasFocus(); })) return false;
    QTest::keyClick(edit, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(edit, Qt::Key_Backspace);
    return text.isEmpty() || commitText(edit, text);
}

template<class T> T* ownedWidget(MarkdownEditor& editor, const QString& name, bool visibleOnly = false) {
    for (QWidget* widget : QApplication::allWidgets()) {
        if (widget->objectName() != name || (visibleOnly && !widget->isVisible()) || !editor.ownsPopup(widget)) continue;
        if (auto* result = qobject_cast<T*>(widget)) return result;
    }
    return nullptr;
}
QListView* hints(MarkdownEditor& editor) {
    return ownedWidget<QListView>(editor, QStringLiteral("EditorHintList"), true);
}
QModelIndex hintIndex(QListView* view, const QString& value, bool create = false) {
    if (!view || !view->model()) return QModelIndex();
    for (int row = 0; row < view->model()->rowCount(); ++row) {
        const QModelIndex index = view->model()->index(row, 0);
        if (index.data(ValueRole).toString() == value && index.data(CreateRole).toBool() == create) return index;
    }
    return QModelIndex();
}
bool mouseHint(QListView* view, const QModelIndex& index) {
    if (!view || !index.isValid() || !(index.flags() & Qt::ItemIsEnabled)) return false;
    view->scrollTo(index);
    QApplication::processEvents();
    const QRect row = view->visualRect(index);
    return !row.isEmpty() && clickActual(view->viewport(), row.center());
}
void editorKey(MarkdownEditor& editor, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    // A native popup owns the key grab. QCompleter forwards ordinary editing
    // keys to its QTextEdit; do not bypass its real popup event filter.
    QWidget* receiver = hints(editor);
    QTest::keyClick(receiver ? receiver : editor.textEditor(), key, modifiers);
}

bool visibleInkAndCaret(MarkdownEditor& editor, QString* reason) {
    QTextEdit* body = editor.textEditor();
    QWidget* viewport = body->viewport();
    const QSizeF documentSize = body->document()->documentLayout()->documentSize();
    int textBlocks = 0, missingLayouts = 0;
    qreal logicalInkWidth = 0;
    for (QTextBlock block = body->document()->begin(); block.isValid(); block = block.next()) {
        body->document()->documentLayout()->blockBoundingRect(block);
        if (block.text().isEmpty()) continue;
        ++textBlocks;
        if (!block.layout() || block.layout()->lineCount() == 0) { ++missingLayouts; continue; }
        for (int line = 0; line < block.layout()->lineCount(); ++line)
            logicalInkWidth += block.layout()->lineAt(line).naturalTextRect().width();
    }
    const QRect caret = body->cursorRect();
    const QRect visible = viewport->visibleRegion().boundingRect();
    const QColor background = ThemeManager::instance().color(ds::ColorToken::Surface);
    int ink = 0;
    if (viewport->width() > 0 && viewport->height() > 0) {
        // In-memory paint assertion only: no desktop grab, saved screenshot, or
        // manual acceptance. A nonempty document alone missed the zero-layout bug.
        QImage image(viewport->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(background);
        QPainter painter(&image);
        viewport->render(&painter);
        painter.end();
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            const int contrast = qMax(qAbs(pixel.red() - background.red()),
                qMax(qAbs(pixel.green() - background.green()), qAbs(pixel.blue() - background.blue())));
            if (pixel.alpha() > 40 && contrast > 40) ++ink;
        }
    }
    const bool ok = body->isVisible() && !visible.isEmpty() && viewport->height() >= 40
        && documentSize.height() > 0 && textBlocks > 0 && missingLayouts == 0 && logicalInkWidth > 4
        && caret.height() > 0 && visible.contains(caret.center()) && ink > qMax(40, caret.height() * 4);
    if (reason) *reason = QStringLiteral("viewport=%1x%2 visible=%3,%4 %5x%6 documentHeight=%7 textBlocks=%8 missingLayouts=%9 logicalInk=%10 caret=%11,%12 %13x%14 paintedInk=%15")
        .arg(viewport->width()).arg(viewport->height()).arg(visible.x()).arg(visible.y()).arg(visible.width()).arg(visible.height())
        .arg(documentSize.height()).arg(textBlocks).arg(missingLayouts).arg(logicalInkWidth)
        .arg(caret.x()).arg(caret.y()).arg(caret.width()).arg(caret.height()).arg(ink);
    return ok;
}

QPoint linkPoint(MarkdownEditor& editor, const QString& label, const QString& target) {
    QTextEdit* body = editor.textEditor();
    const QTextCursor found = body->document()->find(label);
    if (found.isNull()) return QPoint(-1, -1);
    for (int position = found.selectionStart(); position < found.selectionEnd(); ++position) {
        QTextCursor left(body->document()); left.setPosition(position);
        QTextCursor right = left; right.movePosition(QTextCursor::NextCharacter);
        const QRect a = body->cursorRect(left), b = body->cursorRect(right);
        if (a.top() != b.top() || b.left() <= a.left()) continue;
        const QPoint point((a.left() + b.left()) / 2, a.center().y());
        if (body->viewport()->rect().contains(point) && body->anchorAt(point) == target) return point;
    }
    return QPoint(-1, -1);
}
QWidget* hoverLink(MarkdownEditor& editor, const QString& label, const QString& target) {
    QWidget* window = editor.window();
    window->activateWindow();
    if (!QTest::qWaitForWindowActive(window)) return nullptr;
    const QPoint point = linkPoint(editor, label, target);
    if (point.x() < 0) {
        qWarning() << "No visible anchor for link hover" << label << target;
        return nullptr;
    }
    QWidget* viewport = editor.textEditor()->viewport();
    if (!hoverActual(viewport, QPoint(viewport->width() - 12, viewport->height() - 12))
        || !hoverActual(viewport, point)) return nullptr;
    if (!waitUntil([&editor]() { return ownedWidget<QWidget>(editor, QStringLiteral("LinkHoverTools"), true); })) {
        qWarning() << "Link hover tools did not appear" << point << editor.textEditor()->hasFocus()
                   << editor.textEditor()->textCursor().hasSelection() << QApplication::activePopupWidget();
        return nullptr;
    }
    return ownedWidget<QWidget>(editor, QStringLiteral("LinkHoverTools"), true);
}
QDialog* editLink(MarkdownEditor& editor, const QString& label, const QString& target) {
    QWidget* tools = hoverLink(editor, label, target);
    if (!tools) return nullptr;
    if (!clickActual(tools->findChild<QToolButton*>(QStringLiteral("LinkHoverEdit")))) {
        qWarning() << "Link hover edit is not the actual hit" << tools->geometry();
        return nullptr;
    }
    if (!waitUntil([&editor]() { return ownedWidget<QDialog>(editor, QStringLiteral("LinkEditorPopup"), true); })) {
        qWarning() << "Link editor did not appear after clicking the edit tool";
        return nullptr;
    }
    return ownedWidget<QDialog>(editor, QStringLiteral("LinkEditorPopup"), true);
}
QMenu* gutterMenu(MarkdownEditor& editor, int line) {
    QTextEdit* body = editor.textEditor();
    if (!body->hasFocus()) return nullptr;
    // The caret, not hover, is the only target: place it on the empty logical
    // line with the real keyboard, then click the actually rendered button.
    if (!keyboardToLine(editor, line)) return nullptr;
    auto* gutter = body->findChild<QWidget*>(QStringLiteral("EditorBlockGutter"));
    auto* button = body->findChild<QToolButton*>(QStringLiteral("EditorBlockInsert"));
    if (!gutter || !button || !gutter->isVisible()) return nullptr;
    const QRect caret = body->cursorRect();
    // The indicator must anchor to the caret line (same-line tolerance only;
    // the precise ink-midline geometry has its own dedicated test).
    if (!waitUntil([body, line, button, caret]() {
        return body->textCursor().blockNumber() == line && button->isVisible()
            && qAbs(button->mapToGlobal(button->rect().center()).y()
                    - body->viewport()->mapToGlobal(caret.center()).y()) <= caret.height() / 2 + 2;
    })) {
        qWarning() << "Gutter indicator did not anchor to the caret line" << line << caret
                   << button->geometry() << button->isVisible();
        return nullptr;
    }
    if (!clickActual(button)) {
        qWarning() << "Gutter indicator is not the actual hit" << button->geometry()
                   << button->window()->childAt(button->window()->mapFromGlobal(button->mapToGlobal(button->rect().center())));
        return nullptr;
    }
    if (!waitUntil([&editor]() { return ownedWidget<QMenu>(editor, QStringLiteral("EditorBlockMenu"), true); })) return nullptr;
    return ownedWidget<QMenu>(editor, QStringLiteral("EditorBlockMenu"), true);
}
bool chooseNamedAction(QMenu* menu, const QString& name) {
    QAction* action = menu ? menu->findChild<QAction*>(name) : nullptr;
    if (!action || !action->isEnabled() || !action->isVisible()) return false;
    const QRect row = menu->actionGeometry(action);
    return !row.isEmpty() && clickActual(menu, row.center());
}
bool chooseBlock(QMenu* menu, BlockStyle style) {
    return chooseNamedAction(menu, QStringLiteral("InsertBlock_%1").arg(int(style)));
}
} // namespace

class EditorInteractionTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        m_hadDataDir = qEnvironmentVariableIsSet("KYLINLIST_DATA_DIR");
        m_previousDataDir = qgetenv("KYLINLIST_DATA_DIR");
        m_previousSettingsFormat = QSettings::defaultFormat();
        m_settingsDir.reset(new QTemporaryDir);
        QVERIFY(m_settingsDir->isValid());
        qputenv("KYLINLIST_DATA_DIR", m_settingsDir->path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir->path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, m_settingsDir->path() + QStringLiteral("/system"));
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("kylinlist"), QStringLiteral("kylinlist"));
        settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("light"));
        settings.sync();
        AppStyle::apply(*qobject_cast<QApplication*>(qApp));
        disableNativeWindowShadows();
    }
    void init() {
        m_dir.reset(new QTemporaryDir);
        QVERIFY(m_dir->isValid());
        qputenv("KYLINLIST_DATA_DIR", m_dir->path().toUtf8());
        QString error;
        QVERIFY2(Database::instance().openAt(m_dir->path() + QStringLiteral("/editorinteraction.db"), &error), qPrintable(error));
        m_store.reset(new TaskStore(Database::instance().connection()));
        QVERIFY2(m_store->load(&error), qPrintable(error));
        m_sourceId = m_store->createNote(QStringLiteral("交互测试来源"));
        QVERIFY(!m_sourceId.isEmpty());
        m_touchedClipboard = false;
    }
    void cleanup() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
        if (m_touchedClipboard && QGuiApplication::clipboard()) QGuiApplication::clipboard()->clear();
        m_store.reset();
        Database::instance().close();
        m_dir.reset();
        qputenv("KYLINLIST_DATA_DIR", m_settingsDir->path().toUtf8());
    }
    void cleanupTestCase() {
        QSettings::setDefaultFormat(m_previousSettingsFormat);
        if (m_hadDataDir) qputenv("KYLINLIST_DATA_DIR", m_previousDataDir);
        else qunsetenv("KYLINLIST_DATA_DIR");
    }

    void committedUnicodeHasVisibleInkAndCaret() {
        MarkdownEditor editor;
        configure(editor, QString());
        QVERIFY(expose(editor));
        QVERIFY(focusBody(editor));
        const QString text = QStringLiteral("真实墨迹 中文 🦄 é");
        QVERIFY(commitText(editor.textEditor(), text));
        QTRY_COMPARE(editor.content(), text);
        QCOMPARE(editor.textEditor()->document()->toPlainText(), text);
        QCOMPARE(editor.sourceSelection().second, text.size());
        QString reason;
        QVERIFY2(waitUntil([&]() { return visibleInkAndCaret(editor, &reason); }), qPrintable(reason));
        QTest::keyClick(editor.textEditor(), Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(editor.content().isEmpty());
    }

    void referenceCandidatesUseOnlyLiveTitlesAndKeyboardSelection() {
        const QString task = m_store->createTask(QStringLiteral("项目 任务 🦄"));
        const QString note = m_store->createNote(QStringLiteral("项目 笔记"));
        const QString bodyOnly = m_store->createTask(QStringLiteral("其他标题"));
        Entity other = m_store->entity(bodyOnly); other.body = QStringLiteral("项目只在正文出现");
        QVERIFY(m_store->saveTask(other));
        const QString deleted = m_store->createTask(QStringLiteral("项目 已删除"));
        QVERIFY(m_store->trashTask(deleted));
        MarkdownEditor editor;
        configure(editor, QString());
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        QTest::keyClicks(editor.textEditor(), "[[");
        QTRY_VERIFY(hints(editor));
        QListView* view = hints(editor);
        QVERIFY(hintIndex(view, task).isValid());
        QVERIFY(hintIndex(view, note).isValid());
        QVERIFY(hintIndex(view, bodyOnly).isValid()); // Empty query includes live titles.
        QVERIFY(!hintIndex(view, deleted).isValid());
        QVERIFY(commitText(editor.textEditor(), QStringLiteral("项目")));
        QTRY_VERIFY(hints(editor) && hints(editor)->model()->rowCount() == 2);
        view = hints(editor);
        QVERIFY(hintIndex(view, task).isValid() && hintIndex(view, note).isValid());
        QVERIFY(!hintIndex(view, bodyOnly).isValid());
        QVERIFY(!hintIndex(view, deleted).isValid());
        const QString first = view->currentIndex().data(ValueRole).toString();
        editorKey(editor, Qt::Key_Down);
        QTRY_VERIFY(view->currentIndex().data(ValueRole).toString() != first);
        const QString chosen = view->currentIndex().data(ValueRole).toString();
        QVERIFY(chosen == task || chosen == note);
        editorKey(editor, Qt::Key_Return);
        QTRY_COMPARE(editor.content(), entityReference(chosen, m_store->entity(chosen).title));
        QTRY_VERIFY(!hints(editor));
        QCOMPARE(editor.plainTextContent(), m_store->entity(chosen).title);
        const QTextCursor anchor = editor.textEditor()->document()->find(m_store->entity(chosen).title);
        QVERIFY(!anchor.isNull()); QCOMPARE(anchor.charFormat().anchorHref(), chosen);
        QString reason;
        QVERIFY2(waitUntil([&]() { return visibleInkAndCaret(editor, &reason); }), qPrintable(reason));
    }

    void referencePopupHasVisibleInkAndBoundedScrolling() {
        QString last;
        for (int index = 0; index < 24; ++index)
            last = m_store->createTask(QStringLiteral("候选 %1").arg(index, 2, 10, QLatin1Char('0')));
        QVERIFY(!last.isEmpty());
        MarkdownEditor editor; configure(editor, QString());
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        QTest::keyClicks(editor.textEditor(), "[[");
        QVERIFY(commitText(editor.textEditor(), QStringLiteral("候选")));
        QTRY_VERIFY(hints(editor) && hints(editor)->model()->rowCount() == 24);
        auto* list = hints(editor);
        QVERIFY(list->height() <= 8 * 34 + 8 + 2 * PopupShadowMargin);
        QTRY_VERIFY(list->verticalScrollBar()->maximum() > 0);
        const QColor background = ThemeManager::instance().color(ds::ColorToken::MenuBg);
        QImage image(list->viewport()->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(background);
        QPainter painter(&image); list->viewport()->render(&painter); painter.end();
        int ink = 0;
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (qMax(qAbs(pixel.red() - background.red()), qMax(qAbs(pixel.green() - background.green()),
                    qAbs(pixel.blue() - background.blue()))) > 80) ++ink;
        }
        QVERIFY2(ink > 80, "Visible candidates must paint actual title glyphs, not only populate a model");
        const QModelIndex lastRow = hintIndex(list, last);
        QVERIFY(lastRow.isValid()); QVERIFY(mouseHint(list, lastRow));
        QTRY_COMPARE(editor.content(), entityReference(last, m_store->entity(last).title));
        QTRY_VERIFY(!hints(editor));
    }

    void referenceMouseAcceptanceIsOneUndo() {
        const QString target = m_store->createNote(QStringLiteral("目标 [A] | 🦄"));
        MarkdownEditor editor;
        configure(editor, QString());
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        QVERIFY(commitText(editor.textEditor(), QStringLiteral("前🦄 ")));
        QTest::keyClicks(editor.textEditor(), "[[");
        QVERIFY(commitText(editor.textEditor(), QStringLiteral("目标")));
        const QString before = editor.content();
        QTRY_VERIFY(hints(editor) && hintIndex(hints(editor), target).isValid());
        QVERIFY(mouseHint(hints(editor), hintIndex(hints(editor), target)));
        QTRY_COMPARE(editor.content(), QStringLiteral("前🦄 ") + entityReference(target, m_store->entity(target).title));
        QTRY_VERIFY(editor.textEditor()->hasFocus());
        QTest::keyClick(editor.textEditor(), Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(editor.content(), before);
        QString reason;
        QVERIFY2(waitUntil([&]() { return visibleInkAndCaret(editor, &reason); }), qPrintable(reason));
    }

    void escapeDismissesHintsWithoutClosingTheRealDetail() {
        QVERIFY(!m_store->createTask(QStringLiteral("候选任务")).isEmpty());
        TaskDetailWidget detail(m_store.get());
        detail.resize(560, 660); detail.setOverlayMode(true);
        QVERIFY(detail.setTask(m_sourceId)); QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>(); QVERIFY(editor);
        QSignalSpy closed(&detail, &TaskDetailWidget::closeRequested);
        QSignalSpy focusMode(&detail, &TaskDetailWidget::focusModeRequested);
        QVERIFY(focusBody(*editor));
        QTest::keyClicks(editor->textEditor(), "[[");
        QTRY_VERIFY(hints(*editor));
        const QString before = editor->content();
        editorKey(*editor, Qt::Key_Escape);
        QTRY_VERIFY(!hints(*editor));
        QCOMPARE(editor->content(), before);
        QVERIFY(detail.isVisible()); QCOMPARE(detail.entityId(), m_sourceId);
        QCOMPARE(closed.count(), 0); QCOMPARE(focusMode.count(), 0);
        QVERIFY(focusBody(*editor));
        QVERIFY(commitText(editor->textEditor(), QStringLiteral("未选文字")));
        QTRY_COMPARE(editor->content(), before + QStringLiteral("未选文字"));
        QVERIFY(detail.flush());
        QCOMPARE(m_store->entity(m_sourceId).body, editor->content());
    }

    void escapedMarkersAndCodeNeverTrigger_data() {
        QTest::addColumn<QString>("source"); QTest::addColumn<int>("caret"); QTest::addColumn<QString>("typed");
        QTest::newRow("escaped-opener") << QStringLiteral("\\") << 1 << QStringLiteral("[[");
        QTest::newRow("ordinary-brackets") << QString() << 0 << QStringLiteral("[]");
        QTest::newRow("inline-code") << QStringLiteral("`片段`") << 1 << QStringLiteral("[[");
        QTest::newRow("fenced-code") << QStringLiteral("```\n代码\n```") << 4 << QStringLiteral("[[");
        QTest::newRow("quoted-fenced-code") << QStringLiteral("> ~~~\n> 代码\n> ~~~") << 8 << QStringLiteral("[[");
        QTest::newRow("quoted-marker-inside-top-level-fence") << QStringLiteral("~~~\n> ~~~\n代码\n~~~") << 10 << QStringLiteral("[[");
    }
    void escapedMarkersAndCodeNeverTrigger() {
        QFETCH(QString, source); QFETCH(int, caret); QFETCH(QString, typed);
        QVERIFY(!m_store->createTask(QStringLiteral("候选")).isEmpty());
        MarkdownEditor editor; configure(editor, source);
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        editor.selectSource(caret, caret); // Caret setup only; input still goes through the real focused widget.
        QTest::keyClicks(editor.textEditor(), typed);
        QTRY_COMPARE(editor.content(), source.left(caret) + typed + source.mid(caret));
        QTest::qWait(30);
        QVERIFY(!hints(editor));
    }

    void disabledEmptyMatchAndImePreeditCannotAccept() {
        const QString target = m_store->createTask(QStringLiteral("项目目标"));
        MarkdownEditor editor; configure(editor, QString());
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        QTest::keyClicks(editor.textEditor(), "[[");
        QVERIFY(commitText(editor.textEditor(), QStringLiteral("没有匹配标题")));
        QTRY_VERIFY(hints(editor) && hints(editor)->model()->rowCount() == 1);
        QListView* view = hints(editor);
        const QModelIndex empty = view->model()->index(0, 0);
        QVERIFY(empty.data(ValueRole).toString().isEmpty());
        QVERIFY(!(empty.flags() & Qt::ItemIsEnabled));
        QVERIFY(!(empty.flags() & Qt::ItemIsSelectable));
        const QString before = editor.content();
        QVERIFY(clickActual(view->viewport(), view->visualRect(empty).center()));
        QCOMPARE(editor.content(), before);
        editorKey(editor, Qt::Key_Return);
        QVERIFY(internalTargets(editor.content()).isEmpty());

        editor.setContent(QString()); QVERIFY(focusBody(editor));
        QTest::keyClicks(editor.textEditor(), "[[");
        QTRY_VERIFY(hints(editor));
        QVERIFY(editor.textEditor()->hasFocus());
        QInputMethodEvent preedit(QStringLiteral("项目"), {});
        QApplication::sendEvent(editor.textEditor(), &preedit);
        QTRY_VERIFY(!hints(editor));
        QCOMPARE(editor.content(), QStringLiteral("[["));
        QTest::keyClick(editor.textEditor(), Qt::Key_Down);
        QTest::keyClick(editor.textEditor(), Qt::Key_Return);
        QVERIFY(internalTargets(editor.content()).isEmpty());
        QVERIFY(!editor.content().contains(target)); // Enter must not choose a reference while composing.
        QInputMethodEvent cancel;
        QApplication::sendEvent(editor.textEditor(), &cancel);
        QTest::keyClick(editor.textEditor(), Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(editor.textEditor(), Qt::Key_Backspace);
        QTest::keyClicks(editor.textEditor(), "[[");
        QVERIFY(commitText(editor.textEditor(), QStringLiteral("项目")));
        QTRY_VERIFY(hints(editor) && hintIndex(hints(editor), target).isValid());
        editorKey(editor, Qt::Key_Return);
        QTRY_COMPARE(editor.content(), entityReference(target, m_store->entity(target).title));
    }

    void clearingAndMovingCaretDismissCompletion() {
        MarkdownEditor editor; configure(editor, QString());
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        QVERIFY(commitText(editor.textEditor(), QStringLiteral("前缀 ")));
        QTest::keyClicks(editor.textEditor(), "[["); QTRY_VERIFY(hints(editor));
        editorKey(editor, Qt::Key_Backspace); editorKey(editor, Qt::Key_Backspace);
        QTRY_COMPARE(editor.content(), QStringLiteral("前缀 "));
        QTRY_VERIFY(!hints(editor));
        QTest::keyClicks(editor.textEditor(), "[["); QTRY_VERIFY(hints(editor));
        const QString before = editor.content();
        editorKey(editor, Qt::Key_Home);
        QTRY_VERIFY(!hints(editor)); QCOMPARE(editor.content(), before);
        QTest::keyClick(editor.textEditor(), Qt::Key_End);
        QTRY_VERIFY(hints(editor));
        editorKey(editor, Qt::Key_Left, Qt::ShiftModifier);
        QTRY_VERIFY(!hints(editor));
        QVERIFY(editor.textEditor()->textCursor().hasSelection());
    }

    void canonicalInsertionNavigatesAndSurvivesDatabaseReopen() {
        const QString target = m_store->createTask(QStringLiteral("目标 [标题] | 🦄"));
        TaskDetailWidget detail(m_store.get()); detail.resize(560, 660);
        QVERIFY(detail.setTask(m_sourceId)); QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>(); QVERIFY(editor); QVERIFY(focusBody(*editor));
        QVERIFY(commitText(editor->textEditor(), QStringLiteral("记录 ")));
        QTest::keyClicks(editor->textEditor(), "[[");
        QVERIFY(commitText(editor->textEditor(), QStringLiteral("目标")));
        QTRY_VERIFY(hints(*editor) && hintIndex(hints(*editor), target).isValid());
        QVERIFY(mouseHint(hints(*editor), hintIndex(hints(*editor), target)));
        const QString title = m_store->entity(target).title;
        const QString expected = QStringLiteral("记录 ") + entityReference(target, title);
        QTRY_COMPARE(editor->content(), expected);
        QCOMPARE(editor->plainTextContent(), QStringLiteral("记录 ") + title);
        const QPoint point = linkPoint(*editor, title, target); QVERIFY(point.x() >= 0);
        // 正文关联链接改经独立弹窗信号转发；internalLinkRequested 只保留父任务与命令后导航。
        QSignalSpy navigated(&detail, &TaskDetailWidget::relatedDetailRequested);
        QVERIFY(clickActual(editor->textEditor()->viewport(), point));
        QTRY_COMPARE(navigated.count(), 1); QCOMPARE(navigated.first().at(0).toString(), target);
        QVERIFY(detail.finishEditing()); QCOMPARE(m_store->entity(m_sourceId).body, expected);
        detail.hide();
        TaskStore reloaded(Database::instance().connection()); QString error;
        QVERIFY2(reloaded.load(&error), qPrintable(error));
        TaskDetailWidget reopened(&reloaded); reopened.resize(560, 660);
        QVERIFY(reopened.setTask(m_sourceId)); QVERIFY(expose(reopened));
        auto* restored = reopened.findChild<MarkdownEditor*>(); QVERIFY(restored); QVERIFY(focusBody(*restored));
        QCOMPARE(restored->content(), expected);
        QCOMPARE(restored->plainTextContent(), QStringLiteral("记录 ") + title);
        const QTextCursor anchor = restored->textEditor()->document()->find(title);
        QVERIFY(!anchor.isNull()); QCOMPARE(anchor.charFormat().anchorHref(), target);
        QString reason;
        QVERIFY2(waitUntil([&]() { return visibleInkAndCaret(*restored, &reason); }), qPrintable(reason));
        QVERIFY(reopened.finishEditing());
    }

    void transientWidgetsInvalidateWithContext_data() {
        QTest::addColumn<QString>("change");
        QTest::newRow("entity-switch") << QStringLiteral("entity");
        QTest::newRow("store-reset") << QStringLiteral("reset");
        QTest::newRow("read-only") << QStringLiteral("readonly");
    }
    void transientWidgetsInvalidateWithContext() {
        QFETCH(QString, change);
        const QString target = m_store->createTask(QStringLiteral("目标"));
        const QString other = m_store->createNote(QStringLiteral("另一上下文"));
        MarkdownEditor editor; configure(editor, QString());
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        const auto invalidate = [&]() {
            if (change == QLatin1String("entity")) editor.setEntityId(other);
            else if (change == QLatin1String("reset")) return m_store->load();
            else editor.textEditor()->setReadOnly(true);
            return true;
        };
        QTest::keyClicks(editor.textEditor(), "[["); QTRY_VERIFY(hints(editor));
        const QString trigger = editor.content();
        QVERIFY(invalidate()); QTRY_VERIFY(!hints(editor)); QCOMPARE(editor.content(), trigger);

        editor.textEditor()->setReadOnly(false); editor.setEntityId(m_sourceId);
        const QString linked = entityReference(target, QStringLiteral("固定别名"));
        editor.setContent(linked); QVERIFY(focusBody(editor));
        QPointer<QDialog> popup = editLink(editor, QStringLiteral("固定别名"), target); QVERIFY(popup);
        QVERIFY(invalidate()); QTRY_VERIFY(!popup || !popup->isVisible()); QCOMPARE(editor.content(), linked);

        editor.textEditor()->setReadOnly(false); editor.setEntityId(m_sourceId);
        const QString paragraphs = QStringLiteral("首行\n\n末行");
        editor.setContent(paragraphs); QVERIFY(focusBody(editor));
        QPointer<QMenu> menu = gutterMenu(editor, 1); QVERIFY(menu);
        QVERIFY(invalidate()); QTRY_VERIFY(!menu || !menu->isVisible()); QCOMPARE(editor.content(), paragraphs);
    }

    void gutterInsertsEveryBlockStyleOnEmptyLineAndOneUndo_data() {
        QTest::addColumn<int>("style"); QTest::addColumn<QString>("replacement");
        QTest::newRow("paragraph") << int(BlockStyle::Paragraph) << QString();
        QTest::newRow("heading1") << int(BlockStyle::Heading1) << QStringLiteral("# ");
        QTest::newRow("heading2") << int(BlockStyle::Heading2) << QStringLiteral("## ");
        QTest::newRow("heading3") << int(BlockStyle::Heading3) << QStringLiteral("### ");
        QTest::newRow("unordered") << int(BlockStyle::UnorderedList) << QStringLiteral("- ");
        QTest::newRow("ordered") << int(BlockStyle::OrderedList) << QStringLiteral("1. ");
        QTest::newRow("task") << int(BlockStyle::TaskList) << QStringLiteral("- [ ] ");
        QTest::newRow("quote") << int(BlockStyle::Quote) << QStringLiteral("> ");
        QTest::newRow("horizontal-rule") << int(BlockStyle::HorizontalRule) << QStringLiteral("---\n");
    }
    void gutterInsertsEveryBlockStyleOnEmptyLineAndOneUndo() {
        QFETCH(int, style); QFETCH(QString, replacement);
        // 空行插入样本：光标停在空逻辑行，菜单在该行写入块标记（水平分割线除外，
        // 它在原行落下 "---" 并留出新的空行）。
        const QString original = QStringLiteral("第一行\n\n第三行");
        const QString expected = QStringLiteral("第一行\n") + replacement + QStringLiteral("\n第三行");
        MarkdownEditor editor; configure(editor, original);
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        QMenu* menu = gutterMenu(editor, 1);
        QVERIFY2(menu, "空行上的插入钮必须随光标出现并可真实点击");
        QVERIFY(chooseBlock(menu, static_cast<BlockStyle>(style)));
        QTRY_COMPARE(editor.content(), expected);
        QTRY_VERIFY(editor.textEditor()->hasFocus());
        // 带标记后该行已非空：光标移上去不得再出现插入钮；正文（空标记）保持可插入。
        QVERIFY(keyboardToLine(editor, 1));
        if (replacement.isEmpty())
            QVERIFY2(waitUntil([&]() { return insertIndicatorVisible(editor); }, 400), "空标记行仍是空行，应保持插入钮");
        else
            QVERIFY2(waitUntil([&]() { return !insertIndicatorVisible(editor); }, 400), "非空标记行不得再显示插入钮");
        QString reason;
        QVERIFY2(waitUntil([&]() { return visibleInkAndCaret(editor, &reason); }), qPrintable(reason));
        QTest::keyClick(editor.textEditor(), Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(editor.content(), original);  // 插入标记是一次完整撤销
    }

    // 非空行的块转换、一次撤销与前缀不叠加改走真实格式面板（FooterFormat ->
    // FormatPanel -> FormatBlockStyles），不再依赖 gutter。
    void formatMenuConvertsNonEmptyBlocksAndReplacesPrefix_data() {
        QTest::addColumn<int>("style"); QTest::addColumn<QString>("replacement");
        QTest::newRow("paragraph") << int(BlockStyle::Paragraph) << QStringLiteral("目标 🦄");
        QTest::newRow("heading1") << int(BlockStyle::Heading1) << QStringLiteral("# 目标 🦄");
        QTest::newRow("heading2") << int(BlockStyle::Heading2) << QStringLiteral("## 目标 🦄");
        QTest::newRow("heading3") << int(BlockStyle::Heading3) << QStringLiteral("### 目标 🦄");
        QTest::newRow("unordered") << int(BlockStyle::UnorderedList) << QStringLiteral("- 目标 🦄");
        QTest::newRow("ordered") << int(BlockStyle::OrderedList) << QStringLiteral("1. 目标 🦄");
        QTest::newRow("task") << int(BlockStyle::TaskList) << QStringLiteral("- [x] 目标 🦄");
        QTest::newRow("quote") << int(BlockStyle::Quote) << QStringLiteral("> 目标 🦄");
        QTest::newRow("horizontal-rule") << int(BlockStyle::HorizontalRule) << QStringLiteral("- [x] 目标 🦄\n---\n");
    }
    void formatMenuConvertsNonEmptyBlocksAndReplacesPrefix() {
        QFETCH(int, style); QFETCH(QString, replacement);
        const QString original = QStringLiteral("第一行\n- [x] 目标 🦄\n第三行");
        const QString expected = QStringLiteral("第一行\n") + replacement + QStringLiteral("\n第三行");
        Entity source = m_store->entity(m_sourceId);
        source.body = original;
        QVERIFY(m_store->saveTask(source));
        TaskDetailWidget detail(m_store.get());
        detail.resize(560, 660);
        QVERIFY(detail.setTask(m_sourceId)); QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>(); QVERIFY(editor);
        QCOMPARE(editor->content(), original);
        QVERIFY(focusBody(*editor));
        QVERIFY(keyboardToLine(*editor, 1));  // 非空行：光标所在即转换目标
        const auto chooseStyle = [&]() -> bool {
            QFrame* panel = detail.findChild<QFrame*>(QStringLiteral("FormatPanel"));
            if (!panel || !panel->isVisible()) {
                auto* format = detail.findChild<QToolButton*>(QStringLiteral("FooterFormat"));
                if (!format || !format->isVisible() || !clickActual(format)) return false;
                panel = detail.findChild<QFrame*>(QStringLiteral("FormatPanel"));
            }
            if (!panel || !waitUntil([panel]() { return panel->isVisible(); })) return false;
            auto* heading = panel->findChild<QToolButton*>(QStringLiteral("FormatHeading"));
            if (!heading || !heading->isVisible() || !clickActual(heading)) return false;
            QMenu* menu = panel->findChild<QMenu*>(QStringLiteral("FormatBlockStyles"));
            return menu && waitUntil([menu]() { return menu->isVisible(); })
                && chooseNamedAction(menu, QStringLiteral("FormatBlock_%1").arg(style));
        };
        QVERIFY2(chooseStyle(), "格式面板的文本类型菜单必须可经真实点击打开");
        QTRY_COMPARE(editor->content(), expected);
        QCOMPARE(editor->content().count(QStringLiteral("目标 🦄")), 1);
        QCOMPARE(editor->content().count(QStringLiteral("第一行")), 1);
        QCOMPARE(editor->content().count(QStringLiteral("第三行")), 1);
        QTRY_VERIFY(editor->textEditor()->hasFocus());
        QTest::keyClick(editor->textEditor(), Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(editor->content(), original);  // 整行转换是一次完整撤销
        // Reapplying a prefix style replaces, rather than stacks, the old marker.
        if (static_cast<BlockStyle>(style) != BlockStyle::HorizontalRule) {
            QVERIFY(chooseStyle());
            QTRY_COMPARE(editor->content(), expected);
            QVERIFY(chooseStyle());
            QTRY_COMPARE(editor->content(), expected);
        } else QCOMPARE(expected.count(QStringLiteral("\n---\n")), 1);
        QVERIFY(detail.finishEditing());
        QCOMPARE(m_store->entity(m_sourceId).body, editor->content());
    }

    void insertIndicatorRequiresBodyFocus() {
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* away = new QLineEdit(&window);
        away->setObjectName(QStringLiteral("FocusAway"));
        MarkdownEditor editor(&window);
        configure(editor, QStringLiteral("甲行文本\n\n乙行文本"));
        layout->addWidget(&editor);
        layout->addWidget(away);
        window.resize(760, 620);
        QVERIFY(expose(window)); QVERIFY(focusBody(editor));
        QVERIFY(keyboardToLine(editor, 1));
        QVERIFY2(waitUntil([&]() { return insertIndicatorVisible(editor); }), "聚焦空行应显示插入钮");
        QVERIFY(clickActual(away));
        QVERIFY(waitUntil([away]() { return away->hasFocus(); }));
        QTRY_VERIFY(!editor.textEditor()->hasFocus());
        QTRY_VERIFY(!insertIndicatorVisible(editor));  // 失焦即隐藏
        QVERIFY(focusBody(editor));
        QVERIFY(keyboardToLine(editor, 1));
        QVERIFY2(waitUntil([&]() { return insertIndicatorVisible(editor); }), "重新聚焦正文应恢复插入钮");
    }

    void insertIndicatorHidesOnNonEmptyAndWhitespaceLines() {
        MarkdownEditor editor;
        configure(editor, QStringLiteral("甲行文本\n乙行文本\n \t\n\n尾行文本"));
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        for (const int line : {0, 1, 2}) {
            QVERIFY(keyboardToLine(editor, line));
            QVERIFY2(waitUntil([&]() { return !insertIndicatorVisible(editor); }, 400),
                qPrintable(QStringLiteral("第 %1 行（非空/空白字符）不得显示插入钮").arg(line)));
        }
        QVERIFY(keyboardToLine(editor, 3));
        QVERIFY2(waitUntil([&]() { return insertIndicatorVisible(editor); }, 400), "真正空行应显示插入钮");
    }

    void insertIndicatorHidesForSelection() {
        MarkdownEditor editor;
        configure(editor, QStringLiteral("甲行文本\n\n乙行文本"));
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        QTextEdit* body = editor.textEditor();
        QVERIFY(keyboardToLine(editor, 1));
        QVERIFY2(waitUntil([&]() { return insertIndicatorVisible(editor); }), "空行初始应显示插入钮");
        QTest::keyClick(body, Qt::Key_Up, Qt::ShiftModifier);  // 跨行选区
        QTRY_VERIFY(body->textCursor().hasSelection());
        QTRY_VERIFY(!insertIndicatorVisible(editor));
        // 点击空行本身即可清除选区并恢复（光标是唯一目标，不是悬停）。
        const QRect lineCaret = body->cursorRect(QTextCursor(body->document()->findBlockByNumber(1)));
        QVERIFY(clickActual(body->viewport(), lineCaret.center()));
        QTRY_VERIFY(!body->textCursor().hasSelection());
        QVERIFY2(waitUntil([&]() { return insertIndicatorVisible(editor); }), "清除选区后应恢复插入钮");
    }

    void insertIndicatorHidesWhileComposing() {
        MarkdownEditor editor;
        const QString original = QStringLiteral("甲行文本\n\n乙行文本");
        configure(editor, original);
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        QTextEdit* body = editor.textEditor();
        QVERIFY(keyboardToLine(editor, 1));
        QVERIFY2(waitUntil([&]() { return insertIndicatorVisible(editor); }), "空行初始应显示插入钮");
        auto* edit = qobject_cast<EditorTextEdit*>(body); QVERIFY(edit);
        QSignalSpy composing(edit, &EditorTextEdit::compositionChanged);
        QInputMethodEvent preedit(QStringLiteral("拼音预编辑"), {});
        QApplication::sendEvent(body, &preedit);
        QTRY_VERIFY(!insertIndicatorVisible(editor));  // IME 预编辑期间隐藏
        QInputMethodEvent done;  // 空 preedit 结束合成
        QApplication::sendEvent(body, &done);
        QTRY_VERIFY(insertIndicatorVisible(editor));
        QVERIFY(composing.count() >= 2);
        QCOMPARE(editor.content(), original);  // 预编辑不落入正文
    }

    void insertIndicatorHidesWhenReadOnlyOrDisabled() {
        MarkdownEditor editor;
        configure(editor, QStringLiteral("甲行文本\n\n乙行文本"));
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        QTextEdit* body = editor.textEditor();
        QVERIFY(keyboardToLine(editor, 1));
        QVERIFY2(waitUntil([&]() { return insertIndicatorVisible(editor); }), "空行初始应显示插入钮");
        body->setReadOnly(true);
        QTest::keyClick(body, Qt::Key_Down); QTest::keyClick(body, Qt::Key_Up);  // 只读仍可移动光标
        QTRY_VERIFY(!insertIndicatorVisible(editor));
        body->setReadOnly(false);
        QVERIFY(focusBody(editor));
        QVERIFY(keyboardToLine(editor, 1));
        QVERIFY2(waitUntil([&]() { return insertIndicatorVisible(editor); }), "恢复可写应重新显示插入钮");
        body->setEnabled(false);
        QTRY_VERIFY(!insertIndicatorVisible(editor));
        body->setEnabled(true);
        QVERIFY(focusBody(editor));
        QVERIFY(keyboardToLine(editor, 1));
        QVERIFY2(waitUntil([&]() { return insertIndicatorVisible(editor); }), "恢复启用应重新显示插入钮");
    }

    void insertIndicatorHidesInsideCodeFenceAndHorizontalRule() {
        MarkdownEditor editor;
        // 行 2 是代码围栏内的空行；行 6 的 --- 渲染为空白分隔线；行 5/7 是普通空行。
        configure(editor, QStringLiteral("```js\nconst a = 1;\n\nconst b = 2;\n```\n\n---\n\n尾行文本"));
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        QVERIFY(keyboardToLine(editor, 2));
        QVERIFY2(waitUntil([&]() { return !insertIndicatorVisible(editor); }, 400), "代码块内的空行不得显示插入钮");
        QVERIFY(keyboardToLine(editor, 5));
        QVERIFY2(waitUntil([&]() { return insertIndicatorVisible(editor); }, 400), "围栏外的普通空行应显示插入钮");
        QVERIFY(keyboardToLine(editor, 6));
        QVERIFY2(waitUntil([&]() { return !insertIndicatorVisible(editor); }, 400), "渲染为空白分隔线的行不得显示插入钮");
        QVERIFY(keyboardToLine(editor, 7));
        QVERIFY2(waitUntil([&]() { return insertIndicatorVisible(editor); }, 400), "分隔线后的普通空行应显示插入钮");
    }

    void insertIndicatorStaysOnCaretLineWhileHoveringOthers() {
        MarkdownEditor editor;
        configure(editor, QStringLiteral("甲行文本\n\n乙行文本\n\n丙行文本"));
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        QTextEdit* body = editor.textEditor();
        QVERIFY(keyboardToLine(editor, 1));
        QToolButton* button = insertButton(editor); QVERIFY(button);
        QVERIFY2(waitUntil([&]() { return insertIndicatorVisible(editor); }), "空行初始应显示插入钮");
        const int anchoredY = button->mapToGlobal(button->rect().center()).y();
        QWidget* viewport = body->viewport();
        QWidget* gutter = body->findChild<QWidget*>(QStringLiteral("EditorBlockGutter")); QVERIFY(gutter);
        const QRect otherLine = body->cursorRect(QTextCursor(body->document()->findBlockByNumber(3)));
        // 真实悬停另一空行与其 gutter 位置：插入钮不随悬停移动（原生事件，非 sendEvent）。
        QVERIFY(hoverActual(viewport, QPoint(viewport->width() - 12, otherLine.center().y())));
        QVERIFY(hoverActual(gutter, QPoint(gutter->width() / 2, otherLine.center().y())));
        QVERIFY2(waitUntil([button, anchoredY]() {
            return button->isVisible() && button->mapToGlobal(button->rect().center()).y() == anchoredY;
        }, 400), "悬停其他行不得移动或隐藏插入钮");
    }

    // 几何：以 QTextLayout 基线 + 同字体渲染的“中Ag”参考墨迹实测中线核对按钮
    // 墨迹中线（1x/2x），不用 rect==rect 的同义反复；并核对悬停不添加底色。
    void insertIndicatorGeometryMatchesReferenceInkMidline() {
        MarkdownEditor editor;
        configure(editor, QStringLiteral("第一行文本\n\n第三行文本"));
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        QVERIFY(keyboardToLine(editor, 1));
        QTextEdit* body = editor.textEditor();
        QToolButton* button = insertButton(editor); QVERIFY(button);
        QVERIFY2(waitUntil([&]() { return insertIndicatorVisible(editor); }), "空行插入钮未显示");
        QCOMPARE(button->focusPolicy(), Qt::NoFocus);
        const QTextBlock block = body->textCursor().block();
        const QRect caret = body->cursorRect();
        const QFont referenceFont = block.charFormat().font().resolve(body->document()->defaultFont());
        const QTextLayout* layout = block.layout();
        QVERIFY(layout && layout->lineCount() > 0);
        const qreal baseline = caret.top() + layout->lineAt(0).ascent();
        const QFontMetricsF metrics(referenceFont);
        for (const qreal dpr : {1.0, 2.0}) {
            // 参考墨迹：用同一字体把“中Ag”渲染到透明位图，实测基线到墨迹中线的距离。
            const qreal pad = 12.0;
            QImage reference(qCeil((metrics.horizontalAdvance(QStringLiteral("中Ag")) + 2 * pad) * dpr),
                             qCeil((metrics.height() + 2 * pad) * dpr), QImage::Format_ARGB32_Premultiplied);
            reference.fill(Qt::transparent);
            reference.setDevicePixelRatio(dpr);
            const qreal drawBaseline = pad + metrics.ascent();
            {
                QPainter painter(&reference);
                painter.setFont(referenceFont);
                painter.drawText(QPointF(pad, drawBaseline), QStringLiteral("中Ag"));
            }
            const QRect referenceInk = inkBounds(reference);
            QVERIFY2(!referenceInk.isEmpty(), "参考文字必须产生真实墨迹");
            const qreal referenceMid = (referenceInk.top() + referenceInk.bottom()) / 2.0 / dpr;
            const qreal midAboveBaseline = drawBaseline - referenceMid;
            // 按钮墨迹：同一 DPR 下内存渲染，取图标墨迹中线（全局坐标比对）。
            QImage shot(qCeil(button->width() * dpr), qCeil(button->height() * dpr),
                        QImage::Format_ARGB32_Premultiplied);
            shot.fill(Qt::transparent);
            shot.setDevicePixelRatio(dpr);
            {
                QPainter painter(&shot);
                button->render(&painter);
            }
            const QRect buttonInk = inkBounds(shot);
            QVERIFY2(!buttonInk.isEmpty(), "插入钮必须绘制可见图标");
            const qreal buttonMidGlobal = button->mapToGlobal(QPoint()).y()
                + (buttonInk.top() + buttonInk.bottom()) / 2.0 / dpr;
            const qreal expectedMidGlobal = body->viewport()->mapToGlobal(QPoint()).y()
                + baseline - midAboveBaseline;
            QVERIFY2(qAbs(buttonMidGlobal - expectedMidGlobal) <= 2.0,
                qPrintable(QStringLiteral("dpr=%1 按钮墨迹中线 %2 期望 %3（基线 %4）")
                    .arg(dpr).arg(buttonMidGlobal).arg(expectedMidGlobal).arg(baseline)));
        }
        // 无悬停底色：真实悬停前后渲染逐像素一致。
        QImage idle(button->width(), button->height(), QImage::Format_ARGB32_Premultiplied);
        idle.fill(Qt::transparent);
        {
            QPainter painter(&idle);
            button->render(&painter);
        }
        QVERIFY(hoverActual(button, button->rect().center()));
        QImage hovered(idle.size(), QImage::Format_ARGB32_Premultiplied);
        hovered.fill(Qt::transparent);
        {
            QPainter painter(&hovered);
            button->render(&painter);
        }
        QVERIFY2(idle == hovered, "悬停不得给插入钮添加底色");
    }

    void tagCandidatesAndCreateSignalNeverAutoEraseMetadata() {
        Entity source = m_store->entity(m_sourceId); source.tags = QStringList({QStringLiteral("保留")});
        QVERIFY(m_store->saveTask(source));
        const QString donorId = m_store->createTask(QStringLiteral("标签来源"));
        Entity donor = m_store->entity(donorId); donor.tags = QStringList({QStringLiteral("工作")}); QVERIFY(m_store->saveTask(donor));
        TaskDetailWidget detail(m_store.get()); detail.resize(560, 660);
        QVERIFY(detail.setTask(m_sourceId)); QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>(); QVERIFY(editor); QVERIFY(focusBody(*editor));
        QSignalSpy selected(editor, &MarkdownEditor::tagSelected);
        QTest::keyClicks(editor->textEditor(), "#"); QVERIFY(commitText(editor->textEditor(), QStringLiteral("工")));
        QTRY_VERIFY(hints(*editor) && hintIndex(hints(*editor), QStringLiteral("工作")).isValid());
        QVERIFY(mouseHint(hints(*editor), hintIndex(hints(*editor), QStringLiteral("工作"))));
        QTRY_COMPARE(selected.count(), 1); QCOMPARE(selected.first().at(0).toString(), QStringLiteral("工作"));
        QTRY_VERIFY(m_store->entity(m_sourceId).tags.contains(QStringLiteral("工作")));
        QTest::keyClick(editor->textEditor(), Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(editor->textEditor(), Qt::Key_Backspace);
        QTRY_VERIFY(editor->content().isEmpty()); QVERIFY(detail.flush());
        QVERIFY(m_store->entity(m_sourceId).tags.contains(QStringLiteral("工作")));
        QVERIFY(m_store->entity(m_sourceId).tags.contains(QStringLiteral("保留")));
        QCOMPARE(selected.count(), 1);
        QTest::keyClicks(editor->textEditor(), "#"); QVERIFY(commitText(editor->textEditor(), QStringLiteral("新标签")));
        QTRY_VERIFY(hints(*editor) && hintIndex(hints(*editor), QStringLiteral("新标签"), true).isValid());
        const QModelIndex create = hintIndex(hints(*editor), QStringLiteral("新标签"), true);
        QVERIFY(create.flags() & Qt::ItemIsEnabled); QVERIFY(create.data(CreateRole).toBool());
        editorKey(*editor, Qt::Key_Return);
        QTRY_COMPARE(selected.count(), 2); QCOMPARE(selected.last().at(0).toString(), QStringLiteral("新标签"));
        QTRY_VERIFY(m_store->entity(m_sourceId).tags.contains(QStringLiteral("新标签")));
        QTest::keyClick(editor->textEditor(), Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(editor->textEditor(), Qt::Key_Backspace); QVERIFY(detail.flush());
        const QStringList tags = m_store->entity(m_sourceId).tags;
        QVERIFY(tags.contains(QStringLiteral("工作")) && tags.contains(QStringLiteral("新标签")) && tags.contains(QStringLiteral("保留")));
        QTest::keyClicks(editor->textEditor(), "#"); QVERIFY(commitText(editor->textEditor(), QStringLiteral("未选择")));
        QTRY_VERIFY(hints(*editor)); editorKey(*editor, Qt::Key_Escape); QVERIFY(detail.flush());
        QCOMPARE(selected.count(), 2); QVERIFY(!m_store->entity(m_sourceId).tags.contains(QStringLiteral("未选择")));
    }

    void realHoverDistinguishesSameLineLinksAndCopiesCanonicalSource() {
        QClipboard* clipboard = QGuiApplication::clipboard(); if (!clipboard) QSKIP("The active Qt platform provides no clipboard.");
        m_touchedClipboard = true; clipboard->setText(QStringLiteral("Synthetic clipboard sentinel"));
        const QString target = m_store->createTask(QStringLiteral("当前目标"));
        const QString first = QStringLiteral("甲 [A] | 🦄"), second = QStringLiteral("乙 [B] \\ 路径");
        const QString url = QStringLiteral("https://example.com/路径(a)");
        const QString source = entityReference(target, first) + QStringLiteral(" / ") + entityReference(target, second)
            + QStringLiteral(" / ") + markdownLink(url, QStringLiteral("外部文档"));
        MarkdownEditor editor; configure(editor, source);
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        QWidget* tools = hoverLink(editor, first, target); QVERIFY(tools);
        QVERIFY(editor.textEditor()->hasFocus());
        QCOMPARE(tools->size(), QSize(70, 36));
        const QRect card(tools->mapToGlobal(QPoint()), tools->size());
        QWidget* viewport = editor.textEditor()->viewport();
        const QPoint shadowMargin = viewport->mapFromGlobal(card.topLeft() + QPoint(8, -3));
        QVERIFY(viewport->rect().contains(shadowMargin));
        // The separate transparent shadow must not intercept this real body hit.
        QVERIFY(clickActual(viewport, shadowMargin));
        QVERIFY(editor.textEditor()->hasFocus()); QCOMPARE(editor.content(), source);
        tools = hoverLink(editor, first, target); QVERIFY(tools);
        QVERIFY(clickActual(tools->findChild<QToolButton*>(QStringLiteral("LinkHoverCopy"))));
        QTRY_COMPARE(clipboard->text(), entityReference(target, first));
        tools = hoverLink(editor, second, target); QVERIFY(tools);
        QVERIFY(clickActual(tools->findChild<QToolButton*>(QStringLiteral("LinkHoverCopy"))));
        QTRY_COMPARE(clipboard->text(), entityReference(target, second));
        QDialog* popup = editLink(editor, second, target); QVERIFY(popup);
        auto* label = popup->findChild<QLineEdit*>(QStringLiteral("LinkText"));
        auto* destination = popup->findChild<QLineEdit*>(QStringLiteral("LinkTarget"));
        QVERIFY(label && destination);
        QCOMPARE(label->text(), second); QCOMPARE(destination->text(), target);
        QVERIFY(clickActual(popup->findChild<QToolButton*>(QStringLiteral("LinkEditorClose"))));
        QTRY_VERIFY(!popup->isVisible());
        tools = hoverLink(editor, QStringLiteral("外部文档"), url); QVERIFY(tools);
        QVERIFY(clickActual(tools->findChild<QToolButton*>(QStringLiteral("LinkHoverCopy"))));
        QTRY_COMPARE(clipboard->text(), url); QCOMPARE(editor.content(), source);
    }

    void linkEditConfirmRemoveAndCancelUseRealWidgets() {
        const QString target = m_store->createTask(QStringLiteral("目标"));
        const QString first = QStringLiteral("保留 [甲]"), second = QStringLiteral("编辑 [乙] | 🦄");
        const QString prefix = QStringLiteral("前🦄 ") + entityReference(target, first) + QStringLiteral(" 与 ");
        const QString source = prefix + entityReference(target, second) + QStringLiteral(" 后");
        MarkdownEditor editor; configure(editor, source);
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        QDialog* popup = editLink(editor, second, target); QVERIFY(popup);
        auto* text = popup->findChild<QLineEdit*>(QStringLiteral("LinkText"));
        auto* destination = popup->findChild<QLineEdit*>(QStringLiteral("LinkTarget"));
        const QString replacementLabel = QStringLiteral("新 [标签] | \\ 🦄");
        const QString url = QStringLiteral("https://example.com/路径(a(b))?q=二");
        QVERIFY(replaceField(text, replacementLabel)); QVERIFY(replaceField(destination, url));
        auto* confirm = popup->findChild<QPushButton*>(QStringLiteral("LinkConfirm")); QVERIFY(confirm && confirm->isEnabled());
        QVERIFY(clickActual(confirm));
        const QString edited = prefix + markdownLink(url, replacementLabel) + QStringLiteral(" 后");
        QTRY_COMPARE(editor.content(), edited);
        QCOMPARE(editor.content().count(entityReference(target, first)), 1);
        popup = editLink(editor, replacementLabel, url); QVERIFY(popup);
        text = popup->findChild<QLineEdit*>(QStringLiteral("LinkText"));
        QVERIFY(replaceField(text, QStringLiteral("取消的文字")));
        QVERIFY(clickActual(popup->findChild<QToolButton*>(QStringLiteral("LinkEditorClose"))));
        QTRY_VERIFY(!popup->isVisible()); QCOMPARE(editor.content(), edited);
        popup = editLink(editor, replacementLabel, url); QVERIFY(popup);
        auto* remove = popup->findChild<QPushButton*>(QStringLiteral("LinkRemove")); QVERIFY(remove && remove->isVisible());
        QVERIFY(clickActual(remove));
        QTRY_COMPARE(markdownLinks(editor.content()).size(), 1);
        QCOMPARE(markdownLinks(editor.content()).first().target, target);
        QCOMPARE(editor.plainTextContent(), QStringLiteral("前🦄 ") + first + QStringLiteral(" 与 ") + replacementLabel + QStringLiteral(" 后"));
        const QString unwrapped = editor.content();
        QVERIFY(!unwrapped.contains(url));
        QTest::keyClick(editor.textEditor(), Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(editor.content(), edited);
        QString reason;
        QVERIFY2(waitUntil([&]() { return visibleInkAndCaret(editor, &reason); }), qPrintable(reason));
    }

    void unsafeExternalInputDisablesConfirmWithoutChangingSource() {
        const QString url = QStringLiteral("https://example.com/a");
        const QString source = markdownLink(url, QStringLiteral("安全文档"));
        MarkdownEditor editor; configure(editor, source);
        QVERIFY(expose(editor)); QVERIFY(focusBody(editor));
        QDialog* popup = editLink(editor, QStringLiteral("安全文档"), url); QVERIFY(popup);
        auto* destination = popup->findChild<QLineEdit*>(QStringLiteral("LinkTarget"));
        auto* confirm = popup->findChild<QPushButton*>(QStringLiteral("LinkConfirm")); QVERIFY(destination && confirm);
        for (const QString& unsafe : {QStringLiteral("javascript:alert(1)"), QStringLiteral("data:text/html,test"), QStringLiteral("file:///tmp/test.txt")}) {
            QVERIFY(replaceField(destination, unsafe)); QTRY_VERIFY(!confirm->isEnabled());
            QTest::keyClick(destination, Qt::Key_Return); QApplication::processEvents();
            QVERIFY(popup->isVisible()); QCOMPARE(editor.content(), source);
        }
        QVERIFY(replaceField(destination, QStringLiteral("https://example.com/路径(二)")));
        QTRY_VERIFY(confirm->isEnabled());
        QTest::keyClick(destination, Qt::Key_Escape);
        QTRY_VERIFY(!popup->isVisible()); QCOMPARE(editor.content(), source);
    }

    void clipboardSourceMimeRoundTripsRenderedUnicodeLinks() {
        QClipboard* clipboard = QGuiApplication::clipboard(); if (!clipboard) QSKIP("The active Qt platform provides no clipboard.");
        m_touchedClipboard = true; clipboard->setText(QStringLiteral("Synthetic clipboard sentinel"));
        const QString target = m_store->createNote(QStringLiteral("目标 [标题] | 🦄"));
        const QString source = QStringLiteral("前缀 ") + entityReference(target, m_store->entity(target).title)
            + QStringLiteral(" / ") + markdownLink(QStringLiteral("https://example.com/a(b)"), QStringLiteral("外部 [文档]"));
        QWidget window; auto* layout = new QVBoxLayout(&window);
        auto* origin = new MarkdownEditor(&window); auto* destination = new MarkdownEditor(&window);
        configure(*origin, source); configure(*destination, QString());
        layout->addWidget(origin); layout->addWidget(destination); window.resize(720, 580);
        QVERIFY(expose(window)); QVERIFY(focusBody(*origin));
        const QString visible = origin->textEditor()->document()->toPlainText();
        QTest::keyClick(origin->textEditor(), Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(origin->textEditor(), Qt::Key_C, Qt::ControlModifier);
        QTRY_VERIFY(clipboard->mimeData() && clipboard->mimeData()->hasFormat(QLatin1String(MarkdownMime)));
        QCOMPARE(clipboard->mimeData()->data(QLatin1String(MarkdownMime)), source.toUtf8());
        QCOMPARE(clipboard->text(), visible);
        QVERIFY(focusBody(*destination));
        QTest::keyClick(destination->textEditor(), Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(destination->content(), source);
        QCOMPARE(destination->plainTextContent(), origin->plainTextContent());
        QTRY_VERIFY(!destination->textEditor()->document()->find(m_store->entity(target).title).isNull());
        const QTextCursor anchor = destination->textEditor()->document()->find(m_store->entity(target).title);
        QCOMPARE(anchor.charFormat().anchorHref(), target);
        QString reason;
        QVERIFY2(waitUntil([&]() { return visibleInkAndCaret(*destination, &reason); }), qPrintable(reason));
        QTest::keyClick(destination->textEditor(), Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(destination->content(), QString());
    }

private:
    void configure(MarkdownEditor& editor, const QString& source) {
        editor.setStore(m_store.get()); editor.setEntityId(m_sourceId);
        editor.setBlockControlsEnabled(true); editor.setContent(source); editor.resize(720, 540);
    }
    bool m_hadDataDir = false, m_touchedClipboard = false;
    QByteArray m_previousDataDir;
    QSettings::Format m_previousSettingsFormat = QSettings::NativeFormat;
    std::unique_ptr<QTemporaryDir> m_settingsDir, m_dir;
    std::unique_ptr<TaskStore> m_store;
    QString m_sourceId;
};

QTEST_MAIN(EditorInteractionTest)
#include "tst_editorinteraction.moc"
