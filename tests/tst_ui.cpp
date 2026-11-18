#include <cstdio>
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
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
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QSettings>
#include <QSet>
#include <QSqlQuery>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QSystemTrayIcon>
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
#include <QWindow>
#include <QtTest>
#include <memory>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include "app/NoteManager.h"
#include "app/AutostartSettings.h"
#include "app/EntityTextSession.h"
#include "app/KylinListApp.h"
#include "app/ReminderScheduler.h"
#include "app/Settings.h"
#include "app/TaskStore.h"
#include "app/TrayController.h"
#include "app/updates/InstallIdentity.h"
#include "app/updates/UpdateService.h"
#include "db/Database.h"
#include "domain/InternalLink.h"
#include "domain/TaskDrop.h"
#include "domain/TaskGrouping.h"
#include "domain/ViewSpec.h"
#include "platform/SystemShadow.h"
#include "platform/ScreenFit.h"
#include "platform/Autostart.h"
#include "ui/AppStyle.h"
#include "ui/AppIcons.h"
#include "ui/AppToolTip.h"
#include "ui/ActivityTimelineWidget.h"
#include "ui/EntityCommands.h"
#include "ui/EntityHoverPreview.h"
#include "ui/EntityPicker.h"
#include "ui/AttachmentAppearance.h"
#include "ui/AttachmentWidget.h"
#include "ui/DetailPopups.h"
#include "ui/DisplaySettingsDialog.h"
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
#include "ui/PopupShadow.h"
#include "ui/TaskDateController.h"
#include "ui/SettingsDialog.h"
#include "ui/SmartListSettingsWidget.h"
#include "ui/Markdown.h"
#include "ui/MarkdownEditor.h"
#include "ui/NoteWidget.h"
#include "ui/ReminderPopup.h"
#include "ui/RelatedDetailPopup.h"
#include "ui/StatsDialog.h"
#include "ui/ResizeGrip.h"
#include "ui/TaskDetailWidget.h"
#include "ui/TaskDelegate.h"
#include "ui/TaskListModel.h"
#include "ui/TaskListView.h"
#include "ui/TaskPanel.h"
#include "ui/TaskWidget.h"
#include "ui/SidebarSectionHeader.h"
#include "ui/ToastIsland.h"
#include "ui/UpdateDialog.h"
#include "ui/WindowChrome.h"
#include "ui/WorkspaceGeometry.h"
#include "ui/design/QssBuilder.h"
#include "ui/design/FontManager.h"
#include "ui/design/ThemeManager.h"
#include "ui/design/TypeSystem.h"
using namespace kl;

