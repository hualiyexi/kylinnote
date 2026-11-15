#include <cstdio>
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QHostAddress>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPushButton>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSet>
#include <QSqlQuery>
#include <QSplitter>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFragment>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QtTest>
#include <memory>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include "app/NoteManager.h"
#include "app/KylinListApp.h"
#include "app/ReminderScheduler.h"
#include "app/Settings.h"
#include "app/TaskStore.h"
#include "app/updates/InstallIdentity.h"
#include "app/updates/UpdateService.h"
#include "db/Database.h"
#include "domain/InternalLink.h"
#include "domain/ViewSpec.h"
#include "platform/SystemShadow.h"
#include "ui/AppStyle.h"
#include "ui/AppIcons.h"
#include "ui/AppToolTip.h"
#include "ui/ActivityLogDialog.h"
#include "ui/EntityHoverPreview.h"
#include "ui/EntityPicker.h"
#include "ui/AttachmentWidget.h"
#include "ui/DetailPopups.h"
#include "ui/DatePickerPopup.h"
#include "ui/ColorPickerPopup.h"
#include "ui/EmojiPickerPopup.h"
#include "ui/FolderEditorDialog.h"
#include "ui/ListAppearance.h"
#include "ui/ListEditorDialog.h"
#include "ui/ModalDialog.h"
#include "ui/MenuComboBox.h"
#include "ui/NavigationGeometry.h"
#include "ui/NavigationListWidget.h"
#include "ui/PopupMenu.h"
#include "ui/TaskDateController.h"
#include "ui/SettingsDialog.h"
#include "ui/SmartListSettingsWidget.h"
#include "ui/Markdown.h"
#include "ui/MarkdownEditor.h"
#include "ui/NoteChecklist.h"
#include "ui/NoteWidget.h"
#include "ui/ReminderPopup.h"
#include "ui/StatsDialog.h"
#include "ui/ResizeGrip.h"
#include "ui/TaskDetailWidget.h"
#include "ui/TaskDelegate.h"
#include "ui/TaskListModel.h"
#include "ui/TaskListView.h"
#include "ui/TaskPanel.h"
#include "ui/SidebarSectionHeader.h"
#include "ui/ToastIsland.h"
#include "ui/UpdateDialog.h"
#include "ui/WindowChrome.h"
#include "ui/design/QssBuilder.h"
#include "ui/design/ThemeManager.h"
#include "ui/design/TypeSystem.h"
using namespace kl;

namespace {
QRect rectIn(const QWidget* widget, const QWidget* ancestor) {
    return QRect(widget->mapTo(ancestor, QPoint()), widget->size());
}

QString referenceImage(const QString& directory) {
    QImage image(320, 160, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(QStringLiteral("#F1F5FA")));
    QPainter painter(&image);
    painter.fillRect(QRect(16, 18, 88, 124), QColor(QStringLiteral("#5374BD")));
    painter.fillRect(QRect(120, 24, 174, 14), QColor(QStringLiteral("#B9C9E4")));
    painter.fillRect(QRect(120, 52, 140, 10), QColor(QStringLiteral("#D1DCEB")));
    painter.fillRect(QRect(120, 74, 160, 10), QColor(QStringLiteral("#D1DCEB")));
    painter.setPen(QColor(QStringLiteral("#435675")));
    painter.drawText(QRect(120, 104, 180, 24), Qt::AlignLeft, QStringLiteral("LOCAL SAMPLE"));
    painter.end();
    const QString path = directory + QStringLiteral("/reference.png");
    return image.save(path) ? path : QString();
}

bool nativePopupShadowDisabled(QWidget* popup) {
#ifdef Q_OS_WIN
    if (QApplication::platformName() == QLatin1String("windows")) {
        const HWND handle = reinterpret_cast<HWND>(popup->winId());
        return IsWindow(handle) && !(GetClassLongPtrW(handle, GCL_STYLE) & CS_DROPSHADOW);
    }
#else
    Q_UNUSED(popup);
#endif
    return true;
}

bool savePopupPreview(QWidget* detail, QWidget* popup, const QString& path,
                      const QColor& background = QColor()) {
    const QRect detailBounds(detail->mapToGlobal(QPoint()), detail->size());
    QRect bounds = detailBounds;
    QRect popupBounds;
    if (popup) {
        popupBounds = QRect(popup->mapToGlobal(QPoint()), popup->size());
        bounds = bounds.united(popupBounds);
    }
    QImage image(bounds.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(background.isValid() ? background : detail->palette().window().color());
    QPainter painter(&image);
    painter.drawPixmap(detailBounds.topLeft() - bounds.topLeft(), detail->grab());
    if (popup) painter.drawPixmap(popupBounds.topLeft() - bounds.topLeft(), popup->grab());
    painter.end();
    return image.save(path);
}

// The detail menu may use exec() or popup(). Settle popup activation before
// closing it and dispatching its QAction, so delayed focus events cannot steal
// focus from the command's editor. Close on a missing action to avoid hangs.
bool triggerDetailCommand(TaskDetailWidget& detail, const QString& actionName) {
    auto* button = detail.findChild<QToolButton*>(QStringLiteral("DetailMore"));
    if (!button || !button->isVisible() || !button->isEnabled()) return false;
    bool settled = false, triggered = false;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, &poll, [&]() {
        auto* menu = detail.findChild<QMenu*>(QStringLiteral("DetailMoreMenu"));
        if (menu && menu->isVisible()) {
            auto* action = menu->findChild<QAction*>(actionName);
            settled = true;
            triggered = action && action->isVisible() && action->isEnabled();
            poll.stop();
            QApplication::processEvents();
            menu->close();
            if (triggered) action->trigger();
        } else if (elapsed.elapsed() > 1000) {
            settled = true;
            poll.stop();
            if (auto* active = qobject_cast<QMenu*>(QApplication::activePopupWidget())) active->close();
        }
    });
    poll.start();
    QTest::mouseClick(button, Qt::LeftButton);
    while (!settled && elapsed.elapsed() < 1200) QTest::qWait(10);
    return triggered;
}

class UpdateDialogFixtureServer : public QObject {
public:
    bool start(const QByteArray& manifest) {
        m_manifest = manifest;
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (m_server.hasPendingConnections()) {
                auto* socket = m_server.nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                    const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                    if (!request.contains("\r\n\r\n")) {
                        socket->setProperty("request", request);
                        return;
                    }
                    const QByteArray packet = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n"
                        + QByteArray("Content-Length: ") + QByteArray::number(m_manifest.size())
                        + "\r\nConnection: close\r\n\r\n" + m_manifest;
                    socket->write(packet);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        return m_server.listen(QHostAddress::LocalHost, 0);
    }
    QString baseUrl() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    }
private:
    QTcpServer m_server;
    QByteArray m_manifest;
};
} // namespace

class UiTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        // CI 把 stdout 接成管道时是整块缓冲，测试进程若异常退出，之前所有
        // 结果都会丢失，只剩一个空日志。先关掉缓冲，崩溃点才能定位。
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        AppStyle::apply(*qobject_cast<QApplication*>(qApp));
        disableNativeWindowShadows();
    }
    void init() {
        m_dir.reset(new QTemporaryDir);
        QVERIFY(m_dir->isValid());
        qputenv("KYLINLIST_DATA_DIR", m_dir->path().toUtf8());
        QSettings::setPath(
            QSettings::IniFormat, QSettings::UserScope, m_dir->path() + QStringLiteral("/settings"));
        for (const QString& key : {QStringLiteral("all"), QStringLiteral("today"),
                                   QStringLiteral("tomorrow"), QStringLiteral("next7days"),
                                   QStringLiteral("inbox"),
                                   QStringLiteral("completed"), QStringLiteral("abandoned"),
                                   QStringLiteral("trash")})
            Settings::instance().setSmartListVisibility(key, Settings::SmartListVisibility::Show);
        ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
        QString error;
        QVERIFY2(Database::instance().openAt(m_dir->path() + QStringLiteral("/kylinlist.db"), &error),
                 qPrintable(error));
        m_store.reset(new TaskStore(Database::instance().connection()));
        QVERIFY(m_store->load(&error));
    }
    void cleanup() {
        m_store.reset();
        Database::instance().close();
        m_dir.reset();
    }
    void iconSystemResolvesEverySemanticIcon() {
        // 每个语义图标都必须能落到滴答图标集里，并且真的画出东西。
        for (int i = static_cast<int>(IconId::None) + 1; i < static_cast<int>(IconId::Count); ++i) {
            const auto id = static_cast<IconId>(i);
            const QString name = AppIcons::iconName(id);
            QVERIFY2(!name.isEmpty(), qPrintable(QStringLiteral("没有映射：%1").arg(i)));
            QVERIFY2(AppIcons::hasIcon(name), qPrintable(name));
            const QPixmap pixmap = AppIcons::icon(id, 16).pixmap(16, 16);
            QVERIFY2(!pixmap.isNull(), qPrintable(name));
            const QImage image = pixmap.toImage();
            int ink = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (qAlpha(image.pixel(x, y)) > 40) ++ink;
            QVERIFY2(ink > 8, qPrintable(name));
        }
        // 滴答自家品牌标识按约定不接入。
        QVERIFY(!AppIcons::hasIcon(QStringLiteral("logo")));
        QVERIFY(!AppIcons::hasIcon(QStringLiteral("connect-logo")));
        // 字符串直通：不经过枚举也能点名取图标。
        QVERIFY(AppIcons::hasIcon(QStringLiteral("quick-today")));
        QVERIFY(AppIcons::icon(QStringLiteral("no-such-icon")).isNull());
        const struct { IconId id; const char* name; } expected[] = {
            {IconId::All, "all-list"}, {IconId::Inbox, "inbox-list"},
            {IconId::Tomorrow, "tomorrow-list"}, {IconId::Upcoming, "today-icon-su"},
            {IconId::Overdue, "custom-smart-list"}, {IconId::Done, "sidebar-completed-list"},
            {IconId::Abandoned, "wont-do"}, {IconId::Trash, "delete"},
            {IconId::ArchivedLists, "closed-list"}
        };
        for (const auto& entry : expected)
            QCOMPARE(AppIcons::iconName(entry.id), QString::fromLatin1(entry.name));
        QCOMPARE(AppIcons::iconName(IconId::Today),
                 QStringLiteral("today-icon-%1").arg(toAppTime(appNow()).date().day()));
        TaskList noteList; noteList.kind = EntityKind::Note;
        QCOMPARE(ListAppearance::iconName(noteList), QStringLiteral("note-list"));
    }
    void iconKeepsSourceAspectRatio() {
        // 3dots 的 viewBox 是 12.542x2.552，不能按正方形拉伸。
        const QImage image = AppIcons::icon(QStringLiteral("3dots"), 24).pixmap(24, 24).toImage();
        QVERIFY(!image.isNull());
        const auto rowHasInk = [&image](int y) {
            for (int x = 0; x < image.width(); ++x)
                if (qAlpha(image.pixel(x, y)) > 40) return true;
            return false;
        };
        QVERIFY(!rowHasInk(0));
        QVERIFY(!rowHasInk(image.height() - 1));
        QVERIFY(rowHasInk(image.height() / 2));
    }
    void iconPaintHonorsRequestedColor() {
        QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        AppIcons::paint(&painter, IconId::Add, QRectF(0, 0, 32, 32), QColor(Qt::red));
        painter.end();
        // 细描边在 16px 下会被抗锯齿摊平，这里只看色相：明显偏红且不是灰阶。
        bool red = false;
        for (int y = 0; y < image.height() && !red; ++y)
            for (int x = 0; x < image.width() && !red; ++x) {
                const QRgb pixel = image.pixel(x, y);
                red = qAlpha(pixel) > 100 && qRed(pixel) > qGreen(pixel) + 60 &&
                      qRed(pixel) > qBlue(pixel) + 60;
            }
        QVERIFY(red);
    }
    void updateDialogShowsAccumulatedNotesAndPostpone() {
        updates::InstallIdentity identity;
        identity.product = QStringLiteral("kylinlist");
        identity.platform = updates::currentBuildPlatform();
        identity.arch = updates::currentBuildArch();
        identity.variant = identity.platform == QStringLiteral("linux")
            ? QStringLiteral("system-qt") : QStringLiteral("setup");
        identity.version = QStringLiteral("1.1.0");
        const QString artifactPath = QStringLiteral("files/1.3.0/%1-%2-%3/update.pkg")
            .arg(identity.platform, identity.arch, identity.variant);
        const QByteArray manifest = QStringLiteral(R"JSON({
  "schemaVersion": 1, "channel": "stable", "version": "1.3.0",
  "publishedAt": "2026-11-14T08:00:00Z", "notes": "THIRD_RELEASE",
  "history": [
    {"version":"1.3.0","publishedAt":"2026-11-14T08:00:00Z","notes":"THIRD_RELEASE"},
    {"version":"1.1.0","publishedAt":"2026-11-12T08:00:00Z","notes":"OLD_RELEASE"},
    {"version":"1.2.0","publishedAt":"2026-11-13T08:00:00Z","notes":"SECOND_RELEASE"}
  ],
  "artifacts": [{"platform":"%1","arch":"%2","variant":"%3",
    "packageVersion":"1.3.0","path":"%4","size":4,
    "sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]
})JSON").arg(identity.platform, identity.arch, identity.variant, artifactPath).toUtf8();
        UpdateDialogFixtureServer server;
        QVERIFY(server.start(manifest));
        updates::UpdateService service(identity);
        service.setBaseUrl(server.baseUrl());
        service.checkNow();
        QTRY_VERIFY_WITH_TIMEOUT(service.state() == updates::UpdateService::State::UpdateAvailable, 5000);

        UpdateDialog dialog(&service);
        dialog.setAutomaticPrompt(true);
        dialog.show();
        auto* notes = dialog.findChild<QPlainTextEdit*>(QStringLiteral("UpdateNotes"));
        auto* postpone = dialog.findChild<QPushButton*>(QStringLiteral("UpdatePostpone"));
        auto* retry = dialog.findChild<QPushButton*>(QStringLiteral("UpdateRetryCheck"));
        auto* close = dialog.findChild<QPushButton*>(QStringLiteral("UpdateClose"));
        QVERIFY(notes && postpone && retry && close);
        const QString text = notes->toPlainText();
        QVERIFY(!text.contains(QStringLiteral("OLD_RELEASE")));
        QVERIFY(text.indexOf(QStringLiteral("SECOND_RELEASE")) >= 0);
        QVERIFY(text.indexOf(QStringLiteral("SECOND_RELEASE")) < text.indexOf(QStringLiteral("THIRD_RELEASE")));
        QVERIFY(postpone->isVisible());
        QVERIFY(!retry->isVisible());
        QVERIFY(!close->isVisible());
        QSignalSpy postponed(&dialog, &UpdateDialog::postponeRequested);
        QTest::mouseClick(postpone, Qt::LeftButton);
        QCOMPARE(postponed.count(), 1);
        QVERIFY(!dialog.isVisible());
    }
    void updateSettingsRestoreDefaultServer() {
        UpdateSettingsDialog dialog(QStringLiteral("http://127.0.0.1:9000"), true);
        dialog.show();
        auto* url = dialog.findChild<QLineEdit*>(QStringLiteral("UpdateServerUrl"));
        auto* restore = dialog.findChild<QPushButton*>(QStringLiteral("UpdateServerRestoreDefault"));
        QVERIFY(url && restore);
        QTest::mouseClick(restore, Qt::LeftButton);
        QCOMPARE(url->text(), Settings::defaultUpdateBaseUrl());
        dialog.reject();
    }
    void statsDialogShowsOnlyLocalSummary() {
        const QString taskId = m_store->createTask(QStringLiteral("仅用于本机统计的任务"));
        QVERIFY(!taskId.isEmpty());
        QVERIFY(!m_store->createNote(QStringLiteral("仅用于本机统计的笔记")).isEmpty());
        QVERIFY(m_store->setCompleted(taskId, true));
        StatsDialog dialog(m_store.get());
        dialog.show();
        QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("StatsLocalTasks"))->text(), QStringLiteral("1"));
        QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("StatsLocalNotes"))->text(), QStringLiteral("1"));
        QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("StatsLocalCompleted"))->text(), QStringLiteral("1"));
        QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("StatsLocalDays"))->text(), QStringLiteral("0 天"));
        QVERIFY(!dialog.findChild<QLabel*>(QStringLiteral("StatsGlobalUsers")));
        QVERIFY(!dialog.findChild<QTreeWidget*>(QStringLiteral("StatsDownloads")));
        QVERIFY(!dialog.findChild<QPushButton*>(QStringLiteral("StatsRefresh")));
        dialog.reject();
    }
    void markdownFormattingAndSafety() {
        const QString html = Markdown::toHtml(QStringLiteral(
            "## 标题\n\n**加粗** *斜体* ~~删除~~\n\n- [ ] 未完成\n- [x] 完成\n\n<script>bad()</script>"));
        QVERIFY(html.contains(QStringLiteral("<strong>加粗</strong>")));
        QVERIFY(html.contains(QStringLiteral("<em>斜体</em>")));
        QVERIFY(html.contains(QStringLiteral("<del>删除</del>")));
        QVERIFY(!html.contains(QStringLiteral("<script>")));
        QVERIFY(html.contains(QStringLiteral("&#9744;")));
        QVERIFY(html.contains(QStringLiteral("&#9745;")));
        MarkdownEditor editor;
        editor.setContent(QStringLiteral("hello"));
        editor.textEditor()->selectAll();
        auto* bold = editor.findChild<QToolButton*>(QStringLiteral("format0"));
        QVERIFY(bold);
        QTest::mouseClick(bold, Qt::LeftButton);
        QCOMPARE(editor.content(), QStringLiteral("**hello**"));
        editor.textEditor()->undo();
        QCOMPARE(editor.content(), QStringLiteral("hello"));
    }
    void markdownInlineExtensionsRender_data() {
        QTest::addColumn<QString>("color");
        QTest::newRow("underline") << QString();
        for (const QString& color : {QStringLiteral("yellow"), QStringLiteral("green"),
                                      QStringLiteral("blue"), QStringLiteral("pink"), QStringLiteral("purple")})
            QTest::newRow(qPrintable(color)) << color;
    }
    void markdownInlineExtensionsRender() {
        QFETCH(QString, color);
        const QString opening = color.isEmpty() ? QStringLiteral("<u>")
            : QStringLiteral("<mark data-color=\"%1\">").arg(color);
        const QString closing = color.isEmpty() ? QStringLiteral("</u>") : QStringLiteral("</mark>");
        const QString source = QStringLiteral("prefix ") + opening + QStringLiteral("formatted")
            + closing + QStringLiteral(" suffix");
        MarkdownEditor editor;
        editor.setContent(source);
        QCOMPARE(editor.content(), source);
        QCOMPARE(editor.plainTextContent(), QStringLiteral("prefix formatted suffix"));
        for (QTextDocument* document : {editor.textEditor()->document(), editor.preview()->document()}) {
            QCOMPARE(document->toPlainText(), QStringLiteral("prefix formatted suffix"));
            const QTextCursor styled = document->find(QStringLiteral("formatted"));
            QVERIFY(!styled.isNull());
            const QTextCharFormat format = styled.charFormat();
            if (color.isEmpty()) {
                QVERIFY(format.fontUnderline());
            } else {
                QVERIFY(format.background().style() != Qt::NoBrush);
                QCOMPARE(format.background().color(), Markdown::highlightColor(color));
            }
            const QTextCursor ordinary = document->find(QStringLiteral("prefix"));
            QVERIFY(!ordinary.isNull());
            QVERIFY(!ordinary.charFormat().fontUnderline());
            QVERIFY(ordinary.charFormat().background().style() == Qt::NoBrush);
        }
    }
    void markdownInlineExtensionsRejectOtherHtml_data() {
        QTest::addColumn<QString>("source");
        QTest::addColumn<QString>("literal");
        const QStringList unsafe{
            QStringLiteral("<script>alert(1)</script>"),
            QStringLiteral("<img src=\"https://example.com/image.png\" onerror=\"bad()\">"),
            QStringLiteral("<a href=\"javascript:bad()\">literal</a>"),
            QStringLiteral("<mark data-color=\"yellow\" onclick=\"bad()\">literal</mark>"),
            QStringLiteral("<mark data-color=\"orange\">literal</mark>"),
            QStringLiteral("<u style=\"color:red\" onclick=\"bad()\">literal</u>")};
        for (int index = 0; index < unsafe.size(); ++index)
            QTest::newRow(qPrintable(QStringLiteral("untrusted-%1").arg(index))) << unsafe.at(index) << unsafe.at(index);
        const QString code = QStringLiteral("<mark data-color=\"yellow\">literal</mark> <u>code</u>");
        QTest::newRow("inline-code") << (QLatin1Char('`') + code + QLatin1Char('`')) << code;
        QTest::newRow("fenced-code") << (QStringLiteral("```html\n") + code + QStringLiteral("\n```")) << code;
    }
    void markdownInlineExtensionsRejectOtherHtml() {
        QFETCH(QString, source);
        QFETCH(QString, literal);
        const QString html = Markdown::toHtml(source);
        QVERIFY(!html.contains(QStringLiteral("<script"), Qt::CaseInsensitive));
        QVERIFY(!html.contains(QStringLiteral("<img"), Qt::CaseInsensitive));
        QTextDocument preview;
        preview.setHtml(html);
        QCOMPARE(preview.toPlainText().trimmed(), literal);
        MarkdownEditor editor;
        editor.setContent(source);
        QCOMPARE(editor.plainTextContent().trimmed(), literal);
        QCOMPARE(editor.content(), source);
        for (QTextDocument* document : {&preview, editor.textEditor()->document()}) {
            for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
                for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
                    const QTextCharFormat format = it.fragment().charFormat();
                    QVERIFY(!format.isImageFormat());
                    QVERIFY(!format.isAnchor());
                    QVERIFY(!format.fontUnderline());
                }
            }
        }
    }
    void markdownNewFormatsPersistAndUndo_data() {
        markdownInlineExtensionsRender_data();
    }
    void markdownNewFormatsPersistAndUndo() {
        QFETCH(QString, color);
        const QString id = m_store->createTask(QStringLiteral("格式持久化与撤销"));
        Entity task = m_store->entity(id); task.body = QStringLiteral("hello"); QVERIFY(m_store->saveTask(task));
        const auto bodyEventCount = [this, &id]() {
            int count = 0;
            for (const EntityEvent& event : m_store->events(id)) if (event.field == QStringLiteral("body")) ++count;
            return count;
        };
        const int beforeEvents = bodyEventCount();
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(id));
        auto* editor = detail.findChild<MarkdownEditor*>(); QVERIFY(editor);
        auto* body = editor->textEditor(); detail.activateWindow(); body->setFocus(); QApplication::processEvents();
        QVERIFY(body->hasFocus()); body->selectAll();
        const QString expected = color.isEmpty() ? QStringLiteral("<u>hello</u>")
            : QStringLiteral("<mark data-color=\"%1\">hello</mark>").arg(color);
        if (color.isEmpty()) editor->applyFormat(MarkdownEditor::FormatAction::Underline);
        else editor->applyHighlight(color);
        QCOMPARE(editor->content(), expected); QVERIFY(detail.flush()); QCOMPARE(m_store->entity(id).body, expected);
        QCOMPARE(bodyEventCount(), beforeEvents); // Explicit save keeps the active editing session open.
        body->undo(); QCOMPARE(editor->content(), QStringLiteral("hello"));
        QVERIFY(detail.flush()); QCOMPARE(m_store->entity(id).body, QStringLiteral("hello"));
        body->redo(); QCOMPARE(editor->content(), expected);
        QVERIFY(detail.finishEditing()); QCOMPARE(m_store->entity(id).body, expected);
        QCOMPARE(bodyEventCount(), beforeEvents + 1);
        QVERIFY(detail.setTask(QString())); QVERIFY(detail.setTask(id));
        QCOMPARE(editor->content(), expected); QCOMPARE(editor->plainTextContent(), QStringLiteral("hello"));
        for (QTextDocument* document : {body->document(), editor->preview()->document()}) {
            QCOMPARE(document->toPlainText(), QStringLiteral("hello"));
            const QTextCursor rendered = document->find(QStringLiteral("hello")); QVERIFY(!rendered.isNull());
            if (color.isEmpty()) QVERIFY(rendered.charFormat().fontUnderline());
            else QCOMPARE(rendered.charFormat().background().color(), Markdown::highlightColor(color));
        }
        TaskStore reloaded(Database::instance().connection()); QVERIFY(reloaded.load());
        QCOMPARE(reloaded.entity(id).body, expected);
    }
    void markdownBlocksKeepIndependentFonts() {
        const QString source = QStringLiteral("## Heading\nPlain paragraph\n\n*italic*\nAnother paragraph\n\n```cpp\nint value;\n```\nLast paragraph");
        MarkdownEditor editor;
        editor.setContent(source);
        const QFont bodyFont = ds::qfont(ds::FontScene::Body);
        const auto verifyParagraphs = [&]() {
            for (const QString& text : {QStringLiteral("Plain paragraph"), QStringLiteral("Another paragraph"), QStringLiteral("Last paragraph")}) {
                const QTextCursor cursor = editor.textEditor()->document()->find(text);
                QVERIFY(!cursor.isNull());
                const QTextCharFormat format = cursor.charFormat();
                QCOMPARE(format.fontWeight(), int(QFont::Normal));
                QVERIFY(!format.fontItalic());
                const qreal size = format.fontPointSize() > 0 ? format.fontPointSize()
                    : editor.textEditor()->document()->defaultFont().pointSizeF();
                QCOMPARE(size, bodyFont.pointSizeF());
            }
        };
        verifyParagraphs();
        QCOMPARE(editor.content(), source);
        editor.setFlowLayout(true);
        editor.resize(360, 500);
        editor.show(); QApplication::processEvents();
        verifyParagraphs();
        QCOMPARE(editor.content(), source);
    }
    void markdownRenderedReselectionPreservesLiteralSource() {
        const QString literal = QStringLiteral("A&B <literal> \"quote\" ") + QString::fromUtf8("\xF0\x9F\x98\x80");
        const QString yellow = QStringLiteral("<mark data-color=\"yellow\">") + literal + QStringLiteral("</mark>");
        const QString blue = QStringLiteral("<mark data-color=\"blue\">") + literal + QStringLiteral("</mark>");
        const QString underlined = QStringLiteral("<mark data-color=\"blue\"><u>") + literal + QStringLiteral("</u></mark>");
        const QString id = m_store->createTask(QStringLiteral("格式切换保留字面内容"));
        Entity task = m_store->entity(id); task.body = literal; QVERIFY(m_store->saveTask(task));
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(id));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle")); QVERIFY(editor && title);
        auto* body = editor->textEditor(); detail.activateWindow(); body->setFocus(); QApplication::processEvents();
        QVERIFY(body->hasFocus()); body->selectAll(); editor->applyHighlight(QStringLiteral("yellow"));
        QCOMPARE(editor->content(), yellow);
        title->setFocus(); QApplication::processEvents(); QVERIFY(!body->hasFocus());
        QCOMPARE(editor->content(), yellow); QCOMPARE(body->toPlainText(), literal);
        QCOMPARE(editor->plainTextContent(), literal); QCOMPARE(editor->preview()->document()->toPlainText(), literal);

        // Select the displayed characters after rendering, rather than selecting source markup.
        QTextCursor selection = body->document()->find(literal); QVERIFY(!selection.isNull());
        QCOMPARE(selection.selectedText(), literal);
        QCOMPARE(selection.charFormat().background().color(), Markdown::highlightColor(QStringLiteral("yellow")));
        body->setTextCursor(selection); editor->applyHighlight(QStringLiteral("blue"));
        QCOMPARE(editor->content(), blue); // Recolor replaces the old mark, retaining every literal character.
        body->undo(); QCOMPARE(editor->content(), yellow);
        body->redo(); QCOMPARE(editor->content(), blue);
        title->setFocus(); QApplication::processEvents(); QVERIFY(!body->hasFocus());
        QCOMPARE(editor->content(), blue); QCOMPARE(body->toPlainText(), literal);
        QCOMPARE(editor->plainTextContent(), literal); QCOMPARE(editor->preview()->document()->toPlainText(), literal);

        selection = body->document()->find(literal); QVERIFY(!selection.isNull());
        QCOMPARE(selection.selectedText(), literal); body->setTextCursor(selection);
        editor->applyFormat(MarkdownEditor::FormatAction::Underline);
        QCOMPARE(editor->content(), underlined);
        body->undo(); QCOMPARE(editor->content(), blue);
        body->redo(); QCOMPARE(editor->content(), underlined);
        title->setFocus(); QApplication::processEvents(); QVERIFY(!body->hasFocus());
        QCOMPARE(editor->content(), underlined); QCOMPARE(body->toPlainText(), literal);
        QCOMPARE(editor->plainTextContent(), literal); QVERIFY(detail.finishEditing());
        QCOMPARE(m_store->entity(id).body, underlined);
        QVERIFY(detail.setTask(QString())); QVERIFY(detail.setTask(id));
        QCOMPARE(editor->content(), underlined); QCOMPARE(editor->plainTextContent(), literal);
        for (QTextDocument* document : {body->document(), editor->preview()->document()}) {
            QCOMPARE(document->toPlainText(), literal);
            const QTextCursor restored = document->find(literal); QVERIFY(!restored.isNull());
            QVERIFY(restored.charFormat().fontUnderline());
            QCOMPARE(restored.charFormat().background().color(), Markdown::highlightColor(QStringLiteral("blue")));
        }
        TaskStore reloaded(Database::instance().connection()); QVERIFY(reloaded.load());
        QCOMPARE(reloaded.entity(id).body, underlined);
    }
    void internalLinkParsingAndRendering() {
        const QString first = newUuid(), second = newUuid();
        const QString body = QStringLiteral("[一](kl:%1) [二](kl:%2) [重复](kl:%1) ![图片](kl:%2) "
                                            "[大写](kl:%3) [外链](https://example.com) 纯文本 kl:%1")
            .arg(first, second, first.toUpper());
        QCOMPARE(internalTargets(body), QStringList({first, second}));
        QVERIFY(isValidLinkTarget(first)); QVERIFY(isValidLinkTarget(QStringLiteral("kl:") + first));
        QVERIFY(!isValidLinkTarget(first.toUpper())); QVERIFY(!isValidLinkTarget(QStringLiteral("https://example.com")));
        QVERIFY(!isValidLinkTarget(QStringLiteral("kl:/") + first)); QVERIFY(!isValidLinkTarget(QStringLiteral("kl:bad")));

        const QString listA = m_store->createList(QStringLiteral("目标清单"));
        const QString listB = m_store->createList(QStringLiteral("来源清单"));
        const QString target = m_store->createTask(QStringLiteral("旧标题"), listA);
        const QString source = m_store->createNote(QStringLiteral("来源笔记"), listB);
        Entity sourceEntity = m_store->entity(source);
        sourceEntity.body = QStringLiteral("[存储文字](kl:%1)").arg(target); QVERIFY(m_store->saveTask(sourceEntity));
        MarkdownEditor editor; editor.setStore(m_store.get()); editor.setContent(sourceEntity.body);
        QVERIFY(editor.preview()->document()->toPlainText().contains(QStringLiteral("旧标题")));
        Entity renamed = m_store->entity(target); renamed.title = QStringLiteral("新标题"); QVERIFY(m_store->saveTask(renamed));
        const QString html = editor.preview()->toHtml();
        QVERIFY(html.contains(QStringLiteral("新标题"))); QVERIFY(!html.contains(QStringLiteral("旧标题")));
        QCOMPARE(m_store->entity(source).body, sourceEntity.body); // 预览刷新不改数据库原文
        QVERIFY(Markdown::toHtml(sourceEntity.body, Markdown::entityMap(m_store->entities())).contains(QStringLiteral("class=\"internal-link\"")));
        const QString external = Markdown::toHtml(QStringLiteral("[安全](https://example.com) <script>bad()</script>"));
        QVERIFY(external.contains(QStringLiteral("href=\"https://example.com\""))); QVERIFY(!external.contains(QStringLiteral("<script>")));
        const QString missing = newUuid();
        const QString broken = Markdown::toHtml(QStringLiteral("[保留文字](kl:%1)").arg(missing), Markdown::entityMap(m_store->entities()));
        QVERIFY(broken.contains(QStringLiteral("<span class=\"broken-link\">保留文字</span>")));
        QVERIFY(!broken.contains(QStringLiteral("href=\"kl:%1\"").arg(missing)));
        QVERIFY(m_store->trashTask(target));
        const QString trash = Markdown::toHtml(sourceEntity.body, Markdown::entityMap(m_store->entities()));
        QVERIFY(trash.contains(QStringLiteral("trash-link"))); QVERIFY(trash.contains(QStringLiteral("回收站")));
        QVERIFY(trash.contains(QStringLiteral("href=\"kl:%1\"").arg(target)));
        QVERIFY(m_store->deletePermanently(target));
        const QString deleted = Markdown::toHtml(sourceEntity.body, Markdown::entityMap(m_store->entities()));
        QVERIFY(deleted.contains(QStringLiteral("<span class=\"broken-link\">存储文字</span>")));
        QVERIFY(!deleted.contains(QStringLiteral("href=\"kl:%1\"").arg(target)));
        QCOMPARE(m_store->entity(source).body, sourceEntity.body);
    }
    void documentPreviewSignalsCrossListsAndHover() {
        const QString listA = m_store->createList(QStringLiteral("甲清单"));
        const QString listB = m_store->createList(QStringLiteral("乙清单"));
        const QString target = m_store->createTask(QStringLiteral("悬浮目标"), listA);
        const QString source = m_store->createNote(QStringLiteral("链接来源"), listB);
        Entity edited = m_store->entity(target); edited.body = QStringLiteral("摘要 **加粗**\n\n第二行");
        edited.dueAt = appNow().addDays(-1); QVERIFY(m_store->saveTask(edited));
        edited = m_store->entity(source); edited.body = QStringLiteral("[目标](kl:%1)").arg(target); QVERIFY(m_store->saveTask(edited));
        DocumentPreview preview; preview.setStore(m_store.get()); preview.resize(500, 240); preview.show();
        preview.setHtml(Markdown::toHtml(m_store->entity(source).body, Markdown::entityMap(m_store->entities())));
        const QTextCursor anchor = preview.document()->find(QStringLiteral("悬浮目标")); QVERIFY(!anchor.isNull());
        QCOMPARE(preview.internalLinkAt(preview.cursorRect(anchor).center()), target);
        QSignalSpy activated(&preview, &DocumentPreview::internalLinkActivated);
        QMetaObject::invokeMethod(&preview, "anchorClicked", Qt::DirectConnection, Q_ARG(QUrl, QUrl(QStringLiteral("kl:") + target)));
        QCOMPARE(activated.count(), 1); QCOMPARE(activated.takeFirst().at(0).toString(), target);
        QVERIFY(m_store->trashTask(target));
        preview.setHtml(Markdown::toHtml(m_store->entity(source).body, Markdown::entityMap(m_store->entities())));
        QMetaObject::invokeMethod(&preview, "anchorClicked", Qt::DirectConnection, Q_ARG(QUrl, QUrl(QStringLiteral("kl:") + target)));
        QCOMPARE(activated.count(), 1); // 回收站链接仍可激活
        QVERIFY(m_store->restoreTask(target));

        TaskPanel panel(m_store.get()); panel.showTask(source);
        auto* detail = panel.findChild<TaskDetailWidget*>(); QVERIFY(detail); QCOMPARE(detail->entityId(), source);
        auto* editor = detail->findChild<MarkdownEditor*>(); QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "internalLinkActivated", Qt::DirectConnection, Q_ARG(QString, target));
        QCOMPARE(detail->entityId(), target); // 跨清单通过同一信号跳转
        QCOMPARE(m_store->entity(source).body, edited.body);

        EntityHoverPreview* hover = preview.findChild<EntityHoverPreview*>(); QVERIFY(hover);
        QTimer* timer = hover->findChild<QTimer*>(QStringLiteral("HoverDelay")); QVERIFY(timer); QCOMPARE(timer->interval(), 500);
        hover->schedule(target, preview.mapToGlobal(QPoint(20, 20))); QVERIFY(!hover->isVisible());
        QTest::qWait(560); QVERIFY(hover->isVisible());
        QCOMPARE(hover->findChild<QLabel*>(QStringLiteral("HoverTitle"))->text(), QStringLiteral("悬浮目标"));
        QCOMPARE(hover->findChild<QLabel*>(QStringLiteral("HoverMeta"))->text(), QStringLiteral("任务 · 已过期"));
        const QString summary = hover->findChild<QLabel*>(QStringLiteral("HoverSummary"))->text();
        QVERIFY(summary.contains(QStringLiteral("摘要"))); QVERIFY(summary.contains(QStringLiteral("加粗")));
        QVERIFY(summary.contains(QStringLiteral("第二行"))); QVERIFY(!summary.contains(QStringLiteral("**")));
        QEvent leave(QEvent::Leave); QApplication::sendEvent(&preview, &leave); QVERIFY(!hover->isVisible());
        const QString note = m_store->createNote(QStringLiteral("笔记悬浮"));
        hover->schedule(note, preview.mapToGlobal(QPoint(20, 20))); QTest::qWait(560); QVERIFY(hover->isVisible());
        QCOMPARE(hover->findChild<QLabel*>(QStringLiteral("HoverMeta"))->text(), QStringLiteral("笔记"));
        hover->schedule(target, preview.mapToGlobal(QPoint(20, 20)));
        QVERIFY(m_store->trashTask(target)); QVERIFY(m_store->deletePermanently(target));
        QTest::qWait(560); QVERIFY(!hover->isVisible()); // 目标消失取消待显示卡片
    }
    void entityPickerSearchAndReferenceInsertion() {
        const QString task = m_store->createTask(QStringLiteral("目标 [A]"));
        const QString note = m_store->createNote(QStringLiteral("另一笔记"));
        const QString deleted = m_store->createTask(QStringLiteral("已删除选项")); QVERIFY(m_store->trashTask(deleted));
        EntityPickerDialog dialog(m_store.get());
        auto* list = dialog.findChild<QListWidget*>(QStringLiteral("EntityPickerList"));
        auto* search = dialog.findChild<QLineEdit*>(QStringLiteral("EntityPickerSearch"));
        QVERIFY(list && search); QCOMPARE(list->count(), 2);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("任务")) || list->item(0)->text().contains(QStringLiteral("笔记")));
        search->setText(QStringLiteral("另一")); QCOMPARE(list->count(), 1);
        QCOMPARE(dialog.selectedEntity().id, note); QCOMPARE(dialog.selectedEntity().kind, EntityKind::Note);
        search->setText(QStringLiteral("[A]")); QCOMPARE(list->count(), 1); QCOMPARE(dialog.selectedEntity().id, task);
        MarkdownEditor editor; editor.setStore(m_store.get());
        auto* button = editor.findChild<QToolButton*>(QStringLiteral("insertEntityReference")); QVERIFY(button); QVERIFY(button->isEnabled());
        QCOMPARE(button->text(), QStringLiteral("实体引用"));
        QVERIFY(editor.insertEntityReference(task));
        QCOMPARE(editor.content(), QStringLiteral("[目标 \\[A\\]](kl:%1)").arg(task));
        QVERIFY(editor.preview()->toHtml().contains(QStringLiteral("目标 [A]")));
        QVERIFY(!editor.insertEntityReference(newUuid()));
        QCOMPARE(editor.textEditor()->acceptRichText(), false);
    }
    void filtersUseShanghaiCalendarAndSearch() {
        const QDateTime now = fromAppTime(QDateTime(QDate(2026, 11, 6), QTime(12, 0), Qt::LocalTime));
        const QDate today = toAppTime(now).date();
        Entity task; task.id = newUuid(); task.title = QStringLiteral("整理材料"); task.listId = newUuid();
        task.startAt = now.addDays(-1); task.dueAt = now; task.tags << QStringLiteral("工作");
        ChecklistItem item; item.text = QStringLiteral("核对清单"); task.checklist.append(item);
        ViewSpec spec; spec.scope = ViewScope::Today;
        QVERIFY(contains(task, spec, today));
        spec.search = QStringLiteral("材料 工作 清单");
        QVERIFY(contains(task, spec, today));
        spec.search = QStringLiteral("材料 其他"); QVERIFY(!contains(task, spec, today));
        spec.search.clear(); task.status = TaskStatus::Completed; task.completed = true;
        QVERIFY(!contains(task, spec, today));
        spec.scope = ViewScope::Completed; QVERIFY(contains(task, spec, today));
        task.deleted = true; QVERIFY(!contains(task, spec, today));
        spec.scope = ViewScope::Trash; QVERIFY(contains(task, spec, today));
    }
    void filtersUseShanghaiDatesAcrossUtcBoundary() {
        const QDateTime now(QDate(2026, 11, 5), QTime(16, 30), Qt::UTC);
        QCOMPARE(toAppTime(now).date(), QDate(2026, 11, 6));
        Entity early; early.id = newUuid(); early.title = QStringLiteral("跨 UTC 日期的上海任务");
        early.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 6), QTime(0, 15), Qt::UTC));
        QCOMPARE(early.dueAt, QDateTime(QDate(2026, 11, 5), QTime(16, 15), Qt::UTC));
        Entity previous = early; previous.id = newUuid();
        previous.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 5), QTime(23, 45), Qt::LocalTime));
        ViewSpec spec; spec.scope = ViewScope::Today;
        QVERIFY(contains(early, spec, QDate(2026, 11, 6)));
        QVERIFY(contains(previous, spec, QDate(2026, 11, 6))); // 今天视图包含有效日期早于今天的任务
        previous.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 7), QTime(0, 0), Qt::LocalTime));
        QVERIFY(!contains(previous, spec, QDate(2026, 11, 6)));
        QCOMPARE(effectiveDate(early), QDate(2026, 11, 6));
        QCOMPARE(scheduleLabel(early), QStringLiteral("截止 11月6日 00:15"));
        early.allDayDate = QDate(2025, 1, 1); QCOMPARE(effectiveDate(early), QDate(2026, 11, 6));
        early.dueAt = QDateTime(); QCOMPARE(effectiveDate(early), QDate(2025, 1, 1));
        early.allDayDate = QDate(); early.startAt = now; QCOMPARE(effectiveDate(early), QDate(2026, 11, 6));
        spec.scope = ViewScope::Tomorrow;
        early.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 7), QTime(23, 59)));
        QVERIFY(contains(early, spec, QDate(2026, 11, 6)));
        early.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 8), QTime(0, 0)));
        QVERIFY(!contains(early, spec, QDate(2026, 11, 6)));
        spec.scope = ViewScope::Next7Days;
        early.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 12), QTime(23, 59)));
        QVERIFY(contains(early, spec, QDate(2026, 11, 6)));
        early.dueAt = fromAppTime(QDateTime(QDate(2026, 11, 13), QTime(0, 0)));
        QVERIFY(!contains(early, spec, QDate(2026, 11, 6)));
    }
    void viewSpecNotesStayOutOfTaskScopes() {
        const QDate today(2026, 11, 6);
        Entity note; note.id = newUuid(); note.kind = EntityKind::Note; note.title = QStringLiteral("保留状态的笔记");
        note.body = QStringLiteral("完整正文"); note.listId = newUuid(); note.dueAt = fromAppTime(QDateTime(today, QTime(9, 0)));
        note.startAt = fromAppTime(QDateTime(today.addDays(-2), QTime(9, 0))); note.priority = 3;
        note.tags << QStringLiteral("标签"); ChecklistItem item; item.text = QStringLiteral("清单内容"); note.checklist.append(item);
        for (const TaskStatus status : {TaskStatus::Completed, TaskStatus::Abandoned}) {
            note.status = status; note.completed = status == TaskStatus::Completed;
            ViewSpec all; QVERIFY(contains(note, all, today));
            ViewSpec list = all; list.scope = ViewScope::List; list.listId = note.listId; QVERIFY(contains(note, list, today));
            list.search = QStringLiteral("正文 标签 清单"); QVERIFY(contains(note, list, today));
            list.search = QStringLiteral("正文 缺失"); QVERIFY(!contains(note, list, today));
            for (const ViewScope scope : {ViewScope::Today, ViewScope::Tomorrow, ViewScope::Next7Days,
                                          ViewScope::Overdue, ViewScope::Completed, ViewScope::Abandoned}) {
                ViewSpec spec; spec.scope = scope; QVERIFY(!contains(note, spec, today));
                spec.renderer = ViewRenderer::Time; QVERIFY(!contains(note, spec, today));
            }
        }
        ViewSpec time; time.renderer = ViewRenderer::Time; QVERIFY(!contains(note, time, today));
        note.deleted = true; ViewSpec trash; trash.scope = ViewScope::Trash; QVERIFY(contains(note, trash, today));
    }
    void viewSpecSectionsOrderingAndArchives() {
        const QDate today(2026, 11, 6);
        const QDateTime base = fromAppTime(QDateTime(today, QTime(12, 0)));
        const auto entity = [&](const char* id, EntityKind kind = EntityKind::Task) {
            Entity value; value.id = QLatin1String(id); value.kind = kind; value.title = value.id; value.createdAt = base; return value;
        };
        Entity pinnedNote = entity("00000000-0000-4000-8000-000000000001", EntityKind::Note);
        Entity pinnedTask = entity("00000000-0000-4000-8000-000000000002");
        Entity futurePinned = entity("00000000-0000-4000-8000-000000000003");
        pinnedNote.status = TaskStatus::Completed; // 残留状态不影响普通清单
        pinnedNote.pinnedAt = base; pinnedTask.pinnedAt = base; futurePinned.pinnedAt = base.addSecs(60);
        pinnedTask.dueAt = base; futurePinned.dueAt = base.addDays(1);
        Entity overdueA = entity("00000000-0000-4000-8000-000000000010");
        Entity overdueB = entity("00000000-0000-4000-8000-000000000011");
        overdueA.dueAt = base.addDays(-1); overdueB.dueAt = overdueA.dueAt;
        Entity todayNine = entity("00000000-0000-4000-8000-000000000020");
        todayNine.dueAt = fromAppTime(QDateTime(today, QTime(9, 0))); // 当天 09:00 不按钟点过期
        Entity noDateA = entity("00000000-0000-4000-8000-000000000030");
        Entity noDateB = entity("00000000-0000-4000-8000-000000000031");
        Entity noteA = entity("00000000-0000-4000-8000-000000000040", EntityKind::Note);
        Entity noteB = entity("00000000-0000-4000-8000-000000000041", EntityKind::Note);
        const QVector<Entity> all{todayNine, noteB, overdueB, noDateA, pinnedTask, futurePinned,
                                  overdueA, pinnedNote, noDateB, noteA};
        ViewSpec list;
        const QVector<ViewSection> sections = buildSections(all, list, today);
        QCOMPARE(sections.size(), 5);
        QCOMPARE(sections.at(0).title, QStringLiteral("置顶")); QCOMPARE(sections.at(1).title, QStringLiteral("已过期"));
        QCOMPARE(sections.at(2).title, QStringLiteral("未过期")); QCOMPARE(sections.at(3).title, QStringLiteral("无日期"));
        QCOMPARE(sections.at(4).title, QStringLiteral("笔记"));
        QCOMPARE(sections.at(0).items.size(), 3);
        QCOMPARE(sections.at(0).items.at(0).id, futurePinned.id);
        QCOMPARE(sections.at(0).items.at(1).id, pinnedNote.id); QCOMPARE(sections.at(0).items.at(2).id, pinnedTask.id);
        QCOMPARE(sections.at(1).items.at(0).id, overdueA.id); QCOMPARE(sections.at(1).items.at(1).id, overdueB.id);
        QCOMPARE(sections.at(2).items.size(), 1); QCOMPARE(sections.at(2).items.first().id, todayNine.id);
        QCOMPARE(sections.at(3).items.at(0).id, noDateA.id); QCOMPARE(sections.at(3).items.at(1).id, noDateB.id);
        QCOMPARE(sections.at(4).items.at(0).id, noteA.id); QCOMPARE(sections.at(4).items.at(1).id, noteB.id);

        ViewSpec time; time.scope = ViewScope::Today; time.renderer = ViewRenderer::Time;
        QVERIFY(!contains(futurePinned, time, today)); // 置顶不豁免今天范围
        QVERIFY(!contains(noDateA, time, today)); QVERIFY(!contains(noteA, time, today));
        const QVector<ViewSection> timed = buildSections(all, time, today);
        QCOMPARE(timed.size(), 3); QCOMPARE(timed.at(0).title, QStringLiteral("置顶"));
        QCOMPARE(timed.at(0).items.size(), 1); QCOMPARE(timed.at(0).items.first().id, pinnedTask.id);
        QCOMPARE(timed.at(1).title, QStringLiteral("已过期")); QCOMPARE(timed.at(2).title, QStringLiteral("未过期"));
        QCOMPARE(timed.at(2).items.first().id, todayNine.id);
        time.scope = ViewScope::Next7Days; QVERIFY(contains(futurePinned, time, today));
        time.scope = ViewScope::Overdue; QVERIFY(!contains(todayNine, time, today));

        Entity completedA = entity("00000000-0000-4000-8000-000000000050");
        Entity completedB = entity("00000000-0000-4000-8000-000000000051");
        completedA.status = completedB.status = TaskStatus::Completed; completedA.completed = completedB.completed = true;
        completedA.completedAt = base; completedB.completedAt = base.addSecs(10); completedA.pinnedAt = base.addSecs(20);
        ViewSpec completed; completed.scope = ViewScope::Completed;
        const QVector<ViewSection> archive = buildSections({completedA, completedB, pinnedNote}, completed, today);
        QCOMPARE(archive.size(), 1); QCOMPARE(archive.first().title, QStringLiteral("已完成"));
        QCOMPARE(archive.first().items.size(), 2); QCOMPARE(archive.first().items.at(0).id, completedB.id);
        QCOMPARE(archive.first().items.at(1).id, completedA.id); // 归档不设置顶段
        completedB.completedAt = completedA.completedAt;
        const QVector<ViewSection> tied = buildSections({completedB, completedA}, completed, today);
        QCOMPARE(tied.first().items.at(0).id, completedA.id); QCOMPARE(tied.first().items.at(1).id, completedB.id);
        Entity abandoned = completedA; abandoned.status = TaskStatus::Abandoned; abandoned.completed = false; abandoned.abandonedAt = base;
        ViewSpec abandonedSpec; abandonedSpec.scope = ViewScope::Abandoned;
        QCOMPARE(buildSections({abandoned}, abandonedSpec, today).first().title, QStringLiteral("已放弃"));
        Entity trashedNote = noteA; trashedNote.deleted = true; trashedNote.deletedAt = base;
        Entity trashedTask = todayNine; trashedTask.deleted = true; trashedTask.deletedAt = base.addSecs(1);
        ViewSpec trash; trash.scope = ViewScope::Trash;
        const QVector<ViewSection> trashSections = buildSections({trashedNote, trashedTask}, trash, today);
        QCOMPARE(trashSections.size(), 1); QCOMPARE(trashSections.first().items.at(0).id, trashedTask.id);
    }
    void taskListModelSectionsAndNoteRoles() {
        const QString task = m_store->createTask(QStringLiteral("模型任务"));
        const QString note = m_store->createNote(QStringLiteral("模型笔记"));
        Entity edited = m_store->entity(task); edited.body = QStringLiteral("- [ ] 正文不是清单"); edited.dueAt = appNow().addDays(-1);
        QVERIFY(m_store->saveTask(edited));
        TaskListModel model(m_store.get()); QCOMPARE(model.entityCount(), 2);
        QVERIFY(model.rowCount() >= 3); // 段头不侵占实体 ID 映射
        bool sawHeader = false;
        for (int row = 0; row < model.rowCount(); ++row) {
            const QModelIndex index = model.index(row, 0);
            if (model.data(index, TaskListModel::HeaderRole).toBool()) {
                sawHeader = true; QVERIFY(model.entityId(index).isEmpty());
                QVERIFY(!(model.flags(index) & Qt::ItemIsSelectable));
            }
        }
        QVERIFY(sawHeader);
        const QModelIndex noteIndex = model.indexForTask(note); QVERIFY(noteIndex.isValid());
        QCOMPARE(model.entityId(noteIndex), note);
        QCOMPARE(model.data(noteIndex, TaskListModel::KindRole).toInt(), static_cast<int>(EntityKind::Note));
        QVERIFY(!(model.flags(noteIndex) & Qt::ItemIsUserCheckable));
        QVERIFY(!model.setData(noteIndex, Qt::Checked, Qt::CheckStateRole));
        QVERIFY(!model.data(noteIndex, TaskListModel::CompletedRole).toBool());
        QVERIFY(model.data(noteIndex, TaskListModel::MetaRole).toString().contains(QStringLiteral("笔记")));
        const QModelIndex taskIndex = model.indexForTask(task); QVERIFY(taskIndex.isValid());
        QVERIFY(model.data(taskIndex, TaskListModel::OverdueRole).toBool());
        QVERIFY(!model.data(taskIndex, TaskListModel::MetaRole).toString().contains(QStringLiteral("清单 0/")));
        QVERIFY(model.setData(taskIndex, Qt::Checked, Qt::CheckStateRole)); QVERIFY(m_store->entity(task).completed);
        QVERIFY(m_store->setPinnedAt(note, appNow()));
        QVERIFY(model.data(model.indexForTask(note), TaskListModel::PinnedRole).toBool());
    }
    void shanghai0030DetailInputIsHostTimeZoneIndependent() {
        const QString id = m_store->createTask(QStringLiteral("上海墙上时间"));
        QVERIFY(isUuid(id));
        TaskDetailWidget detail(m_store.get());
        QVERIFY(detail.setTask(id));
        auto* start = detail.findChild<QDateTimeEdit*>(QStringLiteral("start"));
        auto* reminder = detail.findChild<QDateTimeEdit*>(QStringLiteral("reminder"));
        auto* hasStart = detail.findChild<QCheckBox*>(QStringLiteral("startEnabled"));
        auto* hasReminder = detail.findChild<QCheckBox*>(QStringLiteral("reminderEnabled"));
        QVERIFY(start && reminder && hasStart && hasReminder);

        const QDateTime wall(QDate(2026, 11, 6), QTime(0, 30), Qt::LocalTime);
        hasStart->setChecked(true);
        start->setDateTime(wall);
        hasReminder->setChecked(true);
        reminder->setDateTime(QDateTime(QDate(2026, 11, 6), QTime(0, 15), Qt::LocalTime));
        QVERIFY(detail.flush());
        const QDateTime expectedStart(QDate(2026, 11, 5), QTime(16, 30), Qt::UTC);
        const QDateTime expectedReminder(QDate(2026, 11, 5), QTime(16, 15), Qt::UTC);
        QCOMPARE(m_store->entity(id).startAt, expectedStart);
        QCOMPARE(m_store->entity(id).reminderAt, expectedReminder);

        QVERIFY(detail.setTask(QString()));
        QVERIFY(detail.setTask(id));
        QCOMPARE(start->dateTime().date(), QDate(2026, 11, 6));
        QCOMPARE(start->dateTime().time(), QTime(0, 30));
        QCOMPARE(reminder->dateTime().date(), QDate(2026, 11, 6));
        QCOMPARE(reminder->dateTime().time(), QTime(0, 15));
    }
    void updateDraftProtection() {
        TaskPanel panel(m_store.get());
        panel.show();
        QString what;
        QVERIFY(!panel.hasUncommittedDrafts(&what));
        auto* quick = panel.findChild<QLineEdit*>(QStringLiteral("QuickAdd"));
        QVERIFY(quick);
        quick->setText(QStringLiteral("未提交的草稿"));
        QVERIFY(panel.hasUncommittedDrafts(&what));
        QVERIFY(what.contains(QStringLiteral("快速添加")));
        quick->clear();
        QVERIFY(!panel.hasUncommittedDrafts(&what));

        const QString id = m_store->createTask(QStringLiteral("草稿保护"));
        panel.showTask(id);
        auto* check = panel.findChild<QLineEdit*>(QStringLiteral("ChecklistInput"));
        QVERIFY(check);
        check->setText(QStringLiteral("新检查项"));
        QVERIFY(!panel.hasUncommittedDrafts(&what));  // 输入框隐藏：未进入添加检查项状态
        check->show();
        QVERIFY(panel.hasUncommittedDrafts(&what));
        QVERIFY(what.contains(QStringLiteral("检查项")));
        check->clear();
        check->hide();
        QVERIFY(!panel.hasUncommittedDrafts(&what));
        QVERIFY(panel.flush());
    }
    void workspaceUsesFourColumnsAndNoPermanentStatusBar() {
        TaskPanel panel(m_store.get());
        panel.resize(1240, 820);
        panel.show();
        auto* rail = panel.findChild<QFrame*>(QStringLiteral("ActivityRail"));
        auto* navigationPane = panel.findChild<QFrame*>(QStringLiteral("Sidebar"));
        auto* listPane = panel.findChild<QFrame*>(QStringLiteral("ListPane"));
        auto* detailPane = panel.findChild<QFrame*>(QStringLiteral("DetailFrame"));
        auto* controls = panel.findChild<WindowControls*>();
        QVERIFY(rail && navigationPane && listPane && detailPane && controls);
        QVERIFY(panel.windowFlags() & Qt::FramelessWindowHint);
        QVERIFY(panel.windowFlags() & Qt::NoDropShadowWindowHint);
        QVERIFY(panel.testAttribute(Qt::WA_TranslucentBackground));
        QVERIFY(panel.mask().isEmpty());
        auto* surface = panel.findChild<QWidget*>(QStringLiteral("WorkspaceRoot"));
        QVERIFY(surface && surface->mask().isEmpty());
        QCOMPARE(rectIn(rail, &panel).top(), 21);
        QCOMPARE(rectIn(navigationPane, &panel).top(), 21);
        QCOMPARE(rectIn(listPane, &panel).top(), 21);
        QVERIFY(!panel.findChild<QStatusBar*>());
        QVERIFY(!panel.findChild<QPushButton*>(QStringLiteral("QuickAddButton"))->isVisible());
        QVERIFY(!panel.findChild<QLineEdit*>(QStringLiteral("TaskSearch"))->isVisible());
        QCOMPARE(rail->width(), 52);
        QCOMPARE(rectIn(rail, &panel).left(), 21);
        QCOMPARE(rectIn(navigationPane, &panel).left(), rectIn(rail, &panel).right() + 1);
        QCOMPARE(rectIn(listPane, &panel).left(), rectIn(navigationPane, &panel).right() + 2);
        QVERIFY(detailPane->isVisible());
        QCOMPARE(rectIn(detailPane, &panel).left(), rectIn(listPane, &panel).right() + 2);
        QCOMPARE(rectIn(detailPane, &panel).right(), panel.rect().right() - 21);
        QCOMPARE(rectIn(controls, &panel).top(), 21);
        QCOMPARE(rectIn(controls, &panel).right(), panel.rect().right() - 21);

        const QString id = m_store->createTask(QStringLiteral("展开第四列"));
        panel.showTask(id);
        QTRY_VERIFY(detailPane->isVisible());
        QCOMPARE(rectIn(detailPane, &panel).left(), rectIn(listPane, &panel).right() + 2);
        QCOMPARE(rectIn(detailPane, &panel).right(), panel.rect().right() - 21);
        auto* info = detailPane->findChild<QToolButton*>(QStringLiteral("DetailInfo"));
        QVERIFY(info);
        QVERIFY(rectIn(info, &panel).top() >= rectIn(controls, &panel).bottom());
        QTest::mouseClick(detailPane->findChild<QToolButton*>(QStringLiteral("DetailClose")), Qt::LeftButton);
        QTRY_VERIFY(detailPane->isVisible());
        QVERIFY(panel.findChild<TaskDetailWidget*>()->entityId().isEmpty());
        QCOMPARE(rectIn(listPane, &panel).right(), rectIn(detailPane, &panel).left() - 2);
        panel.showTask(id);
        QTRY_VERIFY(detailPane->isVisible());
        QTest::mouseClick(panel.findChild<QToolButton*>(QStringLiteral("WindowMaximize")), Qt::LeftButton);
        QApplication::processEvents();
        QVERIFY(panel.windowState() & Qt::WindowMaximized);
        QVERIFY(panel.mask().isEmpty());
        QVERIFY(surface->mask().isEmpty());
        QTest::mouseClick(panel.findChild<QToolButton*>(QStringLiteral("WindowMaximize")), Qt::LeftButton);
        QApplication::processEvents();
        QVERIFY(!(panel.windowState() & Qt::WindowMaximized));
        QVERIFY(panel.mask().isEmpty());
        if (QGuiApplication::platformName() == QLatin1String("windows")) {
            auto* border = panel.findChild<QWidget*>(QStringLiteral("WindowBorderOverlay"));
            QVERIFY(border);
            QTRY_COMPARE(border->geometry(), QRect(21, 21, panel.width() - 42, panel.height() - 42));
            panel.showMinimized();
            QApplication::processEvents();
            panel.showNormal();
            QApplication::processEvents();
            QTRY_COMPARE(border->geometry(), QRect(21, 21, panel.width() - 42, panel.height() - 42));
            QCOMPARE(rectIn(listPane, &panel).top(), 21);
            panel.showMaximized();
            QApplication::processEvents();
            QCOMPARE(border->geometry(), QRect(0, 0, panel.width(), panel.height()));
            panel.showMinimized();
            QApplication::processEvents();
            panel.showNormal();
            QApplication::processEvents();
            QTRY_COMPARE(border->geometry(), QRect(21, 21, panel.width() - 42, panel.height() - 42));
        }
    }

    void nativeWindowResizeAndHitTesting() {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() != QLatin1String("windows"))
            QSKIP("需要原生 Windows 窗口");
        TaskPanel panel(m_store.get());
        panel.resize(1100, 720);
        panel.show();
        QApplication::processEvents();
        const HWND hwnd = reinterpret_cast<HWND>(panel.winId());
        QVERIFY((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) != 0);
        RECT frame;
        QVERIFY(GetWindowRect(hwnd, &frame));
        const qreal scale = panel.devicePixelRatioF();
        const int border = qRound(6 * scale);
        struct Probe { int x; int y; LRESULT expected; };
        const Probe probes[] = {
            {2, 100, HTLEFT}, {frame.right - frame.left - 2, 100, HTRIGHT},
            {100, 2, HTTOP}, {100, frame.bottom - frame.top - 2, HTBOTTOM},
            {2, 2, HTTOPLEFT}, {frame.right - frame.left - 2, 2, HTTOPRIGHT},
            {2, frame.bottom - frame.top - 2, HTBOTTOMLEFT},
            {frame.right - frame.left - 2, frame.bottom - frame.top - 2, HTBOTTOMRIGHT}
        };
        for (const Probe& probe : probes) {
            const LPARAM point = MAKELPARAM(frame.left + probe.x, frame.top + probe.y);
            QCOMPARE(SendMessage(hwnd, WM_NCHITTEST, 0, point), probe.expected);
        }
        const int inset = qRound(21 * scale);
        QCOMPARE(SendMessage(hwnd, WM_NCHITTEST, 0,
                             MAKELPARAM(frame.right - inset, frame.top + 200)), HTRIGHT);
        QCOMPARE(SendMessage(hwnd, WM_NCHITTEST, 0,
                             MAKELPARAM(frame.left + inset, frame.top + 200)), HTLEFT);
        QCOMPARE(SendMessage(hwnd, WM_NCHITTEST, 0,
                             MAKELPARAM(frame.left + 200, frame.bottom - inset)), HTBOTTOM);
        QVERIFY(border > 0);
