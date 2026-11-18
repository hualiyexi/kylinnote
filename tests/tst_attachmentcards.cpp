#include <cstdio>
#include <functional>
#include <memory>
#include <utility>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QWidgetAction>
#include <QtTest>

#include "app/TaskStore.h"
#include "db/Database.h"
#include "platform/SystemShadow.h"
#include "ui/AppIcons.h"
#include "ui/AppStyle.h"
#include "ui/AttachmentAppearance.h"
#include "ui/AttachmentWidget.h"
#include "ui/ModalDialog.h"
#include "ui/PopupMenu.h"
#include "ui/design/ThemeManager.h"
#include "ui/design/TypeSystem.h"

using namespace kl;

// 全程只捕获测试进程的 file URL；即使测试回归，也不能启动真实文件关联程序。
class AttachmentFileUrlTrap final : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void capture(const QUrl& url) { urls.append(url); }
};

namespace {
QRect rectIn(const QWidget* widget, const QWidget* ancestor) {
    return QRect(widget->mapTo(ancestor, QPoint()), widget->size());
}

QPixmap labelPixmap(const QLabel* label) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    return label->pixmap(Qt::ReturnByValue);
#else
    return label->pixmap() ? *label->pixmap() : QPixmap();
#endif
}

QByteArray fileBytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.flush();
}

QAbstractButton* rowFor(AttachmentWidget& cards, const QString& attachmentId) {
    for (auto* row : cards.findChildren<QAbstractButton*>(QStringLiteral("AttachmentRow")))
        if (!row->isHidden() && row->property("attachmentId").toString() == attachmentId) return row;
    return nullptr;
}

PopupMenu* menuFor(QAbstractButton* row) {
    auto* button = row ? row->findChild<QToolButton*>(QStringLiteral("AttachmentActions")) : nullptr;
    return button ? qobject_cast<PopupMenu*>(button->menu()) : nullptr;
}

bool clickActualChild(QWidget& window, const QPoint& point, Qt::MouseButton button = Qt::LeftButton) {
    QWidget* hit = window.childAt(point);
    if (!hit) hit = &window;
    QTest::mouseClick(hit, button, Qt::NoModifier, hit->mapFrom(&window, point));
    return true;
}

// Qt 5.12 的 Home 在不同平台样式下并不等价；只走标准 Down / Return。
bool keyboardChoose(QMenu* menu, const QString& objectName) {
    if (!menu || !menu->isVisible()) return false;
    QList<QAction*> selectable;
    for (QAction* action : menu->actions())
        if (!action->isSeparator() && action->isVisible() && action->isEnabled()) selectable.append(action);
    int target = -1;
    for (int index = 0; index < selectable.size(); ++index)
        if (selectable.at(index)->objectName() == objectName) target = index;
    if (target < 0) return false;
    menu->setActiveAction(nullptr);
    for (int index = 0; index <= target; ++index) QTest::keyClick(menu, Qt::Key_Down);
    if (menu->activeAction() != selectable.at(target)) return false;
    QTest::keyClick(menu, Qt::Key_Return);
    return true;
}

enum class MenuEntry { Ellipsis, MouseContext, KeyboardContext };

// 同时支持 exec() 和 popup()；所有断言都留在 timer 回调之外，超时必关嵌套菜单。
bool withAttachmentMenu(QAbstractButton* row, MenuEntry entry,
                        const std::function<void(PopupMenu*)>& inspect) {
    auto* button = row ? row->findChild<QToolButton*>(QStringLiteral("AttachmentActions")) : nullptr;
    QPointer<PopupMenu> expected(menuFor(row));
    if (!button || !expected || !button->isVisible() || !button->isEnabled()) return false;
    bool handled = false;
    QElapsedTimer elapsed; elapsed.start();
    QTimer poll; poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, &poll, [&]() {
        if (expected && expected->isVisible()) {
            poll.stop(); handled = true;
            inspect(expected.data());
            if (expected && expected->isVisible()) expected->close();
        } else if (elapsed.elapsed() > 2500) {
            poll.stop();
            if (QWidget* popup = QApplication::activePopupWidget()) popup->close();
        }
    });
    poll.start();
    if (entry == MenuEntry::Ellipsis) {
        // 30px 命中区的边角必须有效，不只测 18px 图像中心。
        QTest::mouseClick(button, Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
    } else {
        const QPoint point = row->rect().center();
        if (entry == MenuEntry::MouseContext) QTest::mouseClick(row, Qt::RightButton, Qt::NoModifier, point);
        else QTest::keyClick(row, Qt::Key_Menu);
        // QTest 的合成鼠标/键盘事件在部分 Qt 平台不会自动生成 context-menu 事件。
        // 仍走 QWidget 标准上下文事件入口，不调用菜单信号或 QAction 冒充右键。
        if (!handled && expected && !expected->isVisible()) {
            QContextMenuEvent event(entry == MenuEntry::MouseContext ? QContextMenuEvent::Mouse
                                                                    : QContextMenuEvent::Keyboard,
                                    point, row->mapToGlobal(point));
            QApplication::sendEvent(row, &event);
        }
    }
    while (poll.isActive() && elapsed.elapsed() < 2700) QTest::qWait(5);
    return handled;
}

bool chooseAttachmentAction(QAbstractButton* row, const QString& actionName) {
    bool chosen = false;
    const bool opened = withAttachmentMenu(row, MenuEntry::Ellipsis,
        [&](PopupMenu* menu) { chosen = keyboardChoose(menu, actionName); });
    return opened && chosen;
}