namespace {
// Qt 5.12 Windows 的 QMessageBox::showEvent 会无 null 检查地查询 Win32 SC_CLOSE。
// 该 offscreen 宿主没有 native interface；只把真实消息框交互单独留给原生测试。
bool messageBoxRequiresNativePlatform() {
#if defined(Q_OS_WIN) && QT_VERSION < QT_VERSION_CHECK(5, 13, 0)
    return QGuiApplication::platformName() == QLatin1String("offscreen")
        && !QGuiApplication::platformNativeInterface();
#else
    return false;
#endif
}
const char* const MessageBoxNativeOnlyReason =
    "Qt 5.12 Windows offscreen has no native interface; QMessageBox's SC_CLOSE Show hook dereferences it. Run this dialog case with the windows platform.";

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

// 详情「更多」菜单现为真实 QAction（lowerCamel objectName）。仅测试辅助：
// 保留旧断言使用的 Cmd* 名称到此映射，不进生产代码。
QString detailCommandActionName(const QString& requested) {
    if (requested == QLatin1String("CmdTogglePin")) return QStringLiteral("togglePin");
    if (requested == QLatin1String("CmdAbandon")) return QStringLiteral("abandon");
    if (requested == QLatin1String("CmdReactivate")) return QStringLiteral("reactivate");
    if (requested == QLatin1String("CmdViewActivity")) return QStringLiteral("viewActivity");
    if (requested == QLatin1String("CmdDelete")) return QStringLiteral("delete");
    if (requested == QLatin1String("CmdConvertKind")) return QStringLiteral("convertKind");
    return requested;
}

// The detail menu may use exec() or popup(). Settle popup activation before
// closing it and dispatching its QAction, so delayed focus events cannot steal
// focus from the command's editor. Close on a missing action to avoid hangs.
bool triggerDetailCommand(TaskDetailWidget& detail, const QString& actionName) {
    auto* button = detail.findChild<QToolButton*>(QStringLiteral("DetailMore"));
    if (!button || !button->isVisible() || !button->isEnabled()) return false;
    const QString resolvedName = detailCommandActionName(actionName);
    QTimer confirmation;
    confirmation.setInterval(10);
    QObject::connect(&confirmation, &QTimer::timeout, &confirmation, []() {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            if (auto* yes = box->button(QMessageBox::Yes)) yes->click();
    });
    if (resolvedName == QLatin1String("deletePermanently"))
        confirmation.start();  // 仅永久删除仍弹确认框；转换类型不再确认。
    bool settled = false, triggered = false;
    QElapsedTimer elapsed;
    elapsed.start();
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, &poll, [&]() {
        auto* menu = detail.findChild<QMenu*>(QStringLiteral("DetailMoreMenu"));
        if (menu && menu->isVisible()) {
            auto* action = menu->findChild<QAction*>(resolvedName);
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
        qRegisterMetaType<TaskDrop::Intent>("TaskDrop::Intent");
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
        FontManager::instance().setPreference(QStringLiteral("default"));
        QString error;
        QVERIFY2(Database::instance().openAt(m_dir->path() + QStringLiteral("/kylinlist.db"), &error),
                 qPrintable(error));
        m_store.reset(new TaskStore(Database::instance().connection()));
        QVERIFY(m_store->load(&error));
    }
    void cleanup() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
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
        editor.resize(400, 240); editor.show(); editor.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&editor));
        QVERIFY(QTest::qWaitForWindowActive(&editor));
        editor.textEditor()->setFocus(); QTRY_VERIFY(editor.textEditor()->hasFocus());
        editor.textEditor()->selectAll();
        QVERIFY(!editor.findChild<QWidget*>(QStringLiteral("FormatActionHost")));
        QTest::keyClick(editor.textEditor(), Qt::Key_B, Qt::ControlModifier);
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
        // Entity editors share one source history: native QTextEdit undo is disabled, the user
        // path is Ctrl+Z/Ctrl+Y handled by EditorTextEdit and replayed by EntityTextSession.
        QVERIFY(body->hasFocus());
        QTest::keyClick(body, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(editor->content(), QStringLiteral("hello"));
        QVERIFY(detail.flush()); QCOMPARE(m_store->entity(id).body, QStringLiteral("hello"));
        QTest::keyClick(body, Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(editor->content(), expected);
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
        // Shared source history through the real Ctrl+Z/Ctrl+Y user path, as above.
        // 高亮后焦点可能被编辑器弹层/异步收尾移走；真实用户先关掉自己的弹层、
        // 点回正文再按快捷键，不假设弹层关闭会同步归还焦点，也不用基类 undo 绕过共享历史。
        QTest::keyClick(body, Qt::Key_Escape);
        detail.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&detail));
        QVERIFY(body->viewport()->isVisible());
        QVERIFY(!body->viewport()->visibleRegion().isEmpty());
        QTest::mouseClick(body->viewport(), Qt::LeftButton, Qt::NoModifier,
                          body->viewport()->rect().center());
        QTRY_VERIFY(body->hasFocus());
        QTest::keyClick(body, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(editor->content(), yellow);
        QTest::keyClick(body, Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(editor->content(), blue);
        title->setFocus(); QApplication::processEvents(); QVERIFY(!body->hasFocus());
        QCOMPARE(editor->content(), blue); QCOMPARE(body->toPlainText(), literal);
        QCOMPARE(editor->plainTextContent(), literal); QCOMPARE(editor->preview()->document()->toPlainText(), literal);

        selection = body->document()->find(literal); QVERIFY(!selection.isNull());
        QCOMPARE(selection.selectedText(), literal); body->setTextCursor(selection);
        editor->applyFormat(MarkdownEditor::FormatAction::Underline);
        QCOMPARE(editor->content(), underlined);
        // 同上：先关弹层、点回正文取得焦点，再走真实 Ctrl+Z/Ctrl+Y。
        QTest::keyClick(body, Qt::Key_Escape);
        detail.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&detail));
        QVERIFY(body->viewport()->isVisible());
        QVERIFY(!body->viewport()->visibleRegion().isEmpty());
        QTest::mouseClick(body->viewport(), Qt::LeftButton, Qt::NoModifier,
                          body->viewport()->rect().center());
        QTRY_VERIFY(body->hasFocus());
        QTest::keyClick(body, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(editor->content(), blue);
        QTest::keyClick(body, Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(editor->content(), underlined);
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
        const QString body = entityReference(first, QStringLiteral("一")) + QLatin1Char(' ')
            + entityReference(second, QStringLiteral("二")) + QLatin1Char(' ')
            + entityReference(first, QStringLiteral("重复"))
            + QStringLiteral(" ![[%1|图片]] [[%2|大写]] [外链](https://example.com)").arg(second, first.toUpper());
        QCOMPARE(internalTargets(body), QStringList({first, second}));
        QVERIFY(isValidLinkTarget(first));
        QVERIFY(!isValidLinkTarget(first.toUpper())); QVERIFY(!isValidLinkTarget(QStringLiteral("https://example.com")));
        QVERIFY(!isValidLinkTarget(QStringLiteral("not-a-target")));

        const QString listA = m_store->createList(QStringLiteral("目标清单"));
        const QString listB = m_store->createList(QStringLiteral("来源清单"));
        const QString target = m_store->createTask(QStringLiteral("旧标题"), listA);
        const QString source = m_store->createNote(QStringLiteral("来源笔记"), listB);
        Entity sourceEntity = m_store->entity(source);
        sourceEntity.body = entityReference(target); QVERIFY(m_store->saveTask(sourceEntity));
        MarkdownEditor editor; editor.setStore(m_store.get()); editor.setContent(sourceEntity.body);
        QVERIFY(editor.preview()->document()->toPlainText().contains(QStringLiteral("旧标题")));
        Entity renamed = m_store->entity(target); renamed.title = QStringLiteral("新标题"); QVERIFY(m_store->saveTask(renamed));
        const QString html = editor.preview()->toHtml();
        QVERIFY(html.contains(QStringLiteral("新标题"))); QVERIFY(!html.contains(QStringLiteral("旧标题")));
        QCOMPARE(m_store->entity(source).body, sourceEntity.body);
        QVERIFY(Markdown::toHtml(sourceEntity.body, Markdown::entityMap(m_store->entities())).contains(QStringLiteral("class=\"internal-link\"")));
        const QString external = Markdown::toHtml(QStringLiteral("[安全](https://example.com) <script>bad()</script>"));
        QVERIFY(external.contains(QStringLiteral("href=\"https://example.com\""))); QVERIFY(!external.contains(QStringLiteral("<script>")));
        const QString missing = newUuid();
        const QString broken = Markdown::toHtml(entityReference(missing, QStringLiteral("保留文字")), Markdown::entityMap(m_store->entities()));
        QVERIFY(broken.contains(QStringLiteral("<span class=\"broken-link\">保留文字</span>")));
        QVERIFY(!broken.contains(QStringLiteral("href=\"%1\"").arg(missing)));
        QVERIFY(m_store->trashTask(target));
        const QString trash = Markdown::toHtml(sourceEntity.body, Markdown::entityMap(m_store->entities()));
        QVERIFY(trash.contains(QStringLiteral("trash-link"))); QVERIFY(trash.contains(QStringLiteral("回收站")));
        QVERIFY(trash.contains(QStringLiteral("href=\"%1\"").arg(target)));
        QVERIFY(m_store->deletePermanently(target));
        const QString deleted = Markdown::toHtml(sourceEntity.body, Markdown::entityMap(m_store->entities()));
        QVERIFY(deleted.contains(QStringLiteral("关联内容不可用")));
        QVERIFY(!deleted.contains(QStringLiteral("href=\"%1\"").arg(target)));
        QCOMPARE(m_store->entity(source).body, sourceEntity.body);
    }
    void documentPreviewSignalsCrossListsAndHover() {
        const QString listA = m_store->createList(QStringLiteral("甲清单"));
        const QString listB = m_store->createList(QStringLiteral("乙清单"));
        const QString target = m_store->createTask(QStringLiteral("悬浮目标"), listA);
        const QString source = m_store->createNote(QStringLiteral("链接来源"), listB);
        Entity edited = m_store->entity(target); edited.body = QStringLiteral("摘要 **加粗**\n\n第二行");
        edited.dueAt = appNow().addDays(-1); QVERIFY(m_store->saveTask(edited));
        edited = m_store->entity(source); edited.body = entityReference(target); QVERIFY(m_store->saveTask(edited));
        DocumentPreview preview; preview.setStore(m_store.get()); preview.resize(500, 240); preview.show();
        preview.setHtml(Markdown::toHtml(m_store->entity(source).body, Markdown::entityMap(m_store->entities())));
        const QTextCursor anchor = preview.document()->find(QStringLiteral("悬浮目标")); QVERIFY(!anchor.isNull());
        QCOMPARE(preview.internalLinkAt(preview.cursorRect(anchor).center()), target);
        QSignalSpy activated(&preview, &DocumentPreview::internalLinkActivated);
        QMetaObject::invokeMethod(&preview, "anchorClicked", Qt::DirectConnection, Q_ARG(QUrl, QUrl(target)));
        QCOMPARE(activated.count(), 1); QCOMPARE(activated.takeFirst().at(0).toString(), target);
        QVERIFY(m_store->trashTask(target));
        preview.setHtml(Markdown::toHtml(m_store->entity(source).body, Markdown::entityMap(m_store->entities())));
        QMetaObject::invokeMethod(&preview, "anchorClicked", Qt::DirectConnection, Q_ARG(QUrl, QUrl(target)));
        QCOMPARE(activated.count(), 1); // 回收站链接仍可激活
        QVERIFY(m_store->restoreTask(target));

        TaskPanel panel(m_store.get()); panel.showTask(source);
        auto* detail = panel.findChild<TaskDetailWidget*>(); QVERIFY(detail); QCOMPARE(detail->entityId(), source);
        auto* editor = detail->findChild<MarkdownEditor*>(); QVERIFY(editor);
        QMetaObject::invokeMethod(editor, "internalLinkActivated", Qt::DirectConnection, Q_ARG(QString, target));
        QCOMPARE(detail->entityId(), source); // 正文关联不替换原第四列。
        auto* related = panel.relatedDetailPopup(); QVERIFY(related);
        QVERIFY(related->isVisible()); QCOMPARE(related->entityId(), target);
        QVERIFY(related->requestClose());
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
        QVERIFY(!editor.findChild<QToolButton*>(QStringLiteral("insertEntityReference")));
        QCOMPARE(editor.formatLabel(MarkdownEditor::FormatAction::EntityReference), QStringLiteral("关联任务/笔记"));
        QVERIFY(editor.insertEntityReference(task));
        QCOMPARE(editor.content(), entityReference(task, QStringLiteral("目标 [A]")));
        QVERIFY(editor.preview()->toHtml().contains(QStringLiteral("目标 [A]")));
        QVERIFY(!editor.insertEntityReference(newUuid()));
        QCOMPARE(editor.textEditor()->acceptRichText(), false);
    }
    void filtersUseShanghaiCalendarAndSearch() {
        const QDateTime now = fromAppTime(QDateTime(QDate(2026, 11, 6), QTime(12, 0), Qt::LocalTime));
        const QDate today = toAppTime(now).date();
        Entity task; task.id = newUuid(); task.title = QStringLiteral("整理材料"); task.listId = newUuid();
        task.startAt = now.addDays(-1); task.dueAt = now; task.tags << QStringLiteral("工作");
        // 独立检查项已并入正文（schema v8）；清单文字作为正文内容参与搜索。
        task.body = QStringLiteral("- [ ] 核对清单");
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
        early.allDayDate = QDate(2025, 1, 1); early.dueAt = QDateTime();
        QCOMPARE(effectiveDate(early), QDate(2025, 1, 1));
        early.allDayDate = QDate(); early.startAt = QDateTime();
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
        note.listId = newUuid(); note.dueAt = fromAppTime(QDateTime(today, QTime(9, 0)));
        note.startAt = fromAppTime(QDateTime(today.addDays(-2), QTime(9, 0))); note.priority = 3;
        note.tags << QStringLiteral("标签");
        note.body = QStringLiteral("完整正文\n- [ ] 清单内容");
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
        ViewSpec time; time.renderer = ViewRenderer::Time;
        QVERIFY(contains(note, time, today)); // 旧 renderer 标识不再决定筛选，普通视图仍包含笔记。
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
        // 默认分组是时间桶；段 key 现为 GroupKey::stableId()（time:today 等）。
        ViewSpec list;
        QCOMPARE(list.grouping, GroupMode::Time);
        const QVector<ViewSection> sections = buildSections(all, list, today);
        QCOMPARE(sections.size(), 5);
        QCOMPARE(sections.at(0).key, QStringLiteral("pinned"));
        QCOMPARE(sections.at(1).key, QStringLiteral("time:overdue"));
        QCOMPARE(sections.at(2).key, QStringLiteral("time:today"));
        QCOMPARE(sections.at(3).key, QStringLiteral("time:undated"));
        QCOMPARE(sections.at(4).key, QStringLiteral("notes"));
        QCOMPARE(sections.at(0).title, QStringLiteral("已置顶"));
        QCOMPARE(sections.at(2).title, QStringLiteral("今天") + QStringLiteral(" 周五"));
        QCOMPARE(sections.at(0).items.size(), 3);
        // 置顶组内：任务在前、笔记殿后；同类别按有效日期升序。
        QCOMPARE(sections.at(0).items.at(0).id, pinnedTask.id);
        QCOMPARE(sections.at(0).items.at(1).id, futurePinned.id);
        QCOMPARE(sections.at(0).items.at(2).id, pinnedNote.id);
        QCOMPARE(sections.at(1).items.at(0).id, overdueA.id); QCOMPARE(sections.at(1).items.at(1).id, overdueB.id);
        QCOMPARE(sections.at(2).items.size(), 1); QCOMPARE(sections.at(2).items.first().id, todayNine.id);
        QCOMPARE(sections.at(3).items.at(0).id, noDateA.id); QCOMPARE(sections.at(3).items.at(1).id, noDateB.id);
        QCOMPARE(sections.at(4).items.at(0).id, noteA.id); QCOMPARE(sections.at(4).items.at(1).id, noteB.id);

        ViewSpec time; time.scope = ViewScope::Today; time.renderer = ViewRenderer::Time;
        QVERIFY(!contains(futurePinned, time, today)); // 置顶不豁免今天范围
        QVERIFY(!contains(noDateA, time, today)); QVERIFY(!contains(noteA, time, today));
        const QVector<ViewSection> timed = buildSections(all, time, today);
        QCOMPARE(timed.size(), 3); QCOMPARE(timed.at(0).key, QStringLiteral("pinned"));
        QCOMPARE(timed.at(0).items.size(), 1); QCOMPARE(timed.at(0).items.first().id, pinnedTask.id);
        QCOMPARE(timed.at(1).key, QStringLiteral("time:overdue"));
        QCOMPARE(timed.at(1).items.size(), 2);
        QCOMPARE(timed.at(2).key, QStringLiteral("time:today"));
        QCOMPARE(timed.at(2).items.first().id, todayNine.id);
        time.scope = ViewScope::Next7Days; QVERIFY(contains(futurePinned, time, today));
        time.scope = ViewScope::Overdue; QVERIFY(!contains(todayNine, time, today));

        Entity completedA = entity("00000000-0000-4000-8000-000000000050");
        Entity completedB = entity("00000000-0000-4000-8000-000000000051");
        completedA.status = completedB.status = TaskStatus::Completed; completedA.completed = completedB.completed = true;
        completedA.completedAt = base; completedB.completedAt = base.addSecs(10); completedA.pinnedAt = base.addSecs(20);
        ViewSpec completed; completed.scope = ViewScope::Completed;
        const QVector<ViewSection> archive = buildSections({completedA, completedB, pinnedNote}, completed, today);
        QCOMPARE(archive.size(), 1); QCOMPARE(archive.first().key, QStringLiteral("time:undated"));
        QCOMPARE(archive.first().title, QStringLiteral("无日期"));
        QCOMPARE(archive.first().items.size(), 2); QCOMPARE(archive.first().items.at(0).id, completedB.id);
        QCOMPARE(archive.first().items.at(1).id, completedA.id); // 归档不设置顶段
        completedB.completedAt = completedA.completedAt;
        const QVector<ViewSection> tied = buildSections({completedB, completedA}, completed, today);
        QCOMPARE(tied.first().items.at(0).id, completedA.id); QCOMPARE(tied.first().items.at(1).id, completedB.id);
        Entity abandoned = completedA; abandoned.status = TaskStatus::Abandoned; abandoned.completed = false; abandoned.abandonedAt = base;
        ViewSpec abandonedSpec; abandonedSpec.scope = ViewScope::Abandoned;
        QCOMPARE(buildSections({abandoned}, abandonedSpec, today).first().key, QStringLiteral("time:undated"));
        QCOMPARE(buildSections({abandoned}, abandonedSpec, today).first().title, QStringLiteral("无日期"));
        Entity trashedNote = noteA; trashedNote.deleted = true; trashedNote.deletedAt = base;
        Entity trashedTask = todayNine; trashedTask.deleted = true; trashedTask.deletedAt = base.addSecs(1);
        ViewSpec trash; trash.scope = ViewScope::Trash;
        const QVector<ViewSection> trashSections = buildSections({trashedNote, trashedTask}, trash, today);
        QCOMPARE(trashSections.size(), 2);
        QCOMPARE(trashSections.first().key, QStringLiteral("time:today"));
        QCOMPARE(trashSections.first().items.at(0).id, trashedTask.id);
        QCOMPARE(trashSections.last().key, QStringLiteral("time:undated"));
        QCOMPARE(trashSections.last().items.at(0).id, trashedNote.id);
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
        // 旧 QDateTimeEdit 表单已删除；0:30 墙上时间的宿主时区无关性由
        // TaskDateController + DatePickerPopup 承担。
        const QString id = m_store->createTask(QStringLiteral("上海墙上时间"));
        QVERIFY(isUuid(id));
        QWidget owner; owner.resize(400, 250); owner.show();
        TaskDateController controller(m_store.get(), &owner);
        QSignalSpy saved(&controller, &TaskDateController::saved);
        controller.open(id, QRect(owner.mapToGlobal(QPoint(30, 30)), QSize(80, 30)));
        QPointer<DatePickerPopup> picker = owner.findChild<DatePickerPopup*>();
        QVERIFY(picker); QTRY_VERIFY(picker->isVisible());
        picker->setDateTime(fromAppTime(QDateTime(QDate(2026, 11, 6), QTime(0, 30), Qt::LocalTime)));
        picker->setReminder(fromAppTime(QDateTime(QDate(2026, 11, 6), QTime(0, 15), Qt::LocalTime)));
        picker->accept();
        QTRY_COMPARE(saved.count(), 1);
        const QDateTime expectedDue(QDate(2026, 11, 5), QTime(16, 30), Qt::UTC);
        const QDateTime expectedReminder(QDate(2026, 11, 5), QTime(16, 15), Qt::UTC);
        QCOMPARE(m_store->entity(id).dueAt, expectedDue);
        QCOMPARE(m_store->reminders(id).first().triggerAt, expectedReminder);
        QTRY_VERIFY(!picker);

        TaskDetailWidget detail(m_store.get()); QVERIFY(detail.setTask(id));
        auto* chip = detail.findChild<QPushButton*>(QStringLiteral("DateChip")); QVERIFY(chip);
        QVERIFY(chip->text().contains(QStringLiteral("00:30")));
        QVERIFY(!chip->text().contains(QStringLiteral("00:15"))); // 提醒时刻不混入日期片
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
        QVERIFY(!panel.findChild<QLineEdit*>(QStringLiteral("ChecklistInput")));
        auto* body = panel.findChild<QTextEdit*>(QStringLiteral("DescriptionEditor"));
        QVERIFY(body);
        body->setPlainText(QStringLiteral("可由详情 flush 保存的正文草稿"));
        QVERIFY(!panel.hasUncommittedDrafts(&what)); // 详情正文由 flush 链覆盖，不是游离草稿。
        QVERIFY(panel.flush());
        QCOMPARE(m_store->entity(id).body, QStringLiteral("可由详情 flush 保存的正文草稿"));
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
        QVERIFY(!panel.findChild<QPushButton*>(QStringLiteral("QuickAddButton")));
        QVERIFY(!panel.findChild<QLineEdit*>(QStringLiteral("TaskSearch")));
        QVERIFY(!panel.findChild<QToolButton*>(QStringLiteral("RailSearch")));
        QVERIFY(!panel.findChild<QToolButton*>(QStringLiteral("RailNotes")));
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
        auto* info = detailPane->findChild<QToolButton*>(QStringLiteral("DetailPriority"));
        QVERIFY(info);
        QVERIFY(rectIn(info, &panel).top() >= rectIn(controls, &panel).bottom());
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        auto* listHeader = panel.findChild<QFrame*>(QStringLiteral("ListHeader"));
        auto* detailHeader = panel.findChild<QFrame*>(QStringLiteral("DetailHeader"));
        QVERIFY(navigation && listHeader && detailHeader);
        QTRY_VERIFY(navigation->count() > 0);
        const auto verifyTopRowsAligned = [&]() {
            QRect firstRow = navigation->visualItemRect(navigation->item(0));
            firstRow.moveTopLeft(navigation->viewport()->mapTo(&panel, firstRow.topLeft()));
            const auto centerY = [](const QRect& rect) { return rect.top() + rect.height() / 2; };
            const int sharedCenter = rectIn(detailPane, &panel).top()
                + WorkspaceGeometry::FirstRowIconCenter;
            QCOMPARE(centerY(firstRow), sharedCenter);
            QCOMPARE(centerY(rectIn(listHeader, &panel)), sharedCenter);
            QCOMPARE(centerY(rectIn(detailHeader, &panel)), sharedCenter);
            QCOMPARE(centerY(rectIn(info, &panel)), sharedCenter);
            QCOMPARE(rectIn(controls, &panel).top(), rectIn(detailPane, &panel).top());
            QCOMPARE(controls->height(), WorkspaceGeometry::WindowControlHeight);
            QVERIFY(!rectIn(info, &panel).intersects(rectIn(controls, &panel)));
        };
        verifyTopRowsAligned();
        auto* close = detailPane->findChild<QToolButton*>(QStringLiteral("DetailClose"));
        QVERIFY(close && !close->isVisible()); // 普通停靠详情没有关闭按钮。
        QVERIFY(panel.findChild<TaskDetailWidget*>()->setTask(QString()));
        QTRY_VERIFY(detailPane->isVisible());
        QVERIFY(panel.findChild<TaskDetailWidget*>()->entityId().isEmpty());
        QCOMPARE(rectIn(listPane, &panel).right(), rectIn(detailPane, &panel).left() - 2);
        panel.showTask(id);
        QTRY_VERIFY(detailPane->isVisible());
        QTest::mouseClick(panel.findChild<QToolButton*>(QStringLiteral("WindowMaximize")), Qt::LeftButton);
        QApplication::processEvents();
        QVERIFY(panel.windowState() & Qt::WindowMaximized);
        verifyTopRowsAligned();
        QVERIFY(panel.mask().isEmpty());
        QVERIFY(surface->mask().isEmpty());
        QTest::mouseClick(panel.findChild<QToolButton*>(QStringLiteral("WindowMaximize")), Qt::LeftButton);
        QApplication::processEvents();
        QVERIFY(!(panel.windowState() & Qt::WindowMaximized));
        verifyTopRowsAligned();
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

    void workspaceHeaderAlignmentAndSidebarIcons() {
        TaskPanel panel(m_store.get());
        panel.resize(1240, 820);
        panel.show();
        auto* account = panel.findChild<QToolButton*>(QStringLiteral("AccountButton"));
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        auto* sidebar = panel.findChild<QFrame*>(QStringLiteral("Sidebar"));
        auto* listPane = panel.findChild<QFrame*>(QStringLiteral("ListPane"));
        auto* listHeader = panel.findChild<QFrame*>(QStringLiteral("ListHeader"));
        auto* heading = panel.findChild<QLabel*>(QStringLiteral("ListHeading"));
        auto* headingIcon = panel.findChild<QLabel*>(QStringLiteral("ListHeadingIcon"));
        auto* toggle = panel.findChild<QToolButton*>(QStringLiteral("SidebarToggle"));
        auto* more = panel.findChild<QToolButton*>(QStringLiteral("WorkspaceMore"));
        auto* detail = panel.findChild<QFrame*>(QStringLiteral("DetailFrame"));
        auto* detailHeader = panel.findChild<QFrame*>(QStringLiteral("DetailHeader"));
        auto* info = panel.findChild<QToolButton*>(QStringLiteral("DetailPriority"));
        auto* controls = panel.findChild<WindowControls*>();
        QVERIFY(account && navigation && sidebar && listPane && listHeader && heading && headingIcon
                && toggle && more && detail && detailHeader && info && controls);
        QVERIFY(!panel.findChild<QLabel*>(QStringLiteral("ListCount")));
        // 字号/图标升级不应牵动既有顶部控制条与首行中心线。
        QCOMPARE(heading->font().pointSizeF(), 14.0);
        QCOMPARE(heading->font().weight(), int(QFont::Bold));
        QCOMPARE(heading->font().pointSizeF(), ds::qfont(ds::FontScene::DetailTitle).pointSizeF());
        QCOMPARE(heading->font().weight(), ds::qfont(ds::FontScene::DetailTitle).weight());
        QCOMPARE(headingIcon->size(), QSize(24, 24));
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        const QPixmap headingPixmap = headingIcon->pixmap(Qt::ReturnByValue);
#else
        const QPixmap headingPixmap = headingIcon->pixmap() ? *headingIcon->pixmap() : QPixmap();
#endif
        QVERIFY(!headingPixmap.isNull());
        QCOMPARE(QSize(qRound(headingPixmap.width() / headingPixmap.devicePixelRatio()),
                       qRound(headingPixmap.height() / headingPixmap.devicePixelRatio())), QSize(24, 24));
        QCOMPARE(int(WorkspaceGeometry::ListHeaderHeight), 40);
        QCOMPARE(int(WorkspaceGeometry::WindowControlHeight), 28);
        QCOMPARE(int(WorkspaceGeometry::FirstRowIconCenter), 48);
        QCOMPARE(int(WorkspaceGeometry::RailAccountSize), 34);   // 顶部品牌 Logo 34px
        QCOMPARE(int(WorkspaceGeometry::NavigationRowHeight), 35); // 智能清单行高 35px
        QVERIFY(!panel.findChild<QToolButton*>(QStringLiteral("ListSort")));
        QCOMPARE(account->size(), QSize(WorkspaceGeometry::RailAccountSize,
                                       WorkspaceGeometry::RailAccountSize));
        QCOMPARE(rectIn(account, &panel).top(), 21 + WorkspaceGeometry::RailTopMargin);
        auto* railTasks = panel.findChild<QToolButton*>(QStringLiteral("RailTasks"));
        auto* railSync = panel.findChild<QToolButton*>(QStringLiteral("RailSync"));
        auto* railSettings = panel.findChild<QToolButton*>(QStringLiteral("RailSettings"));
        QVERIFY(railTasks && railSync && railSettings);
        for (QToolButton* button : {railTasks, railSync, railSettings}) {
            QCOMPARE(button->size(), QSize(40, 40));
            QCOMPARE(button->iconSize(), QSize(28, 28));
        }
        QVERIFY(railTasks->isChecked());
        QCOMPARE(AppIcons::iconName(IconId::WorkspaceTasks), QStringLiteral("list-sidebar"));
        QCOMPARE(railTasks->icon().pixmap(QSize(28, 28), QIcon::Normal, QIcon::On).toImage(),
                 AppIcons::icon(IconId::WorkspaceTasks, ds::ColorToken::Primary, 28)
                     .pixmap(28, 28).toImage());
        QTRY_VERIFY(navigation->count() > 0);
        QRect firstRow = navigation->visualItemRect(navigation->item(0));
        firstRow.moveTopLeft(navigation->viewport()->mapTo(&panel, firstRow.topLeft()));
        QCOMPARE(firstRow.height(), WorkspaceGeometry::NavigationRowHeight);
        QCOMPARE(firstRow.top(), rectIn(sidebar, &panel).top()
                 + WorkspaceGeometry::NavigationTopMargin);
        QCOMPARE(listHeader->height(), WorkspaceGeometry::ListHeaderHeight);
        QCOMPARE(rectIn(listHeader, &panel).top(), rectIn(listPane, &panel).top()
                 + WorkspaceGeometry::ListHeaderTopMargin);
        QCOMPARE(controls->size(), QSize(138, WorkspaceGeometry::WindowControlHeight));
        QCOMPARE(rectIn(controls, &panel).top(), rectIn(detail, &panel).top());
        for (const QString& name : {QStringLiteral("WindowMinimize"),
                                    QStringLiteral("WindowMaximize"),
                                    QStringLiteral("WindowClose")}) {
            auto* button = panel.findChild<QToolButton*>(name);
            QVERIFY(button);
            QCOMPARE(button->size(), QSize(46, WorkspaceGeometry::WindowControlHeight));
        }
        QCOMPARE(detail->layout()->contentsMargins().top(),
                 WorkspaceGeometry::DetailHeaderTopMargin);

        const auto layoutCenterY = [](const QRect& rect) { return rect.top() + rect.height() / 2; };
        const int sharedCenter = rectIn(detail, &panel).top() + WorkspaceGeometry::FirstRowIconCenter;
        QCOMPARE(layoutCenterY(rectIn(account, &panel)), sharedCenter);
        QCOMPARE(layoutCenterY(firstRow), sharedCenter);
        QCOMPARE(layoutCenterY(rectIn(toggle, &panel)), sharedCenter);
        QCOMPARE(layoutCenterY(rectIn(more, &panel)), sharedCenter);
        QCOMPARE(toggle->iconSize(), QSize(WorkspaceGeometry::SidebarToggleIconSize,
                                          WorkspaceGeometry::SidebarToggleIconSize));

        const QString id = m_store->createTask(QStringLiteral("顶部行对齐"));
        panel.showTask(id);
        QVERIFY(!panel.findChild<QLabel*>(QStringLiteral("ListCount")));
        QTRY_VERIFY(detailHeader->isVisible());
        QVERIFY(info->isVisible());
        QCOMPARE(detailHeader->height(), WorkspaceGeometry::DetailHeaderHeight);
        QCOMPARE(rectIn(detailHeader, &panel).top(), rectIn(detail, &panel).top()
                 + WorkspaceGeometry::DetailHeaderTopMargin);
        QCOMPARE(layoutCenterY(rectIn(detailHeader, &panel)), sharedCenter);
        QCOMPARE(layoutCenterY(rectIn(info, &panel)), sharedCenter);
        QVERIFY(!rectIn(info, &panel).intersects(rectIn(controls, &panel)));
        const auto inkBounds = [](const QImage& image) {
            QRect bounds;
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    if (qAlpha(image.pixel(x, y)) <= 40) continue;
                    bounds = bounds.isNull() ? QRect(x, y, 1, 1)
                                             : bounds.united(QRect(x, y, 1, 1));
                }
            }
            return bounds;
        };
        const QRect collapseInk = inkBounds(AppIcons::icon(IconId::SidebarCollapse, 20)
            .pixmap(20, 20).toImage());
        const QRect expandInk = inkBounds(AppIcons::icon(IconId::SidebarExpand, 20)
            .pixmap(20, 20).toImage());
        QVERIFY(collapseInk.width() >= 12 && collapseInk.height() >= 12);
        QVERIFY(expandInk.width() >= 12 && expandInk.height() >= 12);
        QVERIFY(toggle->icon().pixmap(20, 20).toImage()
                == AppIcons::icon(IconId::SidebarCollapse, 20).pixmap(20, 20).toImage());
        const QString screenshots = QString::fromLocal8Bit(qgetenv("KYLINLIST_SCREENSHOT_DIR"));
        if (!screenshots.isEmpty()) {
            QVERIFY(QDir().mkpath(screenshots));
            const QImage top = panel.grab().toImage().copy(0, 0, panel.width(), 130);
            QVERIFY(top.scaled(top.width() * 2, top.height() * 2,
                               Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .save(screenshots + QStringLiteral("/workspace-header-aligned.png")));
        }
        QTest::mouseClick(toggle, Qt::LeftButton);
        QTRY_VERIFY(!sidebar->isVisible());
        QVERIFY(toggle->icon().pixmap(20, 20).toImage()
                == AppIcons::icon(IconId::SidebarExpand, 20).pixmap(20, 20).toImage());
        if (!screenshots.isEmpty()) {
            const QImage top = panel.grab().toImage().copy(0, 0, panel.width(), 130);
            QVERIFY(top.scaled(top.width() * 2, top.height() * 2,
                               Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .save(screenshots + QStringLiteral("/workspace-sidebar-collapsed.png")));
        }
        QTest::mouseClick(toggle, Qt::LeftButton);
        QTRY_VERIFY(sidebar->isVisible());
    }

    void activityRailCommandsOwnDataAndAppearanceEntries() {
        TaskPanel panel(m_store.get());
        panel.resize(1240, 820);
        panel.show();
        auto* tasks = panel.findChild<QToolButton*>(QStringLiteral("RailTasks"));
        auto* sync = panel.findChild<QToolButton*>(QStringLiteral("RailSync"));
        auto* settings = panel.findChild<QToolButton*>(QStringLiteral("RailSettings"));
        auto* account = panel.findChild<QToolButton*>(QStringLiteral("AccountButton"));
        QVERIFY(tasks && sync && settings && account);
        QVERIFY(!panel.findChild<QToolButton*>(QStringLiteral("RailSearch")));
        QVERIFY(!panel.findChild<QToolButton*>(QStringLiteral("RailNotes")));
        QVERIFY(!panel.findChild<QLineEdit*>(QStringLiteral("TaskSearch")));

        QImage railImage(40, 40, QImage::Format_ARGB32_Premultiplied);
        railImage.fill(Qt::magenta);
        QPainter railPainter(&railImage);
        tasks->render(&railPainter);
        railPainter.end();
        QCOMPARE(railImage.pixelColor(2, 20), QColor(Qt::magenta));
        QCOMPARE(tasks->icon().pixmap(QSize(28, 28), QIcon::Normal, QIcon::On).toImage(),
                 AppIcons::icon(IconId::WorkspaceTasks, ds::ColorToken::Primary, 28)
                     .pixmap(28, 28).toImage());
        QTest::mouseClick(tasks, Qt::LeftButton);
        QVERIFY(tasks->isChecked());
        QCOMPARE(tasks->icon().pixmap(QSize(28, 28), QIcon::Normal, QIcon::On).toImage(),
                 AppIcons::icon(IconId::WorkspaceTasks, ds::ColorToken::Primary, 28)
                     .pixmap(28, 28).toImage());

        const auto commandNames = [](PopupMenu* menu) {
            QStringList names;
            if (menu) {
                for (QAction* action : menu->actions())
                    if (!action->isSeparator()) names.append(action->objectName());
            }
            return names;
        };
        QStringList dataCommands;
        QTimer::singleShot(0, &panel, [&]() {
            auto* menu = panel.findChild<PopupMenu*>(QStringLiteral("DataMenu"));
            dataCommands = commandNames(menu);
            if (menu) menu->close();
        });
        QTest::mouseClick(sync, Qt::LeftButton);
        QCOMPARE(dataCommands, QStringList({QStringLiteral("LocalDataStatus"),
                                           QStringLiteral("CmdExportBackup"),
                                           QStringLiteral("CmdImportBackup"),
                                           QStringLiteral("CmdOpenData")}));

        QStringList accountCommands;
        QTimer::singleShot(0, &panel, [&]() {
            auto* menu = panel.findChild<PopupMenu*>(QStringLiteral("AccountMenu"));
            accountCommands = commandNames(menu);
            if (menu) menu->close();
        });
        QTest::mouseClick(account, Qt::LeftButton);
        QCOMPARE(accountCommands, QStringList({QStringLiteral("CmdUsageStats"),
                                              QStringLiteral("CmdCheckUpdate"),
                                              QStringLiteral("CmdUpdateServer"),
                                              QStringLiteral("CmdQuit")}));
        for (const QString& name : dataCommands) QVERIFY(!accountCommands.contains(name));

        bool settingsOpened = false, generalIsDefault = false, categoryIconsFollowTheme = false;
        bool settingsGeometryCorrect = false, startupRowCorrect = false, closeAtTopRight = false;
        bool startupSynchronized = false;
        QTimer::singleShot(0, &panel, [&]() {
            auto* dialog = panel.findChild<SettingsDialog*>(QStringLiteral("SettingsDialog"));
            if (!dialog) {
                if (QWidget* modal = QApplication::activeModalWidget()) modal->close();
                return;
            }
            settingsOpened = true;
            auto* categories = dialog->findChild<QListWidget*>(QStringLiteral("SettingsCategories"));
            auto* pages = dialog->findChild<QStackedWidget*>(QStringLiteral("SettingsPages"));
            auto* theme = dialog->findChild<QComboBox*>(QStringLiteral("AppearanceTheme"));
            auto* startup = dialog->findChild<QCheckBox*>(QStringLiteral("GeneralAutostart"));
            generalIsDefault = categories && pages && theme && startup && categories->count() == 4
                && categories->item(0)->text() == QStringLiteral("通用")
                && categories->item(1)->text() == QStringLiteral("外观")
                && categories->currentRow() == 0 && pages->currentWidget()
                && pages->currentWidget()->objectName() == QLatin1String("GeneralSettingsPage")
                && startup->isVisible() && startup->accessibleName() == QStringLiteral("开机自启动");
            auto* hint = dialog->findChild<QLabel*>(QStringLiteral("GeneralAutostartHint"));
            auto* error = dialog->findChild<QLabel*>(QStringLiteral("GeneralAutostartError"));
            auto* leftSurface = dialog->findChild<QWidget*>(QStringLiteral("SettingsLeftSurface"));
            settingsGeometryCorrect = categories && pages && leftSurface
                && categories->mapTo(dialog->card(), QPoint()).x() == 0
                && pages->mapTo(dialog->card(), QPoint()).x() == 212
                && leftSurface->width() == 212;
            auto* header = dialog->header();
            auto* close = dialog->findChild<QToolButton*>(QStringLiteral("ModalClose"));
            closeAtTopRight = header && close && header->width() == dialog->card()->width()
                && close->isVisible() && dialog->card()->rect().contains(close->geometry().center())
                && dialog->card()->width() - close->geometry().right() - 1 == 10;
            startupRowCorrect = hint && error && hint->isVisible() && !error->isVisible()
                && startup && startup->size() == QSize(30, 18)
                && startup->parentWidget()->width() > 300;
            startupSynchronized = startup && !startup->isEnabled() && !startup->isChecked();
            if (theme) theme->setCurrentIndex(theme->findData(
                static_cast<int>(ThemeManager::Preference::Dark)));
            if (categories && categories->count() >= 3) {
                categories->setCurrentRow(1);
                categoryIconsFollowTheme = categories->item(1)->icon().pixmap(20, 20).toImage()
                    == AppIcons::icon(IconId::Theme, ds::ColorToken::TextSecondary, 20)
                        .pixmap(20, 20).toImage();
            }
            dialog->close();
        });
        QTest::mouseClick(settings, Qt::LeftButton);
        QVERIFY(settingsOpened); QVERIFY(generalIsDefault); QVERIFY(categoryIconsFollowTheme);
        QVERIFY(settingsGeometryCorrect); QVERIFY(startupRowCorrect); QVERIFY(closeAtTopRight);
        QVERIFY(startupSynchronized);
        QCOMPARE(ThemeManager::instance().preference(), ThemeManager::Preference::Dark);

        bool registered = true;
        AutostartSettings autostart(AutostartSettings::Mode::Portable, AutostartSettings::Backend{
            [&registered]() { return registered; },
            [&registered](bool enabled, QString*) { registered = enabled; return true; }});
        panel.setAutostart(&autostart);
        SettingsDialog dialog(&autostart);
        auto* theme = dialog.findChild<QComboBox*>(QStringLiteral("AppearanceTheme"));
        QVERIFY(theme);
        theme->setCurrentIndex(theme->findData(static_cast<int>(ThemeManager::Preference::System)));
        QCOMPARE(ThemeManager::instance().preference(), ThemeManager::Preference::System);
        ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
        QCOMPARE(theme->currentData().toInt(), static_cast<int>(ThemeManager::Preference::Light));

        bool startupFailureRecovers = false;
        bool failWrite = false;
        AutostartSettings failing(AutostartSettings::Mode::Portable, AutostartSettings::Backend{
            []() { return false; },
            [&failWrite](bool, QString* error) { if (error) *error = QStringLiteral("合成自启失败"); return !failWrite; }});
        failWrite = true;
        SettingsDialog failureDialog(&failing);
        failureDialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&failureDialog));
        auto* switchButton = failureDialog.findChild<QCheckBox*>(QStringLiteral("GeneralAutostart"));
        auto* failure = failureDialog.findChild<QLabel*>(QStringLiteral("GeneralAutostartError"));
        QVERIFY(switchButton && failure); QVERIFY(switchButton->isEnabled());
        startupFailureRecovers = false;
        switchButton->click();
        startupFailureRecovers = !switchButton->isChecked() && failure->isVisible()
            && failure->text() == QStringLiteral("合成自启失败");
        failureDialog.close();

        const QString id = m_store->createTask(QStringLiteral("便签设置入口"));
        QVERIFY(m_store->saveDesktopState(id, NoteColor::Yellow, QRect(20, 20, 360, 320),
                                          0, NoteLayer::Normal, true));
        NoteManager notes(m_store.get());
        notes.setAutostart(&autostart);
        QVERIFY(notes.restoreAll());
        auto* note = notes.note(id); QVERIFY(note);
        note->show(); QVERIFY(QTest::qWaitForWindowExposed(note));
        auto* more = note->findChild<QToolButton*>(QStringLiteral("DetailMore")); QVERIFY(more);
        bool startupOpensFromNote = false;
        QTimer::singleShot(0, note, [note, &startupOpensFromNote, &autostart]() {
            auto* menu = QApplication::activePopupWidget();
            auto* moreMenu = qobject_cast<PopupMenu*>(menu);
            if (!moreMenu) return;
            QMenu* settingsMenu = nullptr;
            for (QAction* action : moreMenu->actions())
                if (action->menu() && action->text() == QStringLiteral("设置")) settingsMenu = action->menu();
            if (!settingsMenu) { moreMenu->close(); return; }
            QAction* appSettings = nullptr;
            for (QAction* action : settingsMenu->actions())
                if (action->text() == QStringLiteral("应用设置…")) appSettings = action;
            if (!appSettings) { moreMenu->close(); return; }
            settingsMenu->popup(moreMenu->mapToGlobal(QPoint(180, 100)));
            QTimer::singleShot(0, settingsMenu, [settingsMenu, appSettings]() {
                emit appSettings->trigger();
            });
            QTimer::singleShot(200, note, [note, &startupOpensFromNote, &autostart]() {
                auto* dialog = qobject_cast<SettingsDialog*>(QApplication::activeModalWidget());
                if (!dialog) {
                    for (QWidget* top : QApplication::topLevelWidgets()) {
                        auto* candidate = qobject_cast<SettingsDialog*>(top);
                        if (candidate && candidate->isVisible()) { dialog = candidate; break; }
                    }
                }
                if (!dialog) return;
                auto* switchButton = dialog->findChild<QCheckBox*>(QStringLiteral("GeneralAutostart"));
                startupOpensFromNote = switchButton && switchButton->isEnabled()
                    && switchButton->isChecked() == autostart.isEnabled()
                    && dialog->header()->width() == dialog->card()->width();
                dialog->close();
            });
        });
        QTest::mouseClick(more, Qt::LeftButton);
        QTRY_VERIFY(startupOpensFromNote);
        QVERIFY(startupFailureRecovers);
    }

    void windowControlHoverBackgroundStaysInsideBorder() {
        TaskPanel panel(m_store.get());
        panel.resize(1100, 760);
        panel.show();
        QTest::qWait(30);
        auto* controls = panel.findChild<WindowControls*>();
        QVERIFY(controls);
        const auto pixel = [](const QImage& image, const QPoint& point) {
            const qreal dpr = image.devicePixelRatio();
            return image.pixelColor(qRound(point.x() * dpr), qRound(point.y() * dpr));
        };
        for (const QString& name : {QStringLiteral("WindowMinimize"),
                                    QStringLiteral("WindowMaximize"),
                                    QStringLiteral("WindowClose")}) {
            auto* button = controls->findChild<QToolButton*>(name);
            QVERIFY(button);
            QTest::mouseMove(&panel, QPoint(30, panel.height() / 2));
            QApplication::processEvents();
            const QImage before = panel.grab().toImage();
            QTest::mouseMove(button, QPoint(button->width() / 2, button->height() / 2));
            QTRY_VERIFY(button->underMouse());
            const QImage hovered = button->grab().toImage();
            QCOMPARE(pixel(hovered, QPoint(button->width() / 2, 1)),
                     ThemeManager::instance().color(ds::ColorToken::Hover));
            QCOMPARE(pixel(hovered, QPoint(button->width() / 2, 0)).alpha(), 0);
            QCOMPARE(pixel(hovered, QPoint(button->width() - 1, button->height() - 1)).alpha(), 0);
            const QImage after = panel.grab().toImage();
            const QPoint borderPoint = button->mapTo(&panel, QPoint(button->width() / 2, 0));
            QCOMPARE(pixel(after, borderPoint), pixel(before, borderPoint));
        }
        QTest::mouseMove(&panel, QPoint(30, panel.height() / 2));
    }

