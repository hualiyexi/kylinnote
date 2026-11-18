#include <cstdio>
#include <functional>
#include <memory>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QtTest>
#include "app/Branding.h"
#include "app/KylinListApp.h"
#include "app/NoteManager.h"
#include "app/TaskStore.h"
#include "app/EntityTextSession.h"
#include "ui/PopupMenu.h"
#include "db/Database.h"
#include "domain/InternalLink.h"
#include "platform/SystemShadow.h"
#include "ui/AppStyle.h"
#include "ui/AttachmentWidget.h"
#include "ui/BrandingIcons.h"
#include "ui/DatePickerPopup.h"
#include "ui/Markdown.h"
#include "ui/MarkdownEditor.h"
#include "ui/ModalDialog.h"
#include "ui/NoteWidget.h"
#include "ui/PopupShadow.h"
#include "ui/RelatedDetailPopup.h"
#include "ui/TaskDetailWidget.h"
#include "ui/TaskListModel.h"
#include "ui/TaskPanel.h"
#include "ui/WindowChrome.h"
#include "ui/design/ThemeManager.h"

using namespace kl;
namespace {
bool waitUntil(const std::function<bool()>& predicate) {
    QElapsedTimer timer; timer.start();
    do {
        QApplication::processEvents();
        if (predicate()) return true;
        QTest::qWait(5);
    } while (timer.elapsed() < 1800);
    return predicate();
}
bool expose(QWidget& widget) {
    const QRect screen = QGuiApplication::primaryScreen()->availableGeometry();
    widget.resize(qMin(widget.width(), screen.width() - 40), qMin(widget.height(), screen.height() - 60));
    widget.move(screen.topLeft() + QPoint(20, 20));
    widget.show(); widget.raise(); widget.activateWindow();
    return QTest::qWaitForWindowExposed(&widget) && QTest::qWaitForWindowActive(&widget);
}
bool clickActual(QWidget* target, QPoint point = QPoint(-1, -1)) {
    QApplication::processEvents();
    if (!target || !target->isVisible() || !target->isEnabled()) {
        qWarning() << "Hit target unavailable" << target;
        return false;
    }
    if (point.x() < 0) point = target->rect().center();
    QWidget* window = target->window();
    if (!window->isActiveWindow() && window->windowType() != Qt::Popup) {
        window->activateWindow();
        if (QGuiApplication::platformName() == QLatin1String("offscreen")) QApplication::setActiveWindow(window);
        if (!QTest::qWaitForWindowActive(window)) {
            qWarning() << "Window activation failed" << window << QApplication::activeWindow();
            return false;
        }
    }
    const QPoint global = target->mapToGlobal(point);
    QWidget* hit = window->childAt(window->mapFromGlobal(global));
    if (!hit && target == window) hit = window;
    if (hit != target && (!hit || !target->isAncestorOf(hit))) {
        qWarning() << "Unexpected deepest hit" << hit << "for" << target << point << target->geometry();
        return false;
    }
    const QString platform = QGuiApplication::platformName();
    if (platform != QLatin1String("offscreen") && platform != QLatin1String("minimal")
        && QApplication::widgetAt(global) != hit) return false;
    QTest::mouseClick(hit, Qt::LeftButton, Qt::NoModifier, hit->mapFromGlobal(global));
    return true;
}
bool replaceText(QTextEdit* edit, const char* text) {
    QApplication::processEvents();
    if (!clickActual(edit->viewport(), QPoint(edit->viewport()->width() - 10, 12))) return false;
    if (!waitUntil([edit]() { return edit->hasFocus(); })) {
        qWarning() << "Body click did not focus" << edit << edit->focusPolicy()
                   << QApplication::focusWidget() << QApplication::activeWindow()
                   << edit->window()->isActiveWindow() << edit->isEnabled() << edit->isReadOnly();
        return false;
    }
    QTest::keyClick(edit, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(edit, Qt::Key_Backspace);
    QTest::keyClicks(edit, text);
    return true;
}
bool replaceTitle(TaskDetailWidget* detail, const char* text) {
    auto* edit = detail->findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
    if (!clickActual(edit)) return false;
    QTest::keyClick(edit, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(edit, Qt::Key_Backspace);
    QTest::keyClicks(edit, text);
    return true;
}
QPoint anchorPoint(QTextEdit* edit, const QString& id) {
    const QRect viewport = edit->viewport()->rect();
    for (int y = 2; y < viewport.height(); y += 2)
        for (int x = 2; x < viewport.width(); x += 2)
            if (edit->anchorAt(QPoint(x, y)) == id) return QPoint(x, y);
    return QPoint(-1, -1);
}
bool clickLink(MarkdownEditor* editor, const QString& id) {
    if (!editor) return false;
    QPoint point;
    if (!waitUntil([&]() { point = anchorPoint(editor->textEditor(), id); return point.x() >= 0; })) return false;
    return clickActual(editor->textEditor()->viewport(), point);
}
bool sql(const QString& statement) {
    QSqlQuery query(Database::instance().connection());
    return query.exec(statement);
}
qint64 revisionFor(const QString& id) {
    QSqlQuery query(Database::instance().connection());
    query.prepare(QStringLiteral("SELECT revision FROM entities WHERE id=?"));
    query.addBindValue(id);
    return query.exec() && query.next() ? query.value(0).toLongLong() : -1;
}
TaskDetailWidget* mainDetail(TaskPanel& panel) {
    return panel.findChild<TaskDetailWidget*>(QStringLiteral("TaskDetailWidget"));
}
RelatedDetailPopup* popup(TaskPanel& panel) { return panel.relatedDetailPopup(); }
TaskDetailWidget* popupDetail(TaskPanel& panel) {
    return popup(panel)->findChild<TaskDetailWidget*>(QStringLiteral("RelatedTaskDetailWidget"));
}
std::shared_ptr<bool> respondToConfirmation(QObject* context, bool accept) {
    const auto clicked = std::make_shared<bool>(false);
    auto* poll = new QTimer(context);
    poll->setInterval(10);
    QObject::connect(poll, &QTimer::timeout, context, [clicked, accept, poll]() {
        auto* dialog = qobject_cast<ConfirmDialog*>(QApplication::activeModalWidget());
        if (!dialog || !dialog->isVisible()) return;
        poll->stop();
        poll->deleteLater();
        const QPointer<ConfirmDialog> guard(dialog);
        QTimer::singleShot(1200, dialog, [guard]() { if (guard) guard->reject(); });
        *clicked = clickActual(dialog->findChild<QPushButton*>(accept
            ? QStringLiteral("ConfirmAccept") : QStringLiteral("ConfirmCancel")));
    });
    poll->start();
    return clicked;
}
TaskPanel* applicationPanel() {
    for (QWidget* widget : QApplication::topLevelWidgets())
        if (auto* panel = qobject_cast<TaskPanel*>(widget)) return panel;
    return nullptr;
}
}  // namespace

class RelatedDetailTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        m_originalData = qgetenv("KYLINLIST_DATA_DIR");
        m_originalBrand = qgetenv("KYLINLIST_BRANDING_DIR");
        m_hadData = qEnvironmentVariableIsSet("KYLINLIST_DATA_DIR");
        m_hadBrand = qEnvironmentVariableIsSet("KYLINLIST_BRANDING_DIR");
        m_originalFormat = QSettings::defaultFormat();
        m_settings.reset(new QTemporaryDir);
        QVERIFY(m_settings->isValid());
        qputenv("KYLINLIST_DATA_DIR", m_settings->path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings->path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, m_settings->path() + QStringLiteral("/system"));
        AppStyle::apply(*qobject_cast<QApplication*>(qApp));
        disableNativeWindowShadows();
    }
    void init() {
        m_dir.reset(new QTemporaryDir);
        QVERIFY(m_dir->isValid());
        qputenv("KYLINLIST_DATA_DIR", m_dir->path().toUtf8());
        qputenv("KYLINLIST_BRANDING_DIR", m_dir->path().toUtf8());
        Branding::load();
        ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
        QSettings panel(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("kylinlist"), QStringLiteral("panel"));
        panel.clear();
        QString error;
        QVERIFY2(Database::instance().openAt(m_dir->path() + QStringLiteral("/related.db"), &error), qPrintable(error));
        m_store.reset(new TaskStore(Database::instance().connection()));
        QVERIFY2(m_store->load(&error), qPrintable(error));
        m_target = seed(QStringLiteral("Related target"), QStringLiteral("Target body"));
        m_source = seed(QStringLiteral("Source item"), entityReference(m_target, QStringLiteral("Open related")));
        m_other = seed(QStringLiteral("Other item"), QStringLiteral("Other body"));
        QVERIFY(!m_source.isEmpty() && !m_target.isEmpty() && !m_other.isEmpty());
    }
    void cleanup() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
        m_store.reset(); Database::instance().close(); m_dir.reset();
    }
    void cleanupTestCase() {
        QSettings::setDefaultFormat(m_originalFormat);
        if (m_hadData) qputenv("KYLINLIST_DATA_DIR", m_originalData); else qunsetenv("KYLINLIST_DATA_DIR");
        if (m_hadBrand) qputenv("KYLINLIST_BRANDING_DIR", m_originalBrand); else qunsetenv("KYLINLIST_BRANDING_DIR");
    }

    void bodyLinkPreservesMainDetailAndNavigation() {
        TaskPanel panel(m_store.get()); panel.showTask(m_source); QVERIFY(expose(panel));
        auto* source = mainDetail(panel); QVERIFY(source);
        auto* editor = source->findChild<MarkdownEditor*>();
        auto* list = panel.findChild<QListView*>(QStringLiteral("TaskList"));
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        auto* scroll = source->findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        QVERIFY(editor && list && navigation && scroll);
        QVERIFY(!panel.findChild<QLineEdit*>(QStringLiteral("TaskSearch")));
        const QString body = editor->content();
        const int navigationRow = navigation->currentRow();
        const QModelIndex selected = list->currentIndex();
        const int offset = scroll->verticalScrollBar()->value();
        QVERIFY(clickLink(editor, m_target));
        QTRY_VERIFY(popup(panel)->isVisible());
        QCOMPARE(mainDetail(panel), source);
        QCOMPARE(source->entityId(), m_source);
        QCOMPARE(list->currentIndex(), selected); QCOMPARE(navigation->currentRow(), navigationRow);
        QCOMPARE(editor->content(), body); QCOMPARE(scroll->verticalScrollBar()->value(), offset);
        auto* detail = popupDetail(panel); QVERIFY(detail && detail != source);
        QCOMPARE(detail->entityId(), m_target);
        QVERIFY(detail->findChild<AttachmentWidget*>());
        QVERIFY(detail->findChild<QPushButton*>(QStringLiteral("DateChip"))->isVisible());
        QVERIFY(!detail->findChild<QToolButton*>(QStringLiteral("DetailClose"))->isVisible());
        QVERIFY(popup(panel)->requestClose());
    }

    void repeatSelfAndCycleReuseOneDetail() {
        Entity target = m_store->entity(m_target);
        target.body = entityReference(m_target, QStringLiteral("Self")) + QLatin1Char('\n')
            + entityReference(m_source, QStringLiteral("Source"));
        QVERIFY(m_store->saveTask(target));
        TaskPanel panel(m_store.get()); panel.showTask(m_source); QVERIFY(expose(panel));
        QVERIFY(clickLink(mainDetail(panel)->findChild<MarkdownEditor*>(), m_target));
        QTRY_VERIFY(popupDetail(panel));
        auto* detail = popupDetail(panel); auto* editor = detail->findChild<MarkdownEditor*>();
        const quint64 generation = editor->contextGeneration();
        panel.openRelatedDetail(m_target);
        QCOMPARE(popupDetail(panel), detail); QCOMPARE(editor->contextGeneration(), generation);
        QVERIFY(clickLink(editor, m_target));
        QCOMPARE(popupDetail(panel), detail); QCOMPARE(editor->contextGeneration(), generation);
        QVERIFY(clickLink(editor, m_source)); QTRY_COMPARE(detail->entityId(), m_source);
        QVERIFY(clickLink(editor, m_target)); QTRY_COMPARE(detail->entityId(), m_target);
        QVERIFY(popup(panel)->isWindow());
        QVERIFY(!popup(panel)->parentWidget());
        QCOMPARE(popup(panel)->findChildren<TaskDetailWidget*>().size(), 1);
        QCOMPARE(mainDetail(panel)->entityId(), m_source);
        QVERIFY(popup(panel)->requestClose());
    }

    void mainWindowSelectionDoesNotCloseRelatedWindow() {
        TaskPanel panel(m_store.get()); panel.showTask(m_source); QVERIFY(expose(panel));
        panel.openRelatedDetail(m_target); QTRY_VERIFY(popup(panel)->isVisible());
        auto* list = panel.findChild<QListView*>(QStringLiteral("TaskList")); QVERIFY(list);
        auto* model = qobject_cast<TaskListModel*>(list->model()); QVERIFY(model);
        const QModelIndex other = model->indexForTask(m_other);
        const QRect row = list->visualRect(other); QVERIFY(row.isValid());
        panel.activateWindow();
        if (QGuiApplication::platformName() == QLatin1String("offscreen")) QApplication::setActiveWindow(&panel);
        QVERIFY(QTest::qWaitForWindowActive(&panel));
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, row.center());
        QTRY_COMPARE(mainDetail(panel)->entityId(), m_other);
        QVERIFY(popup(panel)->isVisible());
        QCOMPARE(popup(panel)->entityId(), m_target);
        QVERIFY(popup(panel)->requestClose());
    }

    void mainWindowEscapeAndQuickAddKeepRelatedWindow() {
        TaskPanel panel(m_store.get()); panel.showTask(m_source); QVERIFY(expose(panel));
        panel.openRelatedDetail(m_target); QTRY_VERIFY(popup(panel)->isVisible());
        panel.activateWindow();
        if (QGuiApplication::platformName() == QLatin1String("offscreen")) QApplication::setActiveWindow(&panel);
        QVERIFY(QTest::qWaitForWindowActive(&panel));
        QTest::keyClick(&panel, Qt::Key_Escape);
        QTRY_VERIFY(mainDetail(panel)->entityId().isEmpty());
        QVERIFY(popup(panel)->isVisible());
        panel.focusQuickAdd();
        QVERIFY(popup(panel)->isVisible());
        QCOMPARE(popup(panel)->entityId(), m_target);
        QVERIFY(popup(panel)->requestClose());
    }

    void escapeDismissesDateThenCard() {
        TaskPanel panel(m_store.get()); panel.showTask(m_source); QVERIFY(expose(panel));
        panel.openRelatedDetail(m_target); auto* detail = popupDetail(panel); QVERIFY(detail);
        QVERIFY(clickActual(detail->findChild<QPushButton*>(QStringLiteral("DateChip"))));
        DatePickerPopup* date = nullptr;
        QTRY_VERIFY((date = detail->findChild<DatePickerPopup*>()) && date->isVisible());
        QTest::keyClick(date, Qt::Key_Escape); QTRY_VERIFY(!date->isVisible());
        QVERIFY(popup(panel)->isVisible());
        QTest::keyClick(popup(panel), Qt::Key_Escape); QTRY_VERIFY(!popup(panel)->isVisible());
        QCOMPARE(mainDetail(panel)->entityId(), m_source);
    }

    void inputMethodEscapeDoesNotDismissCard() {
        TaskPanel panel(m_store.get()); panel.showTask(m_source); QVERIFY(expose(panel));
        panel.openRelatedDetail(m_target); auto* editor = popupDetail(panel)->findChild<MarkdownEditor*>();
        QVERIFY(replaceText(editor->textEditor(), "Draft"));
        QInputMethodEvent preedit(QStringLiteral("test"), {});
        QApplication::sendEvent(editor->textEditor(), &preedit);
        QVERIFY(editor->isComposing());
        QTest::keyClick(editor->textEditor(), Qt::Key_Escape);
        QVERIFY(popup(panel)->isVisible());
        QInputMethodEvent end; QApplication::sendEvent(editor->textEditor(), &end);
        QVERIFY(!editor->isComposing());
        QVERIFY(popup(panel)->requestClose());
        QCOMPARE(m_store->entity(m_target).body, QStringLiteral("Draft"));
    }

    void closeFailureRetainsDraftAndSaveShortcutRetries() {
        TaskPanel panel(m_store.get()); panel.showTask(m_source); QVERIFY(expose(panel));
        panel.openRelatedDetail(m_target); auto* editor = popupDetail(panel)->findChild<MarkdownEditor*>();
        QVERIFY(replaceText(editor->textEditor(), "Pending draft"));
        QVERIFY(sql(QStringLiteral("CREATE TRIGGER reject_text BEFORE UPDATE ON entities BEGIN SELECT RAISE(ABORT,'fixture'); END")));
        QVERIFY(!popup(panel)->requestClose()); QVERIFY(popup(panel)->isVisible());
        auto* windowClose = popup(panel)->findChild<QToolButton*>(QStringLiteral("WindowClose"));
        QVERIFY(windowClose);
        QTest::mouseClick(windowClose, Qt::LeftButton);
        QVERIFY(popup(panel)->isVisible());
        QCOMPARE(editor->content(), QStringLiteral("Pending draft"));
        QCOMPARE(m_store->entity(m_target).body, QStringLiteral("Target body"));
        QVERIFY(!panel.flush());
        QVERIFY(sql(QStringLiteral("DROP TRIGGER reject_text")));
        QTest::keyClick(editor->textEditor(), Qt::Key_S, Qt::ControlModifier);
        QTRY_COMPARE(m_store->entity(m_target).body, QStringLiteral("Pending draft"));
        QVERIFY(popup(panel)->requestClose());
    }

    void historyFailurePreventsCloseAndTargetSwitch() {
        TaskPanel panel(m_store.get()); panel.showTask(m_source); QVERIFY(expose(panel));
        panel.openRelatedDetail(m_target); auto* editor = popupDetail(panel)->findChild<MarkdownEditor*>();
        QVERIFY(replaceText(editor->textEditor(), "Saved body"));
        QVERIFY(sql(QStringLiteral("CREATE TRIGGER reject_history BEFORE INSERT ON entity_events WHEN NEW.type='changed' BEGIN SELECT RAISE(ABORT,'fixture'); END")));
        QVERIFY(!popup(panel)->requestClose());
        QCOMPARE(m_store->entity(m_target).body, QStringLiteral("Saved body"));
        const qint64 revision = revisionFor(m_target);
        panel.openRelatedDetail(m_other); QCOMPARE(popup(panel)->entityId(), m_target);
        QVERIFY(popupDetail(panel)->findChild<QToolButton*>(QStringLiteral("DetailCopyDraft"))->isVisible());
        QVERIFY(sql(QStringLiteral("DROP TRIGGER reject_history")));
        QVERIFY(popup(panel)->requestClose()); QCOMPARE(revisionFor(m_target), revision);
    }

    void differentFieldsMergeAndSameFieldConflictIsAtomic() {
        TaskDetailWidget detail(m_store.get()); detail.resize(450, 600);
        QVERIFY(detail.setTask(m_target)); QVERIFY(expose(detail));
        QVERIFY(replaceTitle(&detail, "Local title"));
        Entity remote = m_store->entity(m_target); remote.body = QStringLiteral("Remote body");
        QVERIFY(m_store->saveTask(remote));
        QVERIFY(detail.flush());
        QCOMPARE(m_store->entity(m_target).title, QStringLiteral("Local title"));
        QCOMPARE(m_store->entity(m_target).body, QStringLiteral("Remote body"));
        auto* editor = detail.findChild<MarkdownEditor*>();
        QCOMPARE(editor->content(), QStringLiteral("Remote body"));
        QVERIFY(replaceText(editor->textEditor(), "Local body"));
        remote = m_store->entity(m_target); remote.body = QStringLiteral("New remote body");
        QVERIFY(m_store->saveTask(remote));
        const Entity before = m_store->entity(m_target);
        const qint64 beforeRevision = revisionFor(m_target);
        QVERIFY(!detail.finishEditing());
        QCOMPARE(editor->content(), QStringLiteral("Local body"));
        QCOMPARE(m_store->entity(m_target).body, before.body);
        QCOMPARE(revisionFor(m_target), beforeRevision);
        QCOMPARE(m_store->entity(m_target).updatedAt, before.updatedAt);
        QVERIFY(detail.findChild<QToolButton*>(QStringLiteral("DetailReloadDraft"))->isVisible());
    }

    void revertingLocalDraftAdoptsRemoteInsteadOfOverwritingIt() {
        TaskDetailWidget detail(m_store.get()); detail.resize(450, 600);
        QVERIFY(detail.setTask(m_target)); QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        QVERIFY(replaceText(editor->textEditor(), "Temporary local body"));
        Entity remote = m_store->entity(m_target); remote.body = QStringLiteral("Remote accepted body");
        QVERIFY(m_store->saveTask(remote));
        QVERIFY(replaceText(editor->textEditor(), "Target body"));
        const qint64 revision = revisionFor(m_target);
        QVERIFY(detail.finishEditing());
        QCOMPARE(editor->content(), QStringLiteral("Remote accepted body"));
        QCOMPARE(revisionFor(m_target), revision);
    }

    void synchronousObserverCannotEraseNewDraft() {
        TaskDetailWidget detail(m_store.get()); detail.resize(450, 600);
        QVERIFY(detail.setTask(m_target)); QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        QVERIFY(replaceText(editor->textEditor(), "First draft"));
        bool edited = false;
        const auto connection = connect(m_store.get(), &TaskStore::taskChanged, &detail, [&](const QString& id) {
            if (id != m_target || edited) return;
            edited = true;
            QTest::keyClick(editor->textEditor(), Qt::Key_End, Qt::ControlModifier);
            QTest::keyClicks(editor->textEditor(), " plus new input");
        });
        QVERIFY(!detail.flush()); disconnect(connection);
        QVERIFY(edited);
        QCOMPARE(m_store->entity(m_target).body, QStringLiteral("First draft"));
        QCOMPARE(editor->content(), QStringLiteral("First draft plus new input"));
        QVERIFY(detail.finishEditing());
        QCOMPARE(m_store->entity(m_target).body, editor->content());
    }

    void cleanDeletedTargetClosesButDirtyTargetIsNotResurrected() {
        TaskPanel panel(m_store.get()); panel.showTask(m_source); QVERIFY(expose(panel));
        panel.openRelatedDetail(m_target); QVERIFY(m_store->trashTask(m_target));
        QVERIFY(popup(panel)->isVisible());
        QVERIFY(!popupDetail(panel)->findChild<MarkdownEditor*>()->isEnabled());
        QVERIFY(m_store->deletePermanently(m_target)); QTRY_VERIFY(!popup(panel)->isVisible());
        panel.openRelatedDetail(m_other); auto* editor = popupDetail(panel)->findChild<MarkdownEditor*>();
        QVERIFY(replaceText(editor->textEditor(), "Orphan draft"));
        QVERIFY(m_store->trashTask(m_other)); QVERIFY(m_store->deletePermanently(m_other));
        QVERIFY(!popup(panel)->requestClose());
        QCOMPARE(editor->content(), QStringLiteral("Orphan draft")); QVERIFY(m_store->entity(m_other).id.isEmpty());
    }

    void restoreFailureUnfreezesAndSuccessfulRestoreClearsOldContext() {
        KylinListApp app; QString error; QVERIFY2(app.init(&error, true, true), qPrintable(error));
        TaskPanel* panel = applicationPanel(); QVERIFY(panel); QVERIFY(expose(*panel));
        auto* store = app.findChild<TaskStore*>(); QVERIFY(store);
        const QString backup = m_dir->path() + QStringLiteral("/restore.klb");
        QVERIFY(store->exportBackup(backup));
        panel->openRelatedDetail(m_target); auto* editor = popupDetail(*panel)->findChild<MarkdownEditor*>();
        QVERIFY(replaceText(editor->textEditor(), "Saved before restore"));
        QVERIFY(!app.importBackup(m_dir->path() + QStringLiteral("/missing.klb")));
        QVERIFY(panel->isEnabled() && popup(*panel)->isVisible());
        QCOMPARE(editor->content(), QStringLiteral("Saved before restore"));
        QVERIFY(app.importBackup(backup));
        QVERIFY(!popup(*panel)->isVisible()); QVERIFY(popup(*panel)->entityId().isEmpty());
        QCOMPARE(store->entity(m_target).body, QStringLiteral("Target body"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
        QCOMPARE(store->entity(m_target).body, QStringLiteral("Target body"));
        QVERIFY(app.prepareForExit()); m_store.reset(); app.shutdown();
    }

    void applicationExitDoesNotBypassPopupSaveFailure() {
        KylinListApp app; QString error; QVERIFY2(app.init(&error, true, true), qPrintable(error));
        TaskPanel* panel = applicationPanel(); QVERIFY(panel); QVERIFY(expose(*panel));
        panel->openRelatedDetail(m_target); auto* editor = popupDetail(*panel)->findChild<MarkdownEditor*>();
        QVERIFY(replaceText(editor->textEditor(), "Exit draft"));
        QVERIFY(sql(QStringLiteral("CREATE TRIGGER reject_exit BEFORE UPDATE ON entities BEGIN SELECT RAISE(ABORT,'fixture'); END")));
        QVERIFY(!app.prepareForExit()); QVERIFY(popup(*panel)->isVisible());
        QCOMPARE(editor->content(), QStringLiteral("Exit draft"));
        QVERIFY(sql(QStringLiteral("DROP TRIGGER reject_exit"))); QVERIFY(app.prepareForExit());
        QCOMPARE(app.findChild<TaskStore*>()->entity(m_target).body, QStringLiteral("Exit draft"));
        m_store.reset(); app.shutdown();
    }

    void storeDestructionReleasesPopupContext() {
        RelatedDetailPopup related(m_store.get());
        related.setAnchorBounds(QRect(QPoint(0, 0), QSize(900, 700)));
        QVERIFY(related.openEntity(m_target));
        m_store.reset(); QTRY_VERIFY(!related.isVisible());
        QVERIFY(related.entityId().isEmpty()); QVERIFY(!related.openEntity(m_target));
    }

    void independentWindowSupportsChromeDragAndEightWayResize() {
        RelatedDetailPopup related(m_store.get());
        related.setAnchorBounds(QRect(QPoint(80, 60), QSize(1000, 700)));
        QVERIFY(related.openEntity(m_target));
        QVERIFY(QTest::qWaitForWindowExposed(&related));
        QVERIFY(related.isWindow());
        QVERIFY(!related.parentWidget());
        QVERIFY(related.testAttribute(Qt::WA_TranslucentBackground));
        QVERIFY(related.windowFlags().testFlag(Qt::FramelessWindowHint));
        QVERIFY(related.windowFlags().testFlag(Qt::NoDropShadowWindowHint));
        auto* controls = related.findChild<WindowControls*>(); QVERIFY(controls);
        auto* minimize = controls->findChild<QToolButton*>(QStringLiteral("WindowMinimize"));
        auto* maximize = controls->findChild<QToolButton*>(QStringLiteral("WindowMaximize"));
        auto* close = controls->findChild<QToolButton*>(QStringLiteral("WindowClose"));
        auto* drag = related.findChild<WindowDragArea*>(QStringLiteral("RelatedDetailDragArea"));
        QVERIFY(minimize && maximize && close && drag);

        const QPoint initial = related.pos();
        const QPoint local = drag->rect().center();
        const QPoint pressGlobal = drag->mapToGlobal(local);
        QWidget* actualHit = QApplication::widgetAt(pressGlobal);
        QVERIFY(actualHit == drag);
        QVERIFY(drag->height() > 0);
        QTest::mousePress(drag, Qt::LeftButton, Qt::NoModifier, local);
        const QPoint moveDelta(38, 24);
        QMouseEvent move(QEvent::MouseMove, drag->mapFromGlobal(pressGlobal + moveDelta),
                         pressGlobal + moveDelta, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(drag, &move);
        QTest::mouseRelease(drag, Qt::LeftButton, Qt::NoModifier,
                            drag->mapFromGlobal(pressGlobal + moveDelta));
        QCOMPARE(related.pos(), initial + moveDelta);

        const QStringList names = {QStringLiteral("RelatedResizeLeft"), QStringLiteral("RelatedResizeRight"),
            QStringLiteral("RelatedResizeTop"), QStringLiteral("RelatedResizeBottom"),
            QStringLiteral("RelatedResizeTopLeft"), QStringLiteral("RelatedResizeTopRight"),
            QStringLiteral("RelatedResizeBottomLeft"), QStringLiteral("RelatedResizeBottomRight")};
        for (const QString& name : names) {
            auto* area = related.findChild<WindowResizeArea*>(name);
            QVERIFY(area && area->isVisible());
            const QRect before = related.geometry();
            QRect expected = before;
            if (name.contains(QLatin1String("Left"))) expected.setLeft(before.left() + 12);
            if (name.contains(QLatin1String("Right"))) expected.setRight(before.right() + 12);
            if (name.contains(QLatin1String("Top"))) expected.setTop(before.top() + 12);
            if (name.contains(QLatin1String("Bottom"))) expected.setBottom(before.bottom() + 12);
            const QPoint point = area->rect().center();
            const QPoint global = area->mapToGlobal(point);
            QTest::mousePress(area, Qt::LeftButton, Qt::NoModifier, point);
            QMouseEvent resize(QEvent::MouseMove, area->mapFromGlobal(global + QPoint(12, 12)),
                               global + QPoint(12, 12), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(area, &resize);
            QTest::mouseRelease(area, Qt::LeftButton, Qt::NoModifier,
                                area->mapFromGlobal(global + QPoint(12, 12)));
            QCOMPARE(related.geometry(), expected);
        }

        QTest::mouseDClick(drag, Qt::LeftButton, Qt::NoModifier, drag->rect().center());
        QTRY_VERIFY(related.isMaximized());
        QVERIFY(related.styleSheet().contains(QStringLiteral("border-top-left-radius:0px")));
        for (const QString& name : names)
            QVERIFY(!related.findChild<WindowResizeArea*>(name)->isVisible());
        QTest::mouseClick(maximize, Qt::LeftButton);
        QTRY_VERIFY(!related.isMaximized());
        QVERIFY(related.styleSheet().contains(QStringLiteral("border-top-left-radius:16px")));
        QTest::mouseClick(minimize, Qt::LeftButton);
        QTRY_VERIFY(related.isMinimized());
        related.showNormal();
        QTRY_VERIFY(related.isVisible() && !related.isMinimized());
        QVERIFY(related.requestClose());
    }

    void roundedCardRetainsShadowAndOverlayBorder() {
        if (QGuiApplication::platformName() == QLatin1String("offscreen"))
            QSKIP("Rounded child compositing and stacking are verified with the native windows platform.");
        TaskPanel panel(m_store.get()); panel.showTask(m_source); QVERIFY(expose(panel));
        panel.openRelatedDetail(m_target);
        QWidget* card = popup(panel)->findChild<QWidget*>(QStringLiteral("RelatedDetailCard")); QVERIFY(card);
        QWidget* border = card->findChild<QWidget*>(QStringLiteral("RelatedDetailBorder")); QVERIFY(border);
        QVERIFY(border->testAttribute(Qt::WA_TransparentForMouseEvents));
        QVERIFY(card->mask().isEmpty()); QVERIFY(popupDetail(panel)->mask().isEmpty());
        QCOMPARE(border->size(), QSize(popup(panel)->width() - 2 * PopupShadowMargin,
                                      popup(panel)->height() - 2 * PopupShadowMargin));
        QImage image(card->size(), QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
        QPainter painter(&image); card->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren); painter.end();
        const QRect content = card->rect().adjusted(PopupShadowMargin, PopupShadowMargin, -PopupShadowMargin, -PopupShadowMargin);
        QVERIFY(image.pixelColor(content.topLeft()).alpha() < 128);
        QCOMPARE(image.pixelColor(content.center()).alpha(), 255);
        const QPoint shadow(content.center().x(), content.bottom() + 8);
        QVERIFY(image.pixelColor(shadow).alpha() > 0 && image.pixelColor(shadow).alpha() < 128);
        for (ds::Theme theme : {ds::Theme::Light, ds::Theme::Dark}) {
            const QColor background = ds::color(ds::ColorToken::Surface, theme);
            QImage composed(image.size(), image.format()); composed.fill(background);
            QPainter composite(&composed); composite.drawImage(0, 0, image); composite.end();
            const QColor ink = image.pixelColor(shadow);
            const auto blended = [&ink](int foreground, int base) {
                return (foreground * ink.alpha() + base * (255 - ink.alpha()) + 127) / 255;
            };
            const QColor actual = composed.pixelColor(shadow);
            QVERIFY(qAbs(actual.red() - blended(ink.red(), background.red())) <= 1);
            QVERIFY(qAbs(actual.green() - blended(ink.green(), background.green())) <= 1);
            QVERIFY(qAbs(actual.blue() - blended(ink.blue(), background.blue())) <= 1);
        }
        QVERIFY(popup(panel)->requestClose());
    }

    void noteConflictAndLayoutSaveNeverOverwriteRemoteText() {
        NoteManager notes(m_store.get());
        QVERIFY(m_store->saveDesktopState(m_target, NoteColor::Yellow, QRect(30, 40, 360, 340), 0, NoteLayer::Normal, true));
        NoteWidget* note = notes.note(m_target); QVERIFY(note); QVERIFY(expose(*note));
        note->focusEditor(); auto* editor = note->findChild<MarkdownEditor*>();
        QVERIFY(replaceText(editor->textEditor(), "Local note draft"));
        Entity remote = m_store->entity(m_target); remote.title = QStringLiteral("Remote title"); remote.body = QStringLiteral("Remote note body");
        QVERIFY(m_store->saveTask(remote));
        const Entity before = m_store->entity(m_target);
        const qint64 beforeRevision = revisionFor(m_target);
        QVERIFY(!note->finishEditing()); QVERIFY(note->textRejected());
        QCOMPARE(editor->content(), QStringLiteral("Local note draft"));
        note->move(note->pos() + QPoint(5, 5));
        QVERIFY(!note->flush());
        QCOMPARE(m_store->entity(m_target).title, before.title); QCOMPARE(m_store->entity(m_target).body, before.body);
        QCOMPARE(revisionFor(m_target), beforeRevision);
        auto* copy = note->findChild<QToolButton*>(QStringLiteral("DetailCopyDraft"));
        QVERIFY(copy); QVERIFY(copy->isVisible());
        const QSize beforeSize = note->size();
        note->resize(360, beforeSize.height());
        QCOMPARE(note->width(), 360);
    }

    void noteAcceptsCleanRemoteChangesAndSavesTextAndGeometry() {
        NoteManager notes(m_store.get());
        QVERIFY(m_store->saveDesktopState(m_target, NoteColor::Yellow, QRect(30, 40, 360, 340), 0, NoteLayer::Normal, true));
        NoteWidget* note = notes.note(m_target); QVERIFY(note); QVERIFY(expose(*note));
        Entity remote = m_store->entity(m_target); remote.body = QStringLiteral("Accepted remote body");
        QVERIFY(m_store->saveTask(remote));
        auto* editor = note->findChild<MarkdownEditor*>(); QCOMPARE(editor->content(), remote.body);
        note->focusEditor(); QVERIFY(replaceText(editor->textEditor(), "Saved note body"));
        note->move(note->pos() + QPoint(7, 9));
        QVERIFY(note->finishEditing());
        QCOMPARE(m_store->entity(m_target).body, QStringLiteral("Saved note body"));
        QCOMPARE(m_store->desktopNote(m_target).geometry, note->geometry());
        QVERIFY(notes.flushAll());
    }

    void noteSynchronousObserverKeepsNewInput() {
        NoteManager notes(m_store.get());
        QVERIFY(m_store->saveDesktopState(m_target, NoteColor::Yellow, QRect(30, 40, 360, 340), 0, NoteLayer::Normal, true));
        NoteWidget* note = notes.note(m_target); QVERIFY(note); QVERIFY(expose(*note)); note->focusEditor();
        auto* editor = note->findChild<MarkdownEditor*>(); QVERIFY(replaceText(editor->textEditor(), "Note first"));
        bool edited = false;
        const auto connection = connect(m_store.get(), &TaskStore::taskChanged, note, [&](const QString& id) {
            if (id != m_target || edited) return;
            edited = true; QTest::keyClick(editor->textEditor(), Qt::Key_End, Qt::ControlModifier);
            QTest::keyClicks(editor->textEditor(), " then next");
        });
        QVERIFY(!note->flush()); disconnect(connection); QVERIFY(edited);
        QCOMPARE(m_store->entity(m_target).body, QStringLiteral("Note first"));
        QCOMPARE(editor->content(), QStringLiteral("Note first then next"));
        QVERIFY(note->finishEditing()); QCOMPARE(m_store->entity(m_target).body, editor->content());
    }

    void openingRelatedPreservesSourceUndo() {
        TaskPanel panel(m_store.get()); panel.showTask(m_source); QVERIFY(expose(panel));
        QVERIFY(!panel.findChild<QToolButton*>(QStringLiteral("RailSearch")));
        QVERIFY(!panel.findChild<QLineEdit*>(QStringLiteral("TaskSearch")));
        auto* detail = mainDetail(panel); auto* editor = detail->findChild<MarkdownEditor*>();
        QTextEdit* body = editor->textEditor();
        QVERIFY(clickActual(body->viewport(), QPoint(body->viewport()->width() - 10, 12)));
        QTest::keyClick(body, Qt::Key_End, Qt::ControlModifier);
        QTest::keyClick(body, Qt::Key_Return);
        QTest::keyClicks(body, "Undo sentinel");
        QVERIFY(m_store->textSession(m_source)->canUndoBody());
        QTextDocument* document = body->document();
        const QString source = editor->content();
        QVERIFY(clickLink(editor, m_target)); QTRY_VERIFY(popup(panel)->isVisible());
        QCOMPARE(detail->entityId(), m_source);
        QCOMPARE(body->document(), document); QCOMPARE(editor->content(), source);
        QVERIFY(popup(panel)->requestClose());
        QVERIFY(clickActual(body->viewport(), QPoint(body->viewport()->width() - 10, body->viewport()->height() - 10)));
        QTRY_VERIFY(body->hasFocus());
        QVERIFY(m_store->textSession(m_source)->canUndoBody());
        QTest::keyClick(body, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(editor->content(), entityReference(m_target, QStringLiteral("Open related")) + QLatin1Char('\n'));
        QTest::keyClick(body, Qt::Key_Y, Qt::ControlModifier);
        QTRY_COMPARE(editor->content(), source);
    }

    void escapeDismissesFormatAndCompletionBeforeCard() {
        TaskPanel panel(m_store.get()); panel.showTask(m_source); QVERIFY(expose(panel));
        panel.openRelatedDetail(m_target); auto* detail = popupDetail(panel);
        QVERIFY(clickActual(detail->findChild<QToolButton*>(QStringLiteral("FooterFormat"))));
        QWidget* format = detail->findChild<QWidget*>(QStringLiteral("FormatPanel"));
        QTRY_VERIFY(format && format->isVisible());
        QTest::keyClick(format, Qt::Key_Escape); QTRY_VERIFY(!format->isVisible());
        QVERIFY(popup(panel)->isVisible());
        auto* editor = detail->findChild<MarkdownEditor*>();
        QVERIFY(replaceText(editor->textEditor(), "[["));
        QWidget* hints = nullptr;
        QTRY_VERIFY(([&]() {
            for (QWidget* widget : QApplication::allWidgets())
                if (widget->objectName() == QLatin1String("EditorHintList") && widget->isVisible()
                    && editor->ownsPopup(widget)) { hints = widget; return true; }
            return false;
        })());
        QTest::keyClick(hints, Qt::Key_Escape); QTRY_VERIFY(!hints->isVisible());
        QVERIFY(popup(panel)->isVisible());
        QVERIFY(popup(panel)->requestClose());
    }

    void conflictReloadRequiresConfirmationAndNeverOverwritesRemote() {
        TaskDetailWidget detail(m_store.get()); detail.resize(450, 600);
        QVERIFY(detail.setTask(m_target)); QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        QVERIFY(replaceText(editor->textEditor(), "Local recovery draft"));
        Entity remote = m_store->entity(m_target); remote.body = QStringLiteral("Remote recovery body");
        QVERIFY(m_store->saveTask(remote)); QVERIFY(!detail.flush());
        auto* reload = detail.findChild<QToolButton*>(QStringLiteral("DetailReloadDraft")); QVERIFY(reload->isVisible());
        auto cancelled = respondToConfirmation(&detail, false);
        QVERIFY(clickActual(reload)); QVERIFY(*cancelled);
        QCOMPARE(editor->content(), QStringLiteral("Local recovery draft"));
        auto accepted = respondToConfirmation(&detail, true);
        QVERIFY(clickActual(reload)); QVERIFY(*accepted);
        QCOMPARE(editor->content(), QStringLiteral("Remote recovery body"));
        QVERIFY(detail.finishEditing());
        QCOMPARE(m_store->entity(m_target).body, QStringLiteral("Remote recovery body"));
    }

    void reloadDoesNotDiscardDraftIfHistoryCancellationFails() {
        TaskDetailWidget detail(m_store.get()); detail.resize(450, 600);
        QVERIFY(detail.setTask(m_target)); QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* session = m_store->textSession(m_target); QVERIFY(session);
        // Keep a known successful write in the shared session while injecting a
        // later conflict; focus changes are tested separately from journal failure.
        QVERIFY(session->updateBody(QStringLiteral("Saved own body"), &detail));
        QVERIFY(detail.flush());
        QVERIFY(session->updateBody(QStringLiteral("Unsaved own draft"), &detail));
        QCOMPARE(editor->content(), QStringLiteral("Unsaved own draft"));
        Entity remote = m_store->entity(m_target); remote.body = QStringLiteral("Other body");
        QVERIFY(m_store->saveTask(remote));
        QVERIFY(sql(QStringLiteral("CREATE TRIGGER reject_cancel BEFORE INSERT ON entity_events WHEN NEW.type='changed' BEGIN SELECT RAISE(ABORT,'fixture'); END")));
        QVERIFY(!detail.finishEditing());
        auto accepted = respondToConfirmation(&detail, true);
        QVERIFY(clickActual(detail.findChild<QToolButton*>(QStringLiteral("DetailReloadDraft")))); QVERIFY(*accepted);
        QCOMPARE(editor->content(), QStringLiteral("Unsaved own draft"));
        QVERIFY(sql(QStringLiteral("DROP TRIGGER reject_cancel")));
        accepted = respondToConfirmation(&detail, true);
        QVERIFY(clickActual(detail.findChild<QToolButton*>(QStringLiteral("DetailReloadDraft")))); QVERIFY(*accepted);
        QCOMPARE(editor->content(), QStringLiteral("Other body"));
    }

    void sourceWidgetMayBeDestroyedBySynchronousObserver() {
        QPointer<TaskDetailWidget> detail = new TaskDetailWidget(m_store.get());
        detail->resize(450, 600); QVERIFY(detail->setTask(m_target)); QVERIFY(expose(*detail));
        QVERIFY(replaceText(detail->findChild<MarkdownEditor*>()->textEditor(), "Final body before destruction"));
        const auto connection = connect(m_store.get(), &TaskStore::taskChanged, this, [&](const QString& id) {
            if (id == m_target && detail) delete detail.data();
        });
        const bool ok = detail->flush(); disconnect(connection);
        QVERIFY(!ok); QVERIFY(!detail);
        QCOMPARE(m_store->entity(m_target).body, QStringLiteral("Final body before destruction"));
    }

    void noteReferenceAndTitleBarHaveSeparateRoutes() {
        Entity source = m_store->entity(m_source); source.body = entityReference(m_target, QStringLiteral("Open note related"));
        QVERIFY(m_store->saveTask(source));
        NoteManager notes(m_store.get());
        QVERIFY(m_store->saveDesktopState(m_source, NoteColor::Yellow, QRect(30, 40, 360, 340), 0, NoteLayer::Normal, true));
        NoteWidget* note = notes.note(m_source); QVERIFY(note); QVERIFY(expose(*note));
        QSignalSpy related(&notes, &NoteManager::relatedDetailRequested);
        QSignalSpy navigation(&notes, &NoteManager::taskOpenRequested);
        auto* editor = note->findChild<MarkdownEditor*>(); QVERIFY(editor);
        QVERIFY(clickLink(editor, m_target));
        QTRY_COMPARE(related.count(), 1); QCOMPARE(navigation.count(), 0);
        QEvent enter(QEvent::Enter); QApplication::sendEvent(note, &enter);
        auto* more = note->findChild<QToolButton*>(QStringLiteral("DetailMore"));
        QVERIFY(more); QTRY_VERIFY(more->isVisible());
        QVERIFY(clickActual(more));
        auto* menu = note->findChild<PopupMenu*>(QStringLiteral("NoteMoreMenu"));
        QVERIFY(menu); QTRY_VERIFY(menu->isVisible());
        QAction* open = nullptr;
        for (auto* action : menu->actions())
            if (action->text() == QStringLiteral("打开主窗口")) open = action;
        QVERIFY(open); menu->setActiveAction(open);
        QTest::keyClick(menu, Qt::Key_Return);
        QTRY_COMPARE(navigation.count(), 1); QCOMPARE(navigation.first().first().toString(), m_source);
        QVERIFY(notes.flushAll());
    }

    void deletedNoteKeepsDraftUntilExplicitDiscard() {
        NoteManager notes(m_store.get());
        QVERIFY(m_store->saveDesktopState(m_target, NoteColor::Yellow, QRect(30, 40, 360, 340), 0, NoteLayer::Normal, true));
        NoteWidget* note = notes.note(m_target); QVERIFY(note); QVERIFY(expose(*note)); note->focusEditor();
        auto* editor = note->findChild<MarkdownEditor*>(); QVERIFY(replaceText(editor->textEditor(), "Deleted note draft"));
        QVERIFY(m_store->trashTask(m_target)); QCOMPARE(notes.count(), 1);
        QVERIFY(!note->finishEditing()); QCOMPARE(editor->content(), QStringLiteral("Deleted note draft"));
        auto accepted = respondToConfirmation(note, true);
        auto* reload = note->findChild<QToolButton*>(QStringLiteral("DetailReloadDraft"));
        QVERIFY(reload);
        QVERIFY(clickActual(reload)); QVERIFY(*accepted);
        QTRY_COMPARE(notes.count(), 0);
        QVERIFY(m_store->entity(m_target).deleted);
        QCOMPARE(m_store->entity(m_target).body, QStringLiteral("Target body"));
        QVERIFY(!m_store->desktopNote(m_target).open);
    }

private:
    QString seed(const QString& title, const QString& body) {
        const QString id = m_store->createTask(title);
        if (id.isEmpty()) return QString();
        Entity entity = m_store->entity(id); entity.body = body;
        return m_store->saveTask(entity) ? id : QString();
    }
    std::unique_ptr<QTemporaryDir> m_settings, m_dir;
    std::unique_ptr<TaskStore> m_store;
    QString m_source, m_target, m_other;
    QByteArray m_originalData, m_originalBrand;
    bool m_hadData = false, m_hadBrand = false;
    QSettings::Format m_originalFormat = QSettings::NativeFormat;
};

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    RelatedDetailTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_relateddetail.moc"