// 真实模态框探针。既不吞 Show，也不伪造业务结果；缺控件/错误模态时仍有 watchdog。
class ModalDriver final {
public:
    explicit ModalDriver(std::function<bool(QDialog*)> inspect) : m_inspect(std::move(inspect)) {
        m_elapsed.start(); m_poll.setInterval(5);
        QObject::connect(&m_poll, &QTimer::timeout, &m_poll, [this]() {
            if (m_inCallback) return;
            if (m_elapsed.elapsed() > 3000) {
                timedOut = true;
                if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
                if (QWidget* popup = QApplication::activePopupWidget()) popup->close();
                return;
            }
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog || m_seen.contains(dialog)) return;
            m_seen.append(dialog); m_inCallback = true;
            if (!m_inspect(dialog)) { ++unexpectedDialogs; dialog->reject(); }
            m_inCallback = false;
        });
        m_poll.start();
    }
    ~ModalDriver() { stop(); }
    void stop() { m_poll.stop(); }
    bool timedOut = false;
    int unexpectedDialogs = 0;
private:
    std::function<bool(QDialog*)> m_inspect;
    QTimer m_poll;
    QElapsedTimer m_elapsed;
    QList<QPointer<QDialog>> m_seen;
    bool m_inCallback = false;
};

class NonNativeFileDialogs final {
public:
    NonNativeFileDialogs() : m_previous(QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs)) {
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    }
    ~NonNativeFileDialogs() { QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, m_previous); }
private:
    bool m_previous;
};

bool dismissAttachmentError(QDialog* dialog, QString* title, QString* message, bool* plainText) {
    if (dialog->objectName() != QLatin1String("AttachmentErrorDialog")) return false;
    auto* text = dialog->findChild<QLabel*>(QStringLiteral("AttachmentErrorText"));
    auto* accept = dialog->findChild<QPushButton*>(QStringLiteral("AttachmentErrorAccept"));
    if (title) *title = dialog->windowTitle();
    if (message) *message = text ? text->text() : QString();
    if (plainText) *plainText = text && text->textFormat() == Qt::PlainText;
    if (accept) QTest::mouseClick(accept, Qt::LeftButton);
    else dialog->reject();
    return true;
}
} // namespace

class AttachmentCardsTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        m_hadDataDir = qEnvironmentVariableIsSet("KYLINLIST_DATA_DIR");
        m_previousDataDir = qgetenv("KYLINLIST_DATA_DIR");
        m_previousSettingsFormat = QSettings::defaultFormat();
        m_settingsDir.reset(new QTemporaryDir); QVERIFY(m_settingsDir->isValid());
        qputenv("KYLINLIST_DATA_DIR", m_settingsDir->path().toUtf8());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir->path());
        AppStyle::apply(*qobject_cast<QApplication*>(qApp));
        disableNativeWindowShadows();
        QDesktopServices::setUrlHandler(QStringLiteral("file"), &m_fileUrls, "capture");
    }
    void init() {
        m_dir.reset(new QTemporaryDir); QVERIFY(m_dir->isValid());
        qputenv("KYLINLIST_DATA_DIR", m_dir->path().toUtf8());
        m_fileUrls.urls.clear();
        ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
        QString error;
        QVERIFY2(Database::instance().openAt(m_dir->path() + QStringLiteral("/attachmentcards.db"), &error), qPrintable(error));
        m_store.reset(new TaskStore(Database::instance().connection()));
        QVERIFY2(m_store->load(&error), qPrintable(error));
    }
    void cleanup() {
        m_store.reset(); Database::instance().close(); m_dir.reset();
        if (m_settingsDir) qputenv("KYLINLIST_DATA_DIR", m_settingsDir->path().toUtf8());
    }
    void cleanupTestCase() {
        QDesktopServices::unsetUrlHandler(QStringLiteral("file"));
        QSettings::setDefaultFormat(m_previousSettingsFormat);
        if (m_hadDataDir) qputenv("KYLINLIST_DATA_DIR", m_previousDataDir);
        else qunsetenv("KYLINLIST_DATA_DIR");
    }
    void appearanceFallbacksAndBinarySizeLabels() {
        using namespace AttachmentAppearance;
        Attachment attachment; attachment.originalName = QStringLiteral("lyrics.lrc"); attachment.mime = QStringLiteral("text/plain");
        QCOMPARE(int(fileType(attachment)), int(FileType::Other)); QCOMPARE(iconName(fileType(attachment)), QStringLiteral("other"));
        attachment.originalName = QStringLiteral("UPPER.DOCX");
        QCOMPARE(int(fileType(attachment)), int(FileType::Document));
        attachment.isImage = true;
        QCOMPARE(int(fileType(attachment)), int(FileType::Image));
        attachment.isImage = false; attachment.originalName = QStringLiteral("no-extension"); attachment.mime = QStringLiteral("application/pdf");
        QCOMPARE(int(fileType(attachment)), int(FileType::Pdf));
        attachment.mime = QStringLiteral("text/plain"); QCOMPARE(int(fileType(attachment)), int(FileType::Text));
        QCOMPARE(sizeLabel(712), QStringLiteral("712 B")); QCOMPARE(sizeLabel(1024), QStringLiteral("1 KB"));
        QCOMPARE(sizeLabel(1536), QStringLiteral("1.5 KB")); QCOMPARE(sizeLabel(1048576), QStringLiteral("1 MB"));
        QCOMPARE(sizeLabel(1614807), QStringLiteral("1.54 MB")); QCOMPARE(sizeLabel(2 * 1048576), QStringLiteral("2 MB"));
    }
    void inlineCardsUseApprovedGeometryAndTypeIcons_data() {
        QTest::addColumn<QString>("fileName"); QTest::addColumn<QString>("typeName");
        QTest::addColumn<QString>("iconName"); QTest::addColumn<bool>("image");
        QTest::newRow("document") << QStringLiteral("report.DOCX") << QStringLiteral("document") << QStringLiteral("doc") << false;
        QTest::newRow("spreadsheet") << QStringLiteral("sheet.xlsx") << QStringLiteral("spreadsheet") << QStringLiteral("xls") << false;
        QTest::newRow("presentation") << QStringLiteral("slides.pptx") << QStringLiteral("presentation") << QStringLiteral("ppt") << false;
        QTest::newRow("pdf") << QStringLiteral("reference.pdf") << QStringLiteral("pdf") << QStringLiteral("pdf") << false;
        QTest::newRow("text") << QStringLiteral("notes.md") << QStringLiteral("text") << QStringLiteral("text") << false;
        QTest::newRow("unknown-text") << QStringLiteral("lyrics.lrc") << QStringLiteral("other") << QStringLiteral("other") << false;
        QTest::newRow("archive") << QStringLiteral("bundle.zip") << QStringLiteral("archive") << QStringLiteral("zip") << false;
        QTest::newRow("audio") << QStringLiteral("sound.mp3") << QStringLiteral("audio") << QStringLiteral("audio") << false;
        QTest::newRow("video") << QStringLiteral("movie.mp4") << QStringLiteral("video") << QStringLiteral("video") << false;
        QTest::newRow("verified-image-before-extension") << QStringLiteral("image.docx") << QStringLiteral("image") << QStringLiteral("photo") << true;
    }
    void inlineCardsUseApprovedGeometryAndTypeIcons() {
        QFETCH(QString, fileName); QFETCH(QString, typeName); QFETCH(QString, iconName); QFETCH(bool, image);
        const QString id = m_store->createTask(QStringLiteral("附件卡片几何"));
        const Attachment first = addFile(id, fileName, QByteArray(1536, 'x'), nullptr, image);
        const Attachment second = addFile(id, QStringLiteral("second.txt"), QByteArray("second"));
        QVERIFY(!first.id.isEmpty() && !second.id.isEmpty());
        AttachmentWidget cards(m_store.get()); cards.setInlineMode(true); cards.setTask(id); cards.resize(420, 180); cards.show();
        QAbstractButton* row = nullptr; QTRY_VERIFY((row = rowFor(cards, first.id)) != nullptr);
        auto* next = rowFor(cards, second.id); QVERIFY(next);
        auto* icon = row->findChild<QLabel*>(QStringLiteral("AttachmentIcon"));
        auto* name = row->findChild<QLabel*>(QStringLiteral("AttachmentName"));
        auto* size = row->findChild<QLabel*>(QStringLiteral("AttachmentSize"));
        auto* actions = row->findChild<QToolButton*>(QStringLiteral("AttachmentActions"));
        QVERIFY(icon && name && size && actions);
        QTRY_COMPARE(row->height(), 56); QCOMPARE(row->minimumHeight(), 56); QCOMPARE(row->maximumHeight(), 56);
        QCOMPARE(row->property("attachmentType").toString(), typeName); QVERIFY(row->property("inlineAttachment").toBool());
        QCOMPARE(icon->size(), QSize(26, 26)); QVERIFY(AppIcons::hasIcon(iconName));
        const QPixmap pixmap = labelPixmap(icon); QVERIFY(!pixmap.isNull());
        QCOMPARE(QSize(qRound(pixmap.width() / pixmap.devicePixelRatio()), qRound(pixmap.height() / pixmap.devicePixelRatio())), QSize(26, 26));
        QCOMPARE(pixmap.toImage(), AppIcons::icon(iconName, ds::ColorToken::Info, 26).pixmap(26, 26).toImage());
        QCOMPARE(name->text(), fileName); QCOMPARE(name->textFormat(), Qt::PlainText);
        QCOMPARE(name->font().pointSizeF(), 10.5); QCOMPARE(size->font().pointSizeF(), 9.0);
        QCOMPARE(name->font().pointSizeF(), ds::qfont(ds::FontScene::Body).pointSizeF());
        QCOMPARE(size->font().pointSizeF(), ds::qfont(ds::FontScene::Caption).pointSizeF());
        QCOMPARE(size->text(), AttachmentAppearance::sizeLabel(first.size));
        QCOMPARE(rectIn(name, row).left(), rectIn(size, row).left());
        const QRect nameRect = rectIn(name, row), sizeRect = rectIn(size, row);
        QCOMPARE(nameRect.height(), name->fontMetrics().height());
        QCOMPARE(sizeRect.height(), size->fontMetrics().height());
        QCOMPARE(sizeRect.top() - nameRect.bottom() - 1, 1);
        const int textTop = nameRect.top();
        const int textBottom = row->height() - sizeRect.bottom() - 1;
        QVERIFY2(qAbs(textTop - textBottom) <= 1,
                 qPrintable(QStringLiteral("Unequal text block padding: top=%1 bottom=%2")
                            .arg(textTop).arg(textBottom)));
        QVERIFY(textTop >= 4 && textBottom >= 4);
        QVERIFY(qAbs(rectIn(icon, row).center().y() - nameRect.united(sizeRect).center().y()) <= 1);

        QCOMPARE(actions->size(), QSize(30, 30)); QCOMPARE(actions->iconSize(), QSize(18, 18));
        QCOMPARE(actions->icon().pixmap(18, 18).toImage(), AppIcons::icon(IconId::MoreTitlebar, 18).pixmap(18, 18).toImage());
        QVERIFY(rectIn(actions, row).left() > rectIn(name, row).right());
        QTRY_COMPARE(rectIn(next, &cards).top() - rectIn(row, &cards).bottom() - 1, 6);
        QVERIFY(row->mask().isEmpty());
        // 无截图：核对运行时实际应用的卡片规则，而不是新增仅供测试查询的圆角 API。
        const QRegularExpression radius(QStringLiteral("QWidget#AttachmentRow\\[inlineAttachment=\"true\"\\]\\s*\\{[^}]*border-radius\\s*:\\s*14px"));
        QVERIFY2(radius.match(qApp->styleSheet()).hasMatch(), "Inline attachment card must use the approved 14px radius");
    }
    void longNamesStayPlainTextAndNeverWidenCards() {
        const QString id = m_store->createTask(QStringLiteral("长文件名"));
        const QString fileName = QStringLiteral("[literal] &amp; ") + QString(130, QLatin1Char('L')) + QStringLiteral(".txt");
        const Attachment attachment = addFile(id, fileName, QByteArray("synthetic long name")); QVERIFY(!attachment.id.isEmpty());
        AttachmentWidget cards(m_store.get()); cards.setInlineMode(true); cards.setTask(id); cards.resize(240, 120); cards.show();
        QAbstractButton* row = nullptr; QTRY_VERIFY((row = rowFor(cards, attachment.id)) != nullptr);
        auto* name = row->findChild<QLabel*>(QStringLiteral("AttachmentName"));
        auto* actions = row->findChild<QToolButton*>(QStringLiteral("AttachmentActions")); QVERIFY(name && actions);
        QTRY_COMPARE(cards.width(), 240); QVERIFY(row->width() <= cards.width()); QCOMPARE(row->height(), 56);
        QCOMPARE(name->textFormat(), Qt::PlainText); QCOMPARE(name->text(), fileName);
        QCOMPARE(name->toolTip(), fileName); QCOMPARE(name->accessibleName(), fileName); QVERIFY(!name->wordWrap());
        QCOMPARE(name->minimumSizeHint().width(), 0); QVERIFY(name->width() > 0);
        QVERIFY(name->fontMetrics().horizontalAdvance(name->text()) > name->width());
        QVERIFY(row->rect().contains(rectIn(actions, row))); QCOMPARE(actions->size(), QSize(30, 30));
        cards.resize(600, 120); QTRY_VERIFY(row->width() > 400);
        cards.resize(240, 120); QTRY_COMPARE(cards.width(), 240); QTRY_VERIFY(row->width() <= 240);
        QCOMPARE(row->height(), 56); QCOMPARE(name->text(), fileName);
    }
    void ellipsisMouseAndKeyboardContextShareOnePopupMenu() {
        const QString id = m_store->createTask(QStringLiteral("同一个附件菜单"));
        const Attachment attachment = addFile(id, QStringLiteral("menu.txt"), QByteArray("synthetic menu")); QVERIFY(!attachment.id.isEmpty());
        AttachmentWidget cards(m_store.get()); cards.setInlineMode(true); cards.setTask(id); cards.resize(360, 120); cards.show();
        cards.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&cards));
        QAbstractButton* row = nullptr; QTRY_VERIFY((row = rowFor(cards, attachment.id)) != nullptr);
        auto* menu = menuFor(row); QVERIFY(menu); QPointer<PopupMenu> identity(menu);
        auto* open = row->findChild<QAction*>(QStringLiteral("OpenAttachment")); QVERIFY(open);
        QCOMPARE(open->data().toString(), attachment.id); QVERIFY(!menu->actions().contains(open));
        QVERIFY(row->contextMenuPolicy() != Qt::ActionsContextMenu);
        for (MenuEntry entry : {MenuEntry::Ellipsis, MenuEntry::MouseContext, MenuEntry::KeyboardContext}) {
            QStringList names, texts, data; QVector<int> heights; int cardWidth = 0;
            bool same = false, entityMenu = false, nativePopup = false, validActions = true, navigated = false;
            QVERIFY(withAttachmentMenu(row, entry, [&](PopupMenu* visible) {
                same = visible == identity.data(); entityMenu = visible->property("entityMenu").toBool();
                nativePopup = visible->windowFlags().testFlag(Qt::Popup)
                    && visible->windowFlags().testFlag(Qt::NoDropShadowWindowHint)
                    && visible->testAttribute(Qt::WA_TranslucentBackground) && visible->mask().isEmpty();
                cardWidth = visible->width() - 2 * PopupMenu::shadowMargin();
                for (QAction* action : visible->actions()) {
                    names.append(action->objectName()); texts.append(action->text()); data.append(action->data().toString());
                    heights.append(visible->actionGeometry(action).height());
                    validActions = validActions && action->isVisible() && action->isEnabled()
                        && !action->isSeparator() && !qobject_cast<QWidgetAction*>(action);
                }
                visible->setActiveAction(nullptr); QTest::keyClick(visible, Qt::Key_Down);
                QAction* first = visible->activeAction(); QTest::keyClick(visible, Qt::Key_Down);
                navigated = first && first->objectName() == QLatin1String("SaveAttachmentAs")
                    && visible->activeAction() && visible->activeAction()->objectName() == QLatin1String("RemoveAttachment");
                QTest::keyClick(visible, Qt::Key_Escape);
            }));
            QVERIFY(same && entityMenu && nativePopup && validActions && navigated); QCOMPARE(cardWidth, 164);
            QCOMPARE(names, QStringList({QStringLiteral("SaveAttachmentAs"), QStringLiteral("RemoveAttachment")}));
            QCOMPARE(texts, QStringList({QStringLiteral("另存为…"), QStringLiteral("删除")}));
            QCOMPARE(data, QStringList({attachment.id, attachment.id})); QCOMPARE(heights, QVector<int>({34, 34}));
            QCOMPARE(menuFor(row), identity.data()); QVERIFY(!menu->isVisible());
        }
    }
    void cardActivationDispatchesOpenActionWithoutExternalProgram_data() {
        QTest::addColumn<QString>("entry");
        for (const char* entry : {"card", "name", "size", "icon", "return", "enter", "space"})
            QTest::newRow(entry) << QString::fromLatin1(entry);
    }
    void cardActivationDispatchesOpenActionWithoutExternalProgram() {
        QFETCH(QString, entry);
        const QString id = m_store->createTask(QStringLiteral("附件卡片打开"));
        const Attachment attachment = addFile(id, QStringLiteral("open.txt"), QByteArray("safe synthetic file URL")); QVERIFY(!attachment.id.isEmpty());
        AttachmentWidget cards(m_store.get()); cards.setInlineMode(true); cards.setTask(id); cards.resize(360, 120); cards.show();
        cards.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&cards));
        QAbstractButton* row = nullptr; QTRY_VERIFY((row = rowFor(cards, attachment.id)) != nullptr);
        auto* open = row->findChild<QAction*>(QStringLiteral("OpenAttachment")); QVERIFY(open);
        QCOMPARE(open->data().toString(), attachment.id); QVERIFY(!menuFor(row)->actions().contains(open));
        QSignalSpy triggered(open, &QAction::triggered);
        if (entry == QLatin1String("return") || entry == QLatin1String("enter") || entry == QLatin1String("space")) {
            clickActualChild(cards, row->mapTo(&cards, QPoint(row->width() / 2, 3)));
            QTRY_VERIFY(row->hasFocus()); triggered.clear(); m_fileUrls.urls.clear();
            QTest::keyClick(row, entry == QLatin1String("space") ? Qt::Key_Space
                : entry == QLatin1String("enter") ? Qt::Key_Enter : Qt::Key_Return);
        } else {
            QWidget* target = row;
            if (entry == QLatin1String("name")) target = row->findChild<QLabel*>(QStringLiteral("AttachmentName"));
            if (entry == QLatin1String("size")) target = row->findChild<QLabel*>(QStringLiteral("AttachmentSize"));
            if (entry == QLatin1String("icon")) target = row->findChild<QLabel*>(QStringLiteral("AttachmentIcon"));
            QVERIFY(target);
            const QPoint point = target == row ? QPoint(row->width() / 2, 3) : target->rect().center();
            clickActualChild(cards, target->mapTo(&cards, point));
        }
        QCOMPARE(triggered.count(), 1); QCOMPARE(m_fileUrls.urls.size(), 1);
        QCOMPARE(QFileInfo(m_fileUrls.urls.first().toLocalFile()).canonicalFilePath(),
                 QFileInfo(m_store->attachmentFilePath(attachment)).canonicalFilePath());
        QCOMPARE(m_store->attachments(id).size(), 1); QVERIFY(m_store->lastError().isEmpty());
    }
    void saveAsDialogAcceptCancelAndRejectsStaleContexts() {
        // 仅本 case 关闭原生文件对话框，离开作用域即恢复原进程属性。
        const NonNativeFileDialogs nonNative;
        const QString outputRoot = m_dir->path() + QStringLiteral("/exports"); QVERIFY(QDir().mkpath(outputRoot));
        const QByteArray payload("synthetic save-as payload; never real user data");
        const QByteArray sentinel("existing destination must survive failed SHA validation");
        const QStringList scenarios{QStringLiteral("accept"), QStringLiteral("cancel"), QStringLiteral("entity-switch"),
            QStringLiteral("store-switch"), QStringLiteral("removed"), QStringLiteral("read-only"), QStringLiteral("corrupt")};
        for (const QString& scenario : scenarios) {
            const QString id = m_store->createTask(QStringLiteral("另存为 ") + scenario);
            const QString otherId = m_store->createTask(QStringLiteral("另一个实体"));
            QString source; const Attachment attachment = addFile(id, scenario + QStringLiteral(".txt"), payload, &source);
            QVERIFY(!attachment.id.isEmpty()); const QString managed = m_store->attachmentFilePath(attachment);
            const QString destination = outputRoot + QLatin1Char('/') + scenario + QStringLiteral(".txt");
            const bool corrupt = scenario == QLatin1String("corrupt");
            if (corrupt) QVERIFY(writeBytes(destination, sentinel));
            TaskStore replacement(Database::instance().connection()); QVERIFY(replacement.load());
            AttachmentWidget cards(m_store.get()); cards.setInlineMode(true); cards.setTask(id);
            if (scenario == QLatin1String("read-only")) cards.setReadOnly(true);
            cards.resize(360, 120); cards.show();
            QAbstractButton* row = nullptr; QTRY_VERIFY((row = rowFor(cards, attachment.id)) != nullptr);
            QSignalSpy changed(m_store.get(), &TaskStore::attachmentsChanged), errors(m_store.get(), &TaskStore::errorOccurred);
            bool fileDialogSeen = false, saveMode = false, correctDefault = false, clickedButton = false, mutationOk = true;
            bool errorPlainText = false; int errorDialogs = 0; QString errorTitle, errorMessage;
            ModalDriver driver([&](QDialog* dialog) {
                if (auto* fileDialog = qobject_cast<QFileDialog*>(dialog)) {
                    fileDialogSeen = true; saveMode = fileDialog->acceptMode() == QFileDialog::AcceptSave;
                    correctDefault = !fileDialog->selectedFiles().isEmpty()
                        && QFileInfo(fileDialog->selectedFiles().first()).fileName() == attachment.originalName;
                    if (corrupt) fileDialog->setOption(QFileDialog::DontConfirmOverwrite, true);
                    fileDialog->selectFile(destination); QApplication::processEvents();
                    if (scenario == QLatin1String("entity-switch")) cards.setTask(otherId);
                    if (scenario == QLatin1String("store-switch")) cards.setStore(&replacement);
                    if (scenario == QLatin1String("removed")) mutationOk = m_store->removeAttachment(id, attachment.id);
                    if (corrupt) mutationOk = writeBytes(managed, QByteArray(payload.size(), 'z'));
                    auto* buttons = fileDialog->findChild<QDialogButtonBox*>();
                    auto* button = buttons ? buttons->button(scenario == QLatin1String("cancel")
                        ? QDialogButtonBox::Cancel : QDialogButtonBox::Save) : nullptr;
                    clickedButton = button && button->isVisible() && button->isEnabled();
                    if (clickedButton) QTest::mouseClick(button, Qt::LeftButton);
                    else fileDialog->reject();
                    return true;
                }
                if (dialog->objectName() == QLatin1String("AttachmentErrorDialog")) {
                    ++errorDialogs; return dismissAttachmentError(dialog, &errorTitle, &errorMessage, &errorPlainText);
                }
                return false;
            });
            const bool chosen = chooseAttachmentAction(row, QStringLiteral("SaveAttachmentAs")); driver.stop();
            QVERIFY2(chosen && fileDialogSeen && saveMode && correctDefault && clickedButton && mutationOk, qPrintable(scenario));
            QVERIFY2(!driver.timedOut && driver.unexpectedDialogs == 0, qPrintable(scenario));
            const bool saved = scenario == QLatin1String("accept") || scenario == QLatin1String("read-only");
            if (saved) {
                QCOMPARE(fileBytes(destination), payload);
                QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(fileBytes(destination), QCryptographicHash::Sha256).toHex()), attachment.sha256);
            } else if (corrupt) QCOMPARE(fileBytes(destination), sentinel);
            else QVERIFY2(!QFileInfo::exists(destination), qPrintable(scenario + QStringLiteral(" must perform zero copies")));
            QCOMPARE(fileBytes(source), payload);
            QCOMPARE(errorDialogs, corrupt ? 1 : 0); QCOMPARE(errors.count(), corrupt ? 1 : 0);
            if (corrupt) {
                QCOMPARE(errorTitle, QStringLiteral("无法另存附件")); QVERIFY(errorPlainText); QVERIFY(!errorMessage.isEmpty());
                QVERIFY(errorMessage.contains(QStringLiteral("SHA"))); QCOMPARE(m_fileUrls.urls.size(), 0);
                QCOMPARE(m_store->attachments(id).first().localState, AttachmentFileState::Corrupt);
            }
            if (scenario == QLatin1String("removed")) { QVERIFY(m_store->attachments(id).isEmpty()); QVERIFY(!QFileInfo::exists(managed)); }
            else { QCOMPARE(m_store->attachments(id).size(), 1); QVERIFY(QFileInfo::exists(managed)); }
            if (!corrupt) QCOMPARE(changed.count(), scenario == QLatin1String("removed") ? 1 : 0);
        }
        QCOMPARE(QDir(outputRoot).entryList(QDir::Files | QDir::NoDotAndDotDot).size(), 3); // accept/read-only + 未被破坏的 sentinel。
    }
    void removalUsesConfirmDialogAndPreservesOriginal_data() {
        QTest::addColumn<bool>("accept"); QTest::newRow("cancel") << false; QTest::newRow("confirm") << true;
    }
    void removalUsesConfirmDialogAndPreservesOriginal() {
        QFETCH(bool, accept);
        const QString id = m_store->createTask(QStringLiteral("删除附件确认"));
        const QByteArray payload("synthetic original is retained"); QString source;
        const Attachment attachment = addFile(id, QStringLiteral("remove & original.txt"), payload, &source); QVERIFY(!attachment.id.isEmpty());
        const QString managed = m_store->attachmentFilePath(attachment);
        AttachmentWidget cards(m_store.get()); cards.setInlineMode(true); cards.setTask(id); cards.resize(360, 120); cards.show();
        QAbstractButton* row = nullptr; QTRY_VERIFY((row = rowFor(cards, attachment.id)) != nullptr);
        QSignalSpy changed(m_store.get(), &TaskStore::attachmentsChanged);
        const int eventsBefore = m_store->events(id).size();
        int confirmations = 0; bool actualButtons = false, plainText = false, danger = false; QString title, text;
        ModalDriver driver([&](QDialog* dialog) {
            auto* confirmation = qobject_cast<ConfirmDialog*>(dialog); if (!confirmation) return false;
            ++confirmations; title = confirmation->dialogTitle();
            auto* label = confirmation->findChild<QLabel*>(QStringLiteral("ConfirmText"));
            auto* yes = confirmation->findChild<QPushButton*>(QStringLiteral("ConfirmAccept"));
            auto* no = confirmation->findChild<QPushButton*>(QStringLiteral("ConfirmCancel"));
            actualButtons = yes && no; plainText = label && label->textFormat() == Qt::PlainText;
            danger = yes && yes->property("danger").toBool(); text = label ? label->text() : QString();
            if (actualButtons) QTest::mouseClick(accept ? yes : no, Qt::LeftButton);
            else confirmation->reject();
            return true;
        });
        const bool chosen = chooseAttachmentAction(row, QStringLiteral("RemoveAttachment")); driver.stop();
        QVERIFY(chosen); QVERIFY(!driver.timedOut); QCOMPARE(driver.unexpectedDialogs, 0);
        QCOMPARE(confirmations, 1); QVERIFY(actualButtons && plainText && danger); QCOMPARE(title, QStringLiteral("删除附件"));
        QVERIFY(text.contains(attachment.originalName)); QVERIFY(text.contains(QStringLiteral("原文件")));
        QCOMPARE(fileBytes(source), payload); QCOMPARE(changed.count(), accept ? 1 : 0);
        QCOMPARE(m_store->attachments(id).size(), accept ? 0 : 1); QCOMPARE(QFileInfo::exists(managed), !accept);
        QCOMPARE(m_store->events(id).size(), eventsBefore + (accept ? 1 : 0));
        if (!accept) QCOMPARE(fileBytes(managed), payload);
    }
    void removalRevalidatesContextWhileConfirmDialogIsOpen_data() {
        QTest::addColumn<QString>("change");
        for (const char* change : {"read-only", "entity-switch", "store-switch", "already-removed"})
            QTest::newRow(change) << QString::fromLatin1(change);
    }
    void removalRevalidatesContextWhileConfirmDialogIsOpen() {
        QFETCH(QString, change);
        const QString id = m_store->createTask(QStringLiteral("模态删除上下文"));
        const QString otherId = m_store->createTask(QStringLiteral("不应误删的实体"));
        QString source; const QByteArray payload("synthetic race fixture");
        const Attachment attachment = addFile(id, QStringLiteral("race.txt"), payload, &source); QVERIFY(!attachment.id.isEmpty());
        const QString managed = m_store->attachmentFilePath(attachment);
        TaskStore replacement(Database::instance().connection()); QVERIFY(replacement.load());
        AttachmentWidget cards(m_store.get()); cards.setInlineMode(true); cards.setTask(id); cards.resize(360, 120); cards.show();
        cards.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&cards));
        QAbstractButton* row = nullptr; QTRY_VERIFY((row = rowFor(cards, attachment.id)) != nullptr);
        clickActualChild(cards, row->mapTo(&cards, QPoint(row->width() / 2, 3))); QTRY_VERIFY(row->hasFocus());
        QSignalSpy changed(m_store.get(), &TaskStore::attachmentsChanged), errors(m_store.get(), &TaskStore::errorOccurred);
        int confirmations = 0; bool mutated = false, buttonFound = false;
        ModalDriver driver([&](QDialog* dialog) {
            auto* confirmation = qobject_cast<ConfirmDialog*>(dialog); if (!confirmation) return false;
            ++confirmations;
            if (change == QLatin1String("read-only")) { cards.setReadOnly(true); mutated = true; }
            if (change == QLatin1String("entity-switch")) { cards.setTask(otherId); mutated = true; }
            if (change == QLatin1String("store-switch")) { cards.setStore(&replacement); mutated = true; }
            if (change == QLatin1String("already-removed")) mutated = m_store->removeAttachment(id, attachment.id);
            auto* accept = confirmation->findChild<QPushButton*>(QStringLiteral("ConfirmAccept")); buttonFound = accept;
            if (accept) QTest::mouseClick(accept, Qt::LeftButton); else confirmation->reject();
            return true;
        });
        // Delete 与菜单必须共用 RemoveAttachment；这里从真实卡片焦点走键盘入口。
        QTest::keyClick(row, Qt::Key_Delete); driver.stop();
        QCOMPARE(confirmations, 1); QVERIFY(mutated && buttonFound); QVERIFY(!driver.timedOut); QCOMPARE(driver.unexpectedDialogs, 0);
        const bool removedExternally = change == QLatin1String("already-removed");
        QCOMPARE(changed.count(), removedExternally ? 1 : 0); QCOMPARE(errors.count(), 0);
        QCOMPARE(m_store->attachments(id).size(), removedExternally ? 0 : 1);
        QCOMPARE(QFileInfo::exists(managed), !removedExternally); QCOMPARE(fileBytes(source), payload);
        QVERIFY(m_store->attachments(otherId).isEmpty());
    }
    void readOnlyAndCompactKeepSaveButNeverDelete_data() {
        QTest::addColumn<bool>("compact"); QTest::newRow("read-only-inline") << false; QTest::newRow("desktop-compact") << true;
    }
    void readOnlyAndCompactKeepSaveButNeverDelete() {
        QFETCH(bool, compact);
        const QString id = m_store->createTask(QStringLiteral("附件只读模式"));
        QString source; const Attachment attachment = addFile(id, QStringLiteral("compact.png"), QByteArray(), &source, true);
        QVERIFY(!attachment.id.isEmpty()); const QByteArray original = fileBytes(source);
        AttachmentWidget cards(m_store.get()); cards.setInlineMode(!compact); cards.setCompact(compact);
        cards.setReadOnly(!compact); cards.setTask(id); cards.resize(360, 140); cards.show();
        cards.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&cards));
        QAbstractButton* row = nullptr; QTRY_VERIFY((row = rowFor(cards, attachment.id)) != nullptr);
        auto* menu = menuFor(row); QVERIFY(menu);
        auto* save = menu->findChild<QAction*>(QStringLiteral("SaveAttachmentAs"));
        auto* remove = menu->findChild<QAction*>(QStringLiteral("RemoveAttachment")); QVERIFY(save && remove);
        QVERIFY(save->isEnabled() && save->isVisible()); QVERIFY(!remove->isEnabled() || !remove->isVisible());
        if (compact) {
            auto* thumbnail = row->findChild<QLabel*>(QStringLiteral("AttachmentThumbnail"));
            auto* name = row->findChild<QLabel*>(QStringLiteral("AttachmentName"));
            auto* size = row->findChild<QLabel*>(QStringLiteral("AttachmentSize")); QVERIFY(thumbnail && name && size);
            QCOMPARE(thumbnail->size(), QSize(52, 40)); QVERIFY(!labelPixmap(thumbnail).isNull());
            QVERIFY(!row->findChild<QLabel*>(QStringLiteral("AttachmentIcon"))); QVERIFY(!row->property("inlineAttachment").toBool());
            QCOMPARE(rectIn(name, row).left(), rectIn(size, row).left()); QVERIFY(rectIn(name, row).bottom() < rectIn(size, row).top());
        } else QCOMPARE(row->height(), 56);
        bool menuAllowsOnlySave = false;
        QVERIFY(withAttachmentMenu(row, MenuEntry::MouseContext, [&](PopupMenu* visible) {
            QStringList selectable;
            for (auto* action : visible->actions()) if (action->isVisible() && action->isEnabled()) selectable.append(action->objectName());
            menuAllowsOnlySave = selectable == QStringList({QStringLiteral("SaveAttachmentAs")});
            QTest::keyClick(visible, Qt::Key_Escape);
        }));
        QVERIFY(menuAllowsOnlySave);
        QSignalSpy changed(m_store.get(), &TaskStore::attachmentsChanged); int dialogs = 0;
        ModalDriver driver([&](QDialog*) { ++dialogs; return false; });
        // 不点击图片卡片（会进图片预览），用 Tab 从真实溢出按钮获得卡片焦点。
        auto* actions = row->findChild<QToolButton*>(QStringLiteral("AttachmentActions")); QVERIFY(actions);
        actions->setFocus(); QTest::keyClick(actions, Qt::Key_Backtab); QTRY_VERIFY(row->hasFocus());
        QTest::keyClick(row, Qt::Key_Delete); remove->trigger(); driver.stop();
        QCOMPARE(dialogs, 0); QVERIFY(!driver.timedOut); QCOMPARE(changed.count(), 0);
        QCOMPARE(m_store->attachments(id).size(), 1); QCOMPARE(fileBytes(source), original);
        QCOMPARE(fileBytes(m_store->attachmentFilePath(attachment)), original);
    }
    void unavailableManagedFilesReportPlainTextFeedback_data() {
        QTest::addColumn<bool>("missing"); QTest::newRow("missing") << true; QTest::newRow("corrupt") << false;
    }
    void unavailableManagedFilesReportPlainTextFeedback() {
        QFETCH(bool, missing);
        const QString id = m_store->createTask(QStringLiteral("失效附件反馈"));
        QString source; const QByteArray payload("valid synthetic source");
        const Attachment attachment = addFile(id, QStringLiteral("unavailable.txt"), payload, &source); QVERIFY(!attachment.id.isEmpty());
        const QString managed = m_store->attachmentFilePath(attachment);
        if (missing) QVERIFY(QFile::remove(managed)); else QVERIFY(writeBytes(managed, QByteArray(payload.size(), 'z')));
        AttachmentWidget cards(m_store.get()); cards.setInlineMode(true); cards.setTask(id); cards.resize(360, 120); cards.show();
        QAbstractButton* row = nullptr; QTRY_VERIFY((row = rowFor(cards, attachment.id)) != nullptr);
        QSignalSpy errors(m_store.get(), &TaskStore::errorOccurred);
        int feedbacks = 0; bool plainText = false; QString title, message;
        ModalDriver driver([&](QDialog* dialog) {
            if (dialog->objectName() != QLatin1String("AttachmentErrorDialog")) return false;
            ++feedbacks; return dismissAttachmentError(dialog, &title, &message, &plainText);
        });
        clickActualChild(cards, row->mapTo(&cards, QPoint(row->width() / 2, 3))); driver.stop();
        QCOMPARE(feedbacks, 1); QVERIFY(!driver.timedOut); QCOMPARE(driver.unexpectedDialogs, 0);
        QCOMPARE(title, QStringLiteral("无法打开附件")); QVERIFY(plainText && !message.isEmpty());
        QCOMPARE(errors.count(), 1); QCOMPARE(m_fileUrls.urls.size(), 0); QCOMPARE(fileBytes(source), payload);
        QCOMPARE(m_store->attachments(id).size(), 1);
        QCOMPARE(m_store->attachments(id).first().localState, missing ? AttachmentFileState::Missing : AttachmentFileState::Corrupt);
    }
private:
    Attachment addFile(const QString& entityId, const QString& name, const QByteArray& payload,
                       QString* source = nullptr, bool image = false) {
        const QString path = m_dir->path() + QLatin1Char('/') + name;
        if (source) *source = path;
        if (image) {
            QImage sample(48, 32, QImage::Format_ARGB32); sample.fill(Qt::blue);
            if (!sample.save(path, "PNG")) return Attachment();
        } else if (!writeBytes(path, payload)) return Attachment();
        return m_store->importAttachment(entityId, path);
    }
    bool m_hadDataDir = false;
    QByteArray m_previousDataDir;
    QSettings::Format m_previousSettingsFormat = QSettings::NativeFormat;
    std::unique_ptr<QTemporaryDir> m_settingsDir;
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<TaskStore> m_store;
    AttachmentFileUrlTrap m_fileUrls;
};

QTEST_MAIN(AttachmentCardsTest)
#include "tst_attachmentcards.moc"