    void workspaceMenuOmitsViewsAndUsesRealActions() {
        TaskPanel panel(m_store.get());
        panel.resize(1240, 820);
        panel.show();
        auto* more = panel.findChild<QToolButton*>(QStringLiteral("WorkspaceMore"));
        QVERIFY(more);
        bool menuOpened = false, settingsOpened = false;
        int menuCardWidth = 0;
        QStringList actionTexts;
        QImage menuImage;
        QTimer::singleShot(0, &panel, [&]() {
            auto* menu = panel.findChild<PopupMenu*>(QStringLiteral("WorkspaceMenu"));
            if (!menu) {
                if (QWidget* popup = QApplication::activePopupWidget()) popup->close();
                return;
            }
            menuOpened = true;
            for (QAction* action : menu->actions())
                if (!action->isSeparator()) actionTexts.append(action->text());
            menuCardWidth = menu->width() - 2 * PopupMenu::shadowMargin();
            menuImage = menu->grab().toImage();
            if (auto* action = menu->findChild<QAction*>(QStringLiteral("HideCompleted")))
                action->trigger();
            if (auto* action = menu->findChild<QAction*>(QStringLiteral("HideDetails")))
                action->trigger();
            if (auto* action = menu->findChild<QAction*>(QStringLiteral("DisplaySettings"))) {
                QTimer::singleShot(0, menu, [&]() {
                    auto* dialog = panel.findChild<DisplaySettingsDialog*>();
                    if (dialog) {
                        settingsOpened = true;
                        if (auto* mode = dialog->findChild<QComboBox*>(QStringLiteral("DateDisplayMode")))
                            mode->setCurrentIndex(1);
                        if (auto* sidebar = dialog->findChild<QRadioButton*>(QStringLiteral("DetailSidebar")))
                            QTest::mouseClick(sidebar, Qt::LeftButton);
                        dialog->close();
                    }
                });
                action->trigger();
            }
            menu->close();
        });
        QTest::mouseClick(more, Qt::LeftButton);
        QVERIFY(menuOpened);
        QCOMPARE(actionTexts, QStringList({QStringLiteral("分组"), QStringLiteral("显示已完成"),
                                           QStringLiteral("显示详细"), QStringLiteral("显示设置")}));
        QVERIFY(!actionTexts.contains(QStringLiteral("视图")));
        QVERIFY(!actionTexts.contains(QStringLiteral("打印")));
        QCOMPARE(menuCardWidth, 202);
        QVERIFY(settingsOpened);
        auto* view = panel.findChild<TaskListView*>(QStringLiteral("TaskList"));
        auto* model = view ? qobject_cast<TaskListModel*>(view->model()) : nullptr;
        QVERIFY(model);
        QVERIFY(model->filter().showCompleted);
        QVERIFY(model->filter().showDetails);
        QVERIFY(model->filter().countdownDates);
        auto* detail = panel.findChild<QFrame*>(QStringLiteral("DetailFrame"));
        QVERIFY(detail);
        panel.resize(700, 620);
        QApplication::processEvents();
        QVERIFY(detail->isVisible());
        QVERIFY(!detail->property("overlay").toBool());
        const QString screenshots = QString::fromLocal8Bit(qgetenv("KYLINLIST_SCREENSHOT_DIR"));
        if (!screenshots.isEmpty() && !menuImage.isNull()) {
            QVERIFY(QDir().mkpath(screenshots));
            QVERIFY(menuImage.scaled(menuImage.width() * 2, menuImage.height() * 2,
                                     Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .save(screenshots + QStringLiteral("/workspace-display-menu.png")));
        }
    }

    void taskDisplayOptionsFilterAndExpandRows() {
        const QString activeId = m_store->createTask(QStringLiteral("带详情的任务"));
        Entity active = m_store->entity(activeId);
        active.body = QStringLiteral("## 第一行\n第二行\n- [x] 检查项");
        active.allDayDate = toAppTime(appNow()).date().addDays(1);
        QVERIFY(m_store->saveTask(active));
        const QString completedId = m_store->createTask(QStringLiteral("已完成的任务"));
        QVERIFY(m_store->setCompleted(completedId, true));

        TaskListModel model(m_store.get());
        QVERIFY(!model.indexForTask(completedId).isValid());
        ViewSpec filter = model.filter();
        filter.showCompleted = true;
        model.setFilter(filter);
        const QModelIndex completed = model.indexForTask(completedId);
        QVERIFY(completed.isValid());
        QCOMPARE(completed.data(TaskListModel::SectionKeyRole).toString(), QStringLiteral("completed"));

        TaskDelegate delegate;
        QStyleOptionViewItem option;
        QModelIndex activeIndex = model.indexForTask(activeId);
        const int normalHeight = delegate.sizeHint(option, activeIndex).height();
        QVERIFY(activeIndex.data(TaskListModel::DetailRole).toString().contains(QStringLiteral("第一行 第二行")));
        QVERIFY(activeIndex.data(TaskListModel::DetailRole).toString().contains(QStringLiteral("检查项")));
        QVERIFY(!activeIndex.data(TaskListModel::DetailRole).toString().contains(QStringLiteral("1/1")));
        filter.showDetails = true;
        model.setFilter(filter);
        activeIndex = model.indexForTask(activeId);
        QCOMPARE(delegate.sizeHint(option, activeIndex).height(), normalHeight + 22);
        option.rect = QRect(0, 0, 600, normalHeight + 22);
        option.state = QStyle::State_Enabled | QStyle::State_Selected;
        QImage rowImage(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
        rowImage.fill(ThemeManager::instance().color(ds::ColorToken::Surface));
        QPainter rowPainter(&rowImage);
        delegate.paint(&rowPainter, option, activeIndex);
        rowPainter.end();
        const QRgb selected = ThemeManager::instance().color(ds::ColorToken::RowSelected).rgba();
        QCOMPARE(rowImage.pixel(300, 0), selected);
        QCOMPARE(rowImage.pixel(300, normalHeight + 21), selected);
        filter.countdownDates = true;
        model.setFilter(filter);
        activeIndex = model.indexForTask(activeId);
        QCOMPARE(activeIndex.data(TaskListModel::ScheduleRole).toString(), QStringLiteral("1天后"));
        const QString screenshots = QString::fromLocal8Bit(qgetenv("KYLINLIST_SCREENSHOT_DIR"));
        if (!screenshots.isEmpty()) {
            TaskPanel panel(m_store.get());
            panel.resize(1000, 700);
            panel.show();
            auto* view = panel.findChild<TaskListView*>(QStringLiteral("TaskList"));
            auto* panelModel = view ? qobject_cast<TaskListModel*>(view->model()) : nullptr;
            QVERIFY(view && panelModel);
            panelModel->setFilter(filter);
            panel.showTask(activeId);
            const QModelIndex index = panelModel->indexForTask(activeId);
            QVERIFY(index.isValid());
            QTRY_COMPARE(view->visualRect(index).height(), normalHeight + 22);
            const QRect row = view->visualRect(index).adjusted(0, -4, 0, 4);
            const QPixmap grabbed = view->viewport()->grab();
            const qreal dpr = grabbed.devicePixelRatio();
            const QRect physical(qRound(row.x() * dpr), qRound(row.y() * dpr),
                                 qRound(row.width() * dpr), qRound(row.height() * dpr));
            const QImage image = grabbed.toImage().copy(physical);
            QVERIFY(QDir().mkpath(screenshots));
            QVERIFY(image.scaled(image.width() * 3, image.height() * 3,
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .save(screenshots + QStringLiteral("/task-row-detail-62.png")));
        }
    }

    void displaySettingsDialogMatchesWebOptions() {
        DisplaySettingsDialog dialog(false, true);
        auto* dateMode = dialog.findChild<QComboBox*>(QStringLiteral("DateDisplayMode"));
        auto* sidebar = dialog.findChild<QRadioButton*>(QStringLiteral("DetailSidebar"));
        auto* popup = dialog.findChild<QRadioButton*>(QStringLiteral("DetailPopup"));
        QVERIFY(dateMode && sidebar && popup);
        QCOMPARE(dateMode->itemText(0), QStringLiteral("任务时间"));
        QCOMPARE(dateMode->itemText(1), QStringLiteral("倒数日"));
        QVERIFY(dialog.popupDetail());
        QSignalSpy spy(&dialog, &DisplaySettingsDialog::optionsChanged);
        dateMode->setCurrentIndex(1);
        QVERIFY(dialog.countdownDates());
        QTest::mouseClick(sidebar, Qt::LeftButton);
        QVERIFY(!dialog.popupDetail());
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.last().at(0).toBool(), true);
        QCOMPARE(spy.last().at(1).toBool(), false);
        const QString screenshots = QString::fromLocal8Bit(qgetenv("KYLINLIST_SCREENSHOT_DIR"));
        if (!screenshots.isEmpty()) {
            dialog.show();
            QApplication::processEvents();
            const QImage image = dialog.grab().toImage();
            QVERIFY(QDir().mkpath(screenshots));
            QVERIFY(image.scaled(image.width() * 2, image.height() * 2,
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .save(screenshots + QStringLiteral("/workspace-display-settings.png")));
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
        auto* dateChip = detail->findChild<QPushButton*>(QStringLiteral("DateChip"));
        QVERIFY(close && dateChip && close->isVisible() && dateChip->isVisible());
        QVERIFY(rectIn(close, &panel).left() < rectIn(dateChip, &panel).left());
        QVERIFY(!detail->findChild<QToolButton*>(QStringLiteral("DetailReminder")));
        auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
        auto* detailHeader = detail->findChild<QFrame*>(QStringLiteral("DetailHeader"));
        auto* info = detail->findChild<QToolButton*>(QStringLiteral("DetailPriority"));
        auto* controls = panel.findChild<WindowControls*>();
        QVERIFY(navigation && navigation->count() > 0 && detailHeader && info && controls);
        QRect firstRow = navigation->visualItemRect(navigation->item(0));
        firstRow.moveTopLeft(navigation->viewport()->mapTo(&panel, firstRow.topLeft()));
        const auto centerY = [](const QRect& rect) { return rect.top() + rect.height() / 2; };
        const int sharedCenter = rectIn(detail, &panel).top()
            + WorkspaceGeometry::FirstRowIconCenter;
        QCOMPARE(centerY(firstRow), sharedCenter);
        QCOMPARE(centerY(rectIn(toggle, &panel)), sharedCenter);
        QCOMPARE(centerY(rectIn(close, &panel)), sharedCenter);
        QCOMPARE(centerY(rectIn(info, &panel)), sharedCenter);
        QCOMPARE(rectIn(detailHeader, &panel).top(), rectIn(detail, &panel).top()
                 + WorkspaceGeometry::DetailHeaderTopMargin);
        QCOMPARE(rectIn(controls, &panel).top(), rectIn(detail, &panel).top());
        QVERIFY(!rectIn(info, &panel).intersects(rectIn(controls, &panel)));
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
        // 日期经真实 DateChip/TaskDateController 提交，不再依赖隐藏日期表单。
        detail->openDatePicker();
        QPointer<DatePickerPopup> picker = detail->findChild<DatePickerPopup*>();
        QVERIFY(picker); QTRY_VERIFY(picker->isVisible());
        const QDateTime due = fromAppTime(QDateTime(QDate(2026, 11, 6), QTime(11, 0)));
        picker->setDateTime(due); picker->accept();
        QTRY_COMPARE(m_store->entity(id).dueAt, due); QTRY_VERIFY(!picker);
        QCOMPARE(m_store->entity(id).body, QStringLiteral("**核对附件**"));
        QCOMPARE(m_store->entity(id).geometry, external.geometry);
        QCOMPARE(m_store->entity(id).color, NoteColor::Blue);
        detail->openDatePicker(); picker = detail->findChild<DatePickerPopup*>();
        QVERIFY(picker); QTRY_VERIFY(picker->isVisible());
        picker->setDateTime(due.addSecs(7200)); picker->reject();
        QTRY_VERIFY(!picker); QCOMPARE(m_store->entity(id).dueAt, due);
        auto* title = detail->findChild<QLineEdit*>(QStringLiteral("TaskTitle")); QVERIFY(title);
        detail->activateWindow(); detail->focusTitle(); QTest::keyClick(title, Qt::Key_Backspace);
        QVERIFY(!detail->flush());
        QCOMPARE(m_store->entity(id).title, QStringLiteral("明天提交材料"));
        QTest::keyClicks(title, QStringLiteral("Saved again")); QVERIFY(detail->flush());
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
        QVERIFY(navigation && quick);
        QVERIFY(!panel.findChild<QComboBox*>(QStringLiteral("ViewRenderer")));
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
        QCOMPARE(model->filter().grouping, GroupMode::Time);
        quick->setText(QStringLiteral("默认新建笔记")); QTest::keyClick(quick, Qt::Key_Return);
        QString noteId;
        for (const Entity& entity : m_store->entities()) if (entity.title == QStringLiteral("默认新建笔记")) noteId = entity.id;
        QVERIFY(isUuid(noteId)); QCOMPARE(m_store->entity(noteId).kind, EntityKind::Note);
        QCOMPARE(m_store->entity(noteId).listId, noteList);
        auto* detail = panel.findChild<TaskDetailWidget*>(); QVERIFY(detail);
        QCOMPARE(detail->entityId(), noteId);
        QVERIFY(detail->findChild<QCheckBox*>(QStringLiteral("TaskCompleted"))->isHidden());
        for (const QString& name : {QStringLiteral("TaskPinned"), QStringLiteral("TaskPinnedAt"),
                                    QStringLiteral("TaskScheduleHost"), QStringLiteral("TaskPriorityHost"),
                                    QStringLiteral("TaskAppearanceHost"), QStringLiteral("ConvertKind")})
            QVERIFY(!detail->findChild<QWidget*>(name));
        // 笔记与任务同样允许排期与提醒：日期芯片可见；头部按钮保持 DetailInfo。
        QVERIFY(detail->findChild<QPushButton*>(QStringLiteral("DateChip"))->isVisible());
        QVERIFY(detail->findChild<QToolButton*>(QStringLiteral("DetailInfo")));
        QVERIFY(!detail->findChild<QToolButton*>(QStringLiteral("DetailPriority")));
        navigation->setCurrentRow(todayRow);
        QCOMPARE(model->filter().scope, ViewScope::Today);
        QCOMPARE(model->filter().grouping, GroupMode::Time);
        QVERIFY(quick->placeholderText().contains(QStringLiteral("收集箱")));
        navigation->setCurrentRow(allRow);
        QCOMPARE(model->filter().scope, ViewScope::AllLists);
        QCOMPARE(model->filter().grouping, GroupMode::Time);
        QVERIFY(m_store->setListFolder(noteList, QString()));
        QVERIFY(m_store->setListFolder(noteList, folder)); QVERIFY(m_store->deleteFolder(folder));
        for (const TaskList& list : m_store->lists()) if (list.id == noteList) QVERIFY(list.folderId.isEmpty());
        bool listStillVisible = false;
        for (int row = 0; row < navigation->count(); ++row)
            if (navigation->item(row)->data(Qt::UserRole + 1).toString() == noteList) listStillVisible = true;
        QVERIFY(listStillVisible);
    }
    void noteDetailMenuConvertsKindWithoutConfirmationAndToasts() {
        // 转换无确认框；成功后宿主窗口出现「已转换」Toast。
        const QString list = m_store->createList(QStringLiteral("转换笔记清单"), QString(), EntityKind::Note);
        const QString noteId = m_store->createNote(QStringLiteral("真实菜单类型转换"), list);
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(noteId));
        auto* completed = detail.findChild<QCheckBox*>(QStringLiteral("TaskCompleted")); QVERIFY(completed);
        QVERIFY(completed->isHidden());
        QVERIFY(triggerDetailCommand(detail, QStringLiteral("CmdConvertKind")));
        QCOMPARE(m_store->entity(noteId).kind, EntityKind::Task); QVERIFY(completed->isVisible());
        QCOMPARE(m_store->entity(noteId).listId, list);
        auto* toast = detail.window()->findChild<ToastIsland*>(QStringLiteral("EntityCommandToast"),
                                                               Qt::FindDirectChildrenOnly);
        QVERIFY(toast && toast->isVisible());
        QCOMPARE(toast->findChild<QLabel*>(QStringLiteral("ToastText"))->text(), QStringLiteral("已转换"));
        toast->hide();
        QVERIFY(triggerDetailCommand(detail, QStringLiteral("CmdConvertKind")));
        QCOMPARE(m_store->entity(noteId).kind, EntityKind::Note); QVERIFY(completed->isHidden());
        QCOMPARE(m_store->entity(noteId).listId, list);
        toast = detail.window()->findChild<ToastIsland*>(QStringLiteral("EntityCommandToast"),
                                                         Qt::FindDirectChildrenOnly);
        QVERIFY(toast && toast->isVisible());
        QCOMPARE(toast->findChild<QLabel*>(QStringLiteral("ToastText"))->text(), QStringLiteral("已转换"));
        toast->hide();
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
                                           QStringLiteral("notes")}));
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
    void quickAddUsesSharedDateControllerAndFocusVisibility() {
        TaskPanel panel(m_store.get());
        panel.resize(1000, 700);
        panel.show();
        auto* quick = panel.findChild<QLineEdit*>(QStringLiteral("QuickAdd"));
        auto* quickFrame = panel.findChild<QFrame*>(QStringLiteral("QuickAddFrame"));
        auto* date = panel.findChild<QToolButton*>(QStringLiteral("QuickAddDate"));
        QVERIFY(quick && quickFrame && date);
        QVERIFY(!date->isVisible());
        const QColor quickBg = ThemeManager::instance().color(ds::ColorToken::QuickAddBg);
        QCOMPARE(quickBg.name(), QStringLiteral("#f7f7f7"));
        const QPixmap frameGrab = quickFrame->grab();
        const QImage frameImage = frameGrab.toImage();
        const int sampleX = qRound(4 * frameGrab.devicePixelRatio());
        QCOMPARE(QColor::fromRgba(frameImage.pixel(sampleX, frameImage.height() / 2)).rgba(),
                 quickBg.rgba());
        QCOMPARE(panel.findChildren<TaskDateController*>().count(), 1);

        quick->setFocus();
        QTRY_VERIFY(date->isVisible());
        QTest::mouseClick(date, Qt::LeftButton);
        QPointer<DatePickerPopup> picker = panel.findChild<DatePickerPopup*>();
        QVERIFY(picker); QTRY_VERIFY(picker->isVisible());
        const QDate chosen = toAppTime(appNow()).date().addDays(2);
        picker->setAllDayDate(chosen);
        picker->accept();
        QTRY_VERIFY(!picker);
        QCOMPARE(date->text(), chosen.toString(QStringLiteral("M月d日")));
        QVERIFY(date->property("hasDate").toBool());
        const QString screenshots = QString::fromLocal8Bit(qgetenv("KYLINLIST_SCREENSHOT_DIR"));
        if (!screenshots.isEmpty()) {
            const QImage image = quickFrame->grab().toImage();
            QVERIFY(QDir().mkpath(screenshots));
            QVERIFY(image.scaled(image.width() * 3, image.height() * 3,
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .save(screenshots + QStringLiteral("/quick-add-date-focused.png")));
        }
        quick->setText(QStringLiteral("共享日期的新任务"));
        QTest::keyClick(quick, Qt::Key_Return);
        QString id;
        for (const Entity& entity : m_store->entities())
            if (entity.title == QStringLiteral("共享日期的新任务")) id = entity.id;
        QVERIFY(isUuid(id));
        QCOMPARE(m_store->entity(id).allDayDate, chosen);
        QTRY_VERIFY(!date->isVisible());
        QVERIFY(!date->property("hasDate").toBool());

        quick->setFocus();
        QTRY_VERIFY(date->isVisible());
        quick->setText(QStringLiteral("不继承旧日期"));
        QTest::keyClick(quick, Qt::Key_Return);
        QString second;
        for (const Entity& entity : m_store->entities())
            if (entity.title == QStringLiteral("不继承旧日期")) second = entity.id;
        QVERIFY(isUuid(second));
        QVERIFY(!m_store->entity(second).allDayDate.isValid());
        QVERIFY(!m_store->entity(second).dueAt.isValid());
    }

    void overlayDetailShowsCloseCompletionAndParentLink() {
        const QString parentId = m_store->createTask(QStringLiteral("父任务"));
        const QString childId = m_store->createSubtask(QStringLiteral("子任务"), parentId);
        QVERIFY(isUuid(childId));
        TaskPanel panel(m_store.get());
        panel.resize(900, 700);
        panel.show();
        panel.showTask(childId);
        auto* frame = panel.findChild<QFrame*>(QStringLiteral("DetailFrame"));
        auto* detail = panel.findChild<TaskDetailWidget*>();
        QTRY_VERIFY(frame && frame->isVisible() && frame->property("overlay").toBool());
        QVERIFY(detail);
        auto* close = detail->findChild<QToolButton*>(QStringLiteral("DetailClose"));
        auto* completed = detail->findChild<QCheckBox*>(QStringLiteral("TaskCompleted"));
        auto* separator = detail->findChild<QFrame*>(QStringLiteral("DetailHeaderSeparatorHost"));
        auto* parentHost = detail->findChild<QWidget*>(QStringLiteral("ParentTaskHost"));
        auto* parentLink = detail->findChild<QPushButton*>(QStringLiteral("ParentTaskLink"));
        auto* parentArrow = detail->findChild<QToolButton*>(QStringLiteral("ParentTaskArrow"));
        auto* title = detail->findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* date = detail->findChild<QPushButton*>(QStringLiteral("DateChip"));
        auto* info = detail->findChild<QToolButton*>(QStringLiteral("DetailPriority"));
        QVERIFY(close && completed && separator && parentHost && parentLink && parentArrow && title && date && info);
        QVERIFY(close->isVisible() && completed->isVisible() && separator->isVisible());
        QVERIFY(parentHost->isVisible() && parentLink->isVisible());
        QVERIFY(rectIn(close, detail).right() < rectIn(completed, detail).left());
        QVERIFY(rectIn(completed, detail).right() < rectIn(date, detail).left());
        QVERIFY(rectIn(separator, detail).right() < rectIn(info, detail).left());
        QCOMPARE(parentLink->height(), 22);
        QCOMPARE(rectIn(parentLink, detail).left(), rectIn(title, detail).left());
        QVERIFY(rectIn(parentLink, detail).left() < rectIn(parentArrow, detail).left());
        QCOMPARE(parentLink->font().pointSizeF(), ds::pointSize(ds::TypeScale::Menu));
        QCOMPARE(rectIn(title, detail).top() - rectIn(parentLink, detail).top() - parentLink->height(), 8);
        const QImage checkImage = completed->grab().toImage();
        const QRgb headerBg = ThemeManager::instance().color(ds::ColorToken::Surface).rgba();
        bool checkInk = false;
        for (int y = 0; y < checkImage.height(); ++y)
            for (int x = 0; x < checkImage.width(); ++x)
                if (qAlpha(checkImage.pixel(x, y)) > 40 && checkImage.pixel(x, y) != headerBg)
                    checkInk = true;
        QVERIFY(checkInk);
        const QString screenshots = QString::fromLocal8Bit(qgetenv("KYLINLIST_SCREENSHOT_DIR"));
        if (!screenshots.isEmpty()) {
            QVERIFY(QDir().mkpath(screenshots));
            const QImage image = detail->grab().toImage().copy(0, 0, detail->width(), 180);
            QVERIFY(image.scaled(image.width() * 2, image.height() * 2,
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .save(screenshots + QStringLiteral("/detail-overlay-parent.png")));
        }
        QTest::mouseClick(parentLink, Qt::LeftButton);
        QTRY_COMPARE(detail->entityId(), parentId);
        QVERIFY(parentHost->isHidden());
        QTest::mouseClick(close, Qt::LeftButton);
        QTRY_VERIFY(!frame->isVisible());

        panel.resize(1240, 820);
        panel.showTask(childId);
        QTRY_VERIFY(frame->isVisible() && !frame->property("overlay").toBool());
        QVERIFY(!close->isVisible());
        QVERIFY(!separator->isVisible());
        QVERIFY(completed->isVisible());
        QVERIFY(parentHost->isVisible());
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

        // 快捷按钮立即接受并关闭：保留既有时刻，无时刻保持全天。
        const QDate today = toAppTime(appNow()).date();
        const QDate expected[] = {today, today.addDays(1), today.addDays(7), today.addMonths(1)};
        for (int index = 0; index < 4; ++index) {
            DatePickerPopup picker; picker.show();
            QToolButton* quick[4] = {nullptr, nullptr, nullptr, nullptr};
            for (auto* button : picker.findChildren<QToolButton*>(QStringLiteral("DatePickerQuick")))
                quick[button->property("quickIndex").toInt()] = button;
            for (auto* button : quick) QVERIFY(button);
            QSignalSpy accepted(&picker, &QDialog::accepted);
            QTest::mouseClick(quick[index], Qt::LeftButton);
            QCOMPARE(accepted.count(), 1);
            QVERIFY(!picker.isVisible());
            QCOMPARE(picker.allDayDate(), expected[index]);
            QVERIFY(!picker.hasTime());
        }
        {
            DatePickerPopup picker; picker.show();
            picker.setDateTime(fromAppTime(QDateTime(today.addDays(-2), QTime(9, 45))));
            QToolButton* todayButton = nullptr;
            for (auto* button : picker.findChildren<QToolButton*>(QStringLiteral("DatePickerQuick")))
                if (button->property("quickIndex").toInt() == 0) todayButton = button;
            QVERIFY(todayButton);
            QTest::mouseClick(todayButton, Qt::LeftButton);
            QVERIFY(!picker.isVisible());
            QVERIFY(picker.hasTime());
            QCOMPARE(toAppTime(picker.dateTime()).date(), today);
            QCOMPARE(toAppTime(picker.dateTime()).time(), QTime(9, 45));
        }

        // 时间内嵌展开：列表选 00:00 是有效明确时刻；清空后回车回到纯日期。
        DatePickerPopup picker; picker.show();
        QPushButton* timeEntry = nullptr;
        for (auto* button : picker.findChildren<QPushButton*>(QStringLiteral("DatePickerEntry")))
            if (button->accessibleName() == QStringLiteral("时间")) timeEntry = button;
        QVERIFY(timeEntry); QTest::mouseClick(timeEntry, Qt::LeftButton);
        auto* timePage = picker.findChild<QWidget*>(QStringLiteral("DatePickerTimeExpansion"));
        QVERIFY(timePage);
        auto* times = timePage->findChild<QListWidget*>(QStringLiteral("DatePickerList"));
        QVERIFY(times); QTRY_VERIFY(times->isVisible());
        QTest::mouseClick(times->viewport(), Qt::LeftButton, Qt::NoModifier,
                          times->visualItemRect(times->item(0)).center());
        QTRY_VERIFY(picker.hasTime());
        QCOMPARE(toAppTime(picker.dateTime()).time(), QTime(0, 0));
        QVERIFY(picker.isVisible());
        QTRY_VERIFY(!timePage->isVisible());
        QTest::mouseClick(timeEntry, Qt::LeftButton);
        auto* timeEdit = timePage->findChild<QLineEdit*>(QStringLiteral("DatePickerInput"));
        QVERIFY(timeEdit); QTRY_VERIFY(timePage->isVisible());
        timeEdit->clear();
        QTest::keyClick(timeEdit, Qt::Key_Return);
        QVERIFY(!picker.hasTime());
        QVERIFY(picker.allDayDate().isValid());
        QTRY_VERIFY(!timePage->isVisible());
        picker.reject();
    }
    void dateControllerStoresDateOnlyAndExplicitMidnight() {
        const QString id = m_store->createTask(QStringLiteral("日期模式"));
        QWidget owner; owner.resize(400, 250); owner.show();
        TaskDateController controller(m_store.get(), &owner);
        QSignalSpy saved(&controller, &TaskDateController::saved);
        const QDate today = toAppTime(appNow()).date();
        const auto openPicker = [&controller, &owner, &id]() -> QPointer<DatePickerPopup> {
            controller.open(id, QRect(owner.mapToGlobal(QPoint(30, 30)), QSize(80, 30)));
            QPointer<DatePickerPopup> picker = owner.findChild<DatePickerPopup*>();
            return picker;
        };
        const auto quickIn = [](DatePickerPopup* picker, int index) {
            for (auto* button : picker->findChildren<QToolButton*>(QStringLiteral("DatePickerQuick")))
                if (button->property("quickIndex").toInt() == index) return button;
            return static_cast<QToolButton*>(nullptr);
        };

        // 快捷“今天”立即接受：保存纯日期（绝不伪造 00:00 时刻）。
        QPointer<DatePickerPopup> picker = openPicker();
        QVERIFY(picker); QTRY_VERIFY(picker->isVisible());
        QTest::mouseClick(quickIn(picker.data(), 0), Qt::LeftButton);
        QTRY_COMPARE(saved.count(), 1);
        QCOMPARE(m_store->entity(id).allDayDate, today);
        QVERIFY(!m_store->entity(id).dueAt.isValid());
        QVERIFY(!m_store->entity(id).startAt.isValid());
        TaskDetailWidget dateOnlyDetail(m_store.get()); QVERIFY(dateOnlyDetail.setTask(id));
        QVERIFY(!dateOnlyDetail.findChild<QPushButton*>(QStringLiteral("DateChip"))->text()
                     .contains(QStringLiteral("00:00")));
        QTRY_VERIFY(!picker);

        // 重新打开载入纯日期；时间子层选 00:00（有效明确时刻）后父层确认。
        picker = openPicker();
        QVERIFY(picker); QTRY_VERIFY(picker->isVisible());
        QVERIFY(!picker->hasTime()); QCOMPARE(picker->allDayDate(), today);
        QPushButton* timeEntry = nullptr;
        for (auto* button : picker->findChildren<QPushButton*>(QStringLiteral("DatePickerEntry")))
            if (button->accessibleName() == QStringLiteral("时间")) timeEntry = button;
        QVERIFY(timeEntry); QTest::mouseClick(timeEntry, Qt::LeftButton);
        auto* timePage = picker->findChild<QWidget*>(QStringLiteral("DatePickerTimeExpansion"));
        QVERIFY(timePage);
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

        // 已有 00:00 时刻时快捷“明天”保留时刻并立即接受。
        picker = openPicker();
        QVERIFY(picker); QTRY_VERIFY(picker->isVisible());
        QVERIFY(picker->hasTime());
        QTest::mouseClick(quickIn(picker.data(), 1), Qt::LeftButton);
        QTRY_COMPARE(saved.count(), 3);
        QVERIFY(m_store->entity(id).dueAt.isValid());
        QCOMPARE(toAppTime(m_store->entity(id).dueAt).date(), today.addDays(1));
        QCOMPARE(toAppTime(m_store->entity(id).dueAt).time(), QTime(0, 0));
        QTRY_VERIFY(!picker);

        // 清空时刻并回车，回到纯日期后父层确认。
        picker = openPicker();
        QVERIFY(picker); QTRY_VERIFY(picker->isVisible());
        QVERIFY(picker->hasTime());
        timeEntry = nullptr;
        for (auto* button : picker->findChildren<QPushButton*>(QStringLiteral("DatePickerEntry")))
            if (button->accessibleName() == QStringLiteral("时间")) timeEntry = button;
        QVERIFY(timeEntry); QTest::mouseClick(timeEntry, Qt::LeftButton);
        auto* timeExpansion = picker->findChild<QWidget*>(QStringLiteral("DatePickerTimeExpansion"));
        QVERIFY(timeExpansion); QTRY_VERIFY(timeExpansion->isVisible());
        auto* timeEdit = timeExpansion->findChild<QLineEdit*>(QStringLiteral("DatePickerInput"));
        QVERIFY(timeEdit);
        timeEdit->clear();
        QTest::keyClick(timeEdit, Qt::Key_Return);
        QVERIFY(!picker->hasTime()); QCOMPARE(picker->allDayDate(), today.addDays(1));
        picker->accept();
        QTRY_COMPARE(saved.count(), 4);
        QVERIFY(!m_store->entity(id).dueAt.isValid());
        QCOMPARE(m_store->entity(id).allDayDate, today.addDays(1));
        QTRY_VERIFY(!picker);

        // 取消（Escape/外点）不写库。
        picker = openPicker();
        QVERIFY(picker); QTRY_VERIFY(picker->isVisible());
        picker->reject();
        QTRY_VERIFY(!picker);
        QCOMPARE(saved.count(), 4);
        QCOMPARE(m_store->entity(id).allDayDate, today.addDays(1));

        TaskStore reloaded(Database::instance().connection());
        QString error; QVERIFY(reloaded.load(&error));
        QCOMPARE(reloaded.entity(id).allDayDate, today.addDays(1));
        QVERIFY(!reloaded.entity(id).dueAt.isValid());
    }
    void dirtyDesktopNoteFlushesOnArchiveAndRetainsDraftAfterDeletion() {
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
        QCOMPARE(notes.count(), 1); // 回收站不接受旧草稿写入，也不能直接丢掉编辑窗口。
        QCOMPARE(m_store->entity(id).body, QStringLiteral("归档之前尚未自动保存的正文"));
        QCOMPARE(editor->content(), QStringLiteral("删除清单前的新正文"));
        QVERIFY(note->textRejected()); QVERIFY(!note->finishEditing());
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

        SettingsDialog settingsDialog(nullptr);
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
        // 解除置顶确保状态实际变化，连续取当前时间可能得到相同值。
        QVERIFY(m_store->setPinnedAt(dated, QDateTime()));
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
    void settingsSelectorsUseSharedComponentAndKeepTextInkVisible() {
        SettingsDialog settings(nullptr); settings.show();
        DisplaySettingsDialog display(false, false); display.show();
        auto* theme = qobject_cast<MenuComboBox*>(settings.findChild<QComboBox*>(QStringLiteral("AppearanceTheme")));
        auto* font = qobject_cast<MenuComboBox*>(settings.findChild<QComboBox*>(QStringLiteral("AppearanceFont")));
        auto* smart = qobject_cast<MenuComboBox*>(settings.findChild<QComboBox*>(QStringLiteral("SmartVisibility_all")));
        auto* date = qobject_cast<MenuComboBox*>(display.findChild<QComboBox*>(QStringLiteral("DateDisplayMode")));
        QVERIFY(theme && font && smart && date);
        smart->setCurrentIndex(1);  // 有内容时显示：覆盖较长的当前值
        font->setCurrentIndex(font->findData(QStringLiteral("noto-sans-sc")));
        MenuComboBox compact;
        compact.addItem(QStringLiteral("紧凑选择")); compact.setFixedHeight(24); compact.show();
        const QVector<MenuComboBox*> combos = {theme, font, smart, date, &compact};
        FontManager& fonts = FontManager::instance();
        for (const QString& id : {QStringLiteral("default"), QStringLiteral("noto-sans-sc"),
                                  QStringLiteral("lxgw-wenkai")}) {
            QVERIFY(fonts.setPreference(id)); QApplication::processEvents();
            for (MenuComboBox* combo : combos) {
                QVERIFY(combo->height() == 24 || combo->height() == 30);
                combo->clearFocus(); QTest::mouseMove(combo, QPoint(-10, -10));
                const qreal dpr = combo->devicePixelRatioF();
                QImage image(qCeil(combo->width() * dpr), qCeil(combo->height() * dpr),
                             QImage::Format_ARGB32_Premultiplied);
                image.fill(Qt::transparent); image.setDevicePixelRatio(dpr);
                { QPainter painter(&image); combo->render(&painter); }
                QRect ink;
                const int right = qMax(0, qFloor((combo->width() - 24) * dpr));
                for (int y = 0; y < image.height(); ++y) for (int x = qFloor(8 * dpr); x < right; ++x) {
                    const QColor pixel = image.pixelColor(x, y);
                    if (pixel.alpha() > 40 && qGray(pixel.red(), pixel.green(), pixel.blue()) < 235)
                        ink = ink.united(QRect(x, y, 1, 1));
                }
                QVERIFY2(!ink.isEmpty(), qPrintable(combo->objectName() + QStringLiteral("没有当前值墨迹")));
                const QRectF expected = QFontMetricsF(combo->font()).tightBoundingRect(combo->currentText());
                const qreal actualHeight = ink.height() / dpr;
                QVERIFY2(actualHeight + 1.5 >= expected.height(),
                    qPrintable(QStringLiteral("%1/%2 字高被裁：实际%3，字体墨迹%4")
                        .arg(combo->objectName(), id).arg(actualHeight).arg(expected.height())));
                QVERIFY2(ink.top() / dpr >= 1.0 && ink.bottom() / dpr <= combo->height() - 2.0,
                    qPrintable(QStringLiteral("%1/%2 文字接触控件边缘：top=%3 bottom=%4 height=%5")
                        .arg(combo->objectName(), id).arg(ink.top() / dpr).arg(ink.bottom() / dpr).arg(combo->height())));
                const qreal mid = (ink.top() + ink.bottom()) / 2.0 / dpr;
                QVERIFY2(qAbs(mid - (combo->height() - 1) / 2.0) <= 1.25,
                    qPrintable(QStringLiteral("%1/%2 文字中线 %3，控件中线 %4")
                        .arg(combo->objectName(), id).arg(mid).arg((combo->height() - 1) / 2.0)));
            }
        }
        QVERIFY(fonts.setPreference(QStringLiteral("default")));
        compact.hide(); display.reject(); settings.reject();
    }

    void bundledInterfaceFontsRegisterAndPreserveEditorState() {
        FontManager& fonts = FontManager::instance();
        QVERIFY(fonts.setPreference(QStringLiteral("default")));
        const QString defaultFamily = fonts.family();
        const QStringList choices = fonts.availableChoices();
        QVERIFY(choices.contains(QStringLiteral("noto-sans-sc")));
        QVERIFY(choices.contains(QStringLiteral("lxgw-wenkai")));
        for (const QString& id : {QStringLiteral("noto-sans-sc"), QStringLiteral("lxgw-wenkai")}) {
            const QString family = fonts.familyFor(id);
            QVERIFY2(!family.isEmpty(), qPrintable(QStringLiteral("Qt 5.12 未注册内置字体 %1").arg(id)));
            QVERIFY(QFontDatabase().families().contains(family));
            for (const QFont::Weight weight : {QFont::Normal, QFont::Bold}) {
                QFont font(family); font.setPointSizeF(12); font.setWeight(weight);
                const QRawFont raw = QRawFont::fromFont(font);
                QVERIFY2(raw.isValid(), qPrintable(QStringLiteral("字体无效：%1/%2").arg(id).arg(weight)));
                // QRawFont 可按平台匹配到已注册的 Medium 独立字族，不强制匹配后的族名等于首选族。
                QVERIFY(!raw.familyName().isEmpty());
                QVERIFY(QFontDatabase().families().contains(raw.familyName()));
                for (const uint codepoint : {0x6e05u, 0x5355u, 0x41u})
                    QVERIFY2(raw.supportsCharacter(codepoint),
                             qPrintable(QStringLiteral("%1 缺少 U+%2").arg(id).arg(codepoint, 0, 16)));
            }
        }

        SettingsDialog settings(nullptr);
        auto* combo = settings.findChild<QComboBox*>(QStringLiteral("AppearanceFont"));
        QVERIFY(combo);
        QVERIFY(combo->findData(QStringLiteral("noto-sans-sc")) >= 0);
        QVERIFY(combo->findData(QStringLiteral("lxgw-wenkai")) >= 0);

        MarkdownEditor editor;
        const QString original = QStringLiteral("初始");
        editor.setContent(original);
        QTextEdit* body = editor.textEditor();
        body->setFocus(); QApplication::processEvents();
        QTextCursor append = body->textCursor(); append.movePosition(QTextCursor::End);
        body->setTextCursor(append); body->insertPlainText(QStringLiteral(" 可撤销"));
        QApplication::processEvents();
        QCOMPARE(editor.content(), QStringLiteral("初始 可撤销"));
        QVERIFY(body->document()->isUndoAvailable());
        QTextCursor selected = body->document()->find(QStringLiteral("可撤销"));
        QVERIFY(!selected.isNull()); body->setTextCursor(selected);
        const int selectionStart = selected.selectionStart(), selectionEnd = selected.selectionEnd();
        const QString before = editor.content();
        QSignalSpy contentChanged(&editor, &MarkdownEditor::contentChanged);
        QVERIFY(fonts.setPreference(QStringLiteral("noto-sans-sc")));
        QCOMPARE(editor.content(), before);
        QCOMPARE(contentChanged.count(), 0);
        QCOMPARE(body->textCursor().selectionStart(), selectionStart);
        QCOMPARE(body->textCursor().selectionEnd(), selectionEnd);
        QCOMPARE(body->font().family(), fonts.familyFor(QStringLiteral("noto-sans-sc")));
        QVERIFY(body->document()->isUndoAvailable());
        body->undo();
        QApplication::processEvents();
        // Qt 5.12 会把字符格式变化纳入撤销；允许多一个字体格式项，但原正文历史绝不能被清空。
        QVERIFY(editor.content() == before || editor.content() == original);
        if (editor.content() != original) {
            QVERIFY(body->document()->isUndoAvailable());
            body->undo();
        }
        QTRY_COMPARE(editor.content(), original);

        QVERIFY(fonts.setPreference(QStringLiteral("lxgw-wenkai")));
        QCOMPARE(ds::qfont(ds::FontScene::Body).family(), fonts.familyFor(QStringLiteral("lxgw-wenkai")));
        QCOMPARE(editor.content(), original);
        QVERIFY(fonts.setPreference(QStringLiteral("default")));
        QCOMPARE(fonts.family(), defaultFamily);
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
        const QRectF icon(rowRect.left() + 36.0, rowRect.center().y() - 6.0, 16.0, 16.0);
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
        const QString dir = qEnvironmentVariable("KYLINLIST_SCREENSHOT_DIR");
        if (!dir.isEmpty()) {
            QDir().mkpath(dir);
            const QRect crop(qRound((rowRect.left() - 8) * dpr), qRound((rowRect.top() - 2) * dpr),
                             qRound(280 * dpr), qRound((rowRect.height() + 4) * dpr));
            QVERIFY(image.copy(crop).scaled(crop.width() * 2, crop.height() * 2,
                                            Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                        .save(dir + QStringLiteral("/section-pinned-header.png")));
        }
    }
    void taskDateAreaShowsPointingHandCursor() {
        const QString list = m_store->createList(QStringLiteral("日期游标"), QString(),
                                                 EntityKind::Task, QString(),
                                                 QStringLiteral("#4CA1FF"));
        const QString id = m_store->createTask(QStringLiteral("带日期任务"), list);
        Entity task = m_store->entity(id);
        task.allDayDate = toAppTime(appNow()).date();
        QVERIFY(m_store->saveTask(task));

        TaskPanel panel(m_store.get()); panel.resize(900, 640); panel.show();
        auto* view = panel.findChild<TaskListView*>(QStringLiteral("TaskList"));
        auto* model = qobject_cast<TaskListModel*>(view->model());
        auto* delegate = view ? qobject_cast<TaskDelegate*>(view->itemDelegate()) : nullptr;
        QVERIFY(view && model && delegate);
        const QModelIndex index = model->indexForTask(id);
        QVERIFY(index.isValid());
        view->scrollTo(index);
        panel.raise(); panel.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&panel));
        QApplication::processEvents();
        const QRect row = view->visualRect(index);
        QVERIFY(row.isValid());
        QStyleOptionViewItem option;
        option.rect = row;
        const QRect date = delegate->dateHitRect(option, index);
        QVERIFY2(!date.isEmpty(), "带日期任务应有可点击日期区域");
        QVERIFY(row.contains(date.center()));
        struct MoveRecorder final : QObject {
            int count = 0;
            QPoint position;
            bool eventFilter(QObject*, QEvent* event) override {
                if (event->type() == QEvent::MouseMove) {
                    ++count; position = static_cast<QMouseEvent*>(event)->pos();
                }
                return false;
            }
        } moves;
        view->viewport()->installEventFilter(&moves);
        // Qt 5.12 的 QWidget 重载只调用 QCursor::setPos，系统可不回发 move；
        // 使用标准 QWindow 重载，从窗口向实际 viewport 分派鼠标事件，兼容 offscreen。
        QVERIFY(panel.windowHandle()); QVERIFY(view->isVisible()); QVERIFY(view->viewport()->hasMouseTracking());
        const auto moveTo = [&panel, view](const QPoint& position) {
            QTest::mouseMove(panel.windowHandle(), view->viewport()->mapTo(&panel, position));
        };
        const QPoint outside(row.left() + 80, row.center().y()); // 避开列边缘的 resize 命中区。
        QVERIFY(!date.contains(outside));
        QTest::mouseMove(panel.windowHandle(), QPoint(10, 10));
        moveTo(outside);
        QTRY_VERIFY2(moves.count > 0 && moves.position == outside,
            qPrintable(QStringLiteral("viewport moves=%1, last=%2,%3, expected=%4,%5")
                .arg(moves.count).arg(moves.position.x()).arg(moves.position.y()).arg(outside.x()).arg(outside.y())));
        moveTo(date.center());
        QTRY_COMPARE(moves.position, date.center());
        QTRY_COMPARE(view->viewport()->cursor().shape(), Qt::PointingHandCursor);
        // 同一行的标题区域仍保持普通指针，并确认 move 已真正投递。
        moveTo(outside);
        QTRY_COMPARE(moves.position, outside);
        QTRY_COMPARE(view->viewport()->cursor().shape(), Qt::ArrowCursor);
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
            for (int x = NavigationGeometry::DisclosureLeft;
                 x < NavigationGeometry::DisclosureLeft
                     + NavigationGeometry::DisclosureGlyphSize; ++x)
                if (folderIcon.pixelColor(x, y).alpha() > 0) ++arrowInk;
        QVERIFY(arrowInk > 0);
        QVERIFY(QRectF(NavigationGeometry::RowHorizontalInset, 10,
                       NavigationGeometry::FolderDisclosureWidth, 16)
                    .contains(QPointF(17, 18)));
        const QImage hovered = render(folder);
        QCOMPARE(hovered.pixelColor(NavigationGeometry::DisclosureLeft + 1, 18).alpha(), 0);
        QStyleOptionViewItem hoverOption;
        hoverOption.rect = QRect(0, 0, 220, 36);
        hoverOption.state = QStyle::State_Enabled | QStyle::State_MouseOver;
        QImage hoverRow(220, 36, QImage::Format_ARGB32_Premultiplied);
        hoverRow.fill(Qt::white);
        QPainter hoverPainter(&hoverRow);
        delegate.paint(&hoverPainter, hoverOption, navigation.model()->index(navigation.row(folder), 0));
        hoverPainter.end();
        QCOMPARE(hoverRow.pixelColor(NavigationGeometry::DisclosureLeft + 1, 18),
                 ThemeManager::instance().color(ds::ColorToken::RowHover));
        QCOMPARE(hoverRow.pixelColor(NavigationGeometry::DisclosureLeft + 1, 18),
                 ThemeManager::instance().color(ds::ColorToken::RowHover));

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
        const auto organizationVisible = [this, navigation]() {
            for (int row = 0; row < navigation->count(); ++row) {
                const auto* item = navigation->item(row);
                if (item->data(NavigationDelegate::SectionRole).toBool()
                    || item->data(NavigationDelegate::DividerRole).toBool()) continue;
                const auto scope = static_cast<ViewScope>(item->data(Qt::UserRole).toInt());
                const QString listId = item->data(Qt::UserRole + 1).toString();
                bool inbox = false;
                for (const TaskList& list : m_store->lists()) inbox |= list.id == listId && list.inbox;
                if (!inbox && (scope == ViewScope::List || scope == ViewScope::Folder))
                    if (!item->isHidden()) return true;
            }
            return false;
        };
        const auto inboxVisible = [this, navigation]() {
            for (int row = 0; row < navigation->count(); ++row) {
                const auto* item = navigation->item(row);
                const QString listId = item->data(Qt::UserRole + 1).toString();
                for (const TaskList& list : m_store->lists())
                    if (list.id == listId && list.inbox) return !item->isHidden();
            }
            return false;
        };
        QVERIFY(organizationVisible());
        QVERIFY(inboxVisible());
        header->setExpanded(true);
        QTest::mouseClick(disclosure, Qt::LeftButton);
        QTRY_VERIFY(!organizationVisible());
        QVERIFY(inboxVisible());
        header = panel.findChild<SidebarSectionHeader*>(QStringLiteral("ListSectionHeader"));
        // 标题整行都可折叠，不需要命中箭头；右侧添加按钮是独立控件。
        QTest::mouseClick(header, Qt::LeftButton, Qt::NoModifier,
                          QPoint(header->width() / 2, header->height() / 2));
        QTRY_VERIFY(organizationVisible());
        QVERIFY(inboxVisible());
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
    void taskRowStatusIconsShareSizeAndCenterline() {
        const QString list = m_store->createList(QStringLiteral("状态清单"), QString(), EntityKind::Note,
                                                 QString(), QStringLiteral("#4CA1FF"));
        const QString note = m_store->createNote(QStringLiteral("状态笔记"), list);
        QVERIFY(!note.isEmpty());
        QVERIFY(!m_store->addReminder(note, appNow().addDays(1)).id.isEmpty());
        const QString source = m_dir->path() + QStringLiteral("/status.png");
        QPixmap png(12, 12); png.fill(QColor(QStringLiteral("#668BE3"))); QVERIFY(png.save(source));
        QVERIFY(!m_store->importAttachment(note, source).id.isEmpty());
        TaskListModel model(m_store.get());
        const QModelIndex index = model.indexForTask(note); QVERIFY(index.isValid());
        QVERIFY(model.data(index, TaskListModel::ReminderVisibleRole).toBool());
        QVERIFY(model.data(index, TaskListModel::AttachmentVisibleRole).toBool());
        QCOMPARE(AppIcons::iconName(IconId::RowNote), QStringLiteral("item-note"));
        const QFontMetrics smallMetrics(AppStyle::font(AppStyle::Font::Small));
        const int listWidth = qMin(72, smallMetrics.horizontalAdvance(QStringLiteral("状态清单"))) + 27;
        // 状态从右往左排：笔记 716、附件 694、提醒 672；提醒左侧 4px 间隙后才是清单。
        const int listX = 760 - 26 - 3 * (18 + 4) - listWidth;
        TaskDelegate delegate;
        for (const qreal dpr : {1.0, 2.0}) {
            QImage image(qRound(760 * dpr), qRound(40 * dpr), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::white); image.setDevicePixelRatio(dpr);
            QPainter painter(&image);
            QStyleOptionViewItem option; option.rect = QRect(0, 0, 760, 40); option.state = QStyle::State_Enabled;
            delegate.paint(&painter, option, index); painter.end();
            const auto ink = [&image, dpr](const QRectF& logical) {
                QRect bounds;
                const int x0 = qFloor(logical.left() * dpr), x1 = qCeil(logical.right() * dpr);
                const int y0 = qFloor(logical.top() * dpr), y1 = qCeil(logical.bottom() * dpr);
                for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
                    const QColor pixel = image.pixelColor(x, y);
                    if (pixel.alpha() > 40 && qGray(pixel.red(), pixel.green(), pixel.blue()) < 220)
                        bounds = bounds.united(QRect(x, y, 1, 1));
                }
                return bounds;
            };
            struct Crop { const char* name; QRectF rect; };
            const Crop crops[] = {
                {"checkbox", QRectF(34, 0, 16, 40)}, {"list", QRectF(listX, 0, 14, 40)},
                {"reminder", QRectF(672, 0, 18, 40)}, {"attachment", QRectF(694, 0, 18, 40)},
                {"note", QRectF(716, 0, 18, 40)}
            };
            QHash<QString, QRect> bounds;
            for (const Crop& crop : crops) {
                const QRect current = ink(crop.rect); bounds.insert(QLatin1String(crop.name), current);
                QVERIFY2(!current.isEmpty(), crop.name);
                const qreal mid = (current.top() + current.bottom()) / 2.0 / dpr;
                QVERIFY2(qAbs(mid - 21.0) <= 1.0,
                         qPrintable(QStringLiteral("%1 DPR=%2 中线=%3，期望21").arg(QLatin1String(crop.name)).arg(dpr).arg(mid)));
            }
            const QRect noteInk = bounds.value(QStringLiteral("note"));
            for (const QString& other : {QStringLiteral("reminder"), QStringLiteral("attachment")}) {
                const QRect reference = bounds.value(other);
                QVERIFY2(qAbs(noteInk.width() - reference.width()) <= qCeil(1.25 * dpr),
                    qPrintable(QStringLiteral("note/%1 宽度不一致：%2 vs %3").arg(other).arg(noteInk.width()).arg(reference.width())));
                QVERIFY2(qAbs(noteInk.height() - reference.height()) <= qCeil(1.25 * dpr),
                    qPrintable(QStringLiteral("note/%1 高度不一致：%2 vs %3").arg(other).arg(noteInk.height()).arg(reference.height())));
            }
        }
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
        SidebarSectionHeader* archiveHeader = nullptr;
        const auto visibleArchiveHeader = [&panel]() -> SidebarSectionHeader* {
            for (auto* candidate : panel.findChildren<SidebarSectionHeader*>(QStringLiteral("ArchiveSectionHeader")))
                if (candidate->isVisible()) return candidate;
            return nullptr;
        };
        QTRY_VERIFY((archiveHeader = visibleArchiveHeader()) != nullptr);
        int archiveRow = -1;
        const auto findArchiveRow = [&navigation, &archiveRow]() {
            archiveRow = -1;
            for (int candidate = 0; candidate < navigation->count(); ++candidate) {
                QWidget* widget = navigation->itemWidget(navigation->item(candidate));
                if (widget && widget->objectName() == QStringLiteral("ArchiveSectionHeader")) {
                    archiveRow = candidate;
                    return true;
                }
            }
            return false;
        };
        QTRY_VERIFY(findArchiveRow());
        QVERIFY(navigation->item(archiveRow)->data(NavigationDelegate::SectionRole).toBool());
        QVERIFY(navigation->item(archiveRow)->data(Qt::UserRole + 1).toString().isEmpty());
        QVERIFY(navigation->item(archiveRow)->data(Qt::DisplayRole).toString().isEmpty());
        QVERIFY(navigation->item(archiveRow)->flags() == Qt::NoItemFlags);
        QVERIFY(!panel.findChild<QToolButton*>(QStringLiteral("ArchiveToggleButton")));
        auto* toggle = archiveHeader->disclosureButton();
        QVERIFY(toggle);
        QTRY_VERIFY(toggle->isVisible());
        QPointer<SidebarSectionHeader> replacedHeader(archiveHeader);
        QTest::mouseClick(toggle, Qt::LeftButton);
        QTRY_VERIFY(!replacedHeader); // 重建后旧 item widget 必须销毁，不能留下同名隐藏控件
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
                && model.data(index, TaskListModel::SectionKeyRole).toString() == QLatin1String("time:overdue"))
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
                && model.data(index, TaskListModel::SectionKeyRole).toString() == QLatin1String("time:overdue"))
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
        const auto render = [&delegate](const QModelIndex& index, bool dragPreview = false) {
            QImage image(560, 40, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            QStyleOptionViewItem option;
            option.rect = QRect(0, 0, 560, 40);
            option.state = QStyle::State_Enabled | QStyle::State_MouseOver;
            if (dragPreview) delegate.paintDragPreview(&painter, option, index);
            else delegate.paint(&painter, option, index);
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
        const QImage dragImage = render(parentIndex, true);
        QCOMPARE(qAlpha(dragImage.pixel(16, 20)), 0);
        bool previewOnlyOmitsColorBarAndSeparator = true;
        for (int y = 0; y < dragImage.height(); ++y) {
            for (int x = 0; x < dragImage.width(); ++x) {
                if ((x >= 16 && x <= 18) || y >= dragImage.height() - 2) continue;
                if (dragImage.pixel(x, y) != parentImage.pixel(x, y)) previewOnlyOmitsColorBarAndSeparator = false;
            }
        }
        QVERIFY(previewOnlyOmitsColorBarAndSeparator);

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
                     QStringLiteral("time:tomorrow"));
        QCOMPARE(model.data(model.indexForTask(child), TaskListModel::SectionKeyRole).toString(),
                 QStringLiteral("time:tomorrow"));
        childOccurrences = 0;
        for (int row = 0; row < model.rowCount(); ++row)
            if (model.entityId(model.index(row, 0)) == child) ++childOccurrences;
        QCOMPARE(childOccurrences, 1); // 子段排在父段之前也只能放进父级子树一次。
        const int filteredEntityCount = model.entityCount();
        QCOMPARE(filteredEntityCount, 2); // 无日期的独立任务不进入最近 7 天。

        const QDateTime childPin = appNow();
        QVERIFY(m_store->setPinnedAt(child, childPin));
        QCOMPARE(model.data(model.indexForTask(child), TaskListModel::SectionKeyRole).toString(),
                 QStringLiteral("time:tomorrow")); // 独立置顶的孩子仍归属于可见父级，不重复显示。
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
        QTRY_COMPARE(model->filter().grouping, GroupMode::Time);
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
                                                    bool before = false, bool after = false,
                                                    bool asChild = false) {
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
            const int depth = currentTarget.data(TaskListModel::DepthRole).toInt();
            const int x = asChild ? rect.right() - 12
                : after ? rect.left() + 24 : rect.center().x();
            const QPoint point(x, before ? rect.top() + 2
                : (after || asChild) ? rect.bottom() - 2 : rect.center().y());
            QDragMoveEvent move(point, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view->viewport(), &move);
            if (asChild) {
                const QImage indicator = view->viewport()->grab().toImage();
                const qreal dpr = indicator.devicePixelRatio();
                const QColor pixel = indicator.pixelColor(qRound((20 + (depth + 1) * 24 + 4) * dpr),
                                                           qRound((rect.bottom() - 1) * dpr));
                if (pixel != AppStyle::color(AppStyle::Color::Accent)) {
                    qDebug() << "missing indented drop indicator" << depth << pixel;
                    return false;
                }
            }
            QDropEvent event(QPointF(point), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view->viewport(), &event);
            const TaskDrop::Intent intent = drops.count() == 1
                ? qvariant_cast<TaskDrop::Intent>(drops.first().at(0)) : TaskDrop::Intent();
            const bool delivered = drops.count() == 1 && intent.isValid()
                && intent.sourceId == sourceId && intent.targetGroup.stableId() == targetKey
                && intent.targetParentId == (asChild ? targetId : QString())
                && intent.placement == (asChild ? TaskDrop::Placement::AsChild
                    : header ? TaskDrop::Placement::IntoGroup
                    : after ? TaskDrop::Placement::After : TaskDrop::Placement::Before);
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
            if (drops.count() != 1) return false;
            const auto intent = qvariant_cast<TaskDrop::Intent>(drops.first().at(0));
            return event.isAccepted() && intent.isValid() && intent.sourceId == sourceId
                && intent.targetGroup.stableId() == key
                && intent.placement == TaskDrop::Placement::IntoGroup;
        };
        QTRY_VERIFY(!headerFor(QStringLiteral("pinned")).isValid());
        QVERIFY(dropOnHeader(todayTask, QStringLiteral("pinned")));
        QTRY_VERIFY(m_store->entity(todayTask).pinnedAt.isValid());
        QCOMPARE(m_store->entity(todayTask).allDayDate, today);
        QVERIFY(!headerFor(QStringLiteral("time:today")).isValid());
        QVERIFY(dropOnHeader(todayTask, QStringLiteral("time:today")));
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
        QVERIFY(drop(todayTask, headerFor(QStringLiteral("time:overdue"))));
        QTRY_COMPARE(m_store->entity(todayTask).allDayDate, today.addDays(-1));

        const QString secondToday = m_store->createTask(QStringLiteral("第二个今天"), list);
        QVERIFY(setDate(secondToday, today));
        QTRY_VERIFY(model->indexForTask(secondToday).isValid());
        QVERIFY(drop(secondToday, headerFor(QStringLiteral("time:next7days"))));
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
        QVERIFY(!headerFor(QStringLiteral("time:today")).isValid());
        QMimeData emptyMime;
        emptyMime.setData(TaskListView::mimeType(), pinCandidate.toUtf8());
        const QPoint enterPoint = view->visualRect(model->indexForTask(pinCandidate)).center();
        QDragEnterEvent emptyEnter(enterPoint, Qt::MoveAction, &emptyMime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &emptyEnter);
        const QModelIndex emptyToday = headerFor(QStringLiteral("time:today"));
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
                 QStringLiteral("time:today"));
        QCOMPARE(qvariant_cast<GroupKey>(model->indexForTask(datedChild).data(TaskListModel::GroupKeyRole)).stableId(),
                 QStringLiteral("time:today"));
        QCOMPARE(effectiveDate(m_store->entity(datedChild)), today.addDays(1));
        QVERIFY(drop(datedChild, model->indexForTask(todayAnchor), false, true));
        QTRY_VERIFY(m_store->entity(datedChild).parentId.isEmpty());
        QCOMPARE(m_store->entity(datedChild).allDayDate, today);
        QTRY_COMPARE(model->indexForTask(datedChild).data(TaskListModel::SectionKeyRole).toString(),
                     QStringLiteral("time:today"));

        const QString singleList = m_store->createList(QStringLiteral("仅置顶清单"));
        const QString onlyPinned = m_store->createTask(QStringLiteral("唯一的置顶任务"), singleList);
        QVERIFY(m_store->setPinnedAt(onlyPinned, appNow()));
        ViewSpec direct; direct.scope = ViewScope::List; direct.listId = singleList;
        model->setFilter(direct);
        QVERIFY(dropOnHeader(onlyPinned, QStringLiteral("time:undated")));
        QTRY_VERIFY(!m_store->entity(onlyPinned).pinnedAt.isValid());
        QVERIFY(!m_store->entity(onlyPinned).allDayDate.isValid());
        QCOMPARE(m_store->entity(onlyPinned).sortOrder, 0);
        QVERIFY(!headerFor(QStringLiteral("pinned")).isValid());
        int undatedHeaders = 0;
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex index = model->index(row, 0);
            if (index.data(TaskListModel::HeaderRole).toBool()
                && index.data(TaskListModel::SectionKeyRole).toString() == QLatin1String("time:undated"))
                ++undatedHeaders;
        }
        QCOMPARE(undatedHeaders, 1); // 正常无日期分区保留，临时投放副本消失。
        QCOMPARE(model->indexForTask(onlyPinned).data(TaskListModel::SectionKeyRole).toString(),
                 QStringLiteral("time:undated"));

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