#else
        QSKIP("Windows 原生窗口测试");
#endif
    }

    void nativeWindowDecorationRestore() {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() != QLatin1String("windows"))
            QSKIP("需要原生 Windows 窗口");
        TaskPanel panel(m_store.get());
        panel.resize(1100, 720);
        panel.show();
        QApplication::processEvents();
        auto* border = panel.findChild<QWidget*>(QStringLiteral("WindowBorderOverlay"));
        QVERIFY(border);
        const auto expected = [&panel]() {
            return QRect(21, 21, panel.width() - 42, panel.height() - 42);
        };
        QTRY_COMPARE(border->geometry(), expected());
        panel.showMinimized();
        QTest::qWait(80);
        panel.showNormal();
        QTRY_COMPARE(border->geometry(), expected());
        panel.showMaximized();
        QTRY_COMPARE(border->geometry(), QRect(0, 0, panel.width(), panel.height()));
        panel.showNormal();
        QTRY_COMPARE(border->geometry(), expected());
#else
        QSKIP("Windows 原生窗口测试");
#endif
    }

    void resizeAreasDragAllWindowEdges() {
        TaskPanel panel(m_store.get());
        panel.resize(1100, 720);
        panel.show();
        QApplication::processEvents();
        const struct { const char* name; Qt::CursorShape cursor; } areas[] = {
            {"WindowResizeLeft", Qt::SizeHorCursor}, {"WindowResizeRight", Qt::SizeHorCursor},
            {"WindowResizeTop", Qt::SizeVerCursor}, {"WindowResizeBottom", Qt::SizeVerCursor},
            {"WindowResizeTopLeft", Qt::SizeFDiagCursor}, {"WindowResizeTopRight", Qt::SizeBDiagCursor},
            {"WindowResizeBottomLeft", Qt::SizeBDiagCursor}, {"WindowResizeBottomRight", Qt::SizeFDiagCursor}
        };
        for (const auto& definition : areas) {
            auto* area = panel.findChild<QWidget*>(QString::fromLatin1(definition.name));
            QVERIFY(area && area->isVisible());
            QCOMPARE(area->cursor().shape(), definition.cursor);
        }
        const QRect start = panel.geometry();
        auto* right = panel.findChild<QWidget*>(QStringLiteral("WindowResizeRight"));
        QTest::mousePress(right, Qt::LeftButton, Qt::NoModifier, QPoint(6, 240));
        QMouseEvent move(QEvent::MouseMove, QPoint(-74, 240), right->mapToGlobal(QPoint(-74, 240)),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(right, &move);
        QTest::mouseRelease(right, Qt::LeftButton, Qt::NoModifier, QPoint(-74, 240));
        QCOMPARE(panel.width(), start.width() - 80);
        QCOMPARE(panel.height(), start.height());
        auto* bottomRight = panel.findChild<QWidget*>(QStringLiteral("WindowResizeBottomRight"));
        QTest::mousePress(bottomRight, Qt::LeftButton, Qt::NoModifier, QPoint(6, 6));
        QMouseEvent cornerMove(QEvent::MouseMove, QPoint(46, 66),
                               bottomRight->mapToGlobal(QPoint(46, 66)),
                               Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(bottomRight, &cornerMove);
        QTest::mouseRelease(bottomRight, Qt::LeftButton, Qt::NoModifier, QPoint(46, 66));
        QCOMPARE(panel.width(), start.width() - 40);
        QCOMPARE(panel.height(), start.height() + 60);
        panel.showMaximized();
        QApplication::processEvents();
        QVERIFY(!right->isVisible());
        panel.showNormal();
        QApplication::processEvents();
        QVERIFY(right->isVisible());
    }

    void detailRedocksAfterWindowEdgeDrag() {
        TaskPanel panel(m_store.get());
        panel.resize(1240, 820);
        panel.show();
        const QString id = m_store->createTask(QStringLiteral("覆盖退出后继续拖动详情"));
        panel.showTask(id);
        QApplication::processEvents();
        auto* detail = panel.findChild<QFrame*>(QStringLiteral("DetailFrame"));
        auto* splitter = panel.findChild<QSplitter*>(QStringLiteral("WorkspaceSplitter"));
        auto* windowResize = panel.findChild<QWidget*>(QStringLiteral("WindowResizeRight"));
        auto* detailResize = panel.findChild<QWidget*>(QStringLiteral("DetailResizeArea"));
        auto* overlayResize = panel.findChild<QWidget*>(QStringLiteral("OverlayDetailResizeArea"));
        auto* shadow = panel.findChild<QWidget*>(QStringLiteral("DetailOverlayShadow"));
        auto* editor = panel.findChild<TaskDetailWidget*>();
        QVERIFY(detail && splitter && windowResize && detailResize && overlayResize && shadow && editor);
        const auto drag = [](QWidget* area, const QList<int>& offsets) {
            const QPoint local(area->width() / 2, 200);
            const QPoint pressGlobal = area->mapToGlobal(local);
            QTest::mousePress(area, Qt::LeftButton, Qt::NoModifier, local);
            for (int offset : offsets) {
                const QPoint global = pressGlobal + QPoint(offset, 0);
                QMouseEvent move(QEvent::MouseMove, area->mapFromGlobal(global), global,
                                 Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(area, &move);
                QApplication::processEvents();
            }
            QTest::mouseRelease(area, Qt::LeftButton, Qt::NoModifier,
                                area->mapFromGlobal(pressGlobal + QPoint(offsets.last(), 0)));
            QApplication::processEvents();
        };
        for (int cycle = 0; cycle < 2; ++cycle) {
            drag(windowResize, {-80, -180, -280, -340});
            QCOMPARE(panel.width(), 900);
            QVERIFY(detail->property("overlay").toBool());
            QVERIFY(overlayResize->isVisible());
            QVERIFY(shadow->isVisible());
            QCOMPARE(editor->entityId(), id);
            if (cycle == 1) {
                drag(overlayResize, {-40, -80});
                QCOMPARE(detail->width(), 380);
            }
            drag(windowResize, {40, 100, 180, 260, 340});
            QCOMPARE(panel.width(), 1240);
            QVERIFY(!panel.isMaximized());
            QCOMPARE(detail->parentWidget(), splitter);
            QCOMPARE(splitter->indexOf(detail), 2);
            QCOMPARE(rectIn(detail, &panel).right(), panel.rect().right() - 21);
            QVERIFY(detailResize->isVisible());
            QVERIFY(!overlayResize->isVisible());
            QVERIFY(!shadow->isVisible());
            const QString screenshots = QString::fromLocal8Bit(qgetenv("KYLINLIST_SCREENSHOT_DIR"));
            if (!screenshots.isEmpty()) {
                QVERIFY(QDir().mkpath(screenshots));
                QVERIFY(panel.grab().save(screenshots + QStringLiteral("/detail-redocked-%1.png").arg(cycle)));
            }
            QVERIFY2(detail->mask().isEmpty(), "Docked detail retains the overlay clipping mask");
            const int detailWidth = detail->width();
            const int detailLeft = rectIn(detail, &panel).left();
            drag(detailResize, {-40, -80});
            QCOMPARE(detail->width(), detailWidth + 80);
            QCOMPARE(rectIn(detail, &panel).left(), detailLeft - 80);
            QCOMPARE(rectIn(detail, &panel).right(), panel.rect().right() - 21);
            QVERIFY(detail->mask().isEmpty());
            QVERIFY(detail->visibleRegion().contains(QPoint(detail->width() - 24, detail->height() / 2)));
            drag(detailResize, {40, 80});
            QCOMPARE(detail->width(), detailWidth);
            QCOMPARE(rectIn(detail, &panel).left(), detailLeft);
            QCOMPARE(editor->entityId(), id);
        }
    }

    void responsiveColumnsAndOverlayDetail() {
        TaskPanel panel(m_store.get());
        panel.resize(1240, 820);
        panel.show();
        auto* rail = panel.findChild<QFrame*>(QStringLiteral("ActivityRail"));
        auto* sidebar = panel.findChild<QFrame*>(QStringLiteral("Sidebar"));
        auto* list = panel.findChild<QFrame*>(QStringLiteral("ListPane"));
        auto* detail = panel.findChild<QFrame*>(QStringLiteral("DetailFrame"));
        auto* splitter = panel.findChild<QSplitter*>(QStringLiteral("WorkspaceSplitter"));
        auto* toggle = panel.findChild<QToolButton*>(QStringLiteral("SidebarToggle"));
        QVERIFY(rail && sidebar && list && detail && splitter && toggle);
        QCOMPARE(splitter->count(), 3);
        QCOMPARE(splitter->handleWidth(), 1);
        QVERIFY(splitter->handle(1) && splitter->handle(2));
        const int detailBeforeDrag = detail->width();
        auto* detailResize = panel.findChild<QWidget*>(QStringLiteral("DetailResizeArea"));
        QVERIFY(detailResize && detailResize->isVisible());
        QCOMPARE(detailResize->cursor().shape(), Qt::SplitHCursor);
        const QPoint handleBeforeDrag = rectIn(splitter->handle(2), &panel).topLeft();
        QTest::mousePress(detailResize, Qt::LeftButton, Qt::NoModifier, QPoint(6, 200));
        QMouseEvent dragDetail(QEvent::MouseMove, QPoint(-74, 200),
                               detailResize->mapToGlobal(QPoint(-74, 200)),
                               Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(detailResize, &dragDetail);
        QTest::mouseRelease(detailResize, Qt::LeftButton, Qt::NoModifier, QPoint(-74, 200));
        QApplication::processEvents();
        QCOMPARE(detail->width(), detailBeforeDrag + 80);
        QCOMPARE(rectIn(splitter->handle(2), &panel).left(), handleBeforeDrag.x() - 80);
        QCOMPARE(rail->width(), 52);
        QCOMPARE(panel.minimumSize(), QSize(620, 560));
        splitter->setSizes({280, 500, 370});
        QApplication::processEvents();
        QCOMPARE(sidebar->width(), 280);
        QCOMPARE(detail->width(), 370);
        QCOMPARE(rail->width(), 52);

        panel.resize(900, 700);
        QApplication::processEvents();
        QVERIFY(sidebar->isVisible());
        QVERIFY(!detail->isVisible());
        QVERIFY(list->isVisible());
        const QRect listBeforeOverlay = rectIn(list, &panel);
        const QString id = m_store->createTask(QStringLiteral("窄窗覆盖详情"));
        panel.showTask(id);
        QTRY_VERIFY(detail->isVisible());
        QCOMPARE(detail->width(), 300);
        QCOMPARE(detail->parentWidget(), panel.findChild<QWidget*>(QStringLiteral("WorkspaceRoot")));
        auto* overlayShadow = panel.findChild<QWidget*>(QStringLiteral("DetailOverlayShadow"));
        QVERIFY(overlayShadow && overlayShadow->isVisible());
        QCOMPARE(overlayShadow->width(), 18);
        QCOMPARE(rectIn(overlayShadow, &panel).right(), rectIn(detail, &panel).left() - 1);
        QVERIFY(!detailResize->isVisible());
        auto* overlayResize = panel.findChild<QWidget*>(QStringLiteral("OverlayDetailResizeArea"));
        QVERIFY(overlayResize && overlayResize->isVisible());
        QCOMPARE(overlayResize->cursor().shape(), Qt::SplitHCursor);
        const int overlayLeft = rectIn(detail, &panel).left();
        QTest::mousePress(overlayResize, Qt::LeftButton, Qt::NoModifier, QPoint(6, 200));
        const QPoint overlayPressGlobal = overlayResize->mapToGlobal(QPoint(6, 200));
        for (const int delta : {-40, -80}) {
            const QPoint global = overlayPressGlobal + QPoint(delta, 0);
            QMouseEvent dragOverlay(QEvent::MouseMove, overlayResize->mapFromGlobal(global), global,
                                    Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(overlayResize, &dragOverlay);
        }
        QTest::mouseRelease(overlayResize, Qt::LeftButton, Qt::NoModifier,
                            overlayResize->mapFromGlobal(overlayPressGlobal + QPoint(-80, 0)));
        QCOMPARE(detail->width(), 380);
        QCOMPARE(rectIn(detail, &panel).left(), overlayLeft - 80);
        QCOMPARE(rectIn(list, &panel), listBeforeOverlay);
        QCOMPARE(rectIn(overlayShadow, &panel).right(), rectIn(detail, &panel).left() - 1);
        QTest::mousePress(overlayResize, Qt::LeftButton, Qt::NoModifier, QPoint(6, 200));
        const QPoint restorePressGlobal = overlayResize->mapToGlobal(QPoint(6, 200));
        for (const int delta : {40, 80}) {
            const QPoint global = restorePressGlobal + QPoint(delta, 0);
            QMouseEvent restoreOverlay(QEvent::MouseMove, overlayResize->mapFromGlobal(global), global,
                                       Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(overlayResize, &restoreOverlay);
        }
        QTest::mouseRelease(overlayResize, Qt::LeftButton, Qt::NoModifier,
                            overlayResize->mapFromGlobal(restorePressGlobal + QPoint(80, 0)));
        QCOMPARE(detail->width(), 300);
        QCOMPARE(rectIn(list, &panel), listBeforeOverlay);
        QCOMPARE(rectIn(detail, &panel).right(), panel.rect().right() - 21);
        QVERIFY(rectIn(detail, &panel).left() > rectIn(list, &panel).left());
        auto* close = detail->findChild<QToolButton*>(QStringLiteral("DetailClose"));
        auto* reminder = detail->findChild<QToolButton*>(QStringLiteral("DetailReminder"));
        QVERIFY(close && reminder);
        QVERIFY(rectIn(close, &panel).left() >= rectIn(reminder, &panel).right());
        QTest::mouseClick(close, Qt::LeftButton);
        QTRY_VERIFY(!detail->isVisible());
        QVERIFY(!overlayShadow->isVisible());
        QVERIFY(!overlayResize->isVisible());
        QCOMPARE(rectIn(list, &panel), listBeforeOverlay);

        panel.resize(700, 620);
        QApplication::processEvents();
        QVERIFY(!sidebar->isVisible());
        QVERIFY(!detail->isVisible());
        QVERIFY(list->isVisible());
        QCOMPARE(rail->width(), 52);
        panel.resize(900, 700);
        QApplication::processEvents();
        QVERIFY(sidebar->isVisible());
        QVERIFY(!detail->isVisible());
        panel.resize(1240, 820);
        QApplication::processEvents();
        QVERIFY(sidebar->isVisible());
        QVERIFY(detail->isVisible());
        QCOMPARE(detail->parentWidget(), splitter);
        QCOMPARE(splitter->indexOf(detail), 2);
        QVERIFY(detail->mask().isEmpty());
        QCOMPARE(rectIn(detail, &panel).right(), panel.rect().right() - 21);
        QVERIFY(detailResize->isVisible());
        QVERIFY(!overlayResize->isVisible());
        QCOMPARE(detail->width(), 370);

        QTest::mouseClick(toggle, Qt::LeftButton);
        QApplication::processEvents();
        QVERIFY(!sidebar->isVisible());
        panel.resize(700, 620);
        panel.resize(1240, 820);
        QApplication::processEvents();
        QVERIFY(!sidebar->isVisible());
        QTest::mouseClick(toggle, Qt::LeftButton);
        QApplication::processEvents();
        QVERIFY(sidebar->isVisible());
    }

    void toastIslandSupportsUndoAndHoverLifetime() {
        ToastIsland toast;
        toast.showMessage(QStringLiteral("普通提示"), 120);
        QVERIFY(toast.isVisible());
        QVERIFY(!toast.findChild<QToolButton*>(QStringLiteral("ToastAction"))->isVisible());
        QTest::qWait(180);
        QVERIFY(!toast.isVisible());

        int calls = 0;
        toast.showAction(QStringLiteral("任务已完成"), QStringLiteral("撤销"), [&calls]() {
            ++calls;
            return true;
        }, 180);
        auto* action = toast.findChild<QToolButton*>(QStringLiteral("ToastAction"));
        QVERIFY(action && action->isVisible());
        QTest::qWait(60);
        QEvent enter(QEvent::Enter);
        QApplication::sendEvent(&toast, &enter);
        const int paused = toast.remainingTime();
        QTest::qWait(230);
        QVERIFY(toast.isVisible());
        QVERIFY(toast.remainingTime() <= paused);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&toast, &leave);
        QTest::qWait(70);
        QVERIFY(toast.isVisible());
        QTest::mouseClick(action, Qt::LeftButton);
        QCOMPARE(calls, 1);
        QVERIFY(!toast.isVisible());
        toast.showMessage(QStringLiteral("再次提示"), 100);
        QTest::qWait(150);
        QVERIFY(!toast.isVisible());
    }

    void taskStatusToastRestoresPreviousState() {
        const QString id = m_store->createTask(QStringLiteral("可撤销的任务状态"));
        TaskPanel panel(m_store.get());
        panel.showTask(id);
        auto* toast = panel.findChild<ToastIsland*>(QStringLiteral("WorkspaceToast"));
        auto* listPane = panel.findChild<QFrame*>(QStringLiteral("ListPane"));
        QVERIFY(toast && listPane);
        QVERIFY(m_store->setCompleted(id, true));
        QTRY_VERIFY(toast->isVisible());
        QCOMPARE(toast->findChild<QLabel*>(QStringLiteral("ToastText"))->text(), QStringLiteral("任务已完成"));
        QVERIFY(toast->parentWidget() == listPane);
        QVERIFY(rectIn(toast, listPane).bottom() < listPane->rect().bottom());
        QTest::mouseClick(toast->findChild<QToolButton*>(QStringLiteral("ToastAction")), Qt::LeftButton);
        QTRY_COMPARE(m_store->entity(id).status, TaskStatus::Active);
        QVERIFY(!m_store->entity(id).completed);
        QVERIFY(m_store->abandonTask(id));
        QTRY_VERIFY(toast->isVisible());
        QCOMPARE(toast->findChild<QLabel*>(QStringLiteral("ToastText"))->text(), QStringLiteral("任务已放弃"));
        QTest::mouseClick(toast->findChild<QToolButton*>(QStringLiteral("ToastAction")), Qt::LeftButton);
        QTRY_COMPARE(m_store->entity(id).status, TaskStatus::Active);
    }

    void quickAddAndDetailPersistWithoutLosingPresentation() {
        TaskPanel panel(m_store.get());
        panel.show();
        auto* quick = panel.findChild<QLineEdit*>(QStringLiteral("QuickAdd"));
        QVERIFY(quick);
        quick->setText(QStringLiteral("明天提交材料"));
        QTest::keyClick(quick, Qt::Key_Return);
        QCOMPARE(m_store->entities().size(), 1);
        const QString id = m_store->entities().first().id;
        QVERIFY(isUuid(id));
        auto* detail = panel.findChild<TaskDetailWidget*>();
        QVERIFY(detail);
        QCOMPARE(detail->entityId(), id);
        auto* description = detail->findChild<QTextEdit*>(QStringLiteral("DescriptionEditor"));
        description->setPlainText(QStringLiteral("**核对附件**"));
        Entity external = m_store->entity(id);
        external.geometry = QRect(45, 60, 340, 360);
        external.color = NoteColor::Blue;
        QVERIFY(m_store->saveTask(external));
        QVERIFY(detail->flush());
        QCOMPARE(m_store->entity(id).body, QStringLiteral("**核对附件**"));
        QCOMPARE(m_store->entity(id).geometry, external.geometry);
        QCOMPARE(m_store->entity(id).color, NoteColor::Blue);
        auto *start = detail->findChild<QDateTimeEdit*>(QStringLiteral("start")),
             *due = detail->findChild<QDateTimeEdit*>(QStringLiteral("due"));
        auto *hasStart = detail->findChild<QCheckBox*>(QStringLiteral("startEnabled")),
             *hasDue = detail->findChild<QCheckBox*>(QStringLiteral("dueEnabled"));
        hasStart->setChecked(true);
        hasDue->setChecked(true);
        start->setDateTime(QDateTime(QDate(2026, 11, 6), QTime(10, 0)));
        due->setDateTime(start->dateTime().addSecs(3600));
        QVERIFY(detail->flush());
        due->setDateTime(start->dateTime().addSecs(-3600));
        QVERIFY(!detail->flush());
        QCOMPARE(m_store->entity(id).dueAt, fromAppTime(start->dateTime().addSecs(3600)));
        due->setDateTime(start->dateTime().addSecs(7200));
        QVERIFY(detail->flush());
        auto* list = panel.findChild<QListView*>(QStringLiteral("TaskList"));
        QVERIFY(list);
        auto* model = qobject_cast<TaskListModel*>(list->model());
        QVERIFY(model);
        const auto index = model->indexForTask(id);
        QVERIFY(index.isValid());
        QCOMPARE(model->data(index, TaskListModel::IdRole).toString(), id);
        QCOMPARE(model->data(index, TaskListModel::IdRole).type(), QVariant::String);
        QVERIFY(model->setData(index, Qt::Checked, Qt::CheckStateRole));
        QVERIFY(m_store->entity(id).completed);
    }
    void organizationNavigationAndQuickAddKind() {
        const QString folder = m_store->createFolder(QStringLiteral("单层文件夹"));
        const QString noteList = m_store->createList(QStringLiteral("笔记清单"), folder, EntityKind::Note);
        TaskPanel panel(m_store.get()); panel.show();
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        auto* quick = panel.findChild<QLineEdit*>(QStringLiteral("QuickAdd"));
        auto* renderer = panel.findChild<QComboBox*>(QStringLiteral("ViewRenderer"));
        QVERIFY(navigation && quick && renderer);
        int noteRow = -1, todayRow = -1, allRow = -1;
        for (int row = 0; row < navigation->count(); ++row) {
            const QListWidgetItem* item = navigation->item(row);
            if (item->data(Qt::UserRole + 1).toString() == noteList) noteRow = row;
            if (item->data(Qt::UserRole).toInt() == static_cast<int>(ViewScope::Today)) todayRow = row;
            if (!item->data(NavigationDelegate::SectionRole).toBool()
                && item->data(Qt::UserRole).toInt() == static_cast<int>(ViewScope::AllLists)) allRow = row;
        }
        QVERIFY(noteRow >= 0 && todayRow >= 0 && allRow >= 0);
        navigation->setCurrentRow(noteRow);
        QVERIFY(navigation->currentItem());
        QCOMPARE(navigation->currentItem()->data(Qt::UserRole + 1).toString(), noteList);
        auto* model = qobject_cast<TaskListModel*>(panel.findChild<QListView*>(QStringLiteral("TaskList"))->model());
        QCOMPARE(model->filter().scope, ViewScope::List);
        QCOMPARE(model->filter().listId, noteList);
        QVERIFY(quick->placeholderText().contains(QStringLiteral("笔记")));
        QVERIFY(quick->placeholderText().contains(QStringLiteral("笔记清单")));
        QVERIFY(!renderer->isEnabled());
        quick->setText(QStringLiteral("默认新建笔记")); QTest::keyClick(quick, Qt::Key_Return);
        QString noteId;
        for (const Entity& entity : m_store->entities()) if (entity.title == QStringLiteral("默认新建笔记")) noteId = entity.id;
        QVERIFY(isUuid(noteId)); QCOMPARE(m_store->entity(noteId).kind, EntityKind::Note);
        QCOMPARE(m_store->entity(noteId).listId, noteList);
        auto* detail = panel.findChild<TaskDetailWidget*>(); QVERIFY(detail);
        QCOMPARE(detail->entityId(), noteId);
        QVERIFY(detail->findChild<QCheckBox*>(QStringLiteral("TaskCompleted"))->isHidden());
        QVERIFY(detail->findChild<QCheckBox*>(QStringLiteral("TaskPinned"))->isHidden());
        // g02 对齐后置顶不再是常驻 checkbox，而是收进详情「更多」菜单的命令；
        // 兼容控件保留 objectName 但隐藏，业务由菜单 QAction 触发。
        QVERIFY(detail->findChild<QCheckBox*>(QStringLiteral("TaskPinnedAt"))->isHidden());
        QVERIFY(detail->findChild<QWidget*>(QStringLiteral("TaskScheduleHost"))->isHidden());
        QVERIFY(detail->findChild<QWidget*>(QStringLiteral("TaskPriorityHost"))->isHidden());
        QVERIFY(detail->findChild<QWidget*>(QStringLiteral("TaskAppearanceHost"))->isHidden());
        QCOMPARE(detail->findChild<QPushButton*>(QStringLiteral("ConvertKind"))->text(), QStringLiteral("转为任务"));
        navigation->setCurrentRow(todayRow); QVERIFY(renderer->isEnabled());
        QVERIFY(quick->placeholderText().contains(QStringLiteral("收集箱")));
        renderer->setCurrentIndex(static_cast<int>(ViewRenderer::Time));
        QCOMPARE(model->filter().renderer, ViewRenderer::Time);
        navigation->setCurrentRow(allRow);
        QVERIFY(!renderer->isEnabled());
        QCOMPARE(model->filter().renderer, ViewRenderer::List);
        QVERIFY(m_store->setListFolder(noteList, QString()));
        QVERIFY(m_store->setListFolder(noteList, folder)); QVERIFY(m_store->deleteFolder(folder));
        for (const TaskList& list : m_store->lists()) if (list.id == noteList) QVERIFY(list.folderId.isEmpty());
        bool listStillVisible = false;
        for (int row = 0; row < navigation->count(); ++row)
            if (navigation->item(row)->data(Qt::UserRole + 1).toString() == noteList) listStillVisible = true;
        QVERIFY(listStillVisible);
    }
    void folderViewAggregatesAndQuickAdds() {
        const QString folder = m_store->createFolder(QStringLiteral("项目文件夹"), QStringLiteral("🏛️"));
        const QString tasks = m_store->createList(QStringLiteral("任务组"), folder, EntityKind::Task,
                                                  QStringLiteral("🛠"));
        const QString notes = m_store->createList(QStringLiteral("记录组"), folder, EntityKind::Note);
        const QString outside = m_store->createList(QStringLiteral("文件夹外"), QString(), EntityKind::Task);
        const QString pinned = m_store->createTask(QStringLiteral("跨清单置顶"), tasks);
        const QString ordinary = m_store->createTask(QStringLiteral("普通任务"), tasks);
        const QString note = m_store->createNote(QStringLiteral("文件夹笔记"), notes);
        const QString excluded = m_store->createTask(QStringLiteral("不应出现"), outside);
        QVERIFY(m_store->setPinnedAt(pinned, appNow()));

        TaskPanel panel(m_store.get()); panel.show();
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        auto* model = qobject_cast<TaskListModel*>(
            panel.findChild<QListView*>(QStringLiteral("TaskList"))->model());
        auto* quick = panel.findChild<QLineEdit*>(QStringLiteral("QuickAdd"));
        QVERIFY(navigation && model && quick);
        const auto folderRow = [navigation](const QString& id) {
            for (int row = 0; row < navigation->count(); ++row)
                if (navigation->item(row)->data(NavigationDelegate::FolderIdRole).toString() == id)
                    return row;
            return -1;
        };
        int row = folderRow(folder);
        QVERIFY(row >= 0);
        QCOMPARE(navigation->item(row)->data(NavigationDelegate::IconNameRole).toString(),
                 QStringLiteral("🏛️"));
        navigation->setCurrentRow(row);
        QTRY_COMPARE(model->filter().scope, ViewScope::Folder);
        QTRY_COMPARE(model->filter().folderId, folder);
        QVERIFY(model->indexForTask(pinned).isValid());
        QVERIFY(model->indexForTask(ordinary).isValid());
        QVERIFY(model->indexForTask(note).isValid());
        QVERIFY(!model->indexForTask(excluded).isValid());
        QStringList sectionKeys;
        for (int index = 0; index < model->rowCount(); ++index) {
            if (!model->data(model->index(index, 0), TaskListModel::HeaderRole).toBool()) continue;
            const QString key = model->data(model->index(index, 0), TaskListModel::SectionKeyRole).toString();
            if (!key.isEmpty()) sectionKeys.append(key);
        }
        QCOMPARE(sectionKeys, QStringList({QStringLiteral("pinned"),
                                           QStringLiteral("list:") + tasks,
                                           QStringLiteral("list:") + notes}));
        QCOMPARE(panel.findChild<QLabel*>(QStringLiteral("ListHeading"))->text(),
                 QStringLiteral("项目文件夹"));
        QVERIFY(quick->placeholderText().contains(QStringLiteral("任务组")));
        quick->setText(QStringLiteral("文件夹内新任务"));
        QTest::keyClick(quick, Qt::Key_Return);
        QString created;
        for (const Entity& entity : m_store->entities())
            if (entity.title == QStringLiteral("文件夹内新任务")) created = entity.id;
        QVERIFY(isUuid(created));
        QCOMPARE(m_store->entity(created).listId, tasks);
        QTRY_VERIFY(model->indexForTask(created).isValid());

        row = folderRow(folder);
        const auto childIsHidden = [navigation, tasks](bool* found = nullptr) {
            for (int index = 0; index < navigation->count(); ++index) {
                QListWidgetItem* item = navigation->item(index);
                if (item->data(Qt::UserRole + 1).toString() != tasks) continue;
                if (found) *found = true;
                return item->isHidden();
            }
            if (found) *found = false;
            return false;
        };
        bool found = false;
        QVERIFY(!childIsHidden(&found));
        QVERIFY(found);
        auto* delegate = qobject_cast<NavigationDelegate*>(navigation->itemDelegate());
        QVERIFY(delegate);
        QStyleOptionViewItem option;
        option.rect = navigation->visualItemRect(navigation->item(row));
        const QModelIndex index = navigation->model()->index(row, 0);
        QMouseEvent toggle(QEvent::MouseButtonRelease,
                           QPointF(option.rect.left() + 17, option.rect.center().y()),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QVERIFY(delegate->editorEvent(&toggle, navigation->model(), option, index));
        QTRY_VERIFY(childIsHidden());
        option.rect = navigation->visualItemRect(navigation->item(folderRow(folder)));
        QMouseEvent expand(QEvent::MouseButtonRelease,
                           QPointF(option.rect.left() + 17, option.rect.center().y()),
                           Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QVERIFY(delegate->editorEvent(&expand, navigation->model(), option,
                                     navigation->model()->index(folderRow(folder), 0)));
        QTRY_VERIFY(!childIsHidden());
    }
    void folderMergeDialogCancellationAndAcceptance() {
        const QString source = m_store->createList(QStringLiteral("来源清单"));
        const QString target = m_store->createList(QStringLiteral("目标清单"));
        TaskPanel panel(m_store.get()); panel.show();
        auto* navigation = panel.findChild<NavigationListWidget*>(QStringLiteral("Navigation"));
        QVERIFY(navigation);
        const int originalFolders = m_store->folders().size();
        const QVector<TaskList> originalLists = m_store->lists();
        QTimer::singleShot(0, [&panel]() {
            const auto dialogs = panel.findChildren<FolderEditorDialog*>();
            if (!dialogs.isEmpty()) dialogs.first()->reject();
        });
        emit navigation->folderMergeRequested(source, target);
        QCOMPARE(m_store->folders().size(), originalFolders);
        // 取消只回滚组织操作；文本失焦仍遵循通用的编辑会话保存规则。
        for (const TaskList& before : originalLists) {
            if (before.id != source && before.id != target) continue;
            for (const TaskList& after : m_store->lists()) {
                if (after.id != before.id) continue;
                QCOMPARE(after.folderId, before.folderId);
                QCOMPARE(after.sortOrder, before.sortOrder);
                QCOMPARE(after.revision, before.revision);
                QCOMPARE(after.updatedAt, before.updatedAt);
            }
        }

        QTimer::singleShot(0, [&panel]() {
            const auto dialogs = panel.findChildren<FolderEditorDialog*>();
            if (dialogs.isEmpty()) return;
            FolderEditorDialog* dialog = dialogs.first();
            dialog->findChild<QLineEdit*>(QStringLiteral("FolderNameEdit"))->setText(
                QStringLiteral("合并后的文件夹"));
            QTest::mouseClick(dialog->findChild<QPushButton*>(QStringLiteral("FolderEditorSave")),
                              Qt::LeftButton);
        });
        emit navigation->folderMergeRequested(source, target);
        QCOMPARE(m_store->folders().size(), originalFolders + 1);
        QString folder;
        for (const Folder& candidate : m_store->folders())
            if (candidate.name == QStringLiteral("合并后的文件夹")) folder = candidate.id;
        QVERIFY(isUuid(folder));
        for (const TaskList& list : m_store->lists())
            if (list.id == source || list.id == target) QCOMPARE(list.folderId, folder);
        auto* model = qobject_cast<TaskListModel*>(
            panel.findChild<QListView*>(QStringLiteral("TaskList"))->model());
        QTRY_COMPARE(model->filter().scope, ViewScope::Folder);
        QTRY_COMPARE(model->filter().folderId, folder);
    }
    void navigationDropTargets() {
        NavigationListWidget navigation;
        navigation.resize(280, 260);
        const auto addList = [&navigation](const QString& id, const QString& folder = QString()) {
            auto* item = new QListWidgetItem(id, &navigation);
            item->setData(Qt::UserRole + 1, id);
            item->setData(NavigationDelegate::ParentFolderRole, folder);
            item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
            item->setSizeHint(QSize(200, 36));
            return item;
        };
        QListWidgetItem* source = addList(QStringLiteral("source"));
        QListWidgetItem* target = addList(QStringLiteral("target"));
        QListWidgetItem* child = addList(QStringLiteral("child"), QStringLiteral("folder"));
        auto* folder = new QListWidgetItem(QStringLiteral("folder"), &navigation);
        folder->setData(NavigationDelegate::FolderIdRole, QStringLiteral("folder"));
        folder->setSizeHint(QSize(200, 36));
        navigation.show();
        QTRY_VERIFY(navigation.visualItemRect(target).height() > 0);
        QMimeData mime;
        mime.setData(NavigationListWidget::mimeType(), QByteArray("source"));
        QSignalSpy merges(&navigation, &NavigationListWidget::folderMergeRequested);
        QSignalSpy reorders(&navigation, &NavigationListWidget::listReorderRequested);

        QPoint center = navigation.visualItemRect(target).center();
        QDragEnterEvent enter(center, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &enter);
        QDragMoveEvent mergeMove(center, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &mergeMove);
        QCOMPARE(target->data(NavigationDelegate::DropTargetRole).toString(), QStringLiteral("target"));
        QDropEvent mergeDrop(QPointF(center), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &mergeDrop);
        QCOMPARE(merges.count(), 1);
        QCOMPARE(merges.first().at(0).toString(), QStringLiteral("source"));
        QCOMPARE(merges.first().at(1).toString(), QStringLiteral("target"));
        QVERIFY(target->data(NavigationDelegate::DropTargetRole).isNull());

        const QPoint before(navigation.visualItemRect(target).center().x(),
                            navigation.visualItemRect(target).top() + 2);
        QDragMoveEvent beforeMove(before, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QDragEnterEvent beforeEnter(before, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &beforeEnter);
        QApplication::sendEvent(navigation.viewport(), &beforeMove);
        QCOMPARE(target->data(NavigationDelegate::DropLineRole).toString(), QStringLiteral("before"));
        QDropEvent beforeDrop(QPointF(before), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &beforeDrop);
        QCOMPARE(reorders.count(), 1);
        QCOMPARE(reorders.last().at(1).toString(), QString());
        QCOMPARE(reorders.last().at(2).toString(), QStringLiteral("target"));
        QCOMPARE(reorders.last().at(3).toBool(), false);

        center = navigation.visualItemRect(child).center();
        QDragEnterEvent childEnter(center, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &childEnter);
        QDragMoveEvent childMove(center, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &childMove);
        QDropEvent childDrop(QPointF(center), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &childDrop);
        QCOMPARE(reorders.count(), 2);
        QCOMPARE(reorders.last().at(1).toString(), QStringLiteral("folder"));
        QCOMPARE(reorders.last().at(2).toString(), QStringLiteral("child"));
        QCOMPARE(reorders.last().at(3).toBool(), false); // 中心落在文件夹内清单之前。

        center = navigation.visualItemRect(folder).center();
        QDragEnterEvent folderEnter(center, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &folderEnter);
        QDragMoveEvent folderMove(center, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &folderMove);
        QDropEvent folderDrop(QPointF(center), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &folderDrop);
        QCOMPARE(reorders.count(), 3);
        QCOMPARE(reorders.last().at(1).toString(), QStringLiteral("folder"));
        QCOMPARE(reorders.last().at(2).toString(), QString());

        center = navigation.visualItemRect(source).center();
        QDragMoveEvent selfMove(center, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &selfMove);
        QVERIFY(source->data(NavigationDelegate::DropTargetRole).isNull());
        QCOMPARE(merges.count(), 1);
        QCOMPARE(reorders.count(), 3);

        // 组织区末尾和可见空白是顶层末尾落点，文件夹中的最后一个清单也能拖出。
        auto* end = new QListWidgetItem(&navigation);
        end->setFlags(Qt::NoItemFlags);
        end->setData(NavigationDelegate::DividerRole, true);
        end->setData(NavigationDelegate::OrganizationEndRole, true);
        end->setSizeHint(QSize(200, 33));
        QMimeData childMime;
        childMime.setData(NavigationListWidget::mimeType(), QByteArray("child"));
        const QPoint endPoint = navigation.visualItemRect(end).center();
        QDragEnterEvent endEnter(endPoint, Qt::MoveAction, &childMime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &endEnter);
        QDragMoveEvent endMove(endPoint, Qt::MoveAction, &childMime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &endMove);
        QCOMPARE(end->data(NavigationDelegate::DropLineRole).toString(), QStringLiteral("after"));
        QDropEvent endDrop(QPointF(endPoint), Qt::MoveAction, &childMime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &endDrop);
        QCOMPARE(reorders.count(), 4);
        QCOMPARE(reorders.last().at(0).toString(), QStringLiteral("child"));
        QCOMPARE(reorders.last().at(1).toString(), QString());
        QCOMPARE(reorders.last().at(2).toString(), QString());

        navigation.resize(280, 360); QApplication::processEvents();
        const QPoint blankPoint(100, navigation.viewport()->height() - 8);
        QDragEnterEvent blankEnter(blankPoint, Qt::MoveAction, &childMime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &blankEnter);
        QDragMoveEvent blankMove(blankPoint, Qt::MoveAction, &childMime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &blankMove);
        QVERIFY(blankMove.isAccepted());
        QDropEvent blankDrop(QPointF(blankPoint), Qt::MoveAction, &childMime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation.viewport(), &blankDrop);
        QVERIFY(blankDrop.isAccepted());
        QCOMPARE(reorders.count(), 5);
        QCOMPARE(reorders.last().at(0).toString(), QStringLiteral("child"));
        QCOMPARE(reorders.last().at(1).toString(), QString());
        QCOMPARE(reorders.last().at(2).toString(), QString());
    }
    void hiddenSmartListDoesNotBlockExplicitTaskNavigation() {
        const QString id = m_store->createTask(QStringLiteral("隐藏垃圾桶中的实体"));
        QVERIFY(m_store->trashTask(id));
        Settings::instance().setSmartListVisibility(
            QStringLiteral("trash"), Settings::SmartListVisibility::Hide);
        TaskPanel panel(m_store.get()); panel.showTask(id);
        auto* model = qobject_cast<TaskListModel*>(
            panel.findChild<QListView*>(QStringLiteral("TaskList"))->model());
        QVERIFY(model);
        QCOMPARE(model->filter().scope, ViewScope::Trash);
        QVERIFY(model->indexForTask(id).isValid());
        QCOMPARE(panel.findChild<TaskDetailWidget*>()->entityId(), id);
        QCOMPARE(panel.findChild<QListWidget*>(QStringLiteral("Navigation"))->currentRow(), -1);
    }
    void popupMenuPreservesActionsAndShadow() {
        QWidget owner; owner.resize(420, 300); owner.show();
        PopupMenu menu(&owner);
        QAction* restore = menu.addAction(QStringLiteral("恢复"));
        menu.addAction(QStringLiteral("永久删除"));
        PopupMenu* child = menu.addMenu(QStringLiteral("子菜单"));
        child->addAction(QStringLiteral("子项"));
        menu.popup(owner.mapToGlobal(QPoint(80, 60)));
        QTRY_VERIFY(menu.isVisible());
        const int margin = PopupMenu::shadowMargin();
        const QRect actionRect = menu.actionGeometry(restore);
        QVERIFY(actionRect.left() >= margin);
        QVERIFY(actionRect.right() < menu.width() - margin);
        QVERIFY(actionRect.top() >= margin);
        QVERIFY(menu.windowFlags().testFlag(Qt::NoDropShadowWindowHint));
        QVERIFY(nativePopupShadowDisabled(&menu));
        const QPixmap grabbed = menu.grab();
        const QImage image = grabbed.toImage();
        const qreal dpr = grabbed.devicePixelRatio();
        QCOMPARE(qAlpha(image.pixel(0, 0)), 0);
        QVERIFY(qAlpha(image.pixel(qRound((margin - 3) * dpr), image.height() / 2)) > 0);
        QSignalSpy triggered(restore, &QAction::triggered);
        QTest::keyClick(&menu, Qt::Key_Down);
        QTest::keyClick(&menu, Qt::Key_Return);
        QTRY_COMPARE(triggered.count(), 1);

        MenuComboBox combo(&owner);
        combo.addItems({QStringLiteral("显示"), QStringLiteral("有内容时显示"), QStringLiteral("隐藏")});
        combo.resize(130, 32); combo.show(); combo.showPopup();
        QPointer<PopupMenu> choices = combo.findChild<PopupMenu*>(QStringLiteral("ComboChoiceMenu"));
        QVERIFY(choices); QTRY_VERIFY(choices->isVisible());
        QAction* hide = choices->actions().at(2);
        QTest::mouseClick(choices, Qt::LeftButton, Qt::NoModifier,
                          choices->actionGeometry(hide).center());
        QTRY_COMPARE(combo.currentIndex(), 2);
        QTRY_VERIFY(choices.isNull() || !choices->isVisible());
    }
    void dateControllerTargetsOnlyItsEntity() {
        const QString first = m_store->createTask(QStringLiteral("本次日期目标"));
        const QString second = m_store->createTask(QStringLiteral("另一个实体"));
        QWidget owner; owner.resize(400, 250); owner.show();
        TaskDateController controller(m_store.get(), &owner);
        QSignalSpy saved(&controller, &TaskDateController::saved);
        controller.open(first, QRect(owner.mapToGlobal(QPoint(30, 30)), QSize(80, 30)));
        auto* picker = owner.findChild<DatePickerPopup*>();
        QVERIFY(picker); QTRY_VERIFY(picker->isVisible());
        Entity updated = m_store->entity(first);
        updated.body = QStringLiteral("弹层打开期间写入的新正文");
        QVERIFY(m_store->saveTask(updated));
        const QDate chosen = toAppTime(appNow()).date().addDays(3);
        picker->setDateTime(fromAppTime(QDateTime(chosen, QTime(12, 30))));
        picker->setHasDate(true);
        picker->accept();
        QTRY_COMPARE(saved.count(), 1);
        QCOMPARE(saved.first().first().toString(), first);
        QCOMPARE(effectiveDate(m_store->entity(first)), chosen);
        QCOMPARE(m_store->entity(first).body, updated.body);
        QVERIFY(!effectiveDate(m_store->entity(second)).isValid());
    }
    void appToolTipCompactAndContextual() {
        QToolButton probe;
        probe.setToolTip(QStringLiteral("添加清单"));
        probe.resize(40, 32); probe.show();
        AppToolTip::showText(probe.mapToGlobal(QPoint(20, 32)), probe.toolTip(), &probe,
                             probe.rect(), 0);
        QTRY_VERIFY(AppToolTip::isVisible());
        QWidget* bubble = AppToolTip::window();
        QVERIFY(bubble);
        QCOMPARE(bubble->objectName(), QStringLiteral("AppToolTip"));
        QVERIFY(bubble->height() <= 26);
        QCOMPARE(bubble->layout()->contentsMargins(), QMargins(8, 3, 8, 3));
        auto* text = bubble->findChild<QLabel*>(QStringLiteral("AppToolTipText"));
        QVERIFY(text); QCOMPARE(text->font().pixelSize(), 13);
        QVERIFY(bubble->windowFlags() & Qt::NoDropShadowWindowHint);
        const QImage image = bubble->grab().toImage();
        QCOMPARE(qAlpha(image.pixel(0, 0)), 0);
        QVERIFY(qAlpha(image.pixel(2, image.height() / 2)) > 150);
        AppToolTip::hideText();

        QPushButton duplicate(QStringLiteral("取消"));
        duplicate.setToolTip(QStringLiteral("取消"));
        duplicate.resize(90, 32); duplicate.show();
        QHelpEvent duplicateEvent(QEvent::ToolTip, QPoint(20, 16),
                                  duplicate.mapToGlobal(QPoint(20, 16)));
        QApplication::sendEvent(&duplicate, &duplicateEvent);
        QVERIFY(!AppToolTip::isVisible());
        QToolButton iconOnly;
        iconOnly.setToolButtonStyle(Qt::ToolButtonIconOnly);
        iconOnly.setText(QStringLiteral("添加清单"));
        iconOnly.setToolTip(iconOnly.text());
        iconOnly.resize(40, 32); iconOnly.show();
        QHelpEvent iconEvent(QEvent::ToolTip, QPoint(20, 16), iconOnly.mapToGlobal(QPoint(20, 16)));
        QApplication::sendEvent(&iconOnly, &iconEvent);
        QTRY_VERIFY(AppToolTip::isVisible());
        AppToolTip::hideText();

        auto* tipLabel = AppToolTip::window()->findChild<QLabel*>(QStringLiteral("AppToolTipText"));
        QVERIFY(tipLabel);
        const QString singleLine = QStringLiteral("测").repeated(30);
        AppToolTip::showText(QPoint(200, 200), singleLine);
        QApplication::processEvents();
        QVERIFY(!tipLabel->wordWrap());
        QVERIFY(tipLabel->width() >= tipLabel->fontMetrics().horizontalAdvance(singleLine));
        const QString wrapped = QStringLiteral("测").repeated(31);
        AppToolTip::showText(QPoint(200, 200), wrapped);
        QApplication::processEvents();
        QVERIFY(tipLabel->wordWrap());
        QVERIFY(tipLabel->maximumWidth() <= 248);
        AppToolTip::hideText();

        const QString id = m_store->createTask(QStringLiteral("短标题"));
        TaskListModel model(m_store.get());
        QListView tasks; tasks.setModel(&model); tasks.resize(560, 100); tasks.show();
        TaskDelegate taskDelegate;
        QStyleOptionViewItem option; option.rect = QRect(0, 0, 560, 40);
        const QPoint titlePos(60, 20);
        QHelpEvent titleEvent(QEvent::ToolTip, titlePos,
                              tasks.viewport()->mapToGlobal(titlePos));
        QVERIFY(taskDelegate.helpEvent(&titleEvent, &tasks, option, model.indexForTask(id)));
        QVERIFY(!AppToolTip::isVisible());
        Entity longTask = m_store->entity(id);
        longTask.title = QStringLiteral("需要省略的完整任务标题").repeated(12);
        QVERIFY(m_store->saveTask(longTask));
        QVERIFY(taskDelegate.helpEvent(&titleEvent, &tasks, option, model.indexForTask(id)));
        QTRY_VERIFY(AppToolTip::isVisible());
        QCOMPARE(AppToolTip::window()->findChild<QLabel*>(QStringLiteral("AppToolTipText"))->text(),
                 longTask.title);
        longTask.title = QStringLiteral("短标题");
        longTask.allDayDate = toAppTime(appNow()).date().addDays(4);
        QVERIFY(m_store->saveTask(longTask));
        AppToolTip::hideText();
        const QPoint datePos(525, 20);
        QHelpEvent dateEvent(QEvent::ToolTip, datePos, tasks.viewport()->mapToGlobal(datePos));
        QVERIFY(taskDelegate.helpEvent(&dateEvent, &tasks, option, model.indexForTask(id)));
        QTRY_VERIFY(AppToolTip::isVisible());
        const QString fullDate = model.data(model.indexForTask(id), TaskListModel::ScheduleTooltipRole).toString();
        QCOMPARE(AppToolTip::window()->findChild<QLabel*>(QStringLiteral("AppToolTipText"))->text(),
                 fullDate);
        QVERIFY(fullDate.contains(QStringLiteral("周")));
        AppToolTip::hideText();

        QListWidget navigation;
        QListWidgetItem shortItem(QStringLiteral("今天")); navigation.addItem(&shortItem);
        NavigationDelegate navigationDelegate;
        QStyleOptionViewItem navOption; navOption.rect = QRect(0, 0, 220, 36);
        const QPoint navPos(60, 18);
        QHelpEvent navEvent(QEvent::ToolTip, navPos, navigation.viewport()->mapToGlobal(navPos));
        QVERIFY(navigationDelegate.helpEvent(&navEvent, &navigation, navOption,
                                             navigation.model()->index(navigation.row(&shortItem), 0)));
        QVERIFY(!AppToolTip::isVisible());
        QListWidgetItem longItem(QStringLiteral("很长的清单名称").repeated(15)); navigation.addItem(&longItem);
        QVERIFY(navigationDelegate.helpEvent(&navEvent, &navigation, navOption,
                                             navigation.model()->index(navigation.row(&longItem), 0)));
        QTRY_VERIFY(AppToolTip::isVisible());
        AppToolTip::hideText();

        EntityHoverPreview preview(m_store.get());
        preview.showEntity(id, QPoint(200, 200));
        QTRY_VERIFY(preview.isVisible());
        QVERIFY(preview.testAttribute(Qt::WA_TranslucentBackground));
        QVERIFY(preview.styleSheet().contains(QStringLiteral("border-radius:6px")));
        QVERIFY(preview.findChild<QLabel*>(QStringLiteral("HoverSummary"))->isHidden());
        preview.cancel();
    }
    void datePickerQuickActionsAndDateOnlyModes() {
        QCOMPARE(DatePickerPopup::quickDate(3, QDate(2026, 1, 31)), QDate(2026, 2, 28));
        QCOMPARE(DatePickerPopup::quickDate(3, QDate(2028, 1, 31)), QDate(2028, 2, 29));
        QCOMPARE(DatePickerPopup::quickDate(3, QDate(2026, 3, 31)), QDate(2026, 4, 30));
        QCOMPARE(DatePickerPopup::quickDate(3, QDate(2026, 12, 31)), QDate(2027, 1, 31));
        QVERIFY(!DatePickerPopup::quickDate(-1, QDate(2026, 1, 31)).isValid());

        DatePickerPopup picker; picker.show();
        QToolButton* quick[4] = {nullptr, nullptr, nullptr, nullptr};
        for (auto* button : picker.findChildren<QToolButton*>(QStringLiteral("DatePickerQuick")))
            quick[button->property("quickIndex").toInt()] = button;
        for (auto* button : quick) QVERIFY(button);
        const QDate today = toAppTime(appNow()).date();
        const QDate expected[] = {today, today.addDays(1), today.addDays(7), today.addMonths(1)};
        for (int index = 0; index < 4; ++index) {
            QTest::mouseClick(quick[index], Qt::LeftButton);
            QVERIFY(picker.hasDate()); QVERIFY(!picker.hasTime());
            QCOMPARE(picker.allDayDate(), expected[index]);
            QVERIFY(!picker.dateTime().isValid());
            QCOMPARE(picker.result(), static_cast<int>(QDialog::Rejected));
        }
        QTest::mouseClick(quick[3], Qt::LeftButton);
        QCOMPARE(picker.allDayDate(), today.addMonths(1)); // 仍以今天为基准，不累计月份

        picker.setDateTime(fromAppTime(QDateTime(today.addDays(-2), QTime(9, 45))));
        QTest::mouseClick(quick[1], Qt::LeftButton);
        QVERIFY(picker.hasTime());
        QCOMPARE(toAppTime(picker.dateTime()).date(), today.addDays(1));
        QCOMPARE(toAppTime(picker.dateTime()).time(), QTime(9, 45));
        QPushButton* timeEntry = nullptr;
        for (auto* button : picker.findChildren<QPushButton*>(QStringLiteral("DatePickerEntry")))
            if (button->accessibleName() == QStringLiteral("时间")) timeEntry = button;
        QVERIFY(timeEntry); QTest::mouseClick(timeEntry, Qt::LeftButton);
        auto* noTime = picker.findChild<QPushButton*>(QStringLiteral("DatePickerNoTime"));
        QVERIFY(noTime); QTRY_VERIFY(noTime->isVisible());
        QTest::mouseClick(noTime, Qt::LeftButton);
        QVERIFY(!picker.hasTime());
        QCOMPARE(picker.allDayDate(), today.addDays(1));
        picker.reject();
    }
    void dateControllerStoresDateOnlyAndExplicitMidnight() {
        const QString id = m_store->createTask(QStringLiteral("日期模式"));
        QWidget owner; owner.resize(400, 250); owner.show();
        TaskDateController controller(m_store.get(), &owner);
        QSignalSpy saved(&controller, &TaskDateController::saved);
        const QDate today = toAppTime(appNow()).date();
        controller.open(id, QRect(owner.mapToGlobal(QPoint(30, 30)), QSize(80, 30)));
        QPointer<DatePickerPopup> picker = owner.findChild<DatePickerPopup*>();
        QVERIFY(picker); QTRY_VERIFY(picker->isVisible());
        QToolButton* todayButton = nullptr;
        for (auto* button : picker->findChildren<QToolButton*>(QStringLiteral("DatePickerQuick")))
            if (button->property("quickIndex").toInt() == 0) todayButton = button;
        QVERIFY(todayButton); QTest::mouseClick(todayButton, Qt::LeftButton);
        picker->accept();
        QTRY_COMPARE(saved.count(), 1);
        QCOMPARE(m_store->entity(id).allDayDate, today);
        QVERIFY(!m_store->entity(id).dueAt.isValid());
        QVERIFY(!m_store->entity(id).startAt.isValid());
        TaskDetailWidget dateOnlyDetail(m_store.get()); QVERIFY(dateOnlyDetail.setTask(id));
        QVERIFY(!dateOnlyDetail.findChild<QPushButton*>(QStringLiteral("DateChip"))->text()
                     .contains(QStringLiteral("00:00")));
        QTRY_VERIFY(!picker);

        controller.open(id, QRect(owner.mapToGlobal(QPoint(30, 30)), QSize(80, 30)));
        picker = owner.findChild<DatePickerPopup*>();
        QVERIFY(picker); QTRY_VERIFY(picker->isVisible());
        QVERIFY(!picker->hasTime()); QCOMPARE(picker->allDayDate(), today);
        QPushButton* timeEntry = nullptr;
        for (auto* button : picker->findChildren<QPushButton*>(QStringLiteral("DatePickerEntry")))
            if (button->accessibleName() == QStringLiteral("时间")) timeEntry = button;
        QVERIFY(timeEntry); QTest::mouseClick(timeEntry, Qt::LeftButton);
        auto* timePage = picker->findChild<QWidget*>(QStringLiteral("DatePickerTimePage"));
        auto* times = timePage->findChild<QListWidget*>(QStringLiteral("DatePickerList"));
        QVERIFY(times); QTRY_VERIFY(times->isVisible());
        QTest::mouseClick(times->viewport(), Qt::LeftButton, Qt::NoModifier,
                          times->visualItemRect(times->item(0)).center());
        QTRY_VERIFY(picker->hasTime());
        QCOMPARE(toAppTime(picker->dateTime()).time(), QTime(0, 0));
        picker->accept();
        QTRY_COMPARE(saved.count(), 2);
        QVERIFY(m_store->entity(id).dueAt.isValid());
        QVERIFY(!m_store->entity(id).allDayDate.isValid());
        QCOMPARE(toAppTime(m_store->entity(id).dueAt).time(), QTime(0, 0));
        TaskDetailWidget midnightDetail(m_store.get()); QVERIFY(midnightDetail.setTask(id));
        QVERIFY(midnightDetail.findChild<QPushButton*>(QStringLiteral("DateChip"))->text()
                    .contains(QStringLiteral("00:00")));
        QTRY_VERIFY(!picker);

        controller.open(id, QRect(owner.mapToGlobal(QPoint(30, 30)), QSize(80, 30)));
        picker = owner.findChild<DatePickerPopup*>();
        QVERIFY(picker); QTRY_VERIFY(picker->isVisible());
        QVERIFY(picker->hasTime());
        for (auto* button : picker->findChildren<QToolButton*>(QStringLiteral("DatePickerQuick")))
            if (button->property("quickIndex").toInt() == 1) button->click();
        QCOMPARE(toAppTime(picker->dateTime()).date(), today.addDays(1));
        QCOMPARE(toAppTime(picker->dateTime()).time(), QTime(0, 0));
        timeEntry = nullptr;
        for (auto* button : picker->findChildren<QPushButton*>(QStringLiteral("DatePickerEntry")))
            if (button->accessibleName() == QStringLiteral("时间")) timeEntry = button;
        QVERIFY(timeEntry); QTest::mouseClick(timeEntry, Qt::LeftButton);
        auto* noTime = picker->findChild<QPushButton*>(QStringLiteral("DatePickerNoTime"));
        QVERIFY(noTime); QTRY_VERIFY(noTime->isVisible()); QTest::mouseClick(noTime, Qt::LeftButton);
        QVERIFY(!picker->hasTime()); QCOMPARE(picker->allDayDate(), today.addDays(1));
        picker->accept();
        QTRY_COMPARE(saved.count(), 3);
        QVERIFY(!m_store->entity(id).dueAt.isValid());
        QCOMPARE(m_store->entity(id).allDayDate, today.addDays(1));
        QTRY_VERIFY(!picker);

        controller.open(id, QRect(owner.mapToGlobal(QPoint(30, 30)), QSize(80, 30)));
        picker = owner.findChild<DatePickerPopup*>();
        QVERIFY(picker); QTRY_VERIFY(picker->isVisible());
        for (auto* button : picker->findChildren<QToolButton*>(QStringLiteral("DatePickerQuick")))
            if (button->property("quickIndex").toInt() == 2) button->click();
        picker->reject();
        QTRY_VERIFY(!picker);
        QCOMPARE(saved.count(), 3);
        QCOMPARE(m_store->entity(id).allDayDate, today.addDays(1));

        TaskStore reloaded(Database::instance().connection());
        QString error; QVERIFY(reloaded.load(&error));
        QCOMPARE(reloaded.entity(id).allDayDate, today.addDays(1));
        QVERIFY(!reloaded.entity(id).dueAt.isValid());
    }
    void dirtyDesktopNoteSurvivesListArchiveAndDeletion() {
        const QString list = m_store->createList(QStringLiteral("待归档清单"));
        const QString id = m_store->createTask(QStringLiteral("脏便签任务"), list);
        QVERIFY(m_store->saveDesktopState(id, NoteColor::Yellow, QRect(30, 40, 360, 320),
                                          0, NoteLayer::Normal, true));
        NoteManager notes(m_store.get()); QVERIFY(notes.restoreAll());
        NoteWidget* note = notes.note(id); QVERIFY(note);
        note->focusEditor(); QApplication::processEvents();
        auto* editor = note->findChild<MarkdownEditor*>(); QVERIFY(editor);
        editor->textEditor()->setPlainText(QStringLiteral("归档之前尚未自动保存的正文"));
        QVERIFY(note->descriptionDirty());
        QVERIFY(m_store->setListArchived(list, true));
        QCOMPARE(notes.count(), 0);
        QCOMPARE(m_store->entity(id).body, QStringLiteral("归档之前尚未自动保存的正文"));
        QVERIFY(m_store->desktopNote(id).open);
        QVERIFY(m_store->setListArchived(list, false));
        QTRY_COMPARE(notes.count(), 1);
        note = notes.note(id); QVERIFY(note);
        note->focusEditor(); QApplication::processEvents();
        editor = note->findChild<MarkdownEditor*>(); QVERIFY(editor);
        editor->textEditor()->setPlainText(QStringLiteral("删除清单前的新正文"));
        QVERIFY(note->descriptionDirty());
        QVERIFY(m_store->deleteList(list));
        QCOMPARE(notes.count(), 0);
        QCOMPARE(m_store->entity(id).body, QStringLiteral("删除清单前的新正文"));
        QVERIFY(m_store->entity(id).deleted);
    }
    void smartListVisibilityModes() {
        TaskPanel panel(m_store.get()); panel.show();
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        auto* listView = panel.findChild<QListView*>(QStringLiteral("TaskList"));
        auto* model = qobject_cast<TaskListModel*>(listView->model());
        QVERIFY(navigation && model);
        const auto rowFor = [navigation](ViewScope scope) {
            for (int row = 0; row < navigation->count(); ++row) {
                const auto* item = navigation->item(row);
                if (!item->data(NavigationDelegate::SectionRole).toBool()
                    && item->data(Qt::UserRole).toInt() == static_cast<int>(scope)) return row;
            }
            return -1;
        };
        QVERIFY(rowFor(ViewScope::AllLists) >= 0);
        QVERIFY(rowFor(ViewScope::Tomorrow) >= 0);
        QVERIFY(rowFor(ViewScope::Next7Days) >= 0);
        QCOMPARE(rowFor(ViewScope::Overdue), -1);

        SettingsDialog settingsDialog;
        QVERIFY(!settingsDialog.findChild<QComboBox*>(QStringLiteral("SmartVisibility_overdue")));
        QSignalSpy changed(&settingsDialog, &SettingsDialog::smartListVisibilityChanged);
        auto* todayCombo = settingsDialog.findChild<QComboBox*>(QStringLiteral("SmartVisibility_today"));
        QVERIFY(todayCombo);
        todayCombo->setCurrentIndex(todayCombo->findData(
            static_cast<int>(Settings::SmartListVisibility::Hide)));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(Settings::instance().smartListVisibility(QStringLiteral("today")),
                 Settings::SmartListVisibility::Hide);
        m_store->createTask(QStringLiteral("无日期任务"));
        QTRY_VERIFY(rowFor(ViewScope::Today) < 0);

        Settings::instance().setSmartListVisibility(
            QStringLiteral("today"), Settings::SmartListVisibility::WhenNonEmpty);
        m_store->createTask(QStringLiteral("仍无日期"));
        QTRY_VERIFY(rowFor(ViewScope::Today) < 0);
        const QString dated = m_store->createTask(QStringLiteral("今天有内容"));
        Entity task = m_store->entity(dated);
        task.allDayDate = toAppTime(appNow()).date();
        QVERIFY(m_store->saveTask(task));
        QTRY_VERIFY(rowFor(ViewScope::Today) >= 0);

        navigation->setCurrentRow(rowFor(ViewScope::Today));
        QTRY_COMPARE(model->filter().scope, ViewScope::Today);
        Settings::instance().setSmartListVisibility(
            QStringLiteral("today"), Settings::SmartListVisibility::Hide);
        QVERIFY(m_store->setPinnedAt(dated, appNow()));
        QTRY_COMPARE(model->filter().scope, ViewScope::AllLists);
        QTRY_VERIFY(rowFor(ViewScope::Today) < 0);

        Settings::instance().setSmartListVisibility(
            QStringLiteral("today"), Settings::SmartListVisibility::WhenNonEmpty);
        QVERIFY(m_store->setPinnedAt(dated, appNow()));
        QTRY_VERIFY(rowFor(ViewScope::Today) >= 0);
        QVERIFY(m_store->trashTask(dated));
        QTRY_VERIFY(rowFor(ViewScope::Today) < 0);
    }
    void listEmojiGlyphPainting() {
        TaskList appearance;
        appearance.name = QStringLiteral("工作");
        appearance.icon = QStringLiteral("🏛️");
        for (const int size : {14, 16, 18, 20}) {
            QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setPen(Qt::NoPen);
            ListAppearance::paint(&painter, appearance, QRectF(0, 0, size, size),
                                  QColor(QStringLiteral("#526575")));
            painter.end();
            int paintedPixels = 0;
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    if (qAlpha(image.pixel(x, y)) != 0) ++paintedPixels;
                    if (x == 0 || y == 0 || x == size - 1 || y == size - 1)
                        QCOMPARE(qAlpha(image.pixel(x, y)), 0);
                }
            }
            QVERIFY2(paintedPixels > 4, qPrintable(QStringLiteral("emoji size %1 was not painted").arg(size)));
        }

        QListWidget navigation;
        auto* item = new QListWidgetItem(QStringLiteral("工作"), &navigation);
        item->setData(NavigationDelegate::IconNameRole, appearance.icon);
        item->setData(NavigationDelegate::ListColorRole, QStringLiteral("#4CA1FF"));
        const QModelIndex index = navigation.model()->index(0, 0);
        NavigationDelegate delegate;
        const auto render = [&delegate, &index](const QImage& initial) {
            QImage image = initial;
            QPainter painter(&image);
            QStyleOptionViewItem option;
            option.rect = QRect(0, 0, 220, 36);
            option.state = QStyle::State_Enabled | QStyle::State_Selected;
            delegate.paint(&painter, option, index);
            painter.end();
            return image;
        };
        QImage blank(220, 36, QImage::Format_ARGB32_Premultiplied);
        blank.fill(Qt::transparent);
        const QImage withGlyph = render(blank);
        item->setData(NavigationDelegate::IconNameRole, QString());
        const QImage withoutGlyph = render(blank);
        bool different = false;
        for (int y = 8; y < 28 && !different; ++y)
            for (int x = 22; x < 42 && !different; ++x)
                different = withGlyph.pixel(x, y) != withoutGlyph.pixel(x, y);
        QVERIFY(different);
    }
    void bundledEmojiFontLoadsWithoutSystemEmoji() {
        const QString fontPath = QFileInfo(QStringLiteral(KYLINLIST_SOURCE_DIR)
            + QStringLiteral("/src/resources/fonts/NotoEmoji-Regular.ttf")).absoluteFilePath();
        QVERIFY2(QFileInfo::exists(fontPath), qPrintable(fontPath));
        qputenv("KYLINLIST_EMOJI_FONT", fontPath.toUtf8());
        const QString family = ds::bundledEmojiFamilyName();
        QVERIFY2(!family.isEmpty(), "Qt 5.12 未能注册随包静态 Emoji 字体");
        QCOMPARE(ds::emojiFont().family(), family);
        QRawFont raw = QRawFont::fromFont(ds::emojiFont());
        QVERIFY(raw.isValid());
        for (const uint codepoint : {0x1f3dbu, 0x1f6e1u, 0x1f522u, 0x1f600u})
            QVERIFY2(raw.supportsCharacter(codepoint),
                     qPrintable(QStringLiteral("随包字体缺少 U+%1").arg(codepoint, 0, 16)));
        TaskList appearance;
        appearance.icon = QStringLiteral("🏛️");
        QImage image(20, 20, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setPen(QColor(QStringLiteral("#526575")));
        ListAppearance::paint(&painter, appearance, QRectF(0, 0, 20, 20),
                              QColor(QStringLiteral("#526575")));
        painter.end();
        int ink = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (qAlpha(image.pixel(x, y)) > 0) ++ink;
        QVERIFY(ink > 4);
    }
    void pinnedSectionHeaderShowsPinIcon() {
        const QString id = m_store->createTask(QStringLiteral("置顶任务"));
        Entity task = m_store->entity(id);
        task.allDayDate = toAppTime(appNow()).date();
        QVERIFY(m_store->saveTask(task));
        QVERIFY(m_store->setPinnedAt(id, appNow()));

        TaskPanel panel(m_store.get()); panel.resize(900, 640); panel.show();
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        auto* view = panel.findChild<TaskListView*>(QStringLiteral("TaskList"));
        auto* model = qobject_cast<TaskListModel*>(view->model());
        QVERIFY(navigation && view && model);
        for (int row = 0; row < navigation->count(); ++row) {
            if (navigation->item(row)->data(Qt::UserRole).toInt()
                == static_cast<int>(ViewScope::Today)) {
                navigation->setCurrentRow(row);
                break;
            }
        }
        QTRY_COMPARE(model->filter().scope, ViewScope::Today);
        QModelIndex header;
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex index = model->index(row, 0);
            if (model->data(index, TaskListModel::HeaderRole).toBool()
                && model->data(index, TaskListModel::SectionKeyRole).toString()
                       == QStringLiteral("pinned")) header = index;
        }
        QVERIFY(header.isValid());
        view->scrollTo(header);
        QApplication::processEvents();
        const QRect rowRect = view->visualRect(header);
        QVERIFY(rowRect.isValid());
        const QImage image = view->viewport()->grab().toImage();
        const qreal dpr = image.devicePixelRatio();
        const QRectF icon(rowRect.left() + 36.0, rowRect.center().y() - 8.0, 16.0, 16.0);
        int orangePixels = 0;
        for (int y = qFloor(icon.top() * dpr); y <= qCeil(icon.bottom() * dpr); ++y) {
            for (int x = qFloor(icon.left() * dpr); x <= qCeil(icon.right() * dpr); ++x) {
                if (!image.rect().contains(x, y)) continue;
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.red() > 200 && pixel.green() > 100 && pixel.green() < 220
                    && pixel.blue() < 90) ++orangePixels;
            }
        }
        QVERIFY2(orangePixels > 20, qPrintable(QStringLiteral("pin icon pixels: %1").arg(orangePixels)));
    }
    void sectionHeadersShareTypography() {
        const QDate today = toAppTime(appNow()).date();
        const auto makeDatedTask = [this](const QString& title, const QDate& date) {
            const QString id = m_store->createTask(title);
            Entity entity = m_store->entity(id);
            entity.allDayDate = date;
            if (!m_store->saveTask(entity)) return QString();
            return id;
        };
        const QString pinnedId = makeDatedTask(QStringLiteral("置顶标题"), today);
        QVERIFY(!pinnedId.isEmpty());
        QVERIFY(m_store->setPinnedAt(pinnedId, appNow()));
        QVERIFY(!makeDatedTask(QStringLiteral("过期标题"), today.addDays(-1)).isEmpty());
        QVERIFY(!makeDatedTask(QStringLiteral("今天标题"), today).isEmpty());

        TaskPanel panel(m_store.get()); panel.resize(900, 640); panel.show();
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        auto* view = panel.findChild<TaskListView*>(QStringLiteral("TaskList"));
        auto* model = qobject_cast<TaskListModel*>(view->model());
        QVERIFY(navigation && view && model);
        for (int row = 0; row < navigation->count(); ++row) {
            if (navigation->item(row)->data(Qt::UserRole).toInt()
                == static_cast<int>(ViewScope::Today)) {
                navigation->setCurrentRow(row);
                break;
            }
        }
        QTRY_COMPARE(model->filter().scope, ViewScope::Today);
        QVERIFY(model->rowCount() >= 3);
        const QImage image = view->viewport()->grab().toImage();
        const qreal dpr = image.devicePixelRatio();
        QVector<int> heights;
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex index = model->index(row, 0);
            if (!model->data(index, TaskListModel::HeaderRole).toBool()) continue;
            const QRect rowRect = view->visualRect(index);
            int top = rowRect.bottom() + 1, bottom = rowRect.top() - 1;
            for (int y = rowRect.top(); y <= rowRect.bottom(); ++y) {
                for (int x = rowRect.left(); x <= rowRect.right(); ++x) {
                    const int px = qRound(x * dpr), py = qRound(y * dpr);
                    if (!image.rect().contains(px, py)) continue;
                    const QColor pixel = image.pixelColor(px, py);
                    // 标题为正文深色；橙色图标、蓝色“顺延”和灰色计数都不在阈值内。
                    if (pixel.red() < 120 && pixel.green() < 120 && pixel.blue() < 120) {
                        top = qMin(top, y);
                        bottom = qMax(bottom, y);
                    }
                }
            }
            QVERIFY2(bottom >= top, qPrintable(QStringLiteral("header %1 has no dark text").arg(row)));
            heights.append(bottom - top + 1);
        }
        QVERIFY2(heights.size() >= 3, qPrintable(QStringLiteral("headers: %1").arg(heights.size())));
        for (int height : heights) QVERIFY(qAbs(height - heights.first()) <= 1);
    }
    void sectionHeaderToggleCollapsesRows() {
        const QDate today = toAppTime(appNow()).date();
        const auto makeDatedTask = [this](const QString& title, const QDate& date) {
            const QString id = m_store->createTask(title);
            Entity entity = m_store->entity(id);
            entity.allDayDate = date;
            return m_store->saveTask(entity) ? id : QString();
        };
        const QString pinned = makeDatedTask(QStringLiteral("可折叠置顶"), today);
        QVERIFY(!pinned.isEmpty());
        QVERIFY(m_store->setPinnedAt(pinned, appNow()));
        const QString normal = makeDatedTask(QStringLiteral("可折叠今天"), today);
        QVERIFY(!normal.isEmpty());

        TaskPanel panel(m_store.get()); panel.resize(900, 640); panel.show();
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        auto* view = panel.findChild<TaskListView*>(QStringLiteral("TaskList"));
        auto* model = qobject_cast<TaskListModel*>(view->model());
        QVERIFY(navigation && view && model);
        for (int row = 0; row < navigation->count(); ++row) {
            if (navigation->item(row)->data(Qt::UserRole).toInt()
                == static_cast<int>(ViewScope::Today)) {
                navigation->setCurrentRow(row);
                break;
            }
        }
        QTRY_COMPARE(model->filter().scope, ViewScope::Today);
        const auto headerFor = [view, model](const QString& key) {
            for (int row = 0; row < model->rowCount(); ++row) {
                const QModelIndex index = model->index(row, 0);
                if (model->data(index, TaskListModel::HeaderRole).toBool()
                    && model->data(index, TaskListModel::SectionKeyRole).toString() == key)
                    return index;
            }
            return QModelIndex();
        };
        const QModelIndex pinnedHeader = headerFor(QStringLiteral("pinned"));
        QVERIFY(pinnedHeader.isValid());
        const int expandedRows = model->rowCount();
        const QString screenshotDir = qEnvironmentVariable("KYLINLIST_SCREENSHOT_DIR");
        if (!screenshotDir.isEmpty()) {
            QVERIFY(QDir().mkpath(screenshotDir));
            QVERIFY(view->grab().save(screenshotDir + QStringLiteral("/section-expanded.png")));
        }
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier,
                          view->visualRect(pinnedHeader).center());
        QTRY_VERIFY(headerFor(QStringLiteral("pinned")).data(
            TaskListModel::SectionCollapsedRole).toBool());
        if (!screenshotDir.isEmpty())
            QVERIFY(view->grab().save(screenshotDir + QStringLiteral("/section-collapsed.png")));
        QCOMPARE(model->rowCount(), expandedRows - 1);
        QVERIFY(!model->indexForTask(pinned).isValid());
        QVERIFY(model->indexForTask(normal).isValid());
        QCOMPARE(headerFor(QStringLiteral("pinned"))
                     .data(TaskListModel::HeaderEntityIdsRole).toStringList().size(), 1);

        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier,
                          view->visualRect(headerFor(QStringLiteral("pinned"))).center());
        QTRY_VERIFY(!headerFor(QStringLiteral("pinned")).data(
            TaskListModel::SectionCollapsedRole).toBool());
        QCOMPARE(model->rowCount(), expandedRows);
        QVERIFY(model->indexForTask(pinned).isValid());
    }
    void folderDisclosureCollapsesLists() {
        const QString folder = m_store->createFolder(QStringLiteral("可折叠文件夹"),
                                                     QStringLiteral("🏛️"));
        const QString list = m_store->createList(QStringLiteral("文件夹内清单"), folder,
                                                 EntityKind::Task, QStringLiteral("🏛️"),
                                                 QStringLiteral("#4CA1FF"));
        QVERIFY(!list.isEmpty());
        TaskPanel panel(m_store.get()); panel.resize(900, 640); panel.show();
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        QVERIFY(navigation);
        const auto itemFor = [navigation](const QString& folderId, const QString& listId) {
            for (int row = 0; row < navigation->count(); ++row) {
                QListWidgetItem* item = navigation->item(row);
                if (!folderId.isEmpty()
                    && item->data(NavigationDelegate::FolderIdRole).toString() == folderId)
                    return item;
                if (!listId.isEmpty() && item->data(Qt::UserRole + 1).toString() == listId)
                    return item;
            }
            return static_cast<QListWidgetItem*>(nullptr);
        };
        QListWidgetItem* folderItem = itemFor(folder, QString());
        QListWidgetItem* listItem = itemFor(QString(), list);
        QVERIFY(folderItem && listItem);
        QVERIFY(!listItem->isHidden());
        QVERIFY(folderItem->data(NavigationDelegate::FolderExpandedRole).toBool());
        const QRect folderRect = navigation->visualItemRect(folderItem);
        QTest::mouseClick(navigation->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(folderRect.left() + 18, folderRect.center().y()));
        QTRY_VERIFY((listItem = itemFor(QString(), list)) != nullptr);
        QTRY_VERIFY(listItem->isHidden());
        QVERIFY(!itemFor(folder, QString())->data(NavigationDelegate::FolderExpandedRole).toBool());

        folderItem = itemFor(folder, QString());
        const QRect collapsedRect = navigation->visualItemRect(folderItem);
        QTest::mouseClick(navigation->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(collapsedRect.left() + 18, collapsedRect.center().y()));
        QTRY_VERIFY((listItem = itemFor(QString(), list)) != nullptr);
        QTRY_VERIFY(!listItem->isHidden());
    }
    void navigationGeometryAndFolderTypography() {
        NavigationDelegate delegate;
        QListWidget navigation;
        auto* top = new QListWidgetItem(&navigation);
        top->setData(NavigationDelegate::IconRole, static_cast<int>(IconId::Folder));
        auto* folder = new QListWidgetItem(&navigation);
        folder->setData(NavigationDelegate::FolderIdRole, QStringLiteral("folder"));
        folder->setData(NavigationDelegate::FolderExpandedRole, true);
        folder->setData(NavigationDelegate::IconRole, static_cast<int>(IconId::Folder));
        auto* child = new QListWidgetItem(&navigation);
        child->setData(NavigationDelegate::ParentFolderRole, QStringLiteral("folder"));
        child->setData(NavigationDelegate::IndentRole, NavigationGeometry::FolderChildIndent);
        child->setData(NavigationDelegate::IconRole, static_cast<int>(IconId::Folder));
        const auto render = [&navigation, &delegate](QListWidgetItem* item) {
            QImage image(220, 36, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            QStyleOptionViewItem option;
            option.rect = QRect(0, 0, 220, 36);
            option.state = QStyle::State_Enabled;
            delegate.paint(&painter, option, navigation.model()->index(navigation.row(item), 0));
            painter.end();
            return image;
        };
        const int iconLeft = NavigationGeometry::RowHorizontalInset
            + NavigationGeometry::ListIconOffset;
        // 普通行与文件夹行使用同一个 20px 图标盒，箭头不影响这条左轴。
        const QImage topIcon = render(top);
        const QImage folderIcon = render(folder);
        QCOMPARE(iconLeft, NavigationGeometry::TitleLeft);
        QCOMPARE(NavigationGeometry::FolderDisclosureWidth, 16);
        // 箭头与图标分别占据左侧区域和 14px 左轴，不再相互重叠。
        QVERIFY(NavigationGeometry::DisclosureLeft
                + NavigationGeometry::DisclosureGlyphSize < iconLeft);
        int arrowInk = 0;
        for (int y = 12; y < 25; ++y)
            for (int x = 0; x < 10; ++x)
                if (folderIcon.pixelColor(x, y).alpha() > 0) ++arrowInk;
        QVERIFY(arrowInk > 0);
        QVERIFY(QRectF(10, 10, 16, 16).contains(QPointF(17, 18)));

        // 子清单缩进 22px，使用 16px 图形；所有导航图标下移 1px。
        QCOMPARE(NavigationGeometry::FolderChildIndent, 26 - 4);
        QImage expectedChild(220, 36, QImage::Format_ARGB32_Premultiplied);
        expectedChild.fill(Qt::transparent);
        QPainter childPainter(&expectedChild);
        childPainter.setRenderHint(QPainter::Antialiasing, true);
        const qreal childGlyphLeft = iconLeft + NavigationGeometry::FolderChildIndent
            + NavigationGeometry::ChildIconGlyphInset;
        const qreal childGlyphTop = 18.0 - NavigationGeometry::IconBoxSize / 2.0
            + NavigationGeometry::ChildIconGlyphInset
            + NavigationGeometry::IconGlyphVerticalOffset;
        AppIcons::paint(&childPainter, IconId::Folder,
                        QRectF(childGlyphLeft, childGlyphTop,
                               NavigationGeometry::ChildIconGlyphSize,
                               NavigationGeometry::ChildIconGlyphSize),
                        AppStyle::color(AppStyle::Color::Muted));
        childPainter.end();
        QVERIFY(render(child) == expectedChild);

        // 文件夹与清单一样使用 14px 常规字重，不再使用小号 DemiBold。
        folder->setData(NavigationDelegate::IconRole, static_cast<int>(IconId::None));
        folder->setText(QStringLiteral("市人民检察院"));
        const QImage actualFolder = render(folder);
        QImage expectedFolder(220, 36, QImage::Format_ARGB32_Premultiplied);
        expectedFolder.fill(Qt::transparent);
        QPainter folderPainter(&expectedFolder);
        folderPainter.setFont(AppStyle::font(AppStyle::Font::Body));
        folderPainter.setPen(AppStyle::color(AppStyle::Color::Text));
        const int folderLabelLeft = iconLeft + NavigationGeometry::IconBoxSize
            + NavigationGeometry::IconLabelGap;
        folderPainter.drawText(QRectF(folderLabelLeft, 0, 130, 36),
                               Qt::AlignLeft | Qt::AlignVCenter, folder->text());
        folderPainter.end();
        QVERIFY(actualFolder.copy(folderLabelLeft, 0, 130, 36)
                == expectedFolder.copy(folderLabelLeft, 0, 130, 36));
        QCOMPARE(folderLabelLeft, iconLeft + NavigationGeometry::FolderChildIndent + 4);

        QCOMPARE(ds::pointSize(ds::TypeScale::Body), 10.5);
        QCOMPARE(ds::pointSize(ds::TypeScale::Menu), 9.75);
        QCOMPARE(ds::qfont(ds::FontScene::Body).weight(), static_cast<int>(QFont::Normal));
        QVERIFY(ds::buildStyleSheet(ds::Theme::Light).contains(
            QStringLiteral("QMenu{font-size:9.75pt;}")));
        TaskPanel panel(m_store.get()); panel.resize(900, 640); panel.show();
        auto* navigationView = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        auto* header = panel.findChild<QFrame*>(QStringLiteral("ListSectionHeader"));
        QVERIFY(navigationView && header);
        const auto labels = header->findChildren<QLabel*>();
        QCOMPARE(labels.size(), 1);
        QCOMPARE(labels.first()->mapTo(navigationView->viewport(), QPoint()).x(), iconLeft);
    }
    void listSectionDisclosureCollapsesOrganization() {
        const QString folder = m_store->createFolder(QStringLiteral("折叠文件夹"));
        const QString child = m_store->createList(QStringLiteral("子清单"), folder);
        const QString top = m_store->createList(QStringLiteral("顶层清单"));
        QVERIFY(!child.isEmpty() && !top.isEmpty());
        TaskPanel panel(m_store.get()); panel.resize(900, 640); panel.show();
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        auto* header = panel.findChild<SidebarSectionHeader*>(QStringLiteral("ListSectionHeader"));
        QVERIFY(navigation && header);
        auto* disclosure = header->disclosureButton();
        QVERIFY(disclosure);
        QCOMPARE(disclosure->x(), NavigationGeometry::DisclosureLeft);
        const auto organizationVisible = [navigation]() {
            for (int row = 0; row < navigation->count(); ++row) {
                const auto* item = navigation->item(row);
                if (item->data(NavigationDelegate::SectionRole).toBool()
                    || item->data(NavigationDelegate::DividerRole).toBool()) continue;
                const auto scope = static_cast<ViewScope>(item->data(Qt::UserRole).toInt());
                if (scope == ViewScope::List || scope == ViewScope::Folder)
                    if (!item->isHidden()) return true;
            }
            return false;
        };
        QVERIFY(organizationVisible());
        header->setExpanded(true);
        QTest::mouseClick(disclosure, Qt::LeftButton);
        QTRY_VERIFY(!organizationVisible());
        disclosure = panel.findChild<SidebarSectionHeader*>(
            QStringLiteral("ListSectionHeader"))->disclosureButton();
        QTest::mouseClick(disclosure, Qt::LeftButton);
        QTRY_VERIFY(organizationVisible());
    }
    void sidebarSectionHeaderUsesOrdinaryRowHover() {
        TaskPanel panel(m_store.get()); panel.resize(900, 640); panel.show();
        auto* header = panel.findChild<QFrame*>(QStringLiteral("ListSectionHeader"));
        QVERIFY(header);
        auto* card = header->findChild<QFrame*>(QStringLiteral("SidebarSectionCard"));
        QVERIFY(card);
        QVERIFY(!card->graphicsEffect());
        QEvent enter(QEvent::Enter);
        QApplication::sendEvent(header, &enter);
        QApplication::processEvents();
        QVERIFY(card->property("hovered").toBool());
        const QImage hovered = card->grab().toImage();
        QCOMPARE(hovered.pixelColor(3, hovered.height() / 2),
                 ThemeManager::instance().color(ds::ColorToken::RowHover));
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(header, &leave);
    }
    void rowStatesUseSoftBackgroundsWithoutBorders() {
        const QString list = m_store->createList(QStringLiteral("状态样式"), QString(),
                                                 EntityKind::Task, QStringLiteral("💼"),
                                                 QStringLiteral("#4CA1FF"));
        QVERIFY(!list.isEmpty());
        const QString selectedId = m_store->createTask(QStringLiteral("选中样式"), list);
        Entity selectedTask = m_store->entity(selectedId);
        selectedTask.allDayDate = toAppTime(appNow()).date();
        QVERIFY(m_store->saveTask(selectedTask));
        const QString hoveredId = m_store->createTask(QStringLiteral("悬浮样式"), list);
        QVERIFY(!hoveredId.isEmpty());

        TaskPanel panel(m_store.get()); panel.resize(900, 640); panel.show();
        auto* view = panel.findChild<TaskListView*>(QStringLiteral("TaskList"));
        auto* model = qobject_cast<TaskListModel*>(view->model());
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        QVERIFY(view && model && navigation);
        auto* taskDelegate = qobject_cast<TaskDelegate*>(view->itemDelegate());
        auto* navDelegate = qobject_cast<NavigationDelegate*>(navigation->itemDelegate());
        QVERIFY(taskDelegate && navDelegate);
        const QColor selectedColor = ThemeManager::instance().color(ds::ColorToken::RowSelected);
        const QColor hoverColor = ThemeManager::instance().color(ds::ColorToken::RowHover);
        const auto render = [](QAbstractItemDelegate* delegate, const QModelIndex& index,
                               const QSize& size, QStyle::State state) {
            QImage image(size, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::white);
            QPainter painter(&image);
            QStyleOptionViewItem option;
            option.rect = QRect(QPoint(0, 0), size);
            option.state = QStyle::State_Enabled | state;
            delegate->paint(&painter, option, index);
            painter.end();
            return image;
        };

        const QModelIndex selectedIndex = model->indexForTask(selectedId);
        QVERIFY(selectedIndex.isValid());
        QImage image = render(taskDelegate, selectedIndex, QSize(600, 44), QStyle::State_Selected);
        // 任务行背景从左侧 20px 开始；左边缘与内部都是选中底色，没有强调色边框。
        QCOMPARE(image.pixelColor(21, 22), selectedColor);
        QCOMPARE(image.pixelColor(30, 4), selectedColor);
        image = render(taskDelegate, model->indexForTask(hoveredId), QSize(600, 44),
                       QStyle::State_MouseOver);
        QCOMPARE(image.pixelColor(21, 22), hoverColor);
        QCOMPARE(image.pixelColor(30, 4), hoverColor);

        int allRow = -1;
        for (int row = 0; row < navigation->count(); ++row) {
            if (!navigation->item(row)->data(NavigationDelegate::SectionRole).toBool()
                && navigation->item(row)->data(Qt::UserRole).toInt()
                       == static_cast<int>(ViewScope::AllLists)) allRow = row;
        }
        QVERIFY(allRow >= 0);
        const QModelIndex navIndex = navigation->model()->index(allRow, 0);
        QVERIFY(navIndex.isValid());
        QImage navImage = render(navDelegate, navIndex, QSize(220, 36), QStyle::State_Selected);
        QCOMPARE(navImage.pixelColor(11, 18), selectedColor);
        QCOMPARE(navImage.pixelColor(30, 4), selectedColor);
        navImage = render(navDelegate, navIndex, QSize(220, 36), QStyle::State_MouseOver);
        QCOMPARE(navImage.pixelColor(11, 18), hoverColor);
    }
    void workspaceColumnsUseOwnBackgroundTokens() {
        const QString list = m_store->createList(QStringLiteral("列底色"), QString(),
                                                 EntityKind::Task, QStringLiteral("💼"),
                                                 QStringLiteral("#4CA1FF"));
        const QString id = m_store->createTask(QStringLiteral("列底色任务"), list);
        Entity task = m_store->entity(id);
        task.allDayDate = toAppTime(appNow()).date();
        QVERIFY(m_store->saveTask(task));

        TaskPanel panel(m_store.get()); panel.resize(1200, 780); panel.show();
        auto* view = panel.findChild<TaskListView*>(QStringLiteral("TaskList"));
        auto* model = qobject_cast<TaskListModel*>(view->model());
        auto* rail = panel.findChild<QFrame*>(QStringLiteral("ActivityRail"));
        auto* sidebar = panel.findChild<QFrame*>(QStringLiteral("Sidebar"));
        auto* listPane = panel.findChild<QFrame*>(QStringLiteral("ListPane"));
        auto* detail = panel.findChild<QFrame*>(QStringLiteral("DetailFrame"));
        QVERIFY(view && model && rail && sidebar && listPane && detail);
        view->setCurrentIndex(model->indexForTask(id));
        QTest::qWait(30);

        const auto sample = [](QWidget* widget, const QPoint& point) {
            const QImage image = widget->grab().toImage();
            const qreal dpr = image.devicePixelRatio();
            return image.pixelColor(qRound(point.x() * dpr), qRound(point.y() * dpr));
        };
        const auto verify = [&](ThemeManager::Preference preference) {
            ThemeManager::instance().setPreference(preference);
            QApplication::processEvents();
            const auto& theme = ThemeManager::instance();
            QCOMPARE(sample(rail, QPoint(rail->width() / 2, rail->height() / 2)),
                     theme.color(ds::ColorToken::RailBg));
            QCOMPARE(sample(sidebar, QPoint(sidebar->width() / 2, sidebar->height() - 24)),
                     theme.color(ds::ColorToken::Sidebar));
            QCOMPARE(sample(listPane, QPoint(listPane->width() / 2, listPane->height() - 30)),
                     theme.color(ds::ColorToken::Surface));
            QCOMPARE(sample(detail, QPoint(detail->width() - 24, detail->height() / 2)),
                     theme.color(ds::ColorToken::Surface));
        };
        verify(ThemeManager::Preference::Light);
        verify(ThemeManager::Preference::Dark);
    }
    void windowShadowRemovedInDarkTheme() {
        TaskPanel panel(m_store.get()); panel.resize(900, 700); panel.show();
        QTest::qWait(50);
        auto* surface = panel.findChild<QWidget*>(QStringLiteral("WorkspaceRoot"));
        QVERIFY(surface);
        const auto marginAlpha = [surface]() {
            const QImage image = surface->grab().toImage();
            const qreal dpr = image.devicePixelRatio();
            const int x = qRound(18 * dpr), y = qRound(surface->height() / 2.0 * dpr);
            return qAlpha(image.pixel(x, y));
        };
        ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
        QApplication::processEvents();
        QVERIFY(marginAlpha() > 0);
        ThemeManager::instance().setPreference(ThemeManager::Preference::Dark);
        QApplication::processEvents();
        QCOMPARE(marginAlpha(), 0);
    }
    void subtaskDisclosureUsesSectionTriangleStyle() {
        const QString parent = m_store->createTask(QStringLiteral("父任务样式"));
        const QString child = m_store->createSubtask(QStringLiteral("子任务样式"), parent);
        QVERIFY(!child.isEmpty());
        TaskListModel model(m_store.get());
        TaskDelegate delegate;
        const auto render = [&](bool collapsed) {
            QModelIndex index = model.indexForTask(parent);
            if (model.data(index, TaskListModel::ChildrenCollapsedRole).toBool() != collapsed)
                model.toggleChildren(parent);
            index = model.indexForTask(parent);
            QImage image(560, 44, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::white);
            QPainter painter(&image);
            QStyleOptionViewItem option;
            option.rect = QRect(0, 0, 560, 44);
            option.state = QStyle::State_Enabled;
            delegate.paint(&painter, option, index);
            painter.end();
            return image;
        };
        const auto arrowBounds = [](const QImage& image) {
            QRect bounds;
            for (int y = 16; y < 28; ++y) {
                for (int x = 22; x < 34; ++x) {
                    if (image.pixelColor(x, y) == QColor(Qt::white)) continue;
                    bounds = bounds.isNull() ? QRect(x, y, 1, 1) : bounds.united(QRect(x, y, 1, 1));
                }
            }
            return bounds;
        };
        const QRect expanded = arrowBounds(render(false));
        const QRect collapsed = arrowBounds(render(true));
        QVERIFY(!expanded.isNull() && !collapsed.isNull());
        QVERIFY2(expanded.width() > expanded.height() * 1.3,
                 qPrintable(QStringLiteral("expanded arrow %1x%2").arg(expanded.width()).arg(expanded.height())));
        QVERIFY2(collapsed.height() > collapsed.width() * 1.3,
                 qPrintable(QStringLiteral("collapsed arrow %1x%2").arg(collapsed.width()).arg(collapsed.height())));
    }
    void navigationListShowsColorDotAndStableIcon() {
        const QString list = m_store->createList(QStringLiteral("彩色清单"), QString(),
                                                 EntityKind::Task, QString(),
                                                 QStringLiteral("#FFAC38"));
        QVERIFY(!list.isEmpty());
        QVERIFY(!m_store->createTask(QStringLiteral("计数任务"), list).isEmpty());
        TaskPanel panel(m_store.get()); panel.resize(900, 640); panel.show();
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        auto* delegate = navigation ? qobject_cast<NavigationDelegate*>(navigation->itemDelegate()) : nullptr;
        QVERIFY(navigation && delegate);
        int row = -1;
        for (int candidate = 0; candidate < navigation->count(); ++candidate) {
            if (navigation->item(candidate)->data(Qt::UserRole + 1).toString() == list) {
                row = candidate;
                break;
            }
        }
        QVERIFY(row >= 0);
        QImage image(220, 36, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QPainter painter(&image);
        QStyleOptionViewItem option;
        option.rect = QRect(0, 0, 220, 36);
        option.state = QStyle::State_Enabled;
        delegate->paint(&painter, option, navigation->model()->index(row, 0));
        painter.end();
        const auto isListColor = [](const QColor& pixel) {
            return pixel.red() > 200 && pixel.green() > 120 && pixel.green() < 210
                && pixel.blue() < 110;
        };
        int dotPixels = 0, iconPixels = 0;
        for (int y = 10; y < 26; ++y) {
            for (int x = 150; x < 205; ++x)
                if (isListColor(image.pixelColor(x, y))) ++dotPixels;
            for (int x = 20; x < 45; ++x)
                if (isListColor(image.pixelColor(x, y))) ++iconPixels;
        }
        QVERIFY2(dotPixels > 10, qPrintable(QStringLiteral("dot pixels: %1").arg(dotPixels)));
        QCOMPARE(iconPixels, 0);  // 图标保持中性色，不再被清单颜色染色。
    }
    void listAppearanceAndArchiveNavigation() {
        const QString folder = m_store->createFolder(QStringLiteral("工作文件夹"));
        const QString list = m_store->createList(QStringLiteral("工作"), folder, EntityKind::Task,
                                                 QStringLiteral("🏛️"), QStringLiteral("#4CA1FF"));
        const QString id = m_store->createTask(QStringLiteral("过期任务"), list);
        Entity task = m_store->entity(id); task.allDayDate = toAppTime(appNow()).date().addDays(-1);
        QVERIFY(m_store->saveTask(task));
        TaskPanel panel(m_store.get()); panel.show();
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        auto* model = qobject_cast<TaskListModel*>(
            panel.findChild<QListView*>(QStringLiteral("TaskList"))->model());
        QVERIFY(navigation && model);
        const auto rowForList = [navigation](const QString& listId) {
            for (int row = 0; row < navigation->count(); ++row)
                if (navigation->item(row)->data(Qt::UserRole + 1).toString() == listId) return row;
            return -1;
        };
        int row = rowForList(list);
        QVERIFY(row >= 0);
        QCOMPARE(navigation->item(row)->data(NavigationDelegate::IconNameRole).toString(),
                 QStringLiteral("🏛️"));
        QCOMPARE(navigation->item(row)->data(NavigationDelegate::ListColorRole).toString(),
                 QStringLiteral("#4CA1FF"));
        QModelIndex taskIndex = model->indexForTask(id);
        QVERIFY(taskIndex.isValid());
        QCOMPARE(model->data(taskIndex, TaskListModel::ListNameRole).toString(), QStringLiteral("工作"));
        QCOMPARE(model->data(taskIndex, TaskListModel::ListColorRole).toString(),
                 QStringLiteral("#4CA1FF"));
        QCOMPARE(model->data(taskIndex, TaskListModel::ScheduleRole).toString(), QStringLiteral("昨天"));
        TaskList appearance;
        for (const TaskList& candidate : m_store->lists()) if (candidate.id == list) appearance = candidate;
        QVERIFY(!ListAppearance::icon(appearance, 18, QColor()).isNull());

        QVERIFY(m_store->setListArchived(list, true));
        QTRY_VERIFY(!model->indexForTask(id).isValid());
        QTRY_VERIFY(rowForList(list) >= 0);
        QVERIFY(navigation->item(rowForList(list))->isHidden());
        auto* toggle = panel.findChild<QToolButton*>(QStringLiteral("ArchiveToggleButton"));
        QVERIFY(toggle);
        QTest::mouseClick(toggle, Qt::LeftButton);
        QTRY_VERIFY(!navigation->item(rowForList(list))->isHidden());
        navigation->setCurrentRow(rowForList(list));
        QTRY_COMPARE(model->filter().scope, ViewScope::List);
        QTRY_VERIFY(model->indexForTask(id).isValid());
        QVERIFY(m_store->setListArchived(list, false));
        QTRY_VERIFY(rowForList(list) >= 0);
        QVERIFY(!navigation->item(rowForList(list))->isHidden());
    }
    void navigationDividersAndContinuousListColors() {
        const QString red = m_store->createList(QStringLiteral("红色清单"), QString(),
            EntityKind::Task, QString(), QStringLiteral("#EF6161"));
        const QString blue = m_store->createList(QStringLiteral("蓝色清单"), QString(),
            EntityKind::Task, QString(), QStringLiteral("#4CA1FF"));
        const QString first = m_store->createTask(QStringLiteral("第一项"), red);
        const QString second = m_store->createTask(QStringLiteral("第二项"), blue);
        TaskPanel panel(m_store.get()); panel.show();
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        QVERIFY(navigation);
        QStringList topLabels;
        int dividerCount = 0, overdueRow = -1, completedRow = -1;
        for (int row = 0; row < navigation->count(); ++row) {
            const auto* item = navigation->item(row);
            QVERIFY(item->data(Qt::DisplayRole).toString() != QStringLiteral("智能清单"));
            QVERIFY(item->data(Qt::DisplayRole).toString() != QStringLiteral("历史"));
            if (item->data(NavigationDelegate::DividerRole).toBool()) {
                ++dividerCount;
                if (!item->data(NavigationDelegate::OrganizationEndRole).toBool()) {
                    auto* navDelegate = qobject_cast<NavigationDelegate*>(navigation->itemDelegate());
                    QVERIFY(navDelegate);
                    QImage separator(220, 33, QImage::Format_ARGB32_Premultiplied);
                    separator.fill(Qt::white);
                    QPainter separatorPainter(&separator);
                    QStyleOptionViewItem separatorOption;
                    separatorOption.rect = QRect(0, 0, 220, 33);
                    navDelegate->paint(&separatorPainter, separatorOption,
                                       navigation->model()->index(row, 0));
                    separatorPainter.end();
                    QCOMPARE(separator.pixelColor(110, 16),
                             ThemeManager::instance().color(ds::ColorToken::Divider));
                }
                QVERIFY(item->data(Qt::DisplayRole).toString().isEmpty());
                QCOMPARE(item->data(Qt::UserRole).toInt(), -1);
                QCOMPARE(item->sizeHint().height(), 33);
                continue;
            }
            if (item->data(NavigationDelegate::SectionRole).toBool()) continue;
            if (topLabels.size() < 5) topLabels.append(item->text());
            if (item->data(Qt::UserRole).toInt() == static_cast<int>(ViewScope::Overdue)) overdueRow = row;
            if (item->data(Qt::UserRole).toInt() == static_cast<int>(ViewScope::Completed)) completedRow = row;
        }
        QCOMPARE(topLabels, QStringList({QStringLiteral("所有"), QStringLiteral("今天"),
            QStringLiteral("明天"), QStringLiteral("最近7天"), QStringLiteral("收集箱")}));
        QCOMPARE(dividerCount, 2);
        QCOMPARE(overdueRow, -1);  // 独立“已过期”筛选入口已移除，列表内逾期分组保留。
        QVERIFY(completedRow >= 0);

        auto* model = qobject_cast<TaskListModel*>(
            panel.findChild<QListView*>(QStringLiteral("TaskList"))->model());
        QVERIFY(model);
        TaskDelegate delegate;
        QImage rows(400, 80, QImage::Format_ARGB32);
        rows.fill(Qt::white);
        QPainter painter(&rows);
        const QModelIndex indexes[] = {model->indexForTask(first), model->indexForTask(second)};
        const QColor colors[] = {QColor(QStringLiteral("#EF6161")), QColor(QStringLiteral("#4CA1FF"))};
        for (int row = 0; row < 2; ++row) {
            QVERIFY(indexes[row].isValid());
            QStyleOptionViewItem option;
            option.rect = QRect(0, row * 40, 400, 40);
            option.state = QStyle::State_Enabled | (row == 0 ? QStyle::State_Selected : QStyle::State_None);
            delegate.paint(&painter, option, indexes[row]);
            QCOMPARE(delegate.sizeHint(option, indexes[row]).height(), 40);
        }
        painter.end();
        for (int y = 0; y < 80; ++y) {
            const QColor expected = colors[y < 40 ? 0 : 1];
            for (int x = 16; x < 19; ++x)
                QCOMPARE(QColor::fromRgba(rows.pixel(x, y)), expected);
            for (int x = 0; x < 16; ++x)
                QVERIFY(QColor::fromRgba(rows.pixel(x, y)) != expected);
        }
        const QString screenshotDir = qEnvironmentVariable("KYLINLIST_SCREENSHOT_DIR");
        if (!screenshotDir.isEmpty()) {
            QVERIFY(QDir().mkpath(screenshotDir));
            QVERIFY(navigation->grab().save(screenshotDir + QStringLiteral("/navigation-dividers.png")));
            QVERIFY(panel.findChild<QListView*>(QStringLiteral("TaskList"))->grab().save(
                screenshotDir + QStringLiteral("/task-list-colors.png")));
        }
    }
    void taskRowDelegateMetaActions() {
        const QString list = m_store->createList(QStringLiteral("工作"), QString(), EntityKind::Task,
                                                 QStringLiteral("🏛️"), QStringLiteral("#4CA1FF"));
        const QString id = m_store->createTask(QStringLiteral("元信息任务"), list);
        Entity task = m_store->entity(id); task.allDayDate = toAppTime(appNow()).date().addDays(-1);
        QVERIFY(m_store->saveTask(task));
        TaskListModel model(m_store.get());
        const QModelIndex taskIndex = model.indexForTask(id);
        QVERIFY(taskIndex.isValid());
        QModelIndex overdueHeader;
        for (int row = 0; row < model.rowCount(); ++row) {
            const QModelIndex index = model.index(row, 0);
            if (model.data(index, TaskListModel::HeaderRole).toBool()
                && model.data(index, TaskListModel::SectionKeyRole).toString() == QLatin1String("overdue"))
                overdueHeader = index;
        }
        QVERIFY(overdueHeader.isValid());
        QCOMPARE(model.data(overdueHeader, TaskListModel::HeaderActionRole).toString(),
                 QStringLiteral("postpone"));
        QVERIFY(model.data(overdueHeader, TaskListModel::HeaderEntityIdsRole).toStringList().contains(id));

        TaskDelegate delegate;
        QSignalSpy dateSpy(&delegate, &TaskDelegate::dateActivated);
        QSignalSpy listSpy(&delegate, &TaskDelegate::listActivated);
        QSignalSpy postponeSpy(&delegate, &TaskDelegate::postponeActivated);
        QStyleOptionViewItem option; option.rect = QRect(0, 0, 560, 40);
        const QFontMetrics metrics(AppStyle::font(AppStyle::Font::Small));
        const qreal dateWidth = qMin(112.0, metrics.horizontalAdvance(QStringLiteral("昨天")) + 8.0);
        const qreal listWidth = qMin(72.0, metrics.horizontalAdvance(QStringLiteral("工作")) + 0.0) + 27.0;
        QMouseEvent dateEvent(QEvent::MouseButtonRelease,
                              QPointF(560 - 12 - dateWidth / 2, 20),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QVERIFY(delegate.editorEvent(&dateEvent, &model, option, taskIndex));
        QMouseEvent listEvent(QEvent::MouseButtonRelease,
                              QPointF(560 - 12 - dateWidth - 8 - listWidth / 2, 20),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QVERIFY(delegate.editorEvent(&listEvent, &model, option, taskIndex));
        QMouseEvent headerEvent(QEvent::MouseButtonRelease, QPointF(525, 18),
                                Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QVERIFY(delegate.editorEvent(&headerEvent, &model, option, overdueHeader));
        QCOMPARE(dateSpy.count(), 1); QCOMPARE(dateSpy.first().first().toString(), id);
        QCOMPARE(listSpy.count(), 1); QCOMPARE(listSpy.first().first().toString(), list);
        QCOMPARE(postponeSpy.count(), 1);
        QVERIFY(postponeSpy.first().first().toStringList().contains(id));

        ViewSpec direct; direct.scope = ViewScope::List; direct.listId = list;
        model.setFilter(direct);
        bool postponeHidden = true;
        for (int row = 0; row < model.rowCount(); ++row) {
            const QModelIndex index = model.index(row, 0);
            if (model.data(index, TaskListModel::HeaderRole).toBool()
                && model.data(index, TaskListModel::SectionKeyRole).toString() == QLatin1String("overdue"))
                postponeHidden = model.data(index, TaskListModel::HeaderActionRole).toString().isEmpty();
        }
        QVERIFY(postponeHidden);
    }
    void taskHierarchyGeometryAndCollapsing() {
        const QString list = m_store->createList(QStringLiteral("层级清单"), QString(),
            EntityKind::Task, QString(), QStringLiteral("#4CA1FF"));
        const QString parent = m_store->createTask(QStringLiteral("父任务"), list);
        Entity parentValue = m_store->entity(parent);
        parentValue.allDayDate = toAppTime(appNow()).date();
        QVERIFY(m_store->saveTask(parentValue));
        const QString child = m_store->createSubtask(QStringLiteral("子任务"), parent);
        Entity childValue = m_store->entity(child);
        childValue.allDayDate = toAppTime(appNow()).date().addDays(1);
        QVERIFY(m_store->saveTask(childValue));
        m_store->createTask(QStringLiteral("独立任务"), list);

        TaskListModel model(m_store.get());
        QModelIndex parentIndex = model.indexForTask(parent);
        QModelIndex childIndex = model.indexForTask(child);
        QVERIFY(parentIndex.isValid() && childIndex.isValid());
        QCOMPARE(childIndex.row(), parentIndex.row() + 1);
        QCOMPARE(model.data(parentIndex, TaskListModel::HasChildrenRole).toBool(), true);
        QCOMPARE(model.data(childIndex, TaskListModel::DepthRole).toInt(), 1);
        QCOMPARE(model.data(childIndex, TaskListModel::SectionKeyRole).toString(),
                 model.data(parentIndex, TaskListModel::SectionKeyRole).toString());
        const int entityCount = model.entityCount();
        QVERIFY(m_store->setPinnedAt(parent, appNow()));
        QTRY_COMPARE(model.data(model.indexForTask(parent), TaskListModel::SectionKeyRole).toString(),
                     QStringLiteral("pinned"));
        int childOccurrences = 0;
        for (int row = 0; row < model.rowCount(); ++row)
            if (model.entityId(model.index(row, 0)) == child) ++childOccurrences;
        QCOMPARE(childOccurrences, 1);
        QCOMPARE(model.data(model.indexForTask(child), TaskListModel::SectionKeyRole).toString(),
                 QStringLiteral("pinned"));
        QCOMPARE(model.entityCount(), entityCount);
        parentIndex = model.indexForTask(parent);
        childIndex = model.indexForTask(child);
        QCOMPARE(childIndex.row(), parentIndex.row() + 1);
        model.toggleChildren(parent);
        QVERIFY(!model.indexForTask(child).isValid());
        QCOMPARE(model.entityCount(), entityCount);
        model.toggleChildren(parent);
        parentIndex = model.indexForTask(parent);
        childIndex = model.indexForTask(child);
        QVERIFY(childIndex.isValid());

        TaskDelegate delegate;
        const auto render = [&delegate](const QModelIndex& index) {
            QImage image(560, 40, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            QStyleOptionViewItem option;
            option.rect = QRect(0, 0, 560, 40);
            option.state = QStyle::State_Enabled | QStyle::State_MouseOver;
            delegate.paint(&painter, option, index);
            painter.end();
            return image;
        };
        const QImage parentImage = render(parentIndex);
        const QImage childImage = render(childIndex);
        const QColor hover = ThemeManager::instance().color(ds::ColorToken::RowHover);
        QCOMPARE(QColor::fromRgba(parentImage.pixel(21, 20)), hover);
        QCOMPARE(QColor::fromRgba(childImage.pixel(45, 20)), hover);
        QCOMPARE(qAlpha(childImage.pixel(43, 20)), 0);
        const QColor listColor(QStringLiteral("#4CA1FF"));
        QCOMPARE(QColor::fromRgba(parentImage.pixel(16, 20)), listColor);
        QCOMPARE(QColor::fromRgba(childImage.pixel(16, 20)), listColor);

        const int originalEntityCount = model.entityCount();
        parentValue = m_store->entity(parent);
        parentValue.allDayDate = toAppTime(appNow()).date().addDays(1);
        QVERIFY(m_store->saveTask(parentValue));
        childValue = m_store->entity(child);
        childValue.allDayDate = toAppTime(appNow()).date();
        QVERIFY(m_store->saveTask(childValue));
        QVERIFY(m_store->setPinnedAt(parent, QDateTime()));
        ViewSpec next7; next7.scope = ViewScope::Next7Days; next7.renderer = ViewRenderer::Time;
        model.setFilter(next7);
        QTRY_COMPARE(model.data(model.indexForTask(parent), TaskListModel::SectionKeyRole).toString(),
                     QStringLiteral("tomorrow"));
        QCOMPARE(model.data(model.indexForTask(child), TaskListModel::SectionKeyRole).toString(),
                 QStringLiteral("tomorrow"));
        childOccurrences = 0;
        for (int row = 0; row < model.rowCount(); ++row)
            if (model.entityId(model.index(row, 0)) == child) ++childOccurrences;
        QCOMPARE(childOccurrences, 1); // 子段排在父段之前也只能放进父级子树一次。
        const int filteredEntityCount = model.entityCount();
        QCOMPARE(filteredEntityCount, 2); // 无日期的独立任务不进入最近 7 天。

        const QDateTime childPin = appNow();
        QVERIFY(m_store->setPinnedAt(child, childPin));
        QCOMPARE(model.data(model.indexForTask(child), TaskListModel::SectionKeyRole).toString(),
                 QStringLiteral("tomorrow")); // 独立置顶的孩子仍归属于可见父级，不重复显示。
        QCOMPARE(model.indexForTask(child).row(), model.indexForTask(parent).row() + 1);
        childOccurrences = 0;
        for (int row = 0; row < model.rowCount(); ++row)
            if (model.entityId(model.index(row, 0)) == child) ++childOccurrences;
        QCOMPARE(childOccurrences, 1);
        QCOMPARE(model.entityCount(), filteredEntityCount);

        const QString tomorrowPeer = m_store->createTask(QStringLiteral("明日独立任务"), list);
        Entity peerValue = m_store->entity(tomorrowPeer);
        peerValue.allDayDate = toAppTime(appNow()).date().addDays(1);
        QVERIFY(m_store->saveTask(peerValue));
        const int parentRow = model.indexForTask(parent).row();
        const int childRow = model.indexForTask(child).row();
        QVERIFY(parentRow >= 0 && childRow > parentRow);
        model.setDragActive(true, parent);
        QCOMPARE(model.indexForTask(parent).row(), parentRow); // 空的“今天”不能在归组后插回父级之前。
        QCOMPARE(model.indexForTask(child).row(), childRow);
        model.setDragActive(false);
        QCOMPARE(model.indexForTask(parent).row(), parentRow);
        QCOMPARE(model.indexForTask(child).row(), childRow);
    }
    void taskViewDropCommands() {
        const QDate today = toAppTime(appNow()).date();
        const QString list = m_store->createList(QStringLiteral("时间拖动"));
        const QString todayTask = m_store->createTask(QStringLiteral("今天任务"), list);
        const QString pinCandidate = m_store->createTask(QStringLiteral("待置顶"), list);
        const auto setDate = [this](const QString& id, const QDate& date) {
            Entity value = m_store->entity(id); value.allDayDate = date;
            return m_store->saveTask(value);
        };
        QVERIFY(setDate(todayTask, today));
        QVERIFY(setDate(pinCandidate, today.addDays(1)));

        TaskPanel panel(m_store.get()); panel.show();
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        auto* view = panel.findChild<TaskListView*>(QStringLiteral("TaskList"));
        auto* model = qobject_cast<TaskListModel*>(view->model());
        QVERIFY(navigation && view && model);
        for (int row = 0; row < navigation->count(); ++row) {
            if (navigation->item(row)->data(Qt::UserRole).toInt()
                == static_cast<int>(ViewScope::Next7Days)) {
                navigation->setCurrentRow(row);
                break;
            }
        }
        QTRY_COMPARE(model->filter().scope, ViewScope::Next7Days);
        QTRY_COMPARE(model->filter().renderer, ViewRenderer::Time);
        const auto headerFor = [model](const QString& key) {
            for (int row = 0; row < model->rowCount(); ++row) {
                const QModelIndex index = model->index(row, 0);
                if (model->data(index, TaskListModel::HeaderRole).toBool()
                    && model->data(index, TaskListModel::SectionKeyRole).toString() == key)
                    return index;
            }
            return QModelIndex();
        };
        const auto drop = [view, model, headerFor](const QString& sourceId, const QModelIndex& target,
                                                    bool before = false, bool after = false) {
            QMimeData mime;
            QSignalSpy drops(view, &TaskListView::entityDropRequested);
            mime.setData(TaskListView::mimeType(), sourceId.toUtf8());
            const bool header = target.data(TaskListModel::HeaderRole).toBool();
            const QString targetId = target.data(TaskListModel::IdRole).toString();
            const QString targetKey = target.data(TaskListModel::SectionKeyRole).toString();
            const QPoint enterPoint = view->visualRect(model->indexForTask(sourceId)).center();
            QDragEnterEvent enter(enterPoint, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view->viewport(), &enter);
            // 拖动入口会重置模型；按 ID/分区重新解析目标，不保留旧 QModelIndex。
            const QModelIndex currentTarget = header ? headerFor(targetKey) : model->indexForTask(targetId);
            if (!currentTarget.isValid()) {
                QDragLeaveEvent leave; QApplication::sendEvent(view->viewport(), &leave);
                return false;
            }
            view->scrollTo(currentTarget, QAbstractItemView::EnsureVisible);
            QApplication::processEvents();
            const QRect rect = view->visualRect(currentTarget);
            const QPoint point(rect.center().x(),
                               before ? rect.top() + 2 : after ? rect.bottom() - 2 : rect.center().y());
            QDragMoveEvent move(point, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view->viewport(), &move);
            QDropEvent event(QPointF(point), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view->viewport(), &event);
            const bool delivered = drops.count() == 1 && drops.first().at(2).toString() == targetKey;
            if (!delivered)
                qDebug() << "unresolved drop" << sourceId << targetKey << targetId
                         << view->indexAt(point).data(TaskListModel::SectionKeyRole)
                         << move.isAccepted() << drops.count();
            return delivered;
        };
        const auto dropOnHeader = [view, model, headerFor](const QString& sourceId, const QString& key) {
            QMimeData mime;
            QSignalSpy drops(view, &TaskListView::entityDropRequested);
            mime.setData(TaskListView::mimeType(), sourceId.toUtf8());
            const QPoint enterPoint = view->visualRect(model->indexForTask(sourceId)).center();
            QDragEnterEvent enter(enterPoint, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view->viewport(), &enter);
            const QModelIndex header = headerFor(key);
            if (!header.isValid()) {
                QDragLeaveEvent leave; QApplication::sendEvent(view->viewport(), &leave);
                return false;
            }
            view->scrollTo(header, QAbstractItemView::EnsureVisible);
            QApplication::processEvents();
            const QPoint point = view->visualRect(header).center();
            QDragMoveEvent move(point, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view->viewport(), &move);
            if (!move.isAccepted()) {
                QDragLeaveEvent leave; QApplication::sendEvent(view->viewport(), &leave);
                return false;
            }
            QDropEvent event(QPointF(point), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view->viewport(), &event);
            return drops.count() == 1;
        };
        QTRY_VERIFY(!headerFor(QStringLiteral("pinned")).isValid());
        QVERIFY(dropOnHeader(todayTask, QStringLiteral("pinned")));
        QTRY_VERIFY(m_store->entity(todayTask).pinnedAt.isValid());
        QCOMPARE(m_store->entity(todayTask).allDayDate, today);
        QVERIFY(!headerFor(QStringLiteral("today")).isValid());
        QVERIFY(dropOnHeader(todayTask, QStringLiteral("today")));
        QTRY_VERIFY(!m_store->entity(todayTask).pinnedAt.isValid());
        QCOMPARE(m_store->entity(todayTask).allDayDate, today);

        const QString pinAnchor = m_store->createTask(QStringLiteral("置顶锚点"), list);
        QVERIFY(setDate(pinAnchor, today));
        QVERIFY(m_store->setPinnedAt(pinAnchor, appNow()));
        const QString overdueTask = m_store->createTask(QStringLiteral("过期锚点"), list);
        const QString day4 = m_store->createTask(QStringLiteral("第四天"), list);
        const QString day5 = m_store->createTask(QStringLiteral("第五天"), list);
        QVERIFY(setDate(overdueTask, today.addDays(-1)));
        QVERIFY(setDate(day4, today.addDays(4)));
        QVERIFY(setDate(day5, today.addDays(5)));
        QTRY_VERIFY(model->indexForTask(day5).isValid());
        QVERIFY(drop(todayTask, headerFor(QStringLiteral("overdue"))));
        QTRY_COMPARE(m_store->entity(todayTask).allDayDate, today.addDays(-1));

        const QString secondToday = m_store->createTask(QStringLiteral("第二个今天"), list);
        QVERIFY(setDate(secondToday, today));
        QTRY_VERIFY(model->indexForTask(secondToday).isValid());
        QVERIFY(drop(secondToday, headerFor(QStringLiteral("next7days"))));
        QTRY_COMPARE(m_store->entity(secondToday).allDayDate, today.addDays(2));

        QTRY_VERIFY(model->indexForTask(day5).isValid());
        QVERIFY(drop(day5, model->indexForTask(day4), true));
        QTRY_COMPARE(m_store->entity(day5).allDayDate, today.addDays(5));
        QCOMPARE(m_store->entity(day4).allDayDate, today.addDays(4));
        QTRY_VERIFY(model->indexForTask(day5).row() < model->indexForTask(day4).row());

        QVERIFY(drop(pinCandidate, model->indexForTask(pinAnchor)));
        QTRY_VERIFY(m_store->entity(pinCandidate).pinnedAt.isValid());
        QCOMPARE(m_store->entity(pinCandidate).allDayDate, today.addDays(1));
        // 今天已空，dragEnter 应临时提供空分区落点。
        QVERIFY(!headerFor(QStringLiteral("today")).isValid());
        QMimeData emptyMime;
        emptyMime.setData(TaskListView::mimeType(), pinCandidate.toUtf8());
        const QPoint enterPoint = view->visualRect(model->indexForTask(pinCandidate)).center();
        QDragEnterEvent emptyEnter(enterPoint, Qt::MoveAction, &emptyMime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &emptyEnter);
        const QModelIndex emptyToday = headerFor(QStringLiteral("today"));
        QVERIFY(emptyToday.isValid());
        const QPoint emptyPoint = view->visualRect(emptyToday).center();
        QDragMoveEvent emptyMove(emptyPoint, Qt::MoveAction, &emptyMime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &emptyMove);
        QDropEvent emptyDrop(QPointF(emptyPoint), Qt::MoveAction, &emptyMime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &emptyDrop);
        QVERIFY(emptyDrop.isAccepted());
        QTRY_VERIFY(!m_store->entity(pinCandidate).pinnedAt.isValid());
        QCOMPARE(m_store->entity(pinCandidate).allDayDate, today);

        // 子任务虽随父级显示在今天，真实日期为明天；脱离父级后必须按目标段落到今天。
        const QString datedParent = m_store->createTask(QStringLiteral("今日层级父任务"), list);
        QVERIFY(setDate(datedParent, today));
        const QString datedChild = m_store->createSubtask(QStringLiteral("实际日期为明天的子任务"), datedParent);
        QVERIFY(setDate(datedChild, today.addDays(1)));
        const QString todayAnchor = m_store->createTask(QStringLiteral("今日独立锚点"), list);
        QVERIFY(setDate(todayAnchor, today));
        QTRY_VERIFY(model->indexForTask(todayAnchor).isValid());
        QCOMPARE(model->indexForTask(datedChild).data(TaskListModel::SectionKeyRole).toString(),
                 QStringLiteral("today"));
        QCOMPARE(model->indexForTask(datedChild).data(TaskListModel::UnpinnedSectionRole).toString(),
                 QStringLiteral("tomorrow"));
        QVERIFY(drop(datedChild, model->indexForTask(todayAnchor), false, true));
        QTRY_VERIFY(m_store->entity(datedChild).parentId.isEmpty());
        QCOMPARE(m_store->entity(datedChild).allDayDate, today);
        QTRY_COMPARE(model->indexForTask(datedChild).data(TaskListModel::SectionKeyRole).toString(),
                     QStringLiteral("today"));

        const QString singleList = m_store->createList(QStringLiteral("仅置顶清单"));
        const QString onlyPinned = m_store->createTask(QStringLiteral("唯一的置顶任务"), singleList);
        QVERIFY(m_store->setPinnedAt(onlyPinned, appNow()));
        ViewSpec direct; direct.scope = ViewScope::List; direct.listId = singleList;
        model->setFilter(direct);
        QVERIFY(dropOnHeader(onlyPinned, QStringLiteral("undated")));
        QTRY_VERIFY(!m_store->entity(onlyPinned).pinnedAt.isValid());
        QVERIFY(!m_store->entity(onlyPinned).allDayDate.isValid());
        QCOMPARE(m_store->entity(onlyPinned).sortOrder, 0);
        QVERIFY(!headerFor(QStringLiteral("pinned")).isValid());
        int undatedHeaders = 0;
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex index = model->index(row, 0);
            if (index.data(TaskListModel::HeaderRole).toBool()
                && index.data(TaskListModel::SectionKeyRole).toString() == QLatin1String("undated"))
                ++undatedHeaders;
        }
        QCOMPARE(undatedHeaders, 1); // 正常无日期分区保留，临时投放副本消失。
        QCOMPARE(model->indexForTask(onlyPinned).data(TaskListModel::SectionKeyRole).toString(),
                 QStringLiteral("undated"));

        direct.listId = list;
        model->setFilter(direct);
        // 同父排序不能由父任务的显示 pinned 分区推断孩子的实际置顶状态。
        const QString parent = m_store->createTask(QStringLiteral("置顶层级父任务"), list);
        QVERIFY(m_store->setPinnedAt(parent, appNow()));
        const QString child = m_store->createSubtask(QStringLiteral("普通兄弟一"), parent);
        const QString sibling = m_store->createSubtask(QStringLiteral("普通兄弟二"), parent);
        QVERIFY(!child.isEmpty() && !sibling.isEmpty());
        QTRY_VERIFY(model->indexForTask(child).isValid() && model->indexForTask(sibling).isValid());
        QCOMPARE(model->indexForTask(child).data(TaskListModel::SectionKeyRole).toString(),
                 QStringLiteral("pinned"));
        QVERIFY(drop(child, model->indexForTask(sibling), false, true));
        QTRY_VERIFY(!m_store->entity(child).pinnedAt.isValid());
        QCOMPARE(m_store->entity(child).parentId, parent);
        QVERIFY(m_store->entity(parent).pinnedAt.isValid());
        QVERIFY(model->indexForTask(child).row() > model->indexForTask(sibling).row());

        // 文件夹聚合视图禁止把置顶任务误投进另一清单。
        const QString folder = m_store->createFolder(QStringLiteral("拖动落点校验"));
        const QString second = m_store->createList(QStringLiteral("第二清单"), folder);
        const QString foreign = m_store->createTask(QStringLiteral("第二清单锚点"), second);
        QVERIFY(m_store->moveList(list, folder));
        ViewSpec folderFilter; folderFilter.scope = ViewScope::Folder; folderFilter.folderId = folder;
        model->setFilter(folderFilter);
        QVERIFY(dropOnHeader(parent, QStringLiteral("pinned"))); // 进入拖动后关闭临时空置顶段。
        QMimeData folderMime;
        QSignalSpy folderDrops(view, &TaskListView::entityDropRequested);
        folderMime.setData(TaskListView::mimeType(), parent.toUtf8());
        const QModelIndex target = model->indexForTask(foreign);
        QVERIFY(target.isValid());
        const QPoint targetPoint = view->visualRect(target).center();
        QDragEnterEvent folderEnter(targetPoint, Qt::MoveAction, &folderMime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &folderEnter);
        QDragMoveEvent folderMove(targetPoint, Qt::MoveAction, &folderMime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &folderMove);
        QVERIFY(!folderMove.isAccepted());
        QDragLeaveEvent folderLeave; QApplication::sendEvent(view->viewport(), &folderLeave);
        QCOMPARE(m_store->entity(parent).listId, list);
        const qint64 foreignOrder = m_store->entity(foreign).sortOrder;
        QDropEvent folderDrop(QPointF(targetPoint), Qt::MoveAction, &folderMime,
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &folderDrop);
        QVERIFY(!folderDrop.isAccepted());
        QCOMPARE(folderDrops.count(), 0);
        QCOMPARE(m_store->entity(parent).listId, list);
        QCOMPARE(m_store->entity(foreign).sortOrder, foreignOrder);
    }
    void taskContextMenuAddsSubtask() {
        const QDate today = toAppTime(appNow()).date();
        const QString parent = m_store->createTask(QStringLiteral("右键父任务"));
        Entity parentValue = m_store->entity(parent); parentValue.allDayDate = today;
        QVERIFY(m_store->saveTask(parentValue));
        TaskPanel panel(m_store.get()); panel.show();
        auto* view = panel.findChild<QListView*>(QStringLiteral("TaskList"));
        auto* model = qobject_cast<TaskListModel*>(view->model());
        QVERIFY(view && model);
        const QModelIndex parentIndex = model->indexForTask(parent);
        QVERIFY(parentIndex.isValid());

        QTimer::singleShot(0, []() {
            QMenu* entityMenu = nullptr;
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* candidate = qobject_cast<QMenu*>(widget);
                if (candidate && candidate->objectName() == QLatin1String("EntityContextMenu"))
                    entityMenu = candidate;
            }
            if (!entityMenu) return;
            QStringList commands;
            QAction* add = nullptr;
            for (QAction* action : entityMenu->actions()) {
                commands.append(action->objectName());
                if (action->objectName() == QLatin1String("addSubtask")) add = action;
            }
            QVERIFY(!commands.contains(QStringLiteral("editTags")));
            QVERIFY(!commands.contains(QStringLiteral("copyLink")));
            QVERIFY(!commands.contains(QStringLiteral("setDateNextPeriod")));
            QVERIFY(add);
            QTimer::singleShot(0, []() {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) return;
                if (auto* edit = dialog->findChild<QLineEdit*>())
                    edit->setText(QStringLiteral("右键子任务"));
                dialog->accept();
            });
            add->trigger();
            entityMenu->close();
        });
        emit view->customContextMenuRequested(view->visualRect(parentIndex).center());
        QString child;
        for (const Entity& entity : m_store->entities())
            if (entity.parentId == parent) child = entity.id;
        QVERIFY(isUuid(child));
        QCOMPARE(m_store->entity(child).title, QStringLiteral("右键子任务"));
        QCOMPARE(m_store->entity(child).allDayDate, today);
        QTRY_VERIFY(model->indexForTask(child).isValid());
        QCOMPARE(model->data(model->indexForTask(child), TaskListModel::DepthRole).toInt(), 1);
        QCOMPARE(model->data(model->indexForTask(parent), TaskListModel::HasChildrenRole).toBool(), true);
    }
    void listEditorAndAppearancePickers() {
        const QString folder = m_store->createFolder(QStringLiteral("表单文件夹"));
        const QString existingId = m_store->createList(QStringLiteral("已有清单"), folder,
            EntityKind::Task, QStringLiteral("🏛️"), QStringLiteral("#4CA1FF"));

        ListEditorDialog editor(m_store->lists(), m_store->folders());
        editor.setNewListDefaults(folder, EntityKind::Note);
        auto* name = editor.findChild<QLineEdit*>(QStringLiteral("ListNameEdit"));
        auto* save = editor.findChild<QPushButton*>(QStringLiteral("ListEditorSave"));
        QVERIFY(name && save); QVERIFY(!save->isEnabled());
        name->setText(QStringLiteral("已有清单"));
        QVERIFY(!editor.findChild<QLabel*>(QStringLiteral("ListEditorError"))->text().isEmpty());
        name->setText(QStringLiteral("新笔记清单")); QVERIFY(save->isEnabled());
        editor.show();
        auto* iconButton = editor.findChild<QToolButton*>(QStringLiteral("ListIconButton"));
        auto* noneColor = editor.findChild<QToolButton*>(QStringLiteral("ListColorNoneButton"));
        auto* customColor = editor.findChild<QToolButton*>(QStringLiteral("ListCustomColorButton"));
        QVERIFY(iconButton && noneColor && customColor);
        auto* nameField = editor.findChild<QWidget*>(QStringLiteral("ListNameField"));
        QVERIFY(nameField && nameField->isAncestorOf(iconButton) && nameField->isAncestorOf(name));
        QVERIFY(nameField->rect().contains(rectIn(iconButton, nameField)));
        QVERIFY(nameField->rect().contains(rectIn(name, nameField)));
        QTRY_VERIFY(iconButton->isVisible() && noneColor->isVisible() && customColor->isVisible());
        QVERIFY(rectIn(customColor, &editor).right() < editor.width() - 24);
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_COMPARE(editor.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(editor.draft().kind, EntityKind::Note);
        QCOMPARE(editor.draft().folderId, folder);
        QVERIFY(!editor.isEditing());

        QVERIFY(m_store->setListArchived(existingId, true));
        TaskList existing;
        for (const TaskList& list : m_store->lists()) if (list.id == existingId) existing = list;
        ListEditorDialog editExisting(m_store->lists(), m_store->folders());
        editExisting.setList(existing);
        QCOMPARE(editExisting.draft().icon, QStringLiteral("🏛️"));
        QCOMPARE(editExisting.draft().color, QStringLiteral("#4ca1ff"));
        QCOMPARE(editExisting.draft().archivedAtUtcMs, existing.archivedAtUtcMs);
        editExisting.findChild<QLineEdit*>(QStringLiteral("ListNameEdit"))->setText(QStringLiteral("改名清单"));
        editExisting.show();
        QTest::mouseClick(editExisting.findChild<QPushButton*>(QStringLiteral("ListEditorSave")), Qt::LeftButton);
        QTRY_COMPARE(editExisting.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(editExisting.draft().name, QStringLiteral("改名清单"));
        QCOMPARE(editExisting.draft().kind, EntityKind::Task);
        QCOMPARE(editExisting.draft().archivedAtUtcMs, existing.archivedAtUtcMs);

        TaskList legacy = existing;
        legacy.name = QStringLiteral("  ") + QString(70, QChar(0x65E7)) + QStringLiteral("  ");
        ListEditorDialog editLegacy(m_store->lists(), m_store->folders());
        editLegacy.setList(legacy);
        auto* legacyName = editLegacy.findChild<QLineEdit*>(QStringLiteral("ListNameEdit"));
        auto* legacySave = editLegacy.findChild<QPushButton*>(QStringLiteral("ListEditorSave"));
        QCOMPARE(legacyName->text(), legacy.name);
        QVERIFY(legacySave->isEnabled());
        QCOMPARE(editLegacy.draft().name, legacy.name);
        QTest::mouseClick(editLegacy.findChild<QToolButton*>(QStringLiteral("ListColorNoneButton")),
                          Qt::LeftButton);
        QCOMPARE(editLegacy.draft().name, legacy.name);
        legacyName->setText(QString(69, QChar(0x65B0)));
        QVERIFY(!legacySave->isEnabled());
        legacyName->setText(QStringLiteral("已改为短名称"));
        QVERIFY(legacySave->isEnabled());
        editLegacy.setNewListDefaults(QString(), EntityKind::Task);
        QCOMPARE(legacyName->maxLength(), kListNameMaxLength);

        ColorPickerPopup colors;
        colors.setInitialColor(QColor(QStringLiteral("#4CA1FF")));
        QSignalSpy colorSpy(&colors, &ColorPickerPopup::colorSelected);
        colors.show();
        auto* custom = colors.findChild<QPushButton*>(QStringLiteral("ColorCustomEntry"));
        QVERIFY(custom); QTest::mouseClick(custom, Qt::LeftButton);
        auto* hex = colors.findChild<QLineEdit*>(QStringLiteral("ColorHexEdit"));
        QVERIFY(hex); QTRY_VERIFY(hex->isVisible());
        hex->setText(QStringLiteral("12AB34"));
        QTest::mouseClick(colors.findChild<QPushButton*>(QStringLiteral("ColorSave")), Qt::LeftButton);
        QCOMPARE(colorSpy.count(), 1);
        QCOMPARE(colorSpy.first().first().value<QColor>().name(QColor::HexRgb),
                 QStringLiteral("#12ab34"));

        EmojiPickerPopup emojis;
        QSignalSpy emojiSpy(&emojis, &EmojiPickerPopup::emojiSelected);
        emojis.show();
        auto* search = emojis.findChild<QLineEdit*>(QStringLiteral("EmojiSearchEdit"));
        QVERIFY(search); search->setText(QStringLiteral("🏛️"));
        QTest::keyClick(search, Qt::Key_Return);
        QCOMPARE(emojiSpy.count(), 1);
        QCOMPARE(emojiSpy.first().first().toString(), QStringLiteral("🏛️"));
    }
    void reminderPopupSnoozeUsesExactReminderUuid() {
        Entity task;
        task.id = newUuid();
        task.title = QStringLiteral("多提醒任务");
        TaskReminder first;
        first.id = newUuid();
        first.entityId = task.id;
        first.triggerAt = fromUtcMs(1000);
        TaskReminder second = first;
        second.id = newUuid();
        second.triggerAt = fromUtcMs(2000);
        task.reminders << first << second;

        auto* popup = new ReminderPopup(task, second.id);
        QPointer<ReminderPopup> popupGuard(popup);
        QSignalSpy snoozed(popup, &ReminderPopup::snoozeRequested);
        auto* snooze = popup->findChild<QToolButton*>(QStringLiteral("ReminderSnoozeButton"));
        auto* menu = popup->findChild<QMenu*>(QStringLiteral("ReminderSnoozeMenu"));
        QVERIFY(snooze && menu);
        const QPair<QString, int> actions[] = {
            {QStringLiteral("ReminderSnooze15"), 15},
            {QStringLiteral("ReminderSnooze30"), 30},
            {QStringLiteral("ReminderSnooze60"), 60},
            {QStringLiteral("ReminderSnooze180"), 180}
        };
        for (const auto& expected : actions) {
            QAction* action = menu->findChild<QAction*>(expected.first);
            QVERIFY(action);
            action->trigger();
            QCOMPARE(snoozed.count(), 1);
            const QList<QVariant> args = snoozed.takeFirst();
            QCOMPARE(args.size(), 3);
            QCOMPARE(args.at(0).toString(), task.id);
            QCOMPARE(args.at(1).toString(), second.id);
            QVERIFY(args.at(1).toString() != first.id);
            QCOMPARE(args.at(2).toInt(), expected.second);
        }

        QSignalSpy completed(popup, &ReminderPopup::completeRequested);
        auto* done = popup->findChild<QPushButton*>(QStringLiteral("ReminderDoneButton"));
        QVERIFY(done);
        QTest::mouseClick(done, Qt::LeftButton);
        QCOMPARE(completed.count(), 1);
        QCOMPARE(completed.takeFirst().at(0).toString(), task.id);
        auto* dismiss = popup->findChild<QToolButton*>(QStringLiteral("ReminderDismissButton"));
        QVERIFY(dismiss);
        QTest::mouseClick(dismiss, Qt::LeftButton);
        QTRY_VERIFY(!popupGuard);

        auto* openPopup = new ReminderPopup(task, second.id);
        QPointer<ReminderPopup> openGuard(openPopup);
        QSignalSpy opened(openPopup, &ReminderPopup::openRequested);
        auto* title = openPopup->findChild<QPushButton*>(QStringLiteral("ReminderOpenButton"));
        QVERIFY(title);
        openPopup->show();
        QApplication::processEvents();
        QCOMPARE(title->text(), task.title);
        QTest::mouseClick(title, Qt::LeftButton);
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.takeFirst().at(0).toString(), task.id);
        QTRY_VERIFY(!openGuard);
    }

    void datePickerDayButtonsFitTwoDigitLabels() {
        DatePickerPopup popup;
        popup.setDateTime(fromAppTime(QDateTime(QDate(2026, 11, 14), QTime(9, 30))));
        popup.setHasDate(true);
        popup.show();
        QApplication::processEvents();
        const auto days = popup.findChildren<QToolButton*>(QStringLiteral("DatePickerDay"));
        QCOMPARE(days.size(), 42);
        for (QToolButton* day : days) {
            if (day->property("outsideMonth").toBool() || day->text().size() != 2) continue;
            day->ensurePolished();
            const int textWidth = day->fontMetrics().horizontalAdvance(day->text());
            QVERIFY2(textWidth <= day->width() - 2,
                     qPrintable(QStringLiteral("%1: text=%2, cell=%3")
                                    .arg(day->text()).arg(textWidth).arg(day->width())));
            QVERIFY2(day->sizeHint().width() <= day->width(),
                     qPrintable(QStringLiteral("%1 sizeHint %2 exceeds cell %3")
                                    .arg(day->text()).arg(day->sizeHint().width()).arg(day->width())));
        }
        popup.reject();
    }
    void desktopProjectionUsesSameTaskAndCloseOnlyCloses() {
        const QString id = m_store->createTask(QStringLiteral("桌面任务"));
        Entity task = m_store->entity(id);
        task.body = QStringLiteral("原有说明");
        task.dueAt = appNow().addDays(1);
        QVERIFY(m_store->saveTask(task));
        QVERIFY(m_store->saveDesktopState(id, m_store->entity(id).color, m_store->entity(id).geometry,
                                          m_store->entity(id).zOrder, m_store->entity(id).layer, true));
        DesktopNote other;
        other.entityId = id;
        other.deviceId = newUuid();
        other.geometry = QRect(900, 700, 300, 280);
        other.color = NoteColor::Pink;
        other.open = true;
        QVERIFY(m_store->saveDesktopNote(other));
        NoteManager notes(m_store.get());
        QVERIFY(notes.restoreAll());
        NoteWidget* note = notes.note(id);
        QVERIFY(note);
        note->focusEditor();
        auto* editor = note->findChild<MarkdownEditor*>();
        QVERIFY(editor);
        const DesktopNote beforeTextFlush = m_store->desktopNote(id);
        editor->textEditor()->setPlainText(QStringLiteral("**修改后的说明**"));
        QVERIFY(note->flush());
        QCOMPARE(m_store->entity(id).body, QStringLiteral("**修改后的说明**"));
        QCOMPARE(m_store->entity(id).dueAt, task.dueAt);
        const DesktopNote savedDesktop = m_store->desktopNote(id);
        QVERIFY(!savedDesktop.id.isEmpty());
        QCOMPARE(savedDesktop.deviceId, m_store->deviceId());
        QCOMPARE(savedDesktop.updatedAt, beforeTextFlush.updatedAt);
        QVERIFY(!savedDesktop.geometry.isValid()); // 文本 flush 不触碰桌面状态
        QVERIFY(savedDesktop.open);
        QCOMPARE(m_store->desktopNote(id, other.deviceId).geometry, other.geometry);
        QCOMPARE(m_store->desktopNote(id, other.deviceId).color, NoteColor::Pink);
        auto* close = note->findChild<QToolButton*>(QStringLiteral("CloseButton"));
        QVERIFY(close);
        QTest::mouseClick(close, Qt::LeftButton);
        QVERIFY(!m_store->desktopNote(id).id.isEmpty());
        QVERIFY(!m_store->desktopNote(id).open);
        QCOMPARE(m_store->desktopNote(id).geometry, note->geometry());
        QVERIFY(!m_store->entity(id).deleted);
        QVERIFY(m_store->desktopNote(id, other.deviceId).open);
        QCOMPARE(notes.count(), 0);
    }
    void convertedNoteClosesAndRestoresDesktopProjection() {
        const QString id = m_store->createTask(QStringLiteral("转换桌面实体"));
        QVERIFY(m_store->saveDesktopState(id, NoteColor::Green, QRect(30, 40, 320, 300),
                                          1, NoteLayer::Normal, true));
        NoteManager notes(m_store.get());
        QVERIFY(notes.restoreAll()); QCOMPARE(notes.count(), 1);
        QVERIFY(m_store->convertKind(id, EntityKind::Note));
        QCOMPARE(notes.count(), 0); QVERIFY(m_store->desktopNote(id).open);
        QVERIFY(m_store->convertKind(id, EntityKind::Task));
        QCOMPARE(notes.count(), 1); QVERIFY(m_store->desktopNote(id).open);
        QVERIFY(m_store->trashTask(id)); QCOMPARE(notes.count(), 0);
    }
    void abandonedTaskLeavesActiveViewsAndDesktop() {
        const QString id = m_store->createTask(QStringLiteral("已放弃任务"));
        QVERIFY(isUuid(id));
        Entity task = m_store->entity(id);
        task.dueAt = appNow().addDays(-1);
        QVERIFY(m_store->saveTask(task));
        QVERIFY(m_store->saveDesktopState(id, NoteColor::Yellow, QRect(20, 20, 340, 360),
                                          0, NoteLayer::Normal, true));
        NoteManager notes(m_store.get());
        QVERIFY(notes.restoreAll());
        QCOMPARE(notes.count(), 1);
        QVERIFY(m_store->abandonTask(id));
        QCOMPARE(notes.count(), 0);

        ViewSpec filter;
        QVERIFY(!contains(m_store->entity(id), filter, toAppTime(appNow()).date()));
        filter.scope = ViewScope::Completed;
        QVERIFY(!contains(m_store->entity(id), filter, toAppTime(appNow()).date()));
        filter.scope = ViewScope::Abandoned;
        QVERIFY(contains(m_store->entity(id), filter, toAppTime(appNow()).date()));
        TaskListModel model(m_store.get());
        model.setFilter(filter);
        const QModelIndex index = model.indexForTask(id);
        QVERIFY(index.isValid());
        QVERIFY(!model.data(index, TaskListModel::OverdueRole).toBool());
    }

    void noteResizeAndAttachmentPreview() {
        const QString id = m_store->createTask(QStringLiteral("带图片的桌面便签"));
        Entity task = m_store->entity(id);
        task.body = QStringLiteral("## 参考材料\n\n可在这里查看图片与文档。");
        QVERIFY(m_store->saveTask(task));
        QVERIFY(m_store->saveDesktopState(id, m_store->entity(id).color, m_store->entity(id).geometry,
                                          m_store->entity(id).zOrder, m_store->entity(id).layer, true));
        const QString source = m_dir->path() + QStringLiteral("/sample.png");
        QPixmap image(160, 90);
        image.fill(QColor(QStringLiteral("#668BE3")));
        QVERIFY(image.save(source));
        QVERIFY(!m_store->importAttachment(id, source).id.isEmpty());
        QCOMPARE(m_store->attachments(id).size(), 1);
        NoteWidget note(m_store->entity(id));
        note.setStore(m_store.get());
        note.show();
        auto* attachments = note.findChild<AttachmentWidget*>();
        QVERIFY(attachments);
        QVERIFY(attachments->isVisible());
        const auto* thumbnail = note.findChild<AttachmentWidget*>()
                                    ->findChild<QLabel*>(QStringLiteral("AttachmentThumbnail"));
        QVERIFY(thumbnail);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        QVERIFY(!thumbnail->pixmap(Qt::ReturnByValue).isNull());
#else
        QVERIFY(thumbnail->pixmap() && !thumbnail->pixmap()->isNull());
#endif
        auto* grip = note.findChild<ResizeGrip*>();
        QVERIFY(grip);
        const QPoint start = grip->mapToGlobal(QPoint(8, 8));
        QTest::mousePress(grip, Qt::LeftButton, Qt::NoModifier, QPoint(8, 8));
        for (int step = 1; step <= 12; ++step) {
            const QPoint local(8 + step * 4, 8 + step * 2);
            QMouseEvent move(QEvent::MouseMove,
                             local,
                             grip->mapToGlobal(local),
                             Qt::NoButton,
                             Qt::LeftButton,
                             Qt::NoModifier);
            QApplication::sendEvent(grip, &move);
            QCoreApplication::processEvents();
        }
        QTest::mouseRelease(grip, Qt::LeftButton, Qt::NoModifier, QPoint(56, 32));
        QVERIFY(note.width() > DefaultWidth);
        QVERIFY(note.grab().toImage().format() != QImage::Format_Invalid);
    }

    void applicationRestoreCoordinatesEditorsAndReminders() {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const QString id = m_store->createTask(QStringLiteral("应用级恢复"));
        const TaskReminder past = m_store->addReminder(id, now.addSecs(-3600));
        const TaskReminder future = m_store->addReminder(id, now.addSecs(3600));
        QVERIFY(!past.id.isEmpty() && !future.id.isEmpty());
        QVERIFY(m_store->saveDesktopState(id, NoteColor::Yellow, QRect(20, 30, 360, 320), 0, NoteLayer::Normal, true));
        const QString backup = m_dir->path() + QStringLiteral("/app-restore.klb"); QVERIFY(m_store->exportBackup(backup));
        const QString sentinel = m_store->createTask(QStringLiteral("应用恢复覆盖哨兵")); QVERIFY(!sentinel.isEmpty());

        KylinListApp app; QString error; QVERIFY2(app.init(&error, true, true), qPrintable(error));
        QVERIFY(!Settings::instance().autostartInitialized());
        auto* notes = app.findChild<NoteManager*>(); auto* scheduler = app.findChild<ReminderScheduler*>();
        QVERIFY(notes && scheduler); QCOMPARE(notes->count(), 1);
        QSignalSpy triggered(scheduler, &ReminderScheduler::reminderTriggered);
        QVERIFY2(app.importBackup(backup), qPrintable(error));
        QCOMPARE(notes->count(), 0); // 旧桌面编辑器被销毁，不以旧实体 flush 新库
        {
            TaskStore restored(Database::instance().connection()); QVERIFY(restored.load());
            QCOMPARE(restored.entity(id).id, id); QVERIFY(restored.entity(sentinel).id.isEmpty());
            QVERIFY(restored.desktopNote(id).id.isEmpty());
            QVERIFY(restored.isReminderDelivered(past.id)); QVERIFY(!restored.isReminderDelivered(future.id));
        }
        QTest::qWait(600); QCOMPARE(triggered.count(), 0); // 启动调度器不补发已恢复的历史提醒
        scheduler->checkDue(now.addSecs(7200)); QCOMPARE(triggered.count(), 1);
        QCOMPARE(triggered.takeFirst().at(1).toString(), future.id);
        m_store.reset(); app.shutdown();
    }

    void detailShortBodyKeepsChecklistAndAttachmentsAdjacent() {
        const QString id = m_store->createTask(QStringLiteral("紧凑详情"));
        Entity task = m_store->entity(id);
        task.body = QStringLiteral("A short body.");
        ChecklistItem item; item.text = QStringLiteral("First item"); task.checklist.append(item);
        QVERIFY(m_store->saveTask(task));
        const QString source = referenceImage(m_dir->path()); QVERIFY(!source.isEmpty());
        const Attachment attachment = m_store->importAttachment(id, source); QVERIFY(isUuid(attachment.id));
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 720); detail.show(); QVERIFY(detail.setTask(id));
        QTest::qWait(20); // Settle document wrapping and deferred attachment-row replacement before measuring geometry.
        auto* scroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        auto* form = detail.findChild<QWidget*>(QStringLiteral("TaskDetailForm"));
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* checklist = detail.findChild<QListWidget*>(QStringLiteral("TaskChecklist"));
        auto* attachments = detail.findChild<AttachmentWidget*>();
        auto* input = detail.findChild<QLineEdit*>(QStringLiteral("ChecklistInput"));
        QVERIFY(scroll && form && title && editor && checklist && attachments && input);
        QCOMPARE(scroll->widget(), form);
        QVERIFY(form->isAncestorOf(title)); QVERIFY(form->isAncestorOf(editor));
        QTRY_VERIFY(checklist->isVisible()); QTRY_VERIFY(attachments->isVisible());
        QVERIFY(input->isHidden());
        QCOMPARE(checklist->frameShape(), QFrame::NoFrame);
        QTRY_COMPARE(checklist->verticalScrollBar()->maximum(), 0);
        QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
        const QTextCursor last = editor->textEditor()->document()->find(QStringLiteral("body."));
        QVERIFY(!last.isNull());
        const QRect line = editor->textEditor()->cursorRect(last);
        const int bodyBottom = editor->textEditor()->viewport()->mapTo(form, line.bottomLeft()).y();
        const int checklistGap = rectIn(checklist, form).top() - bodyBottom;
        QVERIFY2(checklistGap >= 0 && checklistGap <= 64,
                 qPrintable(QStringLiteral("Checklist starts %1 px after the short body").arg(checklistGap)));
        const int attachmentGap = rectIn(attachments, form).top() - rectIn(checklist, form).bottom();
        QVERIFY2(attachmentGap >= 0 && attachmentGap <= 64,
                 qPrintable(QStringLiteral("Attachments start %1 px after the checklist").arg(attachmentGap)));
        auto* row = attachments->findChild<QWidget*>(QStringLiteral("AttachmentRow")); QVERIFY(row);
        QVERIFY(scroll->viewport()->rect().contains(rectIn(row, scroll->viewport())));

        const QString empty = m_store->createTask(QStringLiteral("没有子项"));
        Entity shortTask = m_store->entity(empty); shortTask.body = task.body; QVERIFY(m_store->saveTask(shortTask));
        QVERIFY(detail.setTask(empty));
        QTRY_VERIFY(checklist->isHidden()); QTRY_VERIFY(attachments->isHidden()); QVERIFY(input->isHidden());
        QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
        QVERIFY(editor->height() < 100);
        QVERIFY2(form->layout()->sizeHint().height() <= title->height() + editor->height() + 80,
                 "Empty checklist/attachment sections must not reserve layout space");

        // Removing the final managed file collapses its slot immediately, without reopening the entity.
        QVERIFY(detail.setTask(id)); QTRY_VERIFY(attachments->isVisible());
        QVERIFY(m_store->removeAttachment(id, attachment.id));
        QTRY_VERIFY(attachments->isHidden());
        QCOMPARE(m_store->entity(id).checklist.first().id, checklist->item(0)->data(Qt::UserRole).toString());
    }

    void detailChecklistCommandsRevealInputAndGrowWithoutInnerScroll() {
        const QString id = m_store->createTask(QStringLiteral("按需添加清单"));
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(id));
        detail.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&detail));
        auto* checklist = detail.findChild<QListWidget*>(QStringLiteral("TaskChecklist"));
        auto* input = detail.findChild<QLineEdit*>(QStringLiteral("ChecklistInput"));
        auto* editor = detail.findChild<MarkdownEditor*>(); QVERIFY(checklist && input && editor);
        QVERIFY(checklist->isHidden()); QVERIFY(input->isHidden());
        QPushButton* legacyAdd = nullptr;
        for (QPushButton* button : detail.findChildren<QPushButton*>())
            if (button->text() == QStringLiteral("添加")) legacyAdd = button;
        QVERIFY(legacyAdd); QVERIFY(legacyAdd->isHidden());
        QVERIFY(triggerDetailCommand(detail, QStringLiteral("AddChecklist")));
        QTRY_VERIFY(input->isVisible()); QTRY_VERIFY(input->hasFocus());
        QTest::keyClicks(input, QStringLiteral("First item")); QTest::keyClick(input, Qt::Key_Return);
        QTRY_COMPARE(checklist->count(), 1); QTRY_VERIFY(checklist->isVisible()); QVERIFY(detail.flush());
        const QString firstId = m_store->entity(id).checklist.first().id; QVERIFY(isUuid(firstId));
        QCOMPARE(checklist->item(0)->data(Qt::UserRole).toString(), firstId);
        const int oneRowHeight = checklist->height();
        for (int row = 2; row <= 12; ++row) {
            input->setText(QStringLiteral("Item %1").arg(row)); QTest::keyClick(input, Qt::Key_Return);
        }
        QVERIFY(detail.flush()); QTRY_COMPARE(checklist->count(), 12);
        QTRY_VERIFY(checklist->height() > oneRowHeight * 5);
        QTRY_COMPARE(checklist->verticalScrollBar()->maximum(), 0);
        for (int row = 0; row < checklist->count(); ++row) {
            const QRect itemRect = checklist->visualItemRect(checklist->item(row));
            QVERIFY2(checklist->viewport()->rect().contains(itemRect), "Every checklist row must fit its own viewport");
            QVERIFY(isUuid(checklist->item(row)->data(Qt::UserRole).toString()));
        }
        checklist->item(0)->setCheckState(Qt::Checked); QVERIFY(detail.flush());
        QCOMPARE(m_store->entity(id).checklist.first().id, firstId);
        QVERIFY(m_store->entity(id).checklist.first().done);
        int doneEvents = 0;
        for (const EntityEvent& event : m_store->events(id))
            if (event.field == QStringLiteral("checklist.%1.done").arg(firstId)) ++doneEvents;
        QCOMPARE(doneEvents, 1);

        const QString other = m_store->createTask(QStringLiteral("工具栏添加清单"));
        QVERIFY(detail.setTask(other)); QVERIFY(input->isHidden());
        QSignalSpy requested(editor, &MarkdownEditor::checklistRequested);
        auto* format = detail.findChild<QToolButton*>(QStringLiteral("FooterFormat")); QVERIFY(format);
        QTest::mouseClick(format, Qt::LeftButton);
        auto* panel = detail.findChild<QFrame*>(QStringLiteral("FormatPanel")); QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible());
        auto* addChecklist = panel->findChild<QToolButton*>(QStringLiteral("FormatChecklist")); QVERIFY(addChecklist);
        QTest::mouseClick(addChecklist, Qt::LeftButton);
        QCOMPARE(requested.count(), 1); QTRY_VERIFY(input->isVisible()); QTRY_VERIFY(input->hasFocus());
        QTest::keyClicks(input, QStringLiteral("Toolbar item")); QTest::keyClick(input, Qt::Key_Return);
        QVERIFY(detail.finishEditing()); QCOMPARE(m_store->entity(other).checklist.size(), 1);
        QCOMPARE(m_store->entity(other).checklist.first().text, QStringLiteral("Toolbar item"));
        QVERIFY(m_store->entity(other).body.isEmpty()); // The command adds a real checklist item, not Markdown syntax.
    }

    void detailHeaderCommandsPersistWithoutSteadySaveLabel() {
        const QString id = m_store->createTask(QStringLiteral("详情菜单操作"));
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(id));
        auto* header = detail.findChild<QFrame*>(QStringLiteral("DetailHeader"));
        auto* date = detail.findChild<QPushButton*>(QStringLiteral("DateChip"));
        auto* info = detail.findChild<QToolButton*>(QStringLiteral("DetailInfo"));
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* completed = detail.findChild<QCheckBox*>(QStringLiteral("TaskCompleted"));
        auto* priority = detail.findChild<QComboBox*>(QStringLiteral("TaskPriority"));
        QVERIFY(header && date && info && title && completed && priority);
        QVERIFY(!header->isAncestorOf(title)); QVERIFY(header->isAncestorOf(date)); QVERIFY(header->isAncestorOf(info));
        QVERIFY(completed->isHidden()); QVERIFY(priority->isHidden());
        QVERIFY(date->isVisible()); QVERIFY(info->isVisible());
        QVERIFY(rectIn(info, header).left() > rectIn(date, header).right());
        for (QPushButton* button : header->findChildren<QPushButton*>())
            if (button->isVisible()) QCOMPARE(button, date);
        for (QToolButton* button : header->findChildren<QToolButton*>())
            if (button->isVisible() && button != info && button->objectName() != QLatin1String("DetailClose"))
                QVERIFY(button->accessibleName().contains(QStringLiteral("提醒")) || button->toolTip().contains(QStringLiteral("提醒")));
        for (QLineEdit* field : header->findChildren<QLineEdit*>()) QVERIFY(!field->isVisible());
        for (QComboBox* field : header->findChildren<QComboBox*>()) QVERIFY(!field->isVisible());
        for (QCheckBox* field : header->findChildren<QCheckBox*>()) QVERIFY(!field->isVisible());

        detail.activateWindow(); detail.focusTitle(); QTest::keyClicks(title, QStringLiteral("Menu keeps this title"));
        QVERIFY(triggerDetailCommand(detail, QStringLiteral("CmdPriority3")));
        QCOMPARE(m_store->entity(id).title, QStringLiteral("Menu keeps this title"));
        QCOMPARE(m_store->entity(id).priority, 3);
        QVERIFY(triggerDetailCommand(detail, QStringLiteral("CmdToggleCompleted")));
        QVERIFY(m_store->entity(id).completed);
        QVERIFY(triggerDetailCommand(detail, QStringLiteral("CmdToggleCompleted")));
        QVERIFY(!m_store->entity(id).completed);
        QVERIFY(triggerDetailCommand(detail, QStringLiteral("CmdPriority0")));
        QCOMPARE(m_store->entity(id).priority, 0); QVERIFY(detail.flush());
        auto* status = detail.findChild<QLabel*>(QStringLiteral("DetailStatus"));
        QVERIFY(!status || !status->isVisible() || status->text().isEmpty());
        QVERIFY(detail.setTask(QString())); QVERIFY(detail.setTask(id));
        QCOMPARE(title->text(), QStringLiteral("Menu keeps this title"));
        QVERIFY(completed->isHidden()); QVERIFY(priority->isHidden());
        QVERIFY(!status || !status->isVisible() || status->text().isEmpty());
    }

    void detailDateChipUsesShanghaiDateAndActiveStatus() {
        const QDate today = toAppTime(appNow()).date();
        const QDate overdue = today.addDays(-3);
        const QString id = m_store->createTask(QStringLiteral("逾期日期展示"));
        Entity task = m_store->entity(id);
        task.dueAt = fromAppTime(QDateTime(overdue, QTime(0, 30), Qt::LocalTime));
        task.allDayDate = today.addDays(5); // An explicit due time takes precedence over an all-day fallback.
        QVERIFY(m_store->saveTask(task));
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(id));
        auto* date = detail.findChild<QPushButton*>(QStringLiteral("DateChip")); QVERIFY(date);
        QVERIFY(date->text().contains(overdue.toString(QStringLiteral("yyyy/MM/dd"))));
        QVERIFY(date->text().contains(QStringLiteral("00:30")));
        QVERIFY(date->text().contains(QStringLiteral("延期3天")));
        QCOMPARE(date->accessibleName(), date->text());
        QVERIFY(m_store->setCompleted(id, true));
        QTRY_VERIFY(!date->text().contains(QStringLiteral("延期")));
        QVERIFY(date->text().contains(overdue.toString(QStringLiteral("yyyy/MM/dd"))));
        QVERIFY(m_store->setStatus(id, TaskStatus::Abandoned));
        QTRY_VERIFY(!date->text().contains(QStringLiteral("延期")));
        QVERIFY(date->text().contains(overdue.toString(QStringLiteral("yyyy/MM/dd"))));
        QVERIFY(m_store->setStatus(id, TaskStatus::Active));
        task = m_store->entity(id); task.dueAt = QDateTime(); task.allDayDate = overdue;
        task.startAt = fromAppTime(QDateTime(today.addDays(5), QTime(11, 45), Qt::LocalTime));
        QVERIFY(m_store->saveTask(task));
        QTRY_VERIFY(date->text().contains(overdue.toString(QStringLiteral("yyyy/MM/dd"))));
        QVERIFY(date->text().contains(QStringLiteral("延期3天")));
        QVERIFY(!date->text().contains(QStringLiteral("11:45")));
        task = m_store->entity(id); task.startAt = QDateTime();
        task.dueAt = fromAppTime(QDateTime(today.addDays(2), QTime(8, 30), Qt::LocalTime));
        QVERIFY(m_store->saveTask(task));
        QTRY_VERIFY(date->text().contains(today.addDays(2).toString(QStringLiteral("yyyy/MM/dd"))));
        QVERIFY(date->text().contains(QStringLiteral("08:30"))); QVERIFY(!date->text().contains(QStringLiteral("延期")));
    }

    void detailLongDocumentUsesOneScrollAtNarrowWidth() {
        const QString id = m_store->createTask(QStringLiteral("标题与长正文一起滚动"));
        Entity task = m_store->entity(id);
        QStringList paragraphs;
        for (int line = 0; line < 60; ++line)
            paragraphs.append(QStringLiteral("Paragraph %1: a long line that wraps at narrow widths and remains editable.").arg(line));
        task.body = paragraphs.join(QLatin1Char('\n'));
        ChecklistItem item; item.text = QStringLiteral("Last checklist item"); task.checklist.append(item);
        QVERIFY(m_store->saveTask(task));
        const QString source = referenceImage(m_dir->path()); QVERIFY(!source.isEmpty());
        QVERIFY(!m_store->importAttachment(id, source).id.isEmpty());
        TaskDetailWidget detail(m_store.get()); detail.resize(360, 560); detail.show(); QVERIFY(detail.setTask(id));
        QTest::qWait(20);
        auto* scroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        auto* form = detail.findChild<QWidget*>(QStringLiteral("TaskDetailForm"));
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* header = detail.findChild<QFrame*>(QStringLiteral("DetailHeader"));
        auto* footer = detail.findChild<QFrame*>(QStringLiteral("DetailFooter"));
        auto* attachments = detail.findChild<AttachmentWidget*>();
        QVERIFY(scroll && form && title && editor && header && footer && attachments);
        QTRY_VERIFY(detail.width() <= 380);
        QVERIFY(form->isAncestorOf(title)); QVERIFY(form->isAncestorOf(editor)); QVERIFY(editor->flowLayout());
        QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > scroll->viewport()->height());
        QCOMPARE(editor->textEditor()->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        QTRY_COMPARE(editor->textEditor()->verticalScrollBar()->maximum(), 0);
        QTRY_COMPARE(editor->textEditor()->horizontalScrollBar()->maximum(), 0);
        QTRY_COMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        QVERIFY(editor->contentHeight() > scroll->viewport()->height());
        scroll->verticalScrollBar()->setValue(0); QApplication::processEvents();
        const QPoint titleBefore = title->mapTo(scroll->viewport(), QPoint());
        const QPoint editorBefore = editor->mapTo(scroll->viewport(), QPoint());
        const QRect headerBefore = header->geometry(), footerBefore = footer->geometry();
        scroll->verticalScrollBar()->setValue(180); QApplication::processEvents();
        QCOMPARE(title->mapTo(scroll->viewport(), QPoint()).y(), titleBefore.y() - 180);
        QCOMPARE(editor->mapTo(scroll->viewport(), QPoint()).y(), editorBefore.y() - 180);
        QVERIFY(rectIn(title, scroll->viewport()).bottom() < 0);
        QCOMPARE(header->geometry(), headerBefore); QCOMPARE(footer->geometry(), footerBefore);
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum()); QApplication::processEvents();
        auto* row = attachments->findChild<QWidget*>(QStringLiteral("AttachmentRow")); QVERIFY(row);
        QVERIFY(scroll->viewport()->rect().contains(rectIn(row, scroll->viewport())));
        QCOMPARE(editor->content(), task.body);

        QSignalSpy heightChanges(editor, &MarkdownEditor::contentHeightChanged);
        editor->textEditor()->setPlainText(QStringLiteral("Short again."));
        QTRY_VERIFY(heightChanges.count() > 0);
        QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
        QTRY_COMPARE(editor->textEditor()->verticalScrollBar()->maximum(), 0);
        QVERIFY(editor->contentHeight() < 100); QVERIFY(detail.flush());
        QCOMPARE(m_store->entity(id).body, QStringLiteral("Short again."));
    }

    void detailInfoPopupShowsRenderedLiveStatisticsAndEntityDates() {
        const QString id = m_store->createTask(QStringLiteral("Two words"));
        Entity task = m_store->entity(id);
        task.body = QStringLiteral("**bold** <u>under</u> <mark data-color=\"yellow\">lit</mark> ")
            + QString::fromUtf8("\xF0\x9F\x98\x80");
        ChecklistItem item; item.text = QStringLiteral("check item"); task.checklist.append(item);
        QVERIFY(m_store->saveTask(task));
        const QString source = referenceImage(m_dir->path()); QVERIFY(!source.isEmpty());
        QVERIFY(!m_store->importAttachment(id, source).id.isEmpty());
        // An existing entity with distinct dates also exercises Shanghai's UTC day boundary.
        const QDateTime created(QDate(2026, 11, 5), QTime(16, 30), Qt::UTC);
        const QDateTime updated(QDate(2026, 11, 8), QTime(16, 15), Qt::UTC);
        QSqlQuery dates(Database::instance().connection());
        dates.prepare(QStringLiteral("UPDATE entities SET created_at_utc_ms=?,updated_at_utc_ms=? WHERE id=?"));
        dates.addBindValue(created.toMSecsSinceEpoch()); dates.addBindValue(updated.toMSecsSinceEpoch()); dates.addBindValue(id);
        QVERIFY(dates.exec()); QVERIFY(m_store->load());
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(id));
        auto* info = detail.findChild<QToolButton*>(QStringLiteral("DetailInfo")); QVERIFY(info);
        QTest::mouseClick(info, Qt::LeftButton);
        auto* popup = detail.findChild<EntityInfoPopup*>(QStringLiteral("EntityInfoPopup")); QVERIFY(popup);
        QTRY_VERIFY(popup->isVisible()); QVERIFY(popup->windowFlags().testFlag(Qt::Popup));
        auto* words = popup->findChild<QLabel*>(QStringLiteral("DetailWordCount"));
        auto* characters = popup->findChild<QLabel*>(QStringLiteral("DetailCharacterCount"));
        auto* createdLabel = popup->findChild<QLabel*>(QStringLiteral("DetailCreatedAt"));
        auto* updatedLabel = popup->findChild<QLabel*>(QStringLiteral("DetailUpdatedAt"));
        QVERIFY(words && characters && createdLabel && updatedLabel);
        QCOMPARE(words->text(), QStringLiteral("7"));
        QCOMPARE(characters->text(), QStringLiteral("35")); // Includes spaces and one non-BMP codepoint, no markup/newlines/filename.
        QCOMPARE(createdLabel->text(), QStringLiteral("2026年11月6日"));
        QCOMPARE(updatedLabel->text(), QStringLiteral("2026年11月9日"));
        QCOMPARE(createdLabel->toolTip(), toAppTime(created).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        QCOMPARE(updatedLabel->toolTip(), toAppTime(updated).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        popup->hide();

        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* body = detail.findChild<QTextEdit*>(QStringLiteral("DescriptionEditor")); QVERIFY(title && body);
        detail.activateWindow(); detail.focusTitle(); QTest::keyClicks(title, QStringLiteral("Only"));
        body->setFocus(); body->setPlainText(QStringLiteral("**changed** text"));
        QTest::mouseClick(info, Qt::LeftButton); QTRY_VERIFY(popup->isVisible());
        QCOMPARE(words->text(), QStringLiteral("5")); QCOMPARE(characters->text(), QStringLiteral("26"));
        popup->hide(); QVERIFY(detail.finishEditing());
        const Entity saved = m_store->entity(id);
        QCOMPARE(saved.title, QStringLiteral("Only")); QCOMPARE(saved.body, QStringLiteral("**changed** text"));
        QTest::mouseClick(info, Qt::LeftButton); QTRY_VERIFY(popup->isVisible());
        QCOMPARE(createdLabel->text(), QStringLiteral("2026年11月6日"));
        QCOMPARE(updatedLabel->toolTip(), toAppTime(saved.updatedAt).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        popup->hide();
    }

    void nativePopupWindowsDisableSystemShadow() {
        const QString id = m_store->createTask(QStringLiteral("浮层阴影检查"));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 640); detail.show();
        QVERIFY(detail.setTask(id)); QApplication::processEvents();
        const auto verify = [](QWidget* popup) {
            // 自有浮层显式声明标志；真实 Windows 窗口再核对系统样式。
            QVERIFY(popup->windowFlags().testFlag(Qt::NoDropShadowWindowHint));
            QVERIFY(nativePopupShadowDisabled(popup));
        };
        const auto verifyNativeOnly = [](QWidget* popup) {
            // Qt 内部菜单不声明 NoDropShadowWindowHint，仅由应用级过滤器清除系统样式。
            QVERIFY(nativePopupShadowDisabled(popup));
        };

        auto* info = detail.findChild<QToolButton*>(QStringLiteral("DetailInfo")); QVERIFY(info);
        QTest::mouseClick(info, Qt::LeftButton);
        auto* infoPopup = detail.findChild<EntityInfoPopup*>(); QVERIFY(infoPopup);
        QTRY_VERIFY(infoPopup->isVisible()); verify(infoPopup); infoPopup->hide();

        auto* group = detail.findChild<QToolButton*>(QStringLiteral("GroupChip")); QVERIFY(group);
        QTest::mouseClick(group, Qt::LeftButton);
        auto* listPopup = detail.findChild<ListSelectPopup*>(); QVERIFY(listPopup);
        QTRY_VERIFY(listPopup->isVisible()); verify(listPopup); listPopup->hide();

        auto* format = detail.findChild<QToolButton*>(QStringLiteral("FooterFormat")); QVERIFY(format);
        QTest::mouseClick(format, Qt::LeftButton);
        auto* formatPanel = detail.findChild<QFrame*>(QStringLiteral("FormatPanel")); QVERIFY(formatPanel);
        QTRY_VERIFY(formatPanel->isVisible()); verify(formatPanel); formatPanel->hide();

        auto* date = detail.findChild<QPushButton*>(QStringLiteral("DateChip")); QVERIFY(date);
        QTest::mouseClick(date, Qt::LeftButton);
        DatePickerPopup* datePopup = nullptr;
        QTRY_VERIFY((datePopup = detail.findChild<DatePickerPopup*>()) != nullptr);
        QTRY_VERIFY(datePopup->isVisible()); verify(datePopup); datePopup->reject();

        // 菜单、下拉列表等 Qt 内部弹出窗口也走同一应用级拦截。
        QMenu menu;
        menu.addAction(QStringLiteral("菜单阴影检查"));
        menu.popup(detail.mapToGlobal(QPoint(24, 24)));
        QTRY_VERIFY(menu.isVisible()); verifyNativeOnly(&menu); menu.hide();

        ReminderPopup reminder(m_store->entity(id));
        reminder.show();
        QTRY_VERIFY(reminder.isVisible()); verify(&reminder);
        auto* snooze = reminder.findChild<QToolButton*>(QStringLiteral("ReminderSnoozeButton"));
        auto* snoozeMenu = reminder.findChild<QMenu*>(QStringLiteral("ReminderSnoozeMenu"));
        QVERIFY(snooze && snoozeMenu);
        snoozeMenu->popup(snooze->mapToGlobal(QPoint(0, snooze->height())));
        QTRY_VERIFY(snoozeMenu->isVisible()); verifyNativeOnly(snoozeMenu);
        snoozeMenu->hide();
        reminder.hide();

        ConfirmDialog confirm(QStringLiteral("确认"), QStringLiteral("正文"),
                              QStringLiteral("删除"), true);
        confirm.show(); verify(&confirm); confirm.reject();
        ListEditorDialog listEditor(m_store->lists(), m_store->folders());
        listEditor.show(); verify(&listEditor); listEditor.reject();
        ColorPickerPopup colorPicker; colorPicker.show(); verify(&colorPicker); colorPicker.reject();
        EmojiPickerPopup emojiPicker; emojiPicker.show(); verify(&emojiPicker); emojiPicker.reject();
        SettingsDialog settings; settings.show(); verify(&settings); settings.reject();
    }

    void detailListSearchMovesByUuidAndPreservesEditingSession() {
        const QString sourceFolder = m_store->createFolder(QStringLiteral("SourceFolder"));
        const QString targetFolder = m_store->createFolder(QStringLiteral("TargetFolder"));
        const QString sourceList = m_store->createList(QStringLiteral("同名清单"), sourceFolder);
        const QString targetList = m_store->createList(QStringLiteral("同名清单"), targetFolder);
        const QString section = m_store->createSection(sourceList, QStringLiteral("原分段"));
        const QString id = m_store->createTask(QStringLiteral("待移动任务"), sourceList, section);
        QVERIFY(isUuid(id)); QVERIFY(isUuid(section));
        Entity task = m_store->entity(id); task.body = QStringLiteral("Original body");
        ChecklistItem item; item.text = QStringLiteral("Keep my identity"); task.checklist.append(item);
        QVERIFY(m_store->saveTask(task));
        const QString itemId = m_store->entity(id).checklist.first().id; QVERIFY(isUuid(itemId));
        const auto bodyEvents = [this, &id]() {
            QVector<EntityEvent> result;
            for (const EntityEvent& event : m_store->events(id))
                if (event.field == QStringLiteral("body")) result.append(event);
            return result;
        };
        const QVector<EntityEvent> baselineEvents = bodyEvents(); QVERIFY(!baselineEvents.isEmpty());
        QCOMPARE(baselineEvents.last().newValue.toString(), task.body);
        const int beforeBodyEvents = baselineEvents.size();
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(id));
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* chip = detail.findChild<QToolButton*>(QStringLiteral("GroupChip"));
        auto* checklist = detail.findChild<QListWidget*>(QStringLiteral("TaskChecklist"));
        QVERIFY(title && editor && chip && checklist);
        detail.activateWindow(); detail.focusTitle(); QTest::keyClicks(title, QStringLiteral("Pending move title"));
        auto* body = editor->textEditor(); body->setFocus(); QApplication::processEvents(); QVERIFY(body->hasFocus());
        const QString pendingBody = QStringLiteral("**Pending body**\nAll lines must survive moving lists.");
        body->setPlainText(pendingBody);
        QTest::mouseClick(chip, Qt::LeftButton);
        auto* popup = detail.findChild<ListSelectPopup*>(QStringLiteral("GroupSelectMenu")); QVERIFY(popup);
        QTRY_VERIFY(popup->isVisible());
        auto* search = popup->findChild<QLineEdit*>(QStringLiteral("ListSearch"));
        auto* tree = popup->findChild<QTreeWidget*>(QStringLiteral("ListSelectTree")); QVERIFY(search && tree);
        QTreeWidgetItem *sourceItem = nullptr, *targetItem = nullptr;
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            const QString listId = (*it)->data(0, Qt::UserRole).toString();
            if (listId == sourceList) sourceItem = *it;
            if (listId == targetList) targetItem = *it;
        }
        QVERIFY(sourceItem && targetItem); QCOMPARE(sourceItem->text(0), targetItem->text(0));
        QVERIFY(tree->currentItem()); QCOMPARE(tree->currentItem()->data(0, Qt::UserRole).toString(), sourceList);
        search->setText(QStringLiteral("no matching list")); QTRY_VERIFY(!tree->isVisible());
        QCOMPARE(m_store->entity(id).listId, sourceList); QCOMPARE(m_store->entity(id).sectionId, section);
        search->clear(); QTest::keyClicks(search, QStringLiteral("TargetFolder"));
        QTRY_VERIFY(tree->isVisible()); QVERIFY(sourceItem->parent()->isHidden());
        QVERIFY(!targetItem->isHidden()); QVERIFY(!targetItem->parent()->isHidden()); QVERIFY(targetItem->parent()->isExpanded());
        tree->scrollToItem(targetItem); QApplication::processEvents();
        const QRect targetRect = tree->visualItemRect(targetItem); QVERIFY(!targetRect.isEmpty());
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier, targetRect.center());
        QTRY_VERIFY(!popup->isVisible());
        QCOMPARE(m_store->entity(id).listId, targetList); QVERIFY(m_store->entity(id).sectionId.isEmpty());
        QCOMPARE(m_store->entity(id).title, QStringLiteral("Pending move title"));
        QCOMPARE(m_store->entity(id).body, pendingBody); QCOMPARE(editor->content(), pendingBody);
        QCOMPARE(m_store->entity(id).checklist.first().id, itemId);
        QCOMPARE(checklist->item(0)->data(Qt::UserRole).toString(), itemId);
        const QVector<EntityEvent> edits = bodyEvents(); QCOMPARE(edits.size(), beforeBodyEvents + 1);
        // Text history stores a full new snapshot; the edit-session baseline is used only to detect a change.
        QVERIFY(!edits.last().oldValue.isValid()); QCOMPARE(edits.last().newValue.toString(), pendingBody);
        checklist->item(0)->setCheckState(Qt::Checked); QVERIFY(detail.finishEditing());
        TaskStore reloaded(Database::instance().connection()); QVERIFY(reloaded.load());
        const Entity moved = reloaded.entity(id);
        QCOMPARE(moved.listId, targetList); QVERIFY(moved.sectionId.isEmpty());
        QCOMPARE(moved.title, QStringLiteral("Pending move title")); QCOMPARE(moved.body, pendingBody);
        QCOMPARE(moved.checklist.size(), 1); QCOMPARE(moved.checklist.first().id, itemId); QVERIFY(moved.checklist.first().done);
        QVERIFY(detail.setTask(QString())); QVERIFY(detail.setTask(id));
        QCOMPARE(title->text(), moved.title); QCOMPARE(editor->content(), pendingBody);
    }

    void newChecklistItemKeepsIdentityForDoneEvent() {
        const QString id = m_store->createTask(QStringLiteral("新清单项日志"));
        TaskDetailWidget detail(m_store.get()); QVERIFY(detail.setTask(id));
        auto* input = detail.findChild<QLineEdit*>(QStringLiteral("ChecklistInput"));
        auto* checklist = detail.findChild<QListWidget*>(QStringLiteral("TaskChecklist"));
        QVERIFY(input && checklist);
        QPushButton* add = nullptr;
        for (QPushButton* button : detail.findChildren<QPushButton*>())
            if (button->text() == QStringLiteral("添加")) add = button;
        QVERIFY(add);
        QTest::keyClicks(input, QStringLiteral("new item")); QTest::mouseClick(add, Qt::LeftButton);
        QCOMPARE(checklist->count(), 1); QVERIFY(detail.flush());
        const QString itemId = m_store->entity(id).checklist.first().id;
        QVERIFY(isUuid(itemId)); QCOMPARE(checklist->item(0)->data(Qt::UserRole).toString(), itemId);

        checklist->item(0)->setCheckState(Qt::Checked); QVERIFY(detail.flush());
        QCOMPARE(m_store->entity(id).checklist.size(), 1);
        QCOMPARE(m_store->entity(id).checklist.first().id, itemId);
        QVERIFY(m_store->entity(id).checklist.first().done);
        QVector<EntityEvent> doneEvents;
        for (const EntityEvent& event : m_store->events(id))
            if (event.field == QStringLiteral("checklist.%1.done").arg(itemId)) doneEvents.append(event);
        QCOMPARE(doneEvents.size(), 1);
        QCOMPARE(doneEvents.first().oldValue.toString(), QStringLiteral("0"));
        QCOMPARE(doneEvents.first().newValue.toString(), QStringLiteral("1"));
    }

    void detailActivitySessionBoundariesAndDialog() {
        const auto withField = [this](const QString& id, const QString& field) {
            QVector<EntityEvent> result;
            for (const EntityEvent& event : m_store->events(id))
                if (event.field == field) result.append(event);
            return result;
        };
        const QString id = m_store->createTask(QStringLiteral("UI活动"));
        Entity initial = m_store->entity(id); ChecklistItem check; check.text = QStringLiteral("原始清单");
        initial.checklist.append(check); QVERIFY(m_store->saveTask(initial));
        const QString checkId = m_store->entity(id).checklist.first().id;
        TaskDetailWidget detail(m_store.get()); detail.resize(800, 700); detail.show(); QVERIFY(detail.setTask(id));
        QLineEdit focusSink(&detail); focusSink.show();
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* body = detail.findChild<QTextEdit*>(QStringLiteral("DescriptionEditor"));
        auto* checklist = detail.findChild<QListWidget*>(QStringLiteral("TaskChecklist"));
        QVERIFY(title && body && checklist); detail.activateWindow(); detail.focusTitle(); QApplication::processEvents(); QVERIFY(title->hasFocus());
        QTest::keyClicks(title, QStringLiteral("abcde")); QTest::qWait(520); // 450ms autosave 不结束会话
        QCOMPARE(m_store->entity(id).title, QStringLiteral("abcde")); QCOMPARE(withField(id, QStringLiteral("title")).size(), 0);
        focusSink.setFocus(); QApplication::processEvents(); QVERIFY(!title->hasFocus());
        QVector<EntityEvent> events = withField(id, QStringLiteral("title")); QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().newValue.toString(), QStringLiteral("abcde")); QVERIFY(!events.first().oldValue.isValid());

        body->setFocus(); QApplication::processEvents(); QVERIFY(body->hasFocus());
        const QString fullBody = QStringLiteral("**完整正文**\n第二行\n第三行"); body->setPlainText(fullBody);
        QTest::qWait(520); QCOMPARE(m_store->entity(id).body, fullBody); QCOMPARE(withField(id, QStringLiteral("body")).size(), 0);
        focusSink.setFocus(); QApplication::processEvents(); events = withField(id, QStringLiteral("body"));
        QCOMPARE(events.size(), 1); QCOMPARE(events.first().newValue.toString(), fullBody);

        checklist->setFocus(); QApplication::processEvents(); checklist->item(0)->setText(QStringLiteral("失焦后的清单全文"));
        QVERIFY(detail.flush()); focusSink.setFocus(); QApplication::processEvents();
        events = withField(id, QStringLiteral("checklist.%1.text").arg(checkId)); QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().newValue.toString(), QStringLiteral("失焦后的清单全文"));
        QCOMPARE(m_store->entity(id).checklist.first().id, checkId);

        const QString other = m_store->createTask(QStringLiteral("切换目标"));
        title->setFocus(); QApplication::processEvents(); QTest::keyClick(title, Qt::Key_X);
        const int beforeSwitch = withField(id, QStringLiteral("title")).size();
        QVERIFY(detail.setTask(other)); QCOMPARE(withField(id, QStringLiteral("title")).size(), beforeSwitch + 1); // 切换实体前提交
        QCOMPARE(detail.entityId(), other); QVERIFY(detail.setTask(QString()));

        Entity scalars = m_store->entity(id); scalars.priority = 3;
        scalars.tags = QStringList{QStringLiteral("活动"), QStringLiteral("测试")}; QVERIFY(m_store->saveTask(scalars));
        QVERIFY(m_store->convertKind(id, EntityKind::Note));
        ActivityLogDialog dialog(m_store.get(), id);
        auto* tree = dialog.findChild<QTreeWidget*>(QStringLiteral("ActivityTree"));
        auto* text = dialog.findChild<QTextEdit*>(QStringLiteral("ActivityDetail")); QVERIFY(tree && text);
        QSet<QString> batches; QTreeWidgetItem* bodyItem = nullptr; QTreeWidgetItem* kindItem = nullptr; bool foldedBatch = false;
        for (const EntityEvent& event : m_store->events(id)) batches.insert(event.batchId);
        QCOMPARE(tree->topLevelItemCount(), batches.size());
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem* batch = tree->topLevelItem(i); QVERIFY(!batch->isExpanded()); if (batch->childCount() > 1) foldedBatch = true;
            for (int j = 0; j < batch->childCount(); ++j) {
                QTreeWidgetItem* item = batch->child(j);
                if (item->text(2) == QStringLiteral("body") && item->text(4) == fullBody) bodyItem = item;
                if (item->text(1) == QStringLiteral("类型转换")) kindItem = item;
            }
        }
        QVERIFY(foldedBatch); QVERIFY(bodyItem && kindItem);
        tree->setCurrentItem(bodyItem); QApplication::processEvents(); QVERIFY(text->toPlainText().contains(fullBody));
        QVERIFY(kindItem->text(4).contains(QStringLiteral("字段已保留")));
        tree->setCurrentItem(kindItem); QApplication::processEvents(); QVERIFY(text->toPlainText().contains(QStringLiteral("字段已保留")));
        const int openBatchCount = tree->topLevelItemCount(); scalars = m_store->entity(id); scalars.title = QStringLiteral("打开后新修改");
        QVERIFY(m_store->saveTask(scalars)); QCOMPARE(tree->topLevelItemCount(), openBatchCount); // 已打开对话框不主动轮询
    }

    void noteActivityAutosaveFlushAndClose() {
        const auto bodyEvents = [this](const QString& id) {
            QVector<EntityEvent> result;
            for (const EntityEvent& event : m_store->events(id))
                if (event.field == QStringLiteral("body")) result.append(event);
            return result;
        };
        const QString id = m_store->createTask(QStringLiteral("便签活动"));
        QVERIFY(m_store->saveDesktopState(id, NoteColor::Yellow, QRect(30, 40, 360, 320), 0, NoteLayer::Normal, true));
        NoteManager notes(m_store.get()); QVERIFY(notes.restoreAll()); NoteWidget* note = notes.note(id); QVERIFY(note);
        auto* editor = note->findChild<MarkdownEditor*>(); QVERIFY(editor); note->focusEditor(); QApplication::processEvents();
        const QString autosaved = QStringLiteral("自动保存中的完整正文\n不会在300ms时写日志");
        editor->textEditor()->setPlainText(autosaved); QTest::qWait(380);
        QCOMPARE(m_store->entity(id).body, autosaved); QCOMPARE(bodyEvents(id).size(), 0);
        QVERIFY(notes.flushAll()); QVector<EntityEvent> events = bodyEvents(id); QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().newValue.toString(), autosaved); QVERIFY(!events.first().oldValue.isValid());
        QVERIFY(m_store->desktopNote(id).open); QCOMPARE(notes.count(), 1);

        QLineEdit focusSink; focusSink.show(); focusSink.setFocus(); QApplication::processEvents(); note->focusEditor(); QApplication::processEvents();
        const QString finalText = QStringLiteral("关闭便签前的最后一条全文"); editor->textEditor()->setPlainText(finalText);
        auto* close = note->findChild<QToolButton*>(QStringLiteral("CloseButton")); QVERIFY(close);
        QTest::mouseClick(close, Qt::LeftButton); QApplication::processEvents();
        events = bodyEvents(id); QCOMPARE(events.size(), 2); QCOMPARE(events.last().newValue.toString(), finalText);
        QCOMPARE(m_store->entity(id).body, finalText); QVERIFY(!m_store->desktopNote(id).open); // 关闭路径 flush 后结束会话，不丢最后修改
    }

    void organizationDragVisualPreview() {
        const QString dir = qEnvironmentVariable("KYLINLIST_SCREENSHOT_DIR");
        if (dir.isEmpty()) return;
        QVERIFY(QDir().mkpath(dir));
        const QDate today = toAppTime(appNow()).date();
        const QString folder = m_store->createFolder(QStringLiteral("设计项目"), QStringLiteral("🏛️"));
        const QString first = m_store->createList(QStringLiteral("数字检察"), folder, EntityKind::Task,
                                                  QStringLiteral("🛠"), QStringLiteral("#4CA1FF"));
        const QString second = m_store->createList(QStringLiteral("生活"), folder, EntityKind::Task,
                                                   QStringLiteral("👶"), QStringLiteral("#FFAC38"));
        const QString parent = m_store->createTask(QStringLiteral("工作网服务维护"), first);
        Entity parentValue = m_store->entity(parent); parentValue.allDayDate = today;
        QVERIFY(m_store->saveTask(parentValue));
        const QString child = m_store->createSubtask(QStringLiteral("可视化平台代码研究"), parent);
        QVERIFY(!child.isEmpty());
        const QString ordinary = m_store->createTask(QStringLiteral("电子数据分析工作台开发"), first);
        const QString other = m_store->createTask(QStringLiteral("互联网门户网站运维合同到期"), second);
        Entity otherValue = m_store->entity(other); otherValue.allDayDate = today.addDays(1);
        QVERIFY(m_store->saveTask(otherValue));
        QVERIFY(m_store->setPinnedAt(parent, appNow()));

        TaskPanel panel(m_store.get()); panel.resize(1240, 820); panel.show();
        auto* navigation = panel.findChild<NavigationListWidget*>(QStringLiteral("Navigation"));
        auto* view = panel.findChild<TaskListView*>(QStringLiteral("TaskList"));
        auto* model = qobject_cast<TaskListModel*>(view->model());
        QVERIFY(navigation && view && model);
        for (int row = 0; row < navigation->count(); ++row) {
            if (navigation->item(row)->data(NavigationDelegate::FolderIdRole).toString() == folder) {
                navigation->setCurrentRow(row);
                break;
            }
        }
        QTRY_COMPARE(model->filter().scope, ViewScope::Folder);
        QTRY_VERIFY(model->indexForTask(child).isValid());
        QTest::mouseMove(view->viewport(), view->visualRect(model->indexForTask(child)).center());
        QApplication::processEvents();
        const auto saveHierarchyDetail = [&panel, view, model, parent, dir](const QString& name) {
            const QPixmap frame = panel.grab();
            const qreal dpr = frame.devicePixelRatio();
            const QPoint origin = view->viewport()->mapTo(&panel, QPoint(0, 0));
            const QRect cropRect(qRound(origin.x() * dpr),
                qRound((origin.y() + view->visualRect(model->indexForTask(parent)).top()) * dpr),
                qRound(view->viewport()->width() * dpr), qRound(80 * dpr));
            QImage crop = frame.toImage().copy(cropRect);
            crop.setDevicePixelRatio(1.0); // copy 保留源 DPR；合成与放大必须使用物理像素。
            QImage onSurface(crop.size(), QImage::Format_ARGB32_Premultiplied);
            onSurface.fill(panel.palette().color(QPalette::Window));
            QPainter painter(&onSurface);
            painter.drawImage(0, 0, crop);
            painter.end();
            return onSurface.scaled(onSurface.width() * 2, onSurface.height() * 2,
                                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .save(dir + QLatin1Char('/') + name);
        };
        QVERIFY(panel.grab().save(dir + QStringLiteral("/folder-hierarchy-light.png")));
        QVERIFY(view->grab().save(dir + QStringLiteral("/folder-task-rows.png")));
        QVERIFY(saveHierarchyDetail(QStringLiteral("task-hierarchy-detail.png")));

        QListWidgetItem* sourceItem = nullptr;
        QListWidgetItem* targetItem = nullptr;
        for (int row = 0; row < navigation->count(); ++row) {
            QListWidgetItem* item = navigation->item(row);
            if (item->data(Qt::UserRole + 1).toString() == first) sourceItem = item;
            if (item->data(Qt::UserRole + 1).toString() == second) targetItem = item;
        }
        QVERIFY(sourceItem && targetItem);
        QMimeData navMime;
        navMime.setData(NavigationListWidget::mimeType(), first.toUtf8());
        const QPoint navPoint = navigation->visualItemRect(targetItem).center();
        QDragEnterEvent navEnter(navPoint, Qt::MoveAction, &navMime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation->viewport(), &navEnter);
        QDragMoveEvent navMove(navPoint, Qt::MoveAction, &navMime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(navigation->viewport(), &navMove);
        QApplication::processEvents();
        QCOMPARE(targetItem->data(NavigationDelegate::DropTargetRole).toString(), QStringLiteral("target"));
        QVERIFY(navigation->grab().save(dir + QStringLiteral("/navigation-merge-outline.png")));
        QDragLeaveEvent navLeave;
        QApplication::sendEvent(navigation->viewport(), &navLeave);

        ThemeManager::instance().setPreference(ThemeManager::Preference::Dark);
        QApplication::processEvents();
        QVERIFY(panel.grab().save(dir + QStringLiteral("/folder-hierarchy-dark.png")));
        QVERIFY(saveHierarchyDetail(QStringLiteral("task-hierarchy-detail-dark.png")));
        ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
        QApplication::processEvents();

        for (int row = 0; row < navigation->count(); ++row) {
            if (navigation->item(row)->data(Qt::UserRole).toInt()
                == static_cast<int>(ViewScope::Next7Days)) {
                navigation->setCurrentRow(row);
                break;
            }
        }
        QTRY_COMPARE(model->filter().renderer, ViewRenderer::Time);
        QMimeData taskMime;
        taskMime.setData(TaskListView::mimeType(), parent.toUtf8());
        const QPoint taskPoint = view->visualRect(model->indexForTask(parent)).center();
        QDragEnterEvent taskEnter(taskPoint, Qt::MoveAction, &taskMime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &taskEnter);
        QApplication::processEvents();
        QVERIFY(view->grab().save(dir + QStringLiteral("/time-empty-drop-sections.png")));
        QDragLeaveEvent taskLeave;
        QApplication::sendEvent(view->viewport(), &taskLeave);
        QVERIFY(!ordinary.isEmpty());
    }
    void visualPreview() {
        if (QGuiApplication::platformName() != QLatin1String("windows"))
            QSKIP("需要原生 Windows 窗口验收浮层与截图");
        const QString group = m_store->createList(QStringLiteral("工作安排"), QString(), EntityKind::Task,
                                                  QStringLiteral("💼"), QStringLiteral("#4CA1FF"));
        const QDate today = toAppTime(appNow()).date();
        Entity task;
        const auto id = m_store->createTask(QStringLiteral("完成本周项目复盘"), group);
        task = m_store->entity(id);
        task.startAt = fromAppTime(QDateTime(today, QTime(9, 0)));
        task.dueAt = fromAppTime(QDateTime(today, QTime(17, 30)));
        task.reminderAt = task.dueAt.addSecs(-600);
        task.priority = 1;
        task.body = QStringLiteral(
            "## 本周要点\n\n整理 **完成情况**，<u>记录需要协调的事项</u>。\n\n"
            "- 汇总本周进展\n- 确认下周安排\n\n"
            "> <mark data-color=\"yellow\">把下一步写清楚</mark>，让事情更容易推进。");
        ChecklistItem one;
        one.text = QStringLiteral("汇总材料");
        one.done = true;
        task.checklist << one;
        one.text = QStringLiteral("核对时间表");
        one.done = false;
        task.checklist << one;
        task.tags << QStringLiteral("本周") << QStringLiteral("协作");
        QVERIFY(m_store->saveTask(task));
        const QString attachmentSource = referenceImage(m_dir->path()); QVERIFY(!attachmentSource.isEmpty());
        QVERIFY(!m_store->importAttachment(id, attachmentSource).id.isEmpty());
        QVERIFY(m_store->saveDesktopState(id, m_store->entity(id).color, m_store->entity(id).geometry,
                                          m_store->entity(id).zOrder, m_store->entity(id).layer, true));
        for (const auto& title : {QStringLiteral("预约下周会议室"),
                                  QStringLiteral("整理个人学习笔记"),
                                  QStringLiteral("确认下周工作清单")})
            QVERIFY(!m_store->createTask(title).isEmpty());
        const QString overdue = m_store->createTask(QStringLiteral("补充上周验收记录"), group);
        Entity overdueTask = m_store->entity(overdue);
        overdueTask.allDayDate = today.addDays(-1);
        QVERIFY(m_store->saveTask(overdueTask));
        const QString tomorrow = m_store->createTask(QStringLiteral("准备明日评审材料"), group);
        Entity tomorrowTask = m_store->entity(tomorrow);
        tomorrowTask.allDayDate = today.addDays(1);
        QVERIFY(m_store->saveTask(tomorrowTask));
        const QString archived = m_store->createList(QStringLiteral("历史参考资料"), QString(),
            EntityKind::Task, QStringLiteral("🗂️"), QStringLiteral("#C09EEB"));
        QVERIFY(!m_store->createTask(QStringLiteral("归档清单中的示例任务"), archived).isEmpty());
        QVERIFY(m_store->setListArchived(archived, true));
        TaskPanel panel(m_store.get());
        panel.resize(1240, 820);
        panel.showTask(id);
        QTest::qWait(100);
        const qreal expectedDpr = qEnvironmentVariable("KYLINLIST_EXPECT_DPR").toDouble();
        if (expectedDpr > 0)
            QVERIFY2(qAbs(panel.devicePixelRatioF() - expectedDpr) < 0.01,
                     qPrintable(QStringLiteral("实际 DPR=%1，预期=%2")
                         .arg(panel.devicePixelRatioF()).arg(expectedDpr)));
        auto* detail = panel.findChild<TaskDetailWidget*>(); QVERIFY(detail);
        auto* attachments = detail->findChild<AttachmentWidget*>(); QVERIFY(attachments);
        QTRY_VERIFY(attachments->isVisible());
        auto* attachmentIcon = attachments->findChild<QLabel*>(QStringLiteral("AttachmentIcon")); QVERIFY(attachmentIcon);
        auto* attachmentName = attachments->findChild<QLabel*>(QStringLiteral("AttachmentName")); QVERIFY(attachmentName);
        QVERIFY(attachmentIcon->isVisible()); QVERIFY(attachmentName->isVisible());
        QCOMPARE(attachmentName->text(), QStringLiteral("reference.png"));
        const QString dir = qEnvironmentVariable("KYLINLIST_SCREENSHOT_DIR");
        if (!dir.isEmpty()) {
            QVERIFY(QDir().mkpath(dir));
            QVERIFY(panel.grab().save(dir + QStringLiteral("/task-panel.png")));
            ThemeManager::instance().setPreference(ThemeManager::Preference::Dark);
            QApplication::processEvents();
            QImage darkPanel = panel.grab().toImage();
            QImage darkOnGray(darkPanel.size(), QImage::Format_ARGB32_Premultiplied);
            darkOnGray.fill(QColor(QStringLiteral("#808080")));
            QPainter darkPainter(&darkOnGray);
            darkPainter.drawImage(0, 0, darkPanel);
            darkPainter.end();
            QVERIFY(darkOnGray.save(dir + QStringLiteral("/task-panel-dark.png")));
            ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
            QApplication::processEvents();
            auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
            const int originalRow = navigation->currentRow();
            for (int row = 0; row < navigation->count(); ++row) {
                const auto* item = navigation->item(row);
                if (!item->data(NavigationDelegate::SectionRole).toBool()
                    && item->data(Qt::UserRole).toInt() == static_cast<int>(ViewScope::Next7Days)) {
                    navigation->setCurrentRow(row);
                    break;
                }
            }
            QApplication::processEvents();
            QVERIFY(panel.grab().save(dir + QStringLiteral("/task-panel-week.png")));
            navigation->setCurrentRow(originalRow);
            auto* addListButton = panel.findChild<QToolButton*>(QStringLiteral("GroupMenuButton"));
            QVERIFY(addListButton);
            QTest::mouseMove(addListButton); QApplication::processEvents();
            QVERIFY(navigation->grab().save(dir + QStringLiteral("/sidebar-list-hover.png")));
            QImage divider = panel.grab().toImage().copy(
                rectIn(panel.findChild<QFrame*>(QStringLiteral("ListPane")), &panel).right() - 12, 16, 40, 100);
            QVERIFY(divider.scaled(240, 600, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .save(dir + QStringLiteral("/workspace-divider-detail.png")));
            QImage corner = panel.grab().toImage().copy(panel.width() - 190, 0, 190, 150);
            QImage onLight(corner.size(), QImage::Format_ARGB32_Premultiplied);
            onLight.fill(QColor(QStringLiteral("#E9E9E9")));
            QPainter cornerPainter(&onLight);
            cornerPainter.drawImage(0, 0, corner);
            cornerPainter.end();
            QVERIFY(corner.save(dir + QStringLiteral("/window-corner-detail.png")));
            QImage bottom = panel.grab().toImage().copy(panel.width() - 190, panel.height() - 150, 190, 150);
            QImage bottomOnLight(bottom.size(), QImage::Format_ARGB32_Premultiplied);
            bottomOnLight.fill(QColor(QStringLiteral("#E9E9E9")));
            QPainter bottomPainter(&bottomOnLight);
            bottomPainter.drawImage(0, 0, bottom);
            bottomPainter.end();
            QVERIFY(bottomOnLight.scaled(760, 600, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .save(dir + QStringLiteral("/window-bottom-corner-detail.png")));
            QVERIFY(m_store->setCompleted(id, true));
            auto* toast = panel.findChild<ToastIsland*>(QStringLiteral("WorkspaceToast"));
            QVERIFY(toast);
            QTRY_VERIFY(toast->isVisible());
            QVERIFY(panel.grab().save(dir + QStringLiteral("/task-panel-toast.png")));
            QVERIFY(m_store->setStatus(id, TaskStatus::Active));
            toast->hide();
            panel.showTask();
            QTest::qWait(30);
            auto* closeDetail = detail->findChild<QToolButton*>(QStringLiteral("DetailClose"));
            QVERIFY(closeDetail);
            QTest::mouseClick(closeDetail, Qt::LeftButton);
            QTRY_VERIFY(panel.findChild<QFrame*>(QStringLiteral("DetailFrame"))->isVisible());
            QVERIFY(panel.findChild<TaskDetailWidget*>()->entityId().isEmpty());
            QVERIFY(panel.grab().save(dir + QStringLiteral("/task-panel-three-columns.png")));
            panel.showTask(id);
            QTest::qWait(30);
            QVERIFY(detail->grab().save(dir + QStringLiteral("/task-detail.png")));
            panel.resize(900, 700);
            QTest::qWait(50);
            QVERIFY(panel.grab().save(dir + QStringLiteral("/task-panel-auto-three-columns.png")));
            panel.showTask(id);
            QTest::qWait(50);
            QVERIFY(panel.grab().save(dir + QStringLiteral("/task-panel-detail-overlay.png")));
            panel.resize(700, 620);
            QTest::qWait(50);
            QVERIFY(panel.grab().save(dir + QStringLiteral("/task-panel-auto-two-columns.png")));
            panel.resize(1240, 820);
            QTest::qWait(50);
            NoteWidget note(m_store->entity(id)); note.setStore(m_store.get());
            note.show();
            QTest::qWait(50);
            QVERIFY(note.grab().save(dir + QStringLiteral("/desktop-note.png")));
            note.hide();
            panel.activateWindow(); panel.raise(); QApplication::processEvents();

            auto* info = detail->findChild<QToolButton*>(QStringLiteral("DetailInfo")); QVERIFY(info);
            QTest::mouseClick(info, Qt::LeftButton);
            auto* infoPopup = detail->findChild<EntityInfoPopup*>(QStringLiteral("EntityInfoPopup")); QVERIFY(infoPopup);
            QTRY_VERIFY(infoPopup->isVisible());
            QVERIFY(nativePopupShadowDisabled(infoPopup));
            QVERIFY(savePopupPreview(detail, infoPopup, dir + QStringLiteral("/detail-info.png")));
            infoPopup->hide();
            auto* format = detail->findChild<QToolButton*>(QStringLiteral("FooterFormat")); QVERIFY(format);
            QTest::mouseClick(format, Qt::LeftButton);
            auto* formatPanel = detail->findChild<QFrame*>(QStringLiteral("FormatPanel")); QVERIFY(formatPanel);
            QTRY_VERIFY(formatPanel->isVisible());
            QVERIFY(nativePopupShadowDisabled(formatPanel));
            QVERIFY(savePopupPreview(detail, formatPanel, dir + QStringLiteral("/detail-toolbar.png")));
            formatPanel->hide();
            auto* group = detail->findChild<QToolButton*>(QStringLiteral("GroupChip")); QVERIFY(group);
            QTest::mouseClick(group, Qt::LeftButton);
            auto* groupPopup = detail->findChild<ListSelectPopup*>(); QVERIFY(groupPopup);
            QTRY_VERIFY(groupPopup->isVisible());
            QVERIFY(nativePopupShadowDisabled(groupPopup));
            QVERIFY(savePopupPreview(detail, groupPopup, dir + QStringLiteral("/detail-list.png")));
            groupPopup->hide();

            auto* dateChip = detail->findChild<QPushButton*>(QStringLiteral("DateChip")); QVERIFY(dateChip);
            QTest::mouseClick(dateChip, Qt::LeftButton);
            DatePickerPopup* datePopup = nullptr;
            QTRY_VERIFY((datePopup = detail->findChild<DatePickerPopup*>()) != nullptr);
            QTRY_VERIFY(datePopup->isVisible());
            QVERIFY(nativePopupShadowDisabled(datePopup));
            QVERIFY(savePopupPreview(detail, datePopup, dir + QStringLiteral("/detail-date-light.png"),
                                     QColor(QStringLiteral("#E9E9E9"))));
            ThemeManager::instance().setPreference(ThemeManager::Preference::Dark);
            QApplication::processEvents();
            QVERIFY(savePopupPreview(detail, datePopup, dir + QStringLiteral("/detail-date-dark.png"),
                                     QColor(QStringLiteral("#24272E"))));
            ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
            QApplication::processEvents();
            QRect calendarBounds;
            const auto dayButtons = datePopup->findChildren<QToolButton*>(QStringLiteral("DatePickerDay"));
            QCOMPARE(dayButtons.size(), 42);
            for (QToolButton* day : dayButtons) {
                const QRect dayBounds = rectIn(day, datePopup);
                calendarBounds = calendarBounds.isNull() ? dayBounds : calendarBounds.united(dayBounds);
            }
            QImage calendarGrid = datePopup->grab().toImage().copy(calendarBounds);
            QVERIFY(calendarGrid.scaled(calendarBounds.width() * 3, calendarBounds.height() * 3,
                                        Qt::KeepAspectRatio, Qt::SmoothTransformation)
                        .save(dir + QStringLiteral("/detail-date-grid.png")));
            QPushButton* timeEntry = nullptr;
            for (auto* button : datePopup->findChildren<QPushButton*>(QStringLiteral("DatePickerEntry")))
                if (button->accessibleName() == QStringLiteral("时间")) timeEntry = button;
            QVERIFY(timeEntry); QTest::mouseClick(timeEntry, Qt::LeftButton);
            auto* noTime = datePopup->findChild<QPushButton*>(QStringLiteral("DatePickerNoTime"));
            QVERIFY(noTime); QTRY_VERIFY(noTime->isVisible());
            QVERIFY(savePopupPreview(detail, datePopup, dir + QStringLiteral("/detail-time-light.png"),
                                     QColor(QStringLiteral("#E9E9E9"))));
            datePopup->reject();

            Entity reminderTask = m_store->entity(id);
            TaskReminder reminderSpec;
            reminderSpec.id = newUuid();
            reminderSpec.entityId = id;
            reminderSpec.triggerAt = appNow();
            reminderTask.reminders << reminderSpec;
            ReminderPopup reminder(reminderTask, reminderSpec.id);
            reminder.show();
            QTRY_VERIFY(reminder.isVisible());
            QVERIFY(nativePopupShadowDisabled(&reminder));
            QVERIFY(savePopupPreview(&reminder, nullptr, dir + QStringLiteral("/reminder-light.png"),
                                     QColor(QStringLiteral("#E9E9E9"))));
            ThemeManager::instance().setPreference(ThemeManager::Preference::Dark);
            QApplication::processEvents();
            QVERIFY(savePopupPreview(&reminder, nullptr, dir + QStringLiteral("/reminder-dark.png"),
                                     QColor(QStringLiteral("#24272E"))));
            ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
            QApplication::processEvents();
            auto* reminderSnooze = reminder.findChild<QToolButton*>(QStringLiteral("ReminderSnoozeButton"));
            auto* reminderMenu = reminder.findChild<QMenu*>(QStringLiteral("ReminderSnoozeMenu"));
            QVERIFY(reminderSnooze && reminderMenu);
            reminderMenu->popup(reminderSnooze->mapToGlobal(QPoint(0, reminderSnooze->height())));
            QTRY_VERIFY(reminderMenu->isVisible());
            QVERIFY(nativePopupShadowDisabled(reminderMenu));
            QVERIFY(savePopupPreview(&reminder, reminderMenu, dir + QStringLiteral("/reminder-snooze-light.png"),
                                     QColor(QStringLiteral("#E9E9E9"))));
            ThemeManager::instance().setPreference(ThemeManager::Preference::Dark);
            QApplication::processEvents();
            QVERIFY(savePopupPreview(&reminder, reminderMenu, dir + QStringLiteral("/reminder-snooze-dark.png"),
                                     QColor(QStringLiteral("#24272E"))));
            ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
            QApplication::processEvents();
            reminderMenu->hide();
            reminder.hide();
            // 原生 Windows 上菜单窗口也必须没有系统直角投影。
            QMenu shadowProbe;
            shadowProbe.addAction(QStringLiteral("菜单阴影检查"));
            shadowProbe.popup(detail->mapToGlobal(QPoint(24, 24)));
            QTRY_VERIFY(shadowProbe.isVisible());
            QVERIFY(nativePopupShadowDisabled(&shadowProbe));
            shadowProbe.hide();

            PopupMenu trashMenu(&panel);
            trashMenu.addAction(AppIcons::icon(IconId::Restore), QStringLiteral("恢复"));
            trashMenu.addAction(AppIcons::icon(IconId::Delete), QStringLiteral("永久删除"));
            trashMenu.popup(panel.mapToGlobal(QPoint(520, 240)));
            QTRY_VERIFY(trashMenu.isVisible());
            QVERIFY(trashMenu.grab().save(dir + QStringLiteral("/trash-context-menu.png")));
            trashMenu.hide();

            ListEditorDialog listEditor(m_store->lists(), m_store->folders(), &panel);
            listEditor.setNewListDefaults(QString(), EntityKind::Task);
            listEditor.findChild<QLineEdit*>(QStringLiteral("ListNameEdit"))->setText(QStringLiteral("旅行计划"));
            listEditor.show(); listEditor.activateWindow();
            listEditor.findChild<QLineEdit*>(QStringLiteral("ListNameEdit"))->setFocus();
            QTRY_VERIFY(listEditor.findChild<QLineEdit*>(QStringLiteral("ListNameEdit"))->hasFocus());
            QTest::qWait(50);
            QVERIFY(listEditor.grab().save(dir + QStringLiteral("/list-editor-light.png")));
            auto* customColor = listEditor.findChild<QToolButton*>(QStringLiteral("ListCustomColorButton"));
            QVERIFY(customColor); QTest::mouseClick(customColor, Qt::LeftButton);
            ColorPickerPopup* colorPicker = nullptr;
            QTRY_VERIFY((colorPicker = listEditor.findChild<ColorPickerPopup*>()) != nullptr);
            QTRY_VERIFY(colorPicker->isVisible());
            QVERIFY(savePopupPreview(&listEditor, colorPicker, dir + QStringLiteral("/list-colors.png")));
            auto* customEntry = colorPicker->findChild<QPushButton*>(QStringLiteral("ColorCustomEntry"));
            QVERIFY(customEntry); QTest::mouseClick(customEntry, Qt::LeftButton);
            QVERIFY(savePopupPreview(&listEditor, colorPicker, dir + QStringLiteral("/list-color-custom.png")));
            colorPicker->reject();
            auto* iconButton = listEditor.findChild<QToolButton*>(QStringLiteral("ListIconButton"));
            QVERIFY(iconButton); QTest::mouseClick(iconButton, Qt::LeftButton);
            EmojiPickerPopup* emojiPicker = nullptr;
            QTRY_VERIFY((emojiPicker = listEditor.findChild<EmojiPickerPopup*>()) != nullptr);
            QTRY_VERIFY(emojiPicker->isVisible());
            QVERIFY(savePopupPreview(&listEditor, emojiPicker, dir + QStringLiteral("/list-emoji.png")));
            emojiPicker->reject();
            ThemeManager::instance().setPreference(ThemeManager::Preference::Dark);
            QApplication::processEvents();
            QVERIFY(listEditor.grab().save(dir + QStringLiteral("/list-editor-dark.png")));
            ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
            QApplication::processEvents(); listEditor.reject();

            SettingsDialog settings(&panel); settings.show(); QTest::qWait(50);
            QVERIFY(settings.grab().save(dir + QStringLiteral("/settings-smart-light.png")));
            ThemeManager::instance().setPreference(ThemeManager::Preference::Dark);
            QApplication::processEvents();
            QVERIFY(settings.grab().save(dir + QStringLiteral("/settings-smart-dark.png")));
            QVERIFY(panel.grab().save(dir + QStringLiteral("/task-panel-dark.png")));
            ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
            QApplication::processEvents(); settings.reject();

            ConfirmDialog confirm(QStringLiteral("删除清单“旅行计划”？"),
                QStringLiteral("清单内的所有任务和笔记也将被移入垃圾桶。"),
                QStringLiteral("删除"), true, &panel);
            confirm.show(); QTest::qWait(30);
            QVERIFY(confirm.grab().save(dir + QStringLiteral("/confirm-delete.png")));
            confirm.reject();

            QToolButton tooltipProbe;
            tooltipProbe.setToolTip(QStringLiteral("添加清单"));
            tooltipProbe.show(); tooltipProbe.activateWindow(); QTest::qWait(30);
            AppToolTip::showText(tooltipProbe.mapToGlobal(QPoint(0, tooltipProbe.height())),
                                 tooltipProbe.toolTip(), &tooltipProbe, tooltipProbe.rect(), 0);
            QTRY_VERIFY(AppToolTip::isVisible());
            QWidget* tooltip = AppToolTip::window();
            QVERIFY(tooltip);
            QVERIFY(tooltip->grab().save(dir + QStringLiteral("/tooltip.png")));
            AppToolTip::hideText(); tooltipProbe.hide();

            TaskDetailWidget narrow(m_store.get()); narrow.resize(360, 700); QVERIFY(narrow.setTask(id)); narrow.show();
            auto* narrowTitle = narrow.findChild<QLineEdit*>(QStringLiteral("TaskTitle")); QVERIFY(narrowTitle);
            narrow.activateWindow(); narrowTitle->setFocus(); narrowTitle->deselect();
            QTest::qWait(50); QVERIFY(narrow.width() <= 380);
            QVERIFY(narrow.grab().save(dir + QStringLiteral("/detail-narrow.png")));
            Entity longTask = m_store->entity(id);
            for (int line = 1; line <= 35; ++line)
                longTask.body += QStringLiteral("\n\n### 第 %1 段\n长文档与标题共用外层滚动区域，检查项和参考附件始终接在正文之后。").arg(line);
            QVERIFY(m_store->saveTask(longTask));
            auto* scroll = narrow.findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
            auto* editor = narrow.findChild<MarkdownEditor*>(); QVERIFY(scroll && editor);
            QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
            QTRY_COMPARE(editor->textEditor()->verticalScrollBar()->maximum(), 0);
            QTRY_COMPARE(scroll->horizontalScrollBar()->maximum(), 0);
            scroll->verticalScrollBar()->setValue(0); QApplication::processEvents();
            QVERIFY(narrow.grab().save(dir + QStringLiteral("/detail-narrow-long.png")));
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum()); QApplication::processEvents();
            QVERIFY(narrow.grab().save(dir + QStringLiteral("/detail-narrow-bottom.png")));
        }
    }

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<TaskStore> m_store;
};
QTEST_MAIN(UiTest)
#include "tst_ui.moc"