        // 右下半区产生缩进落点并设父；源已有后代保持关系，折叠父项成功后自动展开。
        const QString dropParent = m_store->createTask(QStringLiteral("拖放目标父任务"), list);
        const QString dropSource = m_store->createTask(QStringLiteral("拖放源任务"), list);
        const QString dropGrandchild = m_store->createSubtask(QStringLiteral("源任务原有孩子"), dropSource);
        QTRY_VERIFY(model->indexForTask(dropSource).isValid());
        QVERIFY(drop(dropSource, model->indexForTask(dropParent), false, false, true));
        QTRY_COMPARE(m_store->entity(dropSource).parentId, dropParent);
        QCOMPARE(m_store->entity(dropGrandchild).parentId, dropSource);
        QTRY_COMPARE(model->data(model->indexForTask(dropSource), TaskListModel::DepthRole).toInt(), 1);
        QCOMPARE(model->data(model->indexForTask(dropGrandchild), TaskListModel::DepthRole).toInt(), 2);
        model->toggleChildren(dropParent);
        QTRY_VERIFY(model->data(model->indexForTask(dropParent), TaskListModel::ChildrenCollapsedRole).toBool());
        const QString secondDropSource = m_store->createTask(QStringLiteral("折叠后拖入的任务"), list);
        QTRY_VERIFY(model->indexForTask(secondDropSource).isValid());
        QVERIFY(drop(secondDropSource, model->indexForTask(dropParent), false, false, true));
        QTRY_COMPARE(m_store->entity(secondDropSource).parentId, dropParent);
        QTRY_VERIFY(model->indexForTask(secondDropSource).isValid());
        QVERIFY(!model->data(model->indexForTask(dropParent), TaskListModel::ChildrenCollapsedRole).toBool());

        // 文件夹按清单分组：跨组拖动使用 Intent 原子移动整个子树，不再依赖多 bool 信号。
        const QString folder = m_store->createFolder(QStringLiteral("拖动落点校验"));
        const QString second = m_store->createList(QStringLiteral("第二清单"), folder);
        const QString foreign = m_store->createTask(QStringLiteral("第二清单锚点"), second);
        QVERIFY(m_store->moveList(list, folder));
        ViewSpec folderFilter; folderFilter.scope = ViewScope::Folder; folderFilter.folderId = folder;
        folderFilter.grouping = GroupMode::List;
        model->setFilter(folderFilter);
        QSignalSpy folderDrops(view, &TaskListView::entityDropRequested);
        QVERIFY(drop(parent, model->indexForTask(foreign), true));
        QCOMPARE(folderDrops.count(), 1);
        const auto folderIntent = qvariant_cast<TaskDrop::Intent>(folderDrops.first().at(0));
        QCOMPARE(folderIntent.mode, GroupMode::List);
        QCOMPARE(folderIntent.targetGroup.stableId(), QStringLiteral("list:") + second);
        QTRY_COMPARE(m_store->entity(parent).listId, second);
        QCOMPARE(m_store->entity(child).listId, second);
        QCOMPARE(m_store->entity(sibling).listId, second);
        QCOMPARE(m_store->entity(child).parentId, parent);
        QCOMPARE(m_store->entity(sibling).parentId, parent);
        QCOMPARE(m_store->entity(foreign).listId, second);
    }
    void entityContextMenuMatchesWebGeometry() {
        const QString id = m_store->createTask(QStringLiteral("菜单几何任务"));
        EntityCommands commands(m_store.get());
        MenuContext context;
        QMenu* menu = commands.buildEntityMenu(context, id, nullptr);
        QVERIFY(menu);
        auto* popup = qobject_cast<PopupMenu*>(menu);
        QVERIFY(popup);
        QAction* add = nullptr;
        for (QAction* action : menu->actions())
            if (action->objectName() == QLatin1String("addSubtask")) add = action;
        QVERIFY(add);
        const QColor probeColor(1, 2, 3);
        QPixmap probePixmap(18, 18);
        probePixmap.fill(probeColor);
        QAction* probe = new QAction(QIcon(probePixmap), QStringLiteral("图标尺寸探针"), menu);
        probe->setObjectName(QStringLiteral("iconMetricProbe"));
        menu->insertAction(add, probe);
        menu->popup(QPoint(80, 80));
        QTRY_VERIFY(menu->isVisible());
        const int shadow = PopupMenu::shadowMargin();
        const QRect card(shadow, shadow,
                         menu->width() - 2 * shadow, menu->height() - 2 * shadow);
        QCOMPARE(card.width(), 194);
        QAction* separator = nullptr;
        for (QAction* action : menu->actions()) {
            if (!separator && action->isSeparator()) separator = action;
        }
        QVERIFY(add && separator);
        const QRect addRect = menu->actionGeometry(add);
        QCOMPARE(addRect.height(), 34);
        QCOMPARE(addRect.left(), card.left() + 4);
        QCOMPARE(addRect.right(), card.right() - 4);
        QCOMPARE(add->icon().actualSize(QSize(18, 18)), QSize(18, 18));
        QCOMPARE(menu->actionGeometry(separator).height(), 9);
        const QImage menuImage = menu->grab().toImage();
        const qreal dpr = menuImage.devicePixelRatio();
        const int separatorY = menu->actionGeometry(separator).center().y();
        const auto menuPixel = [&](const QPoint& point) {
            return menuImage.pixelColor(qRound(point.x() * dpr), qRound(point.y() * dpr));
        };
        QRect probeInk;
        for (int y = 0; y < menuImage.height(); ++y) {
            for (int x = 0; x < menuImage.width(); ++x) {
                if (menuImage.pixelColor(x, y) != probeColor) continue;
                const QRect pixel(x, y, 1, 1);
                probeInk = probeInk.isNull() ? pixel : probeInk.united(pixel);
            }
        }
        QCOMPARE(probeInk.width(), qRound(18 * dpr));
        QCOMPARE(probeInk.height(), qRound(18 * dpr));
        QCOMPARE(probeInk.left(), qRound((menu->actionGeometry(probe).left() + 12) * dpr));
        bool iconTextGapClear = true;
        const QRect probeRow = menu->actionGeometry(probe);
        const QColor cardColor = ThemeManager::instance().color(ds::ColorToken::MenuBg);
        for (int y = probeRow.top(); y <= probeRow.bottom(); ++y) {
            for (int x = probeRow.left() + 30; x <= probeRow.left() + 37; ++x) {
                if (menuPixel(QPoint(x, y)) != cardColor) iconTextGapClear = false;
            }
        }
        QVERIFY(iconTextGapClear);
        const QColor divider = ThemeManager::instance().color(ds::ColorToken::Divider);
        QCOMPARE(menuPixel(QPoint(card.left() + 1, separatorY)), divider);
        QCOMPARE(menuPixel(QPoint(card.right() - 1, separatorY)), divider);

        QWidget* dateHost = menu->findChild<QWidget*>(QStringLiteral("dateQuickBarWidget"));
        QWidget* priorityHost = menu->findChild<QWidget*>(QStringLiteral("priorityBarWidget"));
        QVERIFY(dateHost && priorityHost);
        QCOMPARE(dateHost->height(), 68);
        QCOMPARE(priorityHost->height(), 68);
        QLabel* dateTitle = dateHost->findChild<QLabel*>(QStringLiteral("QuickBarTitle"));
        QVERIFY(dateTitle);
        QCOMPARE(dateTitle->geometry(), QRect(12, 10, dateHost->width() - 24, 16));
        QVERIFY(qAbs(dateTitle->font().pointSizeF() - 9.0) < 0.01);
        QToolButton* today = dateHost->findChild<QToolButton*>(QStringLiteral("quick-today"));
        QToolButton* high = priorityHost->findChild<QToolButton*>(QStringLiteral("priority-3"));
        QToolButton* noPriority = priorityHost->findChild<QToolButton*>(QStringLiteral("priority-0"));
        QVERIFY(today && high && noPriority);
        QCOMPARE(today->size(), QSize(34, 34));
        QCOMPARE(today->iconSize(), QSize(22, 22));
        QCOMPARE(high->size(), QSize(32, 32));
        QCOMPARE(high->iconSize(), QSize(20, 20));
        QCOMPARE(today->geometry().y(), 32);
        QCOMPARE(high->geometry().y(), 32);
        QVERIFY(noPriority->isChecked());
        menu->close();
        delete menu;
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
        QVERIFY(qobject_cast<PopupMenu*>(menu));
        QVERIFY(menu->testAttribute(Qt::WA_TranslucentBackground));
        QVERIFY(menu->mask().isEmpty());
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

    void reminderPopupRoundedSnoozeMenuAndDragArea() {
        Entity task;
        task.id = newUuid();
        task.title = QStringLiteral("可拖动提醒");
        auto* popup = new ReminderPopup(task);
        const QPointer<ReminderPopup> guard(popup);
        popup->show();
        QVERIFY(QTest::qWaitForWindowExposed(popup));
        auto* snooze = popup->findChild<QToolButton*>(QStringLiteral("ReminderSnoozeButton"));
        auto* menu = popup->findChild<PopupMenu*>(QStringLiteral("ReminderSnoozeMenu"));
        QVERIFY(snooze && menu);
        bool menuSeen = false;
        QImage image;
        QRect card;
        QRect cardGlobal;
        QTimer poll;
        poll.setInterval(20);
        connect(&poll, &QTimer::timeout, popup, [&]() {
            if (!menu->isVisible()) return;
            poll.stop();
            menuSeen = true;
            cardGlobal = QRect(menu->pos() + QPoint(PopupShadowMargin, PopupShadowMargin),
                               menu->size() - QSize(2 * PopupShadowMargin, 2 * PopupShadowMargin));
            image = QImage(menu->size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            menu->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
            painter.end();
            card = menu->rect().adjusted(PopupShadowMargin, PopupShadowMargin,
                                         -PopupShadowMargin, -PopupShadowMargin);
            menu->hide();
        });
        poll.start();
        QTest::mouseClick(snooze, Qt::LeftButton);
        poll.stop();
        QVERIFY(menuSeen);
        QTRY_VERIFY(!menu->isVisible());
        const QRect buttonGlobal(snooze->mapToGlobal(QPoint(0, 0)), snooze->size());
        QCOMPARE(cardGlobal.left(), buttonGlobal.left());
        QCOMPARE(cardGlobal.bottom(), buttonGlobal.top() - 4);
        QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
        QVERIFY(image.pixelColor(card.topLeft()).alpha() < 128);
        QCOMPARE(image.pixelColor(card.center()).alpha(), 255);
        const QColor shadow = image.pixelColor(card.center().x(), card.bottom() + 8);
        QVERIFY(shadow.alpha() > 0 && shadow.alpha() < 128);

        auto* drag = popup->findChild<WindowDragArea*>(QStringLiteral("ReminderDragArea"));
        QVERIFY(drag && drag->isVisible());
        const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
        popup->move(available.topLeft() + QPoint(50, 50));
        QApplication::processEvents();
        const QPoint initial = popup->pos();
        const QPoint local(drag->width() - 50, drag->height() / 2);
        const QPoint pressGlobal = drag->mapToGlobal(local);
        QTest::mousePress(drag, Qt::LeftButton, Qt::NoModifier, local);
        const QPoint delta(42, 26);
        QMouseEvent move(QEvent::MouseMove, drag->mapFromGlobal(pressGlobal + delta),
                         pressGlobal + delta, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(drag, &move);
        QTest::mouseRelease(drag, Qt::LeftButton, Qt::NoModifier,
                            drag->mapFromGlobal(pressGlobal + delta));
        QCOMPARE(popup->pos(), initial + delta);
        QTest::mouseDClick(drag, Qt::LeftButton, Qt::NoModifier, local);
        QVERIFY(!popup->isMaximized());
        popup->close();
        QTRY_VERIFY(!guard);
    }

    void datePickerDayButtonsFitTwoDigitLabels() {
        DatePickerPopup popup;
        popup.setDateTime(fromAppTime(QDateTime(QDate(2026, 11, 14), QTime(9, 30))));
        popup.setHasDate(true);
        popup.show();
        QApplication::processEvents();
        const auto days = popup.findChildren<QToolButton*>(QStringLiteral("DatePickerDay"));
        QCOMPARE(days.size(), 42);
        const QColor background = ThemeManager::instance().color(ds::ColorToken::Surface);
        const auto renderButton = [background](QToolButton* button, qreal dpr) {
            QImage image(QSize(qRound(button->width() * dpr), qRound(button->height() * dpr)),
                         QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(dpr);
            image.fill(background);
            QPainter painter(&image);
            button->render(&painter);
            painter.end();
            return image;
        };
        int compared = 0;
        const QDate today = toAppTime(appNow()).date();
        for (QToolButton* day : days) {
            QCOMPARE(day->size(), QSize(30, 30));
            if (day->property("outsideMonth").toBool() || day->text().size() != 2) continue;
            day->ensurePolished();
            const int textWidth = day->fontMetrics().horizontalAdvance(day->text());
            QVERIFY2(textWidth <= day->width() - 2,
                     qPrintable(QStringLiteral("%1: text=%2, cell=%3")
                                    .arg(day->text()).arg(textWidth).arg(day->width())));
            if (day->isChecked() || day->property("date").toDate() == today
                || day->underMouse() || day->hasFocus() || day->isDown()) continue;

            // sizeHint 含平台样式的建议留白，不等于数字是否被固定格裁剪。
            // 用相同样式的加宽控件作真实 paint 参考，不加入布局、不显示到屏幕。
            QToolButton reference(day->parentWidget());
            reference.setObjectName(day->objectName());
            reference.setText(day->text());
            reference.setFont(day->font());
            reference.setPalette(day->palette());
            reference.setStyleSheet(day->styleSheet());
            reference.setCheckable(day->isCheckable());
            reference.setChecked(day->isChecked());
            reference.setAutoRaise(day->autoRaise());
            reference.setToolButtonStyle(day->toolButtonStyle());
            reference.setIcon(day->icon()); reference.setIconSize(day->iconSize());
            reference.setEnabled(day->isEnabled());
            for (const QByteArray& name : day->dynamicPropertyNames()) {
                if (!name.startsWith("_q_")) // Qt 的内部样式缓存不是业务动态属性。
                    reference.setProperty(name.constData(), day->property(name.constData()));
            }
            reference.resize(day->width() + 80, day->height());
            reference.ensurePolished();
            const qreal dpr = day->devicePixelRatioF();
            QCOMPARE(reference.devicePixelRatioF(), dpr);
            const qreal centerShift = 40.0 * dpr;
            QVERIFY(qAbs(centerShift - qRound(centerShift)) < 0.001);
            const QImage actual = renderButton(day, dpr);
            const QImage wider = renderButton(&reference, dpr);
            QVERIFY(!reference.isVisible());
            const QRect center(qRound(centerShift), 0, actual.width(), actual.height());
            QVERIFY(wider.rect().contains(center));
            const QImage expected = wider.copy(center);

            // 先在完整宽图定位数字，再比较其完整墨迹及 2px 抗锯齿边缘；
            // 不比较圆角背景边缘，也不能把超出原格的文字裁掉后当作参考。
            QRect ink;
            for (int y = 2; y < wider.height() - 2; ++y) {
                for (int x = 2; x < wider.width() - 2; ++x) {
                    const QColor pixel = wider.pixelColor(x, y);
                    const int contrast = qMax(qAbs(pixel.red() - background.red()),
                        qMax(qAbs(pixel.green() - background.green()), qAbs(pixel.blue() - background.blue())));
                    if (contrast > 32) ink = ink.united(QRect(x, y, 1, 1));
                }
            }
            QVERIFY2(!ink.isEmpty(), qPrintable(day->text()));
            QVERIFY2(center.contains(ink), qPrintable(QStringLiteral("%1 reference ink exceeds the 30px cell").arg(day->text())));
            const QRect textRegion = ink.translated(-center.left(), 0).adjusted(-2, -2, 2, 2)
                .intersected(actual.rect());
            QVERIFY2(actual.copy(textRegion) == expected.copy(textRegion),
                     qPrintable(QStringLiteral("%1: fixed cell clips or changes the rendered digits versus the wider reference")
                                    .arg(day->text())));
            ++compared;
        }
        QVERIFY2(compared >= 10, "Compare enough ordinary two-digit dates, not just the selected or hovered states");
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
        const Attachment imported = m_store->importAttachment(id, source);
        QVERIFY(!imported.id.isEmpty());
        QCOMPARE(m_store->attachments(id).size(), 1);
        NoteWidget note(m_store->entity(id));
        note.setStore(m_store.get());
        note.show();
        QApplication::processEvents();
        // 便签内嵌共享 TaskDetailWidget（NoteDetail）；附件是正文下方的 56px 内联卡片。
        auto* detail = note.findChild<TaskDetailWidget*>(QStringLiteral("NoteDetail"));
        QVERIFY(detail);
        QVERIFY(detail->findChild<MarkdownEditor*>());
        auto* attachments = detail->findChild<AttachmentWidget*>();
        QVERIFY(attachments);
        QTRY_VERIFY(detail->isVisible());
        QTRY_VERIFY(attachments->isVisible());
        // 行刷新是异步的：旧行先 hide+deleteLater，控件树里可能残留同名隐藏旧行。
        // 只认“当前可见且绑定刚导入附件ID”的行，而不是第一个同名子控件。
        QWidget* row = nullptr;
        QElapsedTimer bounded; bounded.start();
        while (!row && bounded.elapsed() < 5000) {
            for (QWidget* candidate : attachments->findChildren<QWidget*>(QStringLiteral("AttachmentRow")))
                if (candidate->isVisible() && candidate->property("attachmentId").toString() == imported.id) {
                    row = candidate;
                    break;
                }
            if (!row) QTest::qWait(10);
        }
        QVERIFY2(row, qPrintable(QStringLiteral(
            "没有可见且绑定 %1 的附件卡片：附件区可见=%2 面板可见=%3 附件区矩形=%4x%5 同名行数=%6")
            .arg(imported.id)
            .arg(attachments->isVisible())
            .arg(detail->findChild<QWidget*>(QStringLiteral("DetailAttachmentPane"))
                 ? detail->findChild<QWidget*>(QStringLiteral("DetailAttachmentPane"))->isVisible() : false)
            .arg(attachments->width()).arg(attachments->height())
            .arg(attachments->findChildren<QWidget*>(QStringLiteral("AttachmentRow")).size())));
        QTRY_COMPARE(row->height(), 56);
        // 内联卡片绑定到刚导入的附件本身与其真实类型（PNG → image）。
        QCOMPARE(row->property("inlineAttachment").toBool(), true);
        QCOMPARE(row->property("attachmentId").toString(), imported.id);
        QCOMPARE(row->property("attachmentType").toString(),
                 AttachmentAppearance::typeName(AttachmentAppearance::fileType(imported)));
        QCOMPARE(row->property("attachmentType").toString(), QStringLiteral("image"));
        auto* cardName = row->findChild<QLabel*>(QStringLiteral("AttachmentName"));
        QVERIFY(cardName);
        QCOMPARE(cardName->text(), QStringLiteral("sample.png"));
        auto* cardIcon = row->findChild<QLabel*>(QStringLiteral("AttachmentIcon"));
        QVERIFY(cardIcon);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        QVERIFY(!cardIcon->pixmap(Qt::ReturnByValue).isNull());
#else
        QVERIFY(cardIcon->pixmap() && !cardIcon->pixmap()->isNull());
#endif
        // 卡片有真实墨迹：类型图标是着色位图，卡片底色整面不透明，不是空占位。
        const QImage iconInk = cardIcon->grab().toImage();
        int iconPixels = 0;
        for (int y = 0; y < iconInk.height(); ++y)
            for (int x = 0; x < iconInk.width(); ++x)
                if (qAlpha(iconInk.pixel(x, y)) > 40) ++iconPixels;
        QVERIFY(iconPixels > 8);
        const QImage card = row->grab().toImage();
        int cardInk = 0;
        for (int y = 0; y < card.height(); ++y)
            for (int x = 0; x < card.width(); ++x)
                if (qAlpha(card.pixel(x, y)) > 200) ++cardInk;
        QVERIFY(cardInk * 2 > card.width() * card.height());

        // 旧 ResizeGrip 已被 8 个真实边缘命中区取代（与主窗口共用 WindowResizeArea）。
        QVERIFY(!note.findChild<ResizeGrip*>());
        const struct { const char* name; Qt::CursorShape cursor; } areas[] = {
            {"NoteResizeLeft", Qt::SizeHorCursor}, {"NoteResizeRight", Qt::SizeHorCursor},
            {"NoteResizeTop", Qt::SizeVerCursor}, {"NoteResizeBottom", Qt::SizeVerCursor},
            {"NoteResizeTopLeft", Qt::SizeFDiagCursor}, {"NoteResizeTopRight", Qt::SizeBDiagCursor},
            {"NoteResizeBottomLeft", Qt::SizeBDiagCursor}, {"NoteResizeBottomRight", Qt::SizeFDiagCursor}
        };
        for (const auto& definition : areas) {
            auto* area = note.findChild<QWidget*>(QString::fromLatin1(definition.name));
            QVERIFY2(area && area->isVisible(), definition.name);
            QCOMPARE(area->cursor().shape(), definition.cursor);
        }
        auto* bottomRight = note.findChild<WindowResizeArea*>(QStringLiteral("NoteResizeBottomRight"));
        QVERIFY(bottomRight);
        const QPoint handle(5, 5);
        // 该命中点确实落在右下角缩放区上（原生命中测试，不被纸面或边框遮挡）。
        QCOMPARE(note.childAt(bottomRight->mapTo(&note, handle)), static_cast<QWidget*>(bottomRight));
        note.raise(); note.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&note));
        if (QGuiApplication::platformName() != QLatin1String("offscreen")
            && QGuiApplication::platformName() != QLatin1String("minimal"))
            QCOMPARE(QApplication::widgetAt(bottomRight->mapToGlobal(handle)), static_cast<QWidget*>(bottomRight));
        const QRect start = note.geometry();
        const QPoint pressGlobal = bottomRight->mapToGlobal(handle);
        QTest::mousePress(bottomRight, Qt::LeftButton, Qt::NoModifier, handle);
        const QPoint grown = pressGlobal + QPoint(64, 36);
        QMouseEvent drag(QEvent::MouseMove, bottomRight->mapFromGlobal(grown), grown,
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(bottomRight, &drag);
        QApplication::processEvents();
        QTest::mouseRelease(bottomRight, Qt::LeftButton, Qt::NoModifier, bottomRight->mapFromGlobal(grown));
        QCOMPARE(note.width(), start.width() + 64);
        QCOMPARE(note.height(), start.height() + 36);
        // 拖到极小也只停在窗口最小尺寸：纸面最小 220x180 加两侧阴影边距。
        QTest::mousePress(bottomRight, Qt::LeftButton, Qt::NoModifier, handle);
        const QPoint shrunken = bottomRight->mapToGlobal(handle) + QPoint(-4000, -4000);
        QMouseEvent clamp(QEvent::MouseMove, bottomRight->mapFromGlobal(shrunken), shrunken,
                          Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(bottomRight, &clamp);
        QApplication::processEvents();
        QTest::mouseRelease(bottomRight, Qt::LeftButton, Qt::NoModifier, bottomRight->mapFromGlobal(shrunken));
        QCOMPARE(note.size(), note.minimumSize());
        QCOMPARE(note.size(),
                 NoteWidget::minimumPaperSize() + QSize(2 * PopupShadowMargin, 2 * PopupShadowMargin));
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
        auto* autostart = app.findChild<AutostartSettings*>();
        QVERIFY(autostart);
        QCOMPARE(autostart->mode(), AutostartSettings::Mode::Isolated);
        QVERIFY(!autostart->isAvailable());
        QCOMPARE(Autostart::arguments(), QStringList{QStringLiteral("--background")});
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

    void settingsCloseReturnsFromTopRightAndAutostartSharesPolicy() {
        bool registered = false;
        AutostartSettings autostart(AutostartSettings::Mode::Portable, AutostartSettings::Backend{
            [&registered]() { return registered; },
            [&registered](bool enabled, QString*) { registered = enabled; return true; }});
        const QString id = m_store->createTask(QStringLiteral("便签与主面板共享设置"));
        QVERIFY(m_store->saveDesktopState(id, NoteColor::Yellow, QRect(20, 20, 360, 320),
                                          0, NoteLayer::Normal, true));
        NoteManager notes(m_store.get()); notes.setAutostart(&autostart);
        QVERIFY(notes.restoreAll()); auto* note = notes.note(id); QVERIFY(note);
        note->show();
        TaskPanel panel(m_store.get()); panel.setAutostart(&autostart);
        panel.resize(1240, 820); panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        auto openSettings = [&panel, &autostart]() -> bool {
            auto* rail = panel.findChild<QToolButton*>(QStringLiteral("RailSettings"));
            if (!rail) return false;
            bool clicked = false;
            QString failure;
            QTimer::singleShot(0, &panel, [&panel, &autostart, &clicked, &failure]() {
                auto* dialog = qobject_cast<SettingsDialog*>(QApplication::activeModalWidget());
                if (!dialog) { failure = QStringLiteral("no active modal"); return; }
                auto* close = dialog->findChild<QToolButton*>(QStringLiteral("ModalClose"));
                auto* startup = dialog->findChild<QCheckBox*>(QStringLiteral("GeneralAutostart"));
                if (!close || !startup) { failure = QStringLiteral("missing widgets"); dialog->reject(); return; }
                if (!startup->isVisible()) { failure = QStringLiteral("switch hidden"); dialog->reject(); return; }
                if (!startup->isEnabled()) { failure = QStringLiteral("switch disabled"); dialog->reject(); return; }
                if (dialog->header()->width() != dialog->card()->width()) {
                    failure = QStringLiteral("header width %1 card %2")
                        .arg(dialog->header()->width()).arg(dialog->card()->width());
                    dialog->reject(); return;
                }
                if (dialog->card()->width() - close->geometry().right() - 1 != 10) {
                    failure = QStringLiteral("close inset");
                    dialog->reject(); return;
                }
                const QPoint center = close->rect().center();
                if (QGuiApplication::platformName() != QLatin1String("offscreen")
                    && QGuiApplication::platformName() != QLatin1String("minimal")) {
                    if (!dialog->isActiveWindow()) {
                        dialog->activateWindow();
                        if (!QTest::qWaitForWindowActive(dialog)) { failure = QStringLiteral("not active"); dialog->reject(); return; }
                    }
                    if (QApplication::widgetAt(close->mapToGlobal(center)) != close) {
                        failure = QStringLiteral("close not hit");
                        dialog->reject(); return;
                    }
                }
                startup->click();
                if (autostart.isEnabled() != startup->isChecked()) { failure = QStringLiteral("switch did not sync"); dialog->reject(); return; }
                QTest::mouseClick(close, Qt::LeftButton, Qt::NoModifier, center);
                clicked = !dialog->isVisible();
                if (!clicked) failure = QStringLiteral("close click did not close");
            });
            QTest::mouseClick(rail, Qt::LeftButton);
            if (!clicked) qWarning() << "openSettings failure:" << failure;
            return clicked;
        };
        QVERIFY(openSettings());
        QVERIFY(autostart.isEnabled());
        auto* more = note->findChild<QToolButton*>(QStringLiteral("DetailMore")); QVERIFY(more);
        bool openedFromNote = false;
        QTimer::singleShot(0, note, [note, &openedFromNote]() {
            auto* menu = qobject_cast<PopupMenu*>(QApplication::activePopupWidget());
            if (!menu) return;
            QAction* settingsAction = nullptr;
            for (QAction* action : menu->actions())
                if (action->menu() && action->text() == QStringLiteral("设置")) settingsAction = action;
            if (!settingsAction) { menu->close(); return; }
            QMenu* settingsMenu = settingsAction->menu();
            QTimer::singleShot(0, menu, [menu, settingsAction, &openedFromNote, note]() {
                menu->setActiveAction(settingsAction);
                QTest::keyClick(menu, Qt::Key_Right);
                QTRY_VERIFY_WITH_TIMEOUT(settingsAction->menu() && settingsAction->menu()->isVisible(), 2500);
                QTimer::singleShot(0, menu, [&openedFromNote, menu, settingsAction, note]() {
                    QMenu* submenu = settingsAction->menu();
                    if (!submenu || !submenu->isVisible()) { menu->close(); return; }
                    submenu->activateWindow();
                    QAction* appSettings = nullptr;
                    for (QAction* action : submenu->actions())
                        if (action->text() == QStringLiteral("应用设置…")) appSettings = action;
                    if (!appSettings) { submenu->close(); return; }
                    QTimer::singleShot(150, note, [&openedFromNote]() {
                        auto* dialog = qobject_cast<SettingsDialog*>(QApplication::activeModalWidget());
                        if (!dialog) {
                            for (QWidget* top : QApplication::topLevelWidgets()) {
                                auto* candidate = qobject_cast<SettingsDialog*>(top);
                                if (candidate && candidate->isVisible()) { dialog = candidate; break; }
                            }
                        }
                        if (!dialog) return;
                        auto* startup = dialog->findChild<QCheckBox*>(QStringLiteral("GeneralAutostart"));
                        auto* close = dialog->findChild<QToolButton*>(QStringLiteral("ModalClose"));
                        if (!startup || !close || !startup->isEnabled() || !startup->isChecked()
                            || dialog->header()->width() != dialog->card()->width()) {
                            dialog->reject(); return;
                        }
                        if (QGuiApplication::platformName() != QLatin1String("offscreen")
                            && QGuiApplication::platformName() != QLatin1String("minimal")) {
                            dialog->activateWindow();
                            if (!QTest::qWaitForWindowActive(dialog)
                                || QApplication::widgetAt(close->mapToGlobal(close->rect().center())) != close) {
                                dialog->reject(); return;
                            }
                        }
                        startup->click();
                        QTest::mouseClick(close, Qt::LeftButton);
                        openedFromNote = !dialog->isVisible() && !startup->isChecked();
                    });
                    emit appSettings->trigger();
                });
            });
        });
        QTest::mouseClick(more, Qt::LeftButton);
        QTRY_VERIFY(openedFromNote);
        QVERIFY(!autostart.isEnabled());

        bool failWrite = false;
        AutostartSettings failing(AutostartSettings::Mode::Portable, AutostartSettings::Backend{
            []() { return false; },
            [&failWrite](bool, QString* error) {
                if (error) *error = QStringLiteral("合成自启失败");
                return !failWrite;
            }});
        failWrite = true;
        SettingsDialog failureDialog(&failing); failureDialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&failureDialog));
        auto* startup = failureDialog.findChild<QCheckBox*>(QStringLiteral("GeneralAutostart"));
        auto* failure = failureDialog.findChild<QLabel*>(QStringLiteral("GeneralAutostartError"));
        QVERIFY(startup && failure); QVERIFY(startup->isEnabled());
        startup->click();
        QVERIFY(!startup->isChecked()); QVERIFY(failure->isVisible());
        QCOMPARE(failure->text(), QStringLiteral("合成自启失败"));
        failureDialog.close();

        AutostartSettings isolated(AutostartSettings::Mode::Isolated);
        SettingsDialog isolatedDialog(&isolated);
        QVERIFY(!isolatedDialog.findChild<QCheckBox*>(
            QStringLiteral("GeneralAutostart"))->isEnabled());

        // 开发预览：开关必须可用（用户可手动登记），提示明确说明这是预览；
        // 只有用户点击才写后端，不自动登记。
        bool previewRegistered = false;
        int previewWrites = 0;
        AutostartSettings preview(AutostartSettings::Mode::Preview, AutostartSettings::Backend{
            [&previewRegistered]() { return previewRegistered; },
            [&previewRegistered, &previewWrites](bool enabled, QString*) {
                previewRegistered = enabled;
                ++previewWrites;
                return true;
            }});
        SettingsDialog previewDialog(&preview);
        previewDialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&previewDialog));
        auto* previewStartup = previewDialog.findChild<QCheckBox*>(
            QStringLiteral("GeneralAutostart"));
        auto* previewHint = previewDialog.findChild<QLabel*>(
            QStringLiteral("GeneralAutostartHint"));
        QVERIFY(previewStartup && previewHint);
        QVERIFY(previewStartup->isEnabled());
        QVERIFY(!previewStartup->isChecked());
        QVERIFY(previewHint->text().contains(QStringLiteral("开发预览")));
        previewStartup->click();
        QVERIFY(previewRegistered);
        QCOMPARE(previewWrites, 1);
        previewDialog.close();
    }
    void trayMenuKeepsLockHiddenAndOpensTaskWidget() {
        TrayController tray;
        auto* icon = tray.findChild<QSystemTrayIcon*>();
        QVERIFY(icon);
        auto* menu = qobject_cast<PopupMenu*>(icon->contextMenu());
        QVERIFY(menu);
        QVERIFY(menu->property("entityMenu").toBool());
        QStringList visibleActions;
        for (QAction* action : menu->actions()) {
            if (action->isVisible() && !action->isSeparator()) visibleActions << action->text();
        }
        const QStringList expected{QStringLiteral("添加桌面小部件"),
            QStringLiteral("主窗口置顶"), QStringLiteral("退出")};
        QCOMPARE(visibleActions, expected);
        auto* lock = menu->findChild<QAction*>(QStringLiteral("TrayLockWidgets"));
        QVERIFY(lock && !lock->isVisible() && lock->shortcut().isEmpty());
        auto* submenu = menu->findChild<PopupMenu*>(QStringLiteral("TrayWidgetsMenu"));
        QVERIFY(submenu);
        QCOMPARE(submenu->actions().size(), 1);
        QCOMPARE(submenu->actions().first()->text(), QStringLiteral("任务"));
        QSignalSpy addSpy(&tray, &TrayController::addTaskWidgetRequested);
        menu->popup(QPoint(120, 120));
        QVERIFY(QTest::qWaitForWindowExposed(menu));
        const QString screenshots = QString::fromLocal8Bit(qgetenv("KYLINLIST_SCREENSHOT_DIR"));
        if (!screenshots.isEmpty()) {
            QVERIFY(QDir().mkpath(screenshots));
            const QImage source = menu->grab().toImage();
            QVERIFY(source.save(screenshots + QStringLiteral("/tray-widget-menu.png")));
            const QRect corner(0, 0, qRound(60 * menu->devicePixelRatioF()),
                                     qRound(60 * menu->devicePixelRatioF()));
            for (const auto theme : {ds::Theme::Light, ds::Theme::Dark}) {
                QImage detail(corner.size(), QImage::Format_ARGB32_Premultiplied);
                detail.fill(ds::color(ds::ColorToken::Bg, theme));
                QPainter painter(&detail);
                painter.drawImage(QPoint(), source, corner);
                painter.end();
                const QString name = theme == ds::Theme::Light ? QStringLiteral("light") : QStringLiteral("dark");
                QVERIFY(detail.scaled(detail.size() * 4, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                    .save(screenshots + QStringLiteral("/tray-menu-corner-%1.png").arg(name)));
            }
        }
        menu->setActiveAction(submenu->menuAction());
        QTest::keyClick(menu, Qt::Key_Right);
        QTRY_VERIFY(submenu->isVisible());
        QTest::mouseClick(submenu, Qt::LeftButton, Qt::NoModifier,
                          submenu->actionGeometry(submenu->actions().first()).center());
        QCOMPARE(addSpy.size(), 1);
        QVERIFY(!menu->isVisible());
    }

    void taskWidgetFiltersTasksAndPersistsOptions() {
        const QString inbox = m_store->inboxListId();
        const QString taskId = m_store->createTask(QStringLiteral("小部件已有任务"), inbox);
        const QString noteId = m_store->createNote(QStringLiteral("不应混入任务小部件"), inbox);
        QVERIFY(!taskId.isEmpty() && !noteId.isEmpty());
        TaskWidget widget(m_store.get());
        QCOMPARE(widget.scopeTitle(), QStringLiteral("收集箱"));
        QVERIFY(widget.filter().tasksOnly);
        QStringList ids;
        for (int row = 0; row < widget.model()->rowCount(); ++row) {
            const QString id = widget.model()->data(widget.model()->index(row, 0),
                                                    TaskListModel::IdRole).toString();
            if (!id.isEmpty()) ids << id;
        }
        QVERIFY(ids.contains(taskId));
        QVERIFY(!ids.contains(noteId));
        const QString screenshots = QString::fromLocal8Bit(qgetenv("KYLINLIST_SCREENSHOT_DIR"));
        const auto saveComposite = [&widget, &screenshots](const QString& name) {
            if (screenshots.isEmpty()) return;
            QVERIFY(QDir().mkpath(screenshots));
            const QImage source = widget.grab().toImage();
            QImage target(source.size(), QImage::Format_ARGB32_Premultiplied);
            target.fill(ds::color(ds::ColorToken::Bg, ds::Theme::Light));
            QPainter painter(&target);
            painter.drawImage(0, 0, source);
            painter.end();
            QVERIFY(target.scaled(target.width() * 2, target.height() * 2,
                                  Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .save(screenshots + QLatin1Char('/') + name));
        };
        widget.showWidget();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));
        saveComposite(QStringLiteral("task-widget-dark.png"));

        auto* quick = widget.findChild<QLineEdit*>(QStringLiteral("WidgetQuickAdd"));
        QVERIFY(quick);
        quick->setText(QStringLiteral("小部件快速添加"));
        QTest::keyClick(quick, Qt::Key_Return);
        bool created = false;
        for (const Entity& entity : m_store->entities()) {
            if (entity.title == QStringLiteral("小部件快速添加")) {
                created = entity.kind == EntityKind::Task && entity.listId == inbox;
            }
        }
        QVERIFY(created);
        auto* scope = widget.findChild<QToolButton*>(QStringLiteral("WidgetScope"));
        QVERIFY(scope);
        QTest::mouseClick(scope, Qt::LeftButton);
        auto* scopeMenu = widget.findChild<PopupMenu*>(QStringLiteral("TaskWidgetScopeMenu"));
        QVERIFY(scopeMenu && scopeMenu->isVisible());
        QAction* allTasks = nullptr;
        for (QAction* action : scopeMenu->actions()) {
            if (action->text() == QStringLiteral("全部任务")) allTasks = action;
        }
        QVERIFY(allTasks);
        QTest::mouseClick(scopeMenu, Qt::LeftButton, Qt::NoModifier,
                          scopeMenu->actionGeometry(allTasks).center());
        QCOMPARE(widget.filter().scope, ViewScope::AllLists);
        QVERIFY(!widget.model()->indexForTask(noteId).isValid());

        TaskListModel workspaceModel(m_store.get());
        QVERIFY(workspaceModel.indexForTask(noteId).isValid());
        Entity updated = m_store->entity(taskId);
        updated.title = QStringLiteral("主窗更新的标题");
        QVERIFY(m_store->saveTask(updated));
        QCOMPARE(widget.model()->indexForTask(taskId).data().toString(), updated.title);
        QVERIFY(widget.model()->setData(widget.model()->indexForTask(taskId), Qt::Checked,
                                        Qt::CheckStateRole));
        QVERIFY(m_store->entity(taskId).completed);
        ViewSpec workspaceFilter;
        workspaceFilter.showCompleted = true;
        workspaceModel.setFilter(workspaceFilter);
        QCOMPARE(workspaceModel.indexForTask(taskId).data(Qt::CheckStateRole).toInt(), int(Qt::Checked));

        widget.setShowDetails(true);
        widget.setCountdownDates(true);
        widget.setGrouping(GroupMode::None);
        widget.setAppearance(1);
        saveComposite(QStringLiteral("task-widget-light.png"));
        QVERIFY(widget.filter().showDetails && widget.filter().countdownDates);
        QCOMPARE(widget.filter().grouping, GroupMode::None);
        QCOMPARE(widget.appearance(), 1);
        QVERIFY(widget.findChild<TaskDelegate*>()->hasThemeOverride());
        widget.setLocked(true);
        QVERIFY(!widget.isLocked());
        auto* more = widget.findChild<QToolButton*>(QStringLiteral("WidgetMore"));
        QVERIFY(more);
        QTest::mouseClick(more, Qt::LeftButton);
        auto* moreMenu = widget.findChild<PopupMenu*>(QStringLiteral("TaskWidgetMoreMenu"));
        QTRY_VERIFY(moreMenu && moreMenu->isVisible());
        auto* lock = moreMenu->findChild<QAction*>(QStringLiteral("WidgetLock"));
        QVERIFY(lock && !lock->isVisible());
        QTest::keyClick(moreMenu, Qt::Key_Escape);
        QTRY_VERIFY(!QApplication::activePopupWidget());
        auto* edge = widget.findChild<QWidget*>(QStringLiteral("TaskWidgetResizeRight"));
        QVERIFY(edge && edge->isVisible());
        widget.setGeometry(QRect(70, 90, 420, 500));
        widget.showWidget();
        QVERIFY(widget.isOpen());
        QVERIFY(widget.flush());

        TaskWidget restored(m_store.get());
        QVERIFY(restored.isOpen());
        QVERIFY(!restored.isLocked());
        QCOMPARE(restored.appearance(), 1);
        QVERIFY(restored.filter().showDetails && restored.filter().countdownDates);
        QCOMPARE(restored.filter().grouping, GroupMode::None);
        QCOMPARE(restored.geometry(), widget.geometry());
        restored.closeWidget();
        TaskWidget closed(m_store.get());
        QVERIFY(!closed.isOpen());
    }

    void taskWidgetRestoresOnBackgroundAndTrayControlsPanel() {
        m_store.reset();
        QSettings previous(QSettings::IniFormat, QSettings::UserScope,
                           QStringLiteral("kylinlist"), QStringLiteral("task-widget"));
        previous.setValue(QStringLiteral("locked"), true);
        previous.sync();
        KylinListApp first; QString error; QVERIFY2(first.init(&error, true, true), qPrintable(error));
        auto* icon = first.trayController()->findChild<QSystemTrayIcon*>();
        QVERIFY(icon && icon->contextMenu());
        auto* menu = icon->contextMenu();
        auto* add = menu->findChild<QAction*>(QStringLiteral("TrayAddTaskWidget"));
        auto* topmost = menu->findChild<QAction*>(QStringLiteral("TrayTopmost"));
        QVERIFY(add && topmost);
        add->trigger();
        TaskWidget* widget = first.taskWidget();
        QVERIFY(widget);
        QTRY_VERIFY(widget->isOpen() && widget->isVisible());
        QVERIFY(!widget->isLocked());  // 隐藏功能后不应用旧锁定状态。
        add->trigger();
        QApplication::processEvents();
        QCOMPARE(first.taskWidget(), widget);
        widget->selectScope(ViewScope::AllLists);
        widget->setGeometry(QRect(90, 300, 410, 520));
        topmost->trigger();
        QVERIFY(first.panel()->alwaysOnTop());
        QVERIFY(first.panel()->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        const QRect expectedGeometry = widget->geometry();
        QVERIFY(first.handleSessionEnd());
        QVERIFY(widget->isVisible());
        first.shutdown();

        KylinListApp second; QVERIFY2(second.init(&error, true, true), qPrintable(error));
        TaskWidget* restored = second.taskWidget();
        QVERIFY(restored && restored->isOpen() && restored->isVisible());
        QVERIFY(!restored->isLocked());
        QCOMPARE(restored->filter().scope, ViewScope::AllLists);
        QCOMPARE(restored->geometry(), ScreenFit::fitPaper(expectedGeometry, PopupShadowMargin,
                                                            TaskWidget::minimumPaperSize()));
        QVERIFY(second.panel()->alwaysOnTop());
        restored->closeWidget();
        second.shutdown();

        KylinListApp third; QVERIFY2(third.init(&error, true, true), qPrintable(error));
        QVERIFY(!third.taskWidget()->isVisible());
        TaskPanel* panel = third.panel();
        panel->showTask();
        auto* settings = panel->findChild<QToolButton*>(QStringLiteral("RailSettings"));
        QVERIFY(settings);
        QTimer::singleShot(0, panel, []() {
            auto* dialog = qobject_cast<SettingsDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto* categories = dialog->findChild<QListWidget*>(QStringLiteral("SettingsCategories"));
            if (!categories) { dialog->reject(); return; }
            const auto widgetItems = categories->findItems(QStringLiteral("桌面小部件"), Qt::MatchExactly);
            if (widgetItems.size() != 1) { dialog->reject(); return; }
            categories->setCurrentItem(widgetItems.first());
            auto* button = dialog->findChild<QPushButton*>(QStringLiteral("SettingsAddTaskWidget"));
            if (button && button->isVisible()) QTest::mouseClick(button, Qt::LeftButton);
            else dialog->reject();
        });
        QTest::mouseClick(settings, Qt::LeftButton);
        QTRY_VERIFY(third.taskWidget()->isVisible());
        QCOMPARE(third.taskWidget()->filter().scope, ViewScope::AllLists);
        third.shutdown();
    }

    void desktopNotesSurviveSessionFlushAndColdBackgroundInit() {
        KylinListApp first; QString error; QVERIFY2(first.init(&error, false, true), qPrintable(error));
        auto* notes = first.findChild<NoteManager*>(); QVERIFY(notes);
        const QString openId = first.store()->createTask(QStringLiteral("重启仍打开"));
        const QString closedId = first.store()->createTask(QStringLiteral("主动关闭不恢复"));
        Entity openTask = first.store()->entity(openId); openTask.body = QStringLiteral("会话前正文");
        QVERIFY(first.store()->saveTask(openTask));
        QVERIFY(first.store()->saveDesktopState(openId, NoteColor::Blue, QRect(40, 60, 368, 296),
                                                0, NoteLayer::Normal, true));
        QVERIFY(first.store()->saveDesktopState(closedId, NoteColor::Pink, QRect(440, 60, 368, 296),
                                                0, NoteLayer::Normal, false));
        QCOMPARE(notes->count(), 1);
        auto* note = notes->note(openId); QVERIFY(note);
        QVERIFY(note->isVisible());
        auto* editor = note->detail()->findChild<MarkdownEditor*>(); QVERIFY(editor);
        auto* body = editor->textEditor(); note->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(note));
        QTest::mouseClick(body->viewport(), Qt::LeftButton, Qt::NoModifier,
                          body->viewport()->rect().center());
        QTRY_VERIFY(body->hasFocus());
        QTest::keyClick(body, Qt::Key_End, Qt::ControlModifier);
        QTest::keyClicks(body, QStringLiteral(" updated"));
        note->setPaperGeometry(QRect(60, 80, 380, 300));
        QVERIFY(first.handleSessionEnd()); // 会话提交复用同一保存路径，不发送关闭窗口事件。
        QVERIFY(!QGuiApplication::isFallbackSessionManagementEnabled());
        QVERIFY(note->isVisible());
        QCOMPARE(first.store()->entity(openId).body, QStringLiteral("会话前正文 updated"));
        QCOMPARE(first.store()->desktopNote(openId).open, true);
        QCOMPARE(first.store()->desktopNote(closedId).open, false);
        first.shutdown();
        KylinListApp second;
        QVERIFY2(second.init(&error, true, true), qPrintable(error));
        auto* restoredNotes = second.findChild<NoteManager*>(); QVERIFY(restoredNotes);
        QCOMPARE(restoredNotes->count(), 1);
        auto* restored = restoredNotes->note(openId); QVERIFY(restored);
        QVERIFY(restored->isVisible());
        QCOMPARE(restored->detail()->bodyDraft(), QStringLiteral("会话前正文 updated"));
        QVERIFY(!restoredNotes->note(closedId));
        const auto* panel = second.panel();
        if (QSystemTrayIcon::isSystemTrayAvailable()) QVERIFY(!panel->isVisible());
        QCOMPARE(restored->paperGeometry(), QRect(60, 80, 380, 300));
        second.shutdown();
    }
    void sessionMessageCommitsWithoutClosingDesktopNotes() {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() == QLatin1String("offscreen")
            || QGuiApplication::platformName() == QLatin1String("minimal"))
            QSKIP("真实 Qt 会话消息只在本进程未显示的原生 HWND 中验证");
        KylinListApp app; QString error; QVERIFY2(app.init(&error, true, true), qPrintable(error));
        const QString id = app.store()->createTask(QStringLiteral("原生会话保存"));
        QVERIFY(app.store()->saveDesktopState(id, NoteColor::Yellow, QRect(20, 30, 360, 320),
                                          0, NoteLayer::Normal, true));
        auto* notes = app.findChild<NoteManager*>(); auto* note = notes ? notes->note(id) : nullptr;
        QVERIFY(note); QVERIFY(note->isVisible());
        auto* title = note->findChild<QLineEdit*>(QStringLiteral("TaskTitle")); QVERIFY(title);
        note->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(note));
        QTest::mouseClick(title, Qt::LeftButton); QTRY_VERIFY(title->hasFocus());
        QTest::keyClick(title, Qt::Key_End);
        QTest::keyClicks(title, QStringLiteral(" draft"));
        QWidget probe; const HWND hwnd = reinterpret_cast<HWND>(probe.winId()); // Never shown.
        QVERIFY(hwnd);
        QPointer<NoteWidget> liveNote(note);
        const LRESULT committed = SendMessageW(hwnd, WM_QUERYENDSESSION, 0, 0);
        const LRESULT completed = SendMessageW(hwnd, WM_ENDSESSION, FALSE, 0);
        Q_UNUSED(completed)
        QCOMPARE(committed, LRESULT(1));
        QCOMPARE(app.store()->entity(id).title, QStringLiteral("原生会话保存 draft"));
        QCOMPARE(app.store()->desktopNote(id).open, true);
        QVERIFY(note->isVisible()); QVERIFY(!qGuiApp->isSavingSession());
        app.shutdown();
#else
        QSKIP("仅 Windows 原生会话消息探针");
#endif
    }
    void detailShortBodyKeepsAttachmentsInBottomPane() {
        const QString id = m_store->createTask(QStringLiteral("独立附件区"));
        Entity task = m_store->entity(id);
        task.body = QStringLiteral("A short body.\n- [ ] First item");
        QVERIFY(m_store->saveTask(task));
        const QString source = referenceImage(m_dir->path()); QVERIFY(!source.isEmpty());
        const Attachment attachment = m_store->importAttachment(id, source); QVERIFY(isUuid(attachment.id));
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 720); detail.show(); QVERIFY(detail.setTask(id));
        auto* scroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        auto* form = detail.findChild<QWidget*>(QStringLiteral("TaskDetailForm"));
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* attachments = detail.findChild<AttachmentWidget*>();
        auto* pane = detail.findChild<QWidget*>(QStringLiteral("DetailAttachmentPane"));
        auto* files = detail.findChild<QScrollArea*>(QStringLiteral("DetailAttachmentScroll"));
        auto* footer = detail.findChild<QFrame*>(QStringLiteral("DetailFooter"));
        QVERIFY(scroll && form && title && editor && attachments && pane && files && footer);
        QVERIFY(!detail.findChild<QListWidget*>(QStringLiteral("TaskChecklist")));
        QVERIFY(!detail.findChild<QLineEdit*>(QStringLiteral("ChecklistInput")));
        QCOMPARE(scroll->widget(), form);
        QVERIFY(form->isAncestorOf(title)); QVERIFY(form->isAncestorOf(editor));
        QVERIFY(!form->isAncestorOf(attachments)); QVERIFY(pane->isAncestorOf(attachments));
        QVERIFY(editor->fillAvailableHeight());
        QTRY_VERIFY(pane->isVisible()); QTRY_VERIFY(attachments->isVisible());
        QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
        QTRY_COMPARE(rectIn(pane, &detail).bottom() + 1, rectIn(footer, &detail).top());
        auto* row = attachments->findChild<QWidget*>(QStringLiteral("AttachmentRow")); QVERIFY(row);
        QTRY_VERIFY(files->viewport()->rect().contains(rectIn(row, files->viewport())));
        QVERIFY(editor->height() > editor->contentHeight());
        QCOMPARE(editor->content(), task.body);

        const QString empty = m_store->createTask(QStringLiteral("没有附件"));
        Entity shortTask = m_store->entity(empty); shortTask.body = QStringLiteral("A short body.");
        QVERIFY(m_store->saveTask(shortTask)); QVERIFY(detail.setTask(empty));
        QTRY_VERIFY(attachments->isHidden()); QTRY_VERIFY(pane->isHidden());
        QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
        QTRY_VERIFY(editor->height() > editor->contentHeight());

        QVERIFY(detail.setTask(id)); QTRY_VERIFY(pane->isVisible());
        const int withFiles = editor->height();
        QVERIFY(m_store->removeAttachment(id, attachment.id));
        QTRY_VERIFY(attachments->isHidden()); QTRY_VERIFY(pane->isHidden());
        QTRY_VERIFY(editor->height() > withFiles);
        QCOMPARE(editor->content(), task.body); QCOMPARE(m_store->entity(id).body, task.body);
    }

    void detailMarkdownTasksStayInBodyAndGrowWithoutInnerScroll() {
        const QString id = m_store->createTask(QStringLiteral("正文 Markdown 待办行"));
        Entity task = m_store->entity(id);
        task.body = QStringLiteral("- [ ] First item"); QVERIFY(m_store->saveTask(task));
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(id));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* scroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        QVERIFY(editor && scroll);
        QVERIFY(!detail.findChild<QWidget*>(QStringLiteral("TaskChecklist")));
        QVERIFY(!detail.findChild<QWidget*>(QStringLiteral("ChecklistInput")));
        QVERIFY(!detail.findChild<QWidget*>(QStringLiteral("AddChecklist")));
        QVERIFY(editor->plainTextContent().contains(QStringLiteral("First item")));
        QVERIFY(editor->preview()->document()->toPlainText().contains(QChar(0x2610)));
        const int oneLineHeight = editor->contentHeight();
        QStringList lines;
        for (int row = 1; row <= 30; ++row)
            lines.append(QStringLiteral("- [%1] Item %2").arg(row == 1 ? QLatin1Char('x') : QLatin1Char(' ')).arg(row));
        detail.activateWindow(); editor->textEditor()->setFocus(); QApplication::processEvents();
        editor->textEditor()->setPlainText(lines.join(QLatin1Char('\n')));
        QVERIFY(detail.finishEditing());
        QCOMPARE(m_store->entity(id).body, lines.join(QLatin1Char('\n')));
        QTRY_VERIFY(editor->contentHeight() > oneLineHeight * 5);
        QTRY_COMPARE(editor->textEditor()->verticalScrollBar()->maximum(), 0);
        QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
        QVERIFY(detail.setTask(QString())); QVERIFY(detail.setTask(id));
        QCOMPARE(editor->content(), lines.join(QLatin1Char('\n')));
        QVERIFY(editor->preview()->document()->toPlainText().contains(QChar(0x2611)));

        // 检查项只通过正文块格式操作，不创建独立的检查项控件或实体。
        auto* format = detail.findChild<QToolButton*>(QStringLiteral("FooterFormat")); QVERIFY(format);
        QTest::mouseClick(format, Qt::LeftButton);
        auto* panel = detail.findChild<QFrame*>(QStringLiteral("FormatPanel")); QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible());
        QVERIFY(!panel->findChild<QToolButton*>(QStringLiteral("FormatChecklist")));
        bool hasMarkdownTaskStyle = false;
        for (QAction* action : panel->findChildren<QAction*>())
            hasMarkdownTaskStyle |= action->text() == QStringLiteral("检查项");
        QVERIFY(hasMarkdownTaskStyle);
        panel->hide();
        const QString other = m_store->createTask(QStringLiteral("空正文的任务"));
        QVERIFY(detail.setTask(other)); QVERIFY(editor->content().isEmpty());
        QVERIFY(m_store->entity(other).body.isEmpty());
    }

    void detailHeaderCommandsPersistWithoutSteadySaveLabel() {
        const QString id = m_store->createTask(QStringLiteral("详情菜单操作"));
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(id));
        auto* header = detail.findChild<QFrame*>(QStringLiteral("DetailHeader"));
        auto* date = detail.findChild<QPushButton*>(QStringLiteral("DateChip"));
        auto* info = detail.findChild<QToolButton*>(QStringLiteral("DetailPriority"));
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* completed = detail.findChild<QCheckBox*>(QStringLiteral("TaskCompleted"));
        QVERIFY(header && date && info && title && completed);
        QVERIFY(!detail.findChild<QComboBox*>(QStringLiteral("TaskPriority")));
        QVERIFY(!detail.findChild<QPushButton*>(QStringLiteral("SaveTask")));
        QVERIFY(!header->isAncestorOf(title)); QVERIFY(header->isAncestorOf(date)); QVERIFY(header->isAncestorOf(info));
        QVERIFY(completed->isVisible());
        QVERIFY(date->isVisible()); QVERIFY(info->isVisible());
        QVERIFY(rectIn(info, header).left() > rectIn(date, header).right());
        for (QPushButton* button : header->findChildren<QPushButton*>())
            if (button->isVisible()) QCOMPARE(button, date);
        for (QToolButton* button : header->findChildren<QToolButton*>())
            if (button->isVisible())
                QVERIFY(button == info || button->objectName() == QLatin1String("DetailClose"));
        QVERIFY(!detail.findChild<QToolButton*>(QStringLiteral("DetailReminder")));
        for (QLineEdit* field : header->findChildren<QLineEdit*>()) QVERIFY(!field->isVisible());
        for (QComboBox* field : header->findChildren<QComboBox*>()) QVERIFY(!field->isVisible());
        for (QCheckBox* field : header->findChildren<QCheckBox*>())
            if (field != completed) QVERIFY(!field->isVisible());

        detail.activateWindow(); detail.focusTitle(); QTest::keyClicks(title, QStringLiteral("Menu keeps this title"));
        QVERIFY(triggerDetailCommand(detail, QStringLiteral("CmdTogglePin")));
        QCOMPARE(m_store->entity(id).title, QStringLiteral("Menu keeps this title"));
        QVERIFY(m_store->entity(id).pinnedAt.isValid());
        QTest::mouseClick(completed, Qt::LeftButton); QVERIFY(m_store->entity(id).completed);
        QVERIFY(triggerDetailCommand(detail, QStringLiteral("CmdReactivate")));
        QCOMPARE(m_store->entity(id).status, TaskStatus::Active);
        QVERIFY(!m_store->entity(id).completed);
        QVERIFY(triggerDetailCommand(detail, QStringLiteral("CmdTogglePin")));
        QVERIFY(!m_store->entity(id).pinnedAt.isValid()); QVERIFY(detail.flush());
        auto* status = detail.findChild<QLabel*>(QStringLiteral("DetailStatus"));
        QVERIFY(!status || !status->isVisible() || status->text().isEmpty());
        QVERIFY(detail.setTask(QString())); QVERIFY(detail.setTask(id));
        QCOMPARE(title->text(), QStringLiteral("Menu keeps this title"));
        QVERIFY(completed->isVisible());
        QVERIFY(!status || !status->isVisible() || status->text().isEmpty());
    }

    void detailDateChipUsesShanghaiDateAndActiveStatus() {
        const QDate today = toAppTime(appNow()).date();
        const QDate overdue = today.addDays(-3);
        const QString id = m_store->createTask(QStringLiteral("逾期日期展示"));
        Entity task = m_store->entity(id);
        task.dueAt = fromAppTime(QDateTime(overdue, QTime(0, 30), Qt::LocalTime));
        task.allDayDate = QDate();
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
        task.startAt = QDateTime();
        QVERIFY(m_store->saveTask(task));
        QTRY_VERIFY(date->text().contains(overdue.toString(QStringLiteral("yyyy/MM/dd"))));
        QVERIFY(date->text().contains(QStringLiteral("延期3天")));
        QVERIFY(!date->text().contains(QStringLiteral("11:45")));
        task = m_store->entity(id); task.startAt = QDateTime(); task.allDayDate = QDate();
        task.dueAt = fromAppTime(QDateTime(today.addDays(2), QTime(8, 30), Qt::LocalTime));
        QVERIFY(m_store->saveTask(task));
        QTRY_VERIFY(date->text().contains(today.addDays(2).toString(QStringLiteral("yyyy/MM/dd"))));
        QVERIFY(date->text().contains(QStringLiteral("08:30"))); QVERIFY(!date->text().contains(QStringLiteral("延期")));
    }

    void detailLongBodyScrollsAboveFixedAttachmentPaneAtNarrowWidth() {
        const QString id = m_store->createTask(QStringLiteral("标题与长正文一起滚动"));
        Entity task = m_store->entity(id);
        QStringList paragraphs;
        for (int line = 0; line < 60; ++line)
            paragraphs.append(QStringLiteral("Paragraph %1: a long line that wraps at narrow widths and remains editable.").arg(line));
        task.body = paragraphs.join(QLatin1Char('\n')) + QStringLiteral("\n- [ ] Last body item");
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
        const QRect attachmentBefore = rectIn(attachments, &detail);
        auto* fileScroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailAttachmentScroll"));
        QVERIFY(fileScroll); QVERIFY(!form->isAncestorOf(attachments));
        scroll->verticalScrollBar()->setValue(180); QApplication::processEvents();
        QCOMPARE(title->mapTo(scroll->viewport(), QPoint()).y(), titleBefore.y() - 180);
        QCOMPARE(editor->mapTo(scroll->viewport(), QPoint()).y(), editorBefore.y() - 180);
        QVERIFY(rectIn(title, scroll->viewport()).bottom() < 0);
        QCOMPARE(header->geometry(), headerBefore); QCOMPARE(footer->geometry(), footerBefore);
        QCOMPARE(rectIn(attachments, &detail), attachmentBefore);
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum()); QApplication::processEvents();
        auto* row = attachments->findChild<QWidget*>(QStringLiteral("AttachmentRow")); QVERIFY(row);
        QVERIFY(fileScroll->viewport()->rect().contains(rectIn(row, fileScroll->viewport())));
        QCOMPARE(rectIn(attachments, &detail), attachmentBefore);
        QCOMPARE(editor->content(), task.body);

        QSignalSpy heightChanges(editor, &MarkdownEditor::contentHeightChanged);
        editor->textEditor()->setPlainText(QStringLiteral("Short again."));
        QTRY_VERIFY(heightChanges.count() > 0);
        QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
        QTRY_COMPARE(editor->textEditor()->verticalScrollBar()->maximum(), 0);
        QVERIFY(editor->contentHeight() < 100); QVERIFY(editor->height() > editor->contentHeight()); QVERIFY(detail.flush());
        QCOMPARE(m_store->entity(id).body, QStringLiteral("Short again."));
    }

    void detailInfoPopupShowsRenderedLiveStatisticsAndEntityDates() {
        // 信息弹层现在归属笔记（任务头部按钮是优先级菜单）。
        const QString id = m_store->createNote(QStringLiteral("Two words"));
        Entity task = m_store->entity(id);
        task.body = QStringLiteral("**bold** <u>under</u> <mark data-color=\"yellow\">lit</mark> ")
            + QString::fromUtf8("\xF0\x9F\x98\x80");
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
        QCOMPARE(words->text(), QStringLiteral("5"));
        QCOMPARE(characters->text(), QStringLiteral("25")); // Only title/body: no markup, newline, filename or removed checklist.
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
        QCOMPARE(words->text(), QStringLiteral("3")); QCOMPARE(characters->text(), QStringLiteral("16"));
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

        auto* priority = detail.findChild<QToolButton*>(QStringLiteral("DetailPriority"));
        QVERIFY(priority);
        {
            // 优先级菜单经 exec() 展示并在返回时销毁：在事件循环内完成检查。
            bool seen = false;
            QElapsedTimer elapsed; elapsed.start();
            QTimer poll; poll.setInterval(5);
            QObject::connect(&poll, &QTimer::timeout, &poll, [&]() {
                auto* menu = detail.findChild<PopupMenu*>(QStringLiteral("PriorityMenu"));
                if (menu && menu->isVisible()) {
                    poll.stop(); seen = true;
                    verifyNativeOnly(menu);
                    menu->close();
                } else if (elapsed.elapsed() > 1500) {
                    poll.stop();
                    if (auto* popup = QApplication::activePopupWidget()) popup->close();
                }
            });
            poll.start();
            QTest::mouseClick(priority, Qt::LeftButton);
            while (poll.isActive() && elapsed.elapsed() < 1600) QTest::qWait(5);
            QVERIFY(seen);
        }

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
        auto* snoozeMenu = reminder.findChild<PopupMenu*>(QStringLiteral("ReminderSnoozeMenu"));
        QVERIFY(snooze && snoozeMenu);
        snoozeMenu->popup(snooze->mapToGlobal(QPoint(0, snooze->height())));
        QTRY_VERIFY(snoozeMenu->isVisible()); verify(snoozeMenu);
        snoozeMenu->hide();
        reminder.hide();

        ConfirmDialog confirm(QStringLiteral("确认"), QStringLiteral("正文"),
                              QStringLiteral("删除"), true);
        confirm.show(); verify(&confirm); confirm.reject();
        ListEditorDialog listEditor(m_store->lists(), m_store->folders());
        listEditor.show(); verify(&listEditor); listEditor.reject();
        ColorPickerPopup colorPicker; colorPicker.show(); verify(&colorPicker); colorPicker.reject();
        EmojiPickerPopup emojiPicker; emojiPicker.show(); verify(&emojiPicker); emojiPicker.reject();
        SettingsDialog settings(nullptr); settings.show(); verify(&settings); settings.reject();
    }

    void detailListSearchMovesByUuidAndPreservesEditingSession() {
        const QString sourceFolder = m_store->createFolder(QStringLiteral("SourceFolder"));
        const QString targetFolder = m_store->createFolder(QStringLiteral("TargetFolder"));
        const QString sourceList = m_store->createList(QStringLiteral("同名清单"), sourceFolder);
        const QString targetList = m_store->createList(QStringLiteral("同名清单"), targetFolder);
        const QString looseList = m_store->createList(QStringLiteral("根层清单"));
        const QString section = m_store->createSection(sourceList, QStringLiteral("原分段"));
        const QString id = m_store->createTask(QStringLiteral("待移动任务"), sourceList, section);
        QVERIFY(isUuid(id)); QVERIFY(isUuid(section));
        Entity task = m_store->entity(id);
        task.body = QStringLiteral("Original body\n- [ ] Keep my content");
        QVERIFY(m_store->saveTask(task));
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
        QVERIFY(title && editor && chip);
        QVERIFY(!detail.findChild<QListWidget*>(QStringLiteral("TaskChecklist")));
        QTest::mouseClick(chip, Qt::LeftButton);
        auto* popup = detail.findChild<ListSelectPopup*>(QStringLiteral("GroupSelectMenu")); QVERIFY(popup);
        QTRY_VERIFY(popup->isVisible());
        const int popupMargin = PopupMenu::shadowMargin();
        const QPoint initialChipTop = chip->mapToGlobal(QPoint(0, 0));
        QScreen* screen = QGuiApplication::screenAt(initialChipTop);
        if (!screen) screen = QGuiApplication::primaryScreen();
        QVERIFY(screen);
        const QRect available = screen->availableGeometry();
        const int cardHeight = popup->height() - 2 * popupMargin;
        popup->hide(); QTRY_VERIFY(!popup->isVisible());
        // 小屏或 offscreen 初始位置可能只能向下弹出；在编辑前为向上弹出预留空间。
        if (initialChipTop.y() - cardHeight - 8 < available.top() + popupMargin) {
            const int targetChipTop = available.top() + cardHeight + 8 + popupMargin;
            detail.move(detail.x(), detail.y() + targetChipTop - initialChipTop.y());
            QTRY_COMPARE(chip->mapToGlobal(QPoint(0, 0)).y(), targetChipTop);
        }
        detail.activateWindow(); detail.focusTitle(); QTest::keyClicks(title, QStringLiteral("Pending move title"));
        auto* body = editor->textEditor(); body->setFocus(); QApplication::processEvents(); QVERIFY(body->hasFocus());
        const QString pendingBody = QStringLiteral("**Pending body**\nAll lines must survive moving lists.\n- [ ] Keep my content");
        body->setPlainText(pendingBody);
        QTest::mouseClick(chip, Qt::LeftButton);
        QTRY_VERIFY(popup->isVisible());
        const QPoint chipTop = chip->mapToGlobal(QPoint(0, 0));
        const QRect popupCard = popup->geometry().adjusted(popupMargin, popupMargin,
                                                           -popupMargin, -popupMargin);
        QCOMPARE(popupCard.bottom(), chipTop.y() - 8 - 1);
        auto* search = popup->findChild<QLineEdit*>(QStringLiteral("ListSearch")); QVERIFY(search);
        auto* searchRow = popup->findChild<QWidget*>(QStringLiteral("ListSearchRow")); QVERIFY(searchRow);
        auto* searchIcon = popup->findChild<QLabel*>(QStringLiteral("ListSearchIcon")); QVERIFY(searchIcon);
        auto* searchAction = popup->findChild<QAction*>(QStringLiteral("listSearchAction")); QVERIFY(searchAction);
        auto* sourceMenu = popup->findChild<QMenu*>(QStringLiteral("listFolderMenu.") + sourceFolder);
        auto* targetMenu = popup->findChild<QMenu*>(QStringLiteral("listFolderMenu.") + targetFolder);
        QVERIFY(sourceMenu && targetMenu);
        QApplication::processEvents();
        QPointer<QAction> looseAction = popup->findChild<QAction*>(QStringLiteral("listDestination.") + looseList);
        QVERIFY(!looseAction.isNull());
        QVERIFY(looseAction->isVisible());
        const QRect ordinaryRow = popup->actionGeometry(looseAction.data());
        QCOMPARE(ordinaryRow.height(), 34);
        QCOMPARE(searchRow->geometry(), popup->actionGeometry(searchAction));
        QCOMPARE(searchRow->x(), ordinaryRow.x());
        QCOMPARE(searchRow->width(), ordinaryRow.width());
        QCOMPARE(searchIcon->size(), QSize(18, 18));
        QCOMPARE(search->height(), 18);
        QCOMPARE(searchIcon->y(), search->y());
        QCOMPARE(search->x() - (searchIcon->x() + searchIcon->width()), 8);
        // 独立 search-thin 图标与普通行图标共用同一全局 x 轴（卡片左缘 + 4 + 12）。
        const int searchIconGlobalX = searchIcon->mapToGlobal(QPoint(0, 0)).x();
        QCOMPARE(popup->mapToGlobal(ordinaryRow.topLeft()).x(), popupCard.left() + 4);
        QCOMPARE(searchIconGlobalX, popup->mapToGlobal(ordinaryRow.topLeft()).x() + 12);
        QVERIFY(searchIcon->pixmap());
        QCOMPARE(searchIcon->pixmap()->toImage(),
                 AppIcons::icon(QStringLiteral("search-thin"), ds::ColorToken::TextSecondary, 18)
                     .pixmap(18, 18).toImage());
        QVERIFY(!popup->findChild<QTreeWidget*>());  // 旧树形选择器不复活

        popup->setActiveAction(nullptr); QApplication::processEvents();
        const QImage menuImage = popup->grab().toImage();
        const qreal menuDpr = menuImage.devicePixelRatio();
        const auto rowInk = [&menuImage, menuDpr](const QRect& logical) {
            QRect bounds;
            const int x0 = qMax(0, qFloor(logical.left() * menuDpr));
            const int x1 = qMin(menuImage.width(), qCeil((logical.right() + 1) * menuDpr));
            const int y0 = qMax(0, qFloor(logical.top() * menuDpr));
            const int y1 = qMin(menuImage.height(), qCeil((logical.bottom() + 1) * menuDpr));
            for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
                const QColor pixel = menuImage.pixelColor(x, y);
                if (pixel.alpha() > 40 && qGray(pixel.red(), pixel.green(), pixel.blue()) < 235)
                    bounds = bounds.united(QRect(x, y, 1, 1));
            }
            return bounds;
        };
        const QRect rowIconInk = rowInk(QRect(ordinaryRow.left() + 12, ordinaryRow.top(), 18, 34));
        const QRect rowLabelInk = rowInk(QRect(ordinaryRow.left() + 38, ordinaryRow.top(),
                                               ordinaryRow.width() - 50, 34));
        QVERIFY(!rowIconInk.isEmpty() && !rowLabelInk.isEmpty());
        const qreal rowIconMid = (rowIconInk.top() + rowIconInk.bottom()) / 2.0 / menuDpr;
        const qreal rowLabelMid = (rowLabelInk.top() + rowLabelInk.bottom()) / 2.0 / menuDpr;
        QVERIFY2(qAbs(rowIconMid - rowLabelMid) <= 1.0,
                 qPrintable(QStringLiteral("清单菜单图标中线 %1，文字中线 %2").arg(rowIconMid).arg(rowLabelMid)));
        QVERIFY2(rowLabelMid >= rowIconMid - 1.0, "清单菜单文字不得高于图标");

        // 根层 = 文件夹 QMenu 子菜单 + 平铺清单；当前清单带选中勾。
        QVERIFY(sourceMenu->actions().size() >= 1);
        QCOMPARE(sourceMenu->actions().first()->objectName(), QStringLiteral("listDestination.") + sourceList);
        QCOMPARE(sourceMenu->actions().first()->property("targetListId").toString(), sourceList);
        QVERIFY(sourceMenu->actions().first()->isCheckable());
        QVERIFY(sourceMenu->actions().first()->isChecked());   // 选中勾
        QVERIFY(!targetMenu->actions().first()->isChecked());
        auto* flatAction = popup->findChild<QAction*>(QStringLiteral("listDestination.search.") + sourceList);
        QVERIFY(flatAction); QVERIFY(flatAction->isCheckable()); QVERIFY(flatAction->isChecked());

        // 无匹配：搜索结果隐藏，显示“没有匹配清单”，不写库。
        search->setText(QStringLiteral("no matching list"));
        QApplication::processEvents();
        QVERIFY(!flatAction->isVisible());
        QVERIFY(!sourceMenu->menuAction()->isVisible());
        auto* empty = popup->findChild<QAction*>(QStringLiteral("listDestinationEmpty"));
        QVERIFY(empty && empty->isVisible());
        QCOMPARE(m_store->entity(id).listId, sourceList); QCOMPARE(m_store->entity(id).sectionId, section);

        // 搜索命中：平铺路径“清单 — 文件夹”；Enter 触发首个可见结果完成移动。
        search->clear(); QTest::keyClicks(search, QStringLiteral("TargetFolder"));
        QApplication::processEvents();
        auto* targetSearch = popup->findChild<QAction*>(QStringLiteral("listDestination.search.") + targetList);
        QVERIFY(targetSearch && targetSearch->isVisible());
        QCOMPARE(targetSearch->text(), QStringLiteral("同名清单 — TargetFolder"));
        QVERIFY(!flatAction->isVisible());
        QTest::keyClick(search, Qt::Key_Return);
        QTRY_VERIFY(!popup->isVisible());
        QCOMPARE(m_store->entity(id).listId, targetList); QVERIFY(m_store->entity(id).sectionId.isEmpty());
        QCOMPARE(m_store->entity(id).title, QStringLiteral("Pending move title"));
        QCOMPARE(m_store->entity(id).body, pendingBody); QCOMPARE(editor->content(), pendingBody);
        const QVector<EntityEvent> edits = bodyEvents(); QCOMPARE(edits.size(), beforeBodyEvents + 1);
        // Text history stores a full new snapshot; the edit-session baseline is used only to detect a change.
        QVERIFY(!edits.last().oldValue.isValid()); QCOMPARE(edits.last().newValue.toString(), pendingBody);
        QVERIFY(detail.finishEditing());
        TaskStore reloaded(Database::instance().connection()); QVERIFY(reloaded.load());
        const Entity moved = reloaded.entity(id);
        QCOMPARE(moved.listId, targetList); QVERIFY(moved.sectionId.isEmpty());
        QCOMPARE(moved.title, QStringLiteral("Pending move title")); QCOMPARE(moved.body, pendingBody);
        QVERIFY(detail.setTask(QString())); QVERIFY(detail.setTask(id));
        QCOMPARE(title->text(), moved.title); QCOMPARE(editor->content(), pendingBody);
    }

    void legacyMarkdownBodyUsesBodySnapshotEvents() {
        const QString id = m_store->createTask(QStringLiteral("旧正文日志"));
        TaskDetailWidget detail(m_store.get()); detail.resize(460, 640); detail.show(); QVERIFY(detail.setTask(id));
        auto* editor = detail.findChild<MarkdownEditor*>(); QVERIFY(editor);
        auto* body = editor->textEditor(); detail.activateWindow(); body->setFocus(); QApplication::processEvents();
        const QString unchecked = QStringLiteral("- [ ] new item");
        const QString checked = QStringLiteral("- [x] new item");
        body->setPlainText(unchecked); QVERIFY(detail.finishEditing());
        QCOMPARE(m_store->entity(id).body, unchecked);
        body->setFocus(); QApplication::processEvents(); body->setPlainText(checked);
        QVERIFY(detail.finishEditing()); QCOMPARE(m_store->entity(id).body, checked);
        QStringList snapshots;
        for (const EntityEvent& event : m_store->events(id)) {
            QVERIFY(!event.field.startsWith(QStringLiteral("checklist.")));
            if (event.field != QStringLiteral("body")) continue;
            const QString snapshot = event.newValue.toString();
            if (snapshot == checked) QVERIFY(!event.oldValue.isValid());
            snapshots.append(snapshot);
        }
        QCOMPARE(snapshots.size(), 2);
        QVERIFY(snapshots.contains(unchecked));
        QVERIFY(snapshots.contains(checked));
        QVERIFY(detail.setTask(QString())); QVERIFY(detail.setTask(id));
        QCOMPARE(editor->content(), checked);
        TaskStore reloaded(Database::instance().connection()); QVERIFY(reloaded.load());
        QCOMPARE(reloaded.entity(id).body, checked);
    }

    void detailActivitySessionBoundariesAndInlineTimeline() {
        const auto withField = [this](const QString& id, const QString& field) {
            QVector<EntityEvent> result;
            for (const EntityEvent& event : m_store->events(id))
                if (event.field == field) result.append(event);
            return result;
        };
        const QString id = m_store->createTask(QStringLiteral("UI活动"));
        TaskDetailWidget detail(m_store.get()); detail.resize(800, 700); detail.show(); QVERIFY(detail.setTask(id));
        QLineEdit focusSink(&detail); focusSink.show();
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* body = detail.findChild<QTextEdit*>(QStringLiteral("DescriptionEditor"));
        auto* timeline = detail.findChild<ActivityTimelineWidget*>(QStringLiteral("ActivityTimeline"));
        QVERIFY(title && body && timeline); QVERIFY(!timeline->isVisible());
        detail.activateWindow(); detail.focusTitle(); QApplication::processEvents(); QVERIFY(title->hasFocus());
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

        const QString other = m_store->createTask(QStringLiteral("切换目标"));
        title->setFocus(); QApplication::processEvents(); QTest::keyClick(title, Qt::Key_X);
        const int beforeSwitch = withField(id, QStringLiteral("title")).size();
        QVERIFY(detail.setTask(other)); QCOMPARE(withField(id, QStringLiteral("title")).size(), beforeSwitch + 1);
        QCOMPARE(detail.entityId(), other); QVERIFY(detail.setTask(QString()));

        Entity scalars = m_store->entity(id); scalars.priority = 3;
        scalars.tags = QStringList{QStringLiteral("活动"), QStringLiteral("测试")}; QVERIFY(m_store->saveTask(scalars));
        QVERIFY(m_store->convertKind(id, EntityKind::Note)); QVERIFY(detail.setTask(id));
        QVERIFY(triggerDetailCommand(detail, QStringLiteral("CmdViewActivity")));
        QTRY_VERIFY(timeline->isVisible()); QVERIFY(!timeline->isWindow());
        const auto summaries = [timeline]() {
            QStringList values;
            for (QLabel* label : timeline->findChildren<QLabel*>(QStringLiteral("ActivityTimelineRowSummary")))
                values.append(label->text());
            return values;
        };
        QVERIFY(summaries().contains(QStringLiteral("修改了正文")));
        QVERIFY(summaries().contains(QStringLiteral("转换为了笔记")));
        QVERIFY(!summaries().join(QLatin1Char('\n')).contains(fullBody)); // 行内只摘要，日志仍存完整快照。
        QVERIFY(!detail.findChild<QWidget*>(QStringLiteral("ActivityLogDialog")));
        const int openRows = summaries().size();
        scalars = m_store->entity(id); scalars.title = QStringLiteral("打开后新修改"); QVERIFY(m_store->saveTask(scalars));
        QTRY_COMPARE(summaries().size(), openRows + 1);
        QVERIFY(summaries().contains(QStringLiteral("将标题修改为「打开后新修改」")));
        timeline->refresh(); QCOMPARE(summaries().size(), openRows + 1); // 手动 refresh 不重复行。
        auto* close = timeline->findChild<QToolButton*>(QStringLiteral("ActivityTimelineClose")); QVERIFY(close);
        QSignalSpy closed(timeline, &ActivityTimelineWidget::closeRequested);
        QTest::mouseClick(close, Qt::LeftButton); QCOMPARE(closed.count(), 1); QTRY_VERIFY(!timeline->isVisible());
        scalars = m_store->entity(id); scalars.title = QStringLiteral("收起期间修改"); QVERIFY(m_store->saveTask(scalars));
        QVERIFY(detail.showActivity()); QTRY_VERIFY(timeline->isVisible());
        QVERIFY(summaries().contains(QStringLiteral("将标题修改为「收起期间修改」")));
        QVERIFY(detail.setTask(other)); QVERIFY(!timeline->isVisible());
        QVERIFY(detail.showActivity()); QTRY_VERIFY(timeline->isVisible());
        QVERIFY(!summaries().contains(QStringLiteral("转换为了笔记")));
        QCOMPARE(summaries().size(), 1); // 换实体后只显示目标的创建记录。
        QVERIFY(detail.setTask(QString())); QVERIFY(!timeline->isVisible()); QVERIFY(summaries().isEmpty());
        QVERIFY(!detail.showActivity());
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
        editor->textEditor()->setPlainText(autosaved);
        // 共享文本会话防抖为 450ms：有界等待真实落库，而不是与固定 380ms 竞速。
        QTRY_COMPARE(m_store->entity(id).body, autosaved);
        QCOMPARE(bodyEvents(id).size(), 0);
        QVERIFY(notes.flushAll()); QVector<EntityEvent> events = bodyEvents(id); QCOMPARE(events.size(), 1);
        QCOMPARE(events.first().newValue.toString(), autosaved); QVERIFY(!events.first().oldValue.isValid());
        const QString autosaveEventId = events.first().id;
        QVERIFY(m_store->desktopNote(id).open); QCOMPARE(notes.count(), 1);

        QLineEdit focusSink; focusSink.show(); focusSink.setFocus(); QApplication::processEvents(); note->focusEditor(); QApplication::processEvents();
        const QString finalText = QStringLiteral("关闭便签前的最后一条全文"); editor->textEditor()->setPlainText(finalText);
        auto* close = note->findChild<QToolButton*>(QStringLiteral("CloseButton")); QVERIFY(close);
        QTest::mouseClick(close, Qt::LeftButton); QApplication::processEvents();
        events = bodyEvents(id);
        QCOMPARE(events.size(), 2);
        // 同一毫秒内按 ID 排序，关闭时新增的记录可能排在前面。
        const bool firstIsAutosave = events.first().id == autosaveEventId;
        QVERIFY(firstIsAutosave || events.last().id == autosaveEventId);
        const EntityEvent& closeEvent = firstIsAutosave ? events.last() : events.first();
        QCOMPARE(closeEvent.newValue.toString(), finalText);
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
        QTRY_COMPARE(model->filter().grouping, GroupMode::Time);
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
        task.hasScheduleRange = true;
        task.reminderAt = task.dueAt.addSecs(-600);
        task.priority = 1;
        task.body = QStringLiteral(
            "## 本周要点\n\n整理 **完成情况**，<u>记录需要协调的事项</u>。\n\n"
            "- 汇总本周进展\n- 确认下周安排\n\n"
            "> <mark data-color=\"yellow\">把下一步写清楚</mark>，让事情更容易推进。");
        task.body += QStringLiteral("\n\n- [x] 汇总材料\n- [ ] 核对时间表");
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
            QVERIFY(closeDetail && !closeDetail->isVisible());
            QVERIFY(detail->setTask(QString()));
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

            const QString infoNote = m_store->createNote(QStringLiteral("笔记信息"));
            QVERIFY(detail->setTask(infoNote));
            auto* info = detail->findChild<QToolButton*>(QStringLiteral("DetailInfo")); QVERIFY(info);
            QTest::mouseClick(info, Qt::LeftButton);
            auto* infoPopup = detail->findChild<EntityInfoPopup*>(QStringLiteral("EntityInfoPopup")); QVERIFY(infoPopup);
            QTRY_VERIFY(infoPopup->isVisible());
            QVERIFY(nativePopupShadowDisabled(infoPopup));
            QVERIFY(savePopupPreview(detail, infoPopup, dir + QStringLiteral("/detail-info.png")));
            infoPopup->hide();
            QVERIFY(detail->setTask(id));
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
            auto* timeExpansion = datePopup->findChild<QWidget*>(QStringLiteral("DatePickerTimeExpansion"));
            QVERIFY(timeExpansion); QTRY_VERIFY(timeExpansion->isVisible());
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

            SettingsDialog settings(nullptr, &panel); settings.show(); QTest::qWait(50);
            QVERIFY(settings.grab().save(dir + QStringLiteral("/settings-appearance-light.png")));
            ThemeManager::instance().setPreference(ThemeManager::Preference::Dark);
            QApplication::processEvents();
            QVERIFY(settings.grab().save(dir + QStringLiteral("/settings-appearance-dark.png")));
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
