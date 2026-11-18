// 桌面便签（NoteWidget / NoteManager）与共享文本会话的行为、几何与命中测试。
//
// 范围与口径（见仓库 AGENTS.md 与任务约定）：
// - 只用合成临时数据库与设置目录，绝不读写真实用户数据；
// - 双视图（主窗口 TaskDetailWidget + 桌面便签 NoteDetail）共用同一
//   EntityTextSession：任何编辑先即时镜像到会话与另一视图，再谈持久化；
//   本文件一律在 flush/防抖等待之前比较镜像，不以等待代替断言；
// - 命中测试遵守“原生平台先核对 QApplication::widgetAt，offscreen/minimal
//   只做窗口内 childAt 级核对”；所有合成事件只在控件可见、启用且几何有效后发送；
// - 窗口合成/边框/阴影验证用内存渲染（QWidget::render 到透明 QImage），
//   不抓屏、不落盘截图；offscreen 平台跳过仅原生可验证的断言，其余照常。
//
// 由回归脚本分别执行离屏与原生验证；实际结果见 docs/updates.md。

#include "app/Branding.h"
#include "app/EntityTextSession.h"
#include "app/NoteManager.h"
#include "app/TaskStore.h"
#include "db/Database.h"
#include "domain/Entity.h"
#include "platform/SystemShadow.h"
#include "ui/AppStyle.h"
#include "ui/ColorTheme.h"
#include "ui/DateCalendarPanel.h"
#include "ui/DatePickerPopup.h"
#include "ui/MarkdownEditor.h"
#include "ui/NoteWidget.h"
#include "ui/PopupMenu.h"
#include "ui/PopupShadow.h"
#include "ui/ResizeGrip.h"
#include "ui/SettingsDialog.h"
#include "ui/TaskDetailWidget.h"
#include "ui/design/ThemeManager.h"
#include "ui/design/TypeSystem.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDate>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEvent>
#include <QFrame>
#include <QImage>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollBar>
#include <QScrollArea>
#include <QSettings>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>
#include <QtTest>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>

using namespace kl;

namespace {

// offscreen/minimal 平台没有原生窗口系统；全局命中断言只在原生平台执行。
bool headlessPlatform()
{
    const QString platform = QGuiApplication::platformName();
    return platform == QLatin1String("offscreen") || platform == QLatin1String("minimal");
}

bool waitUntil(const std::function<bool()>& predicate, int timeoutMs = 2500)
{
    QElapsedTimer timer;
    timer.start();
    do {
        QApplication::processEvents();
        if (predicate()) return true;
        QTest::qWait(5);
    } while (timer.elapsed() < timeoutMs);
    return predicate();
}

QRect windowForPaper(const QRect& paper)
{
    return paper.adjusted(-PopupShadowMargin, -PopupShadowMargin,
                          PopupShadowMargin, PopupShadowMargin);
}

QRect localPaperRect(const QWidget& note)
{
    return QRect(PopupShadowMargin, PopupShadowMargin,
                 note.width() - 2 * PopupShadowMargin, note.height() - 2 * PopupShadowMargin);
}

QVariant scalar(const QString& sql)
{
    QSqlQuery query(Database::instance().connection());
    if (query.exec(sql) && query.next()) return query.value(0);
    qWarning() << sql << query.lastError().text();
    return QVariant();
}
QString dbTitle(const QString& id)
{
    return scalar(QStringLiteral("SELECT title FROM entities WHERE id='%1'").arg(id)).toString();
}
QString dbBody(const QString& id)
{
    return scalar(QStringLiteral("SELECT body_markdown FROM entities WHERE id='%1'").arg(id)).toString();
}
qint64 dbRevision(const QString& id)
{
    return scalar(QStringLiteral("SELECT revision FROM entities WHERE id='%1'").arg(id)).toLongLong();
}

// 从 parent 向下找真实鼠标命中的最深子控件：按兄弟栈序（后创建/raise 在上）
// 自顶向下查找，并跳过 WA_TransparentForMouseEvents 的覆盖层（NoteBorder），
// 与 QApplication::widgetAt 的语义一致；offscreen 下用它代替全局命中。
QWidget* deepestMouseTarget(QWidget* parent, const QPoint& pointInParent)
{
    const QPoint global = parent->mapToGlobal(pointInParent);
    QWidget* candidate = parent;
    for (;;) {
        QWidget* next = nullptr;
        const QList<QWidget*> children = candidate->findChildren<QWidget*>(
            QString(), Qt::FindDirectChildrenOnly);
        for (int index = children.size() - 1; index >= 0; --index) {
            QWidget* child = children.at(index);
            if (!child || child->isWindow() || !child->isVisible()) continue;
            if (child->testAttribute(Qt::WA_TransparentForMouseEvents)) continue;
            if (child->rect().contains(child->mapFromGlobal(global))) {
                next = child;
                break;
            }
        }
        if (!next) break;
        candidate = next;
    }
    return candidate;
}

// 真实点击：几何有效（可见、启用、非零）才发事件；窗口未激活先激活；
// 命中的必须就是目标（或其子控件）；原生平台再核对 QApplication::widgetAt。
bool clickWidget(QWidget* target, QPoint point = QPoint(-1, -1))
{
    if (!target || !target->isVisible() || !target->isEnabled()
        || target->width() <= 0 || target->height() <= 0) {
        qWarning() << "hit target geometry invalid" << target;
        return false;
    }
    if (point.x() < 0 || point.y() < 0) point = QPoint(target->width() / 2, target->height() / 2);
    QWidget* window = target->window();
    if (!window->isActiveWindow()) {
        window->activateWindow();
        if (headlessPlatform()) QApplication::setActiveWindow(window);
        if (!QTest::qWaitForWindowActive(window)) {
            qWarning() << "window activation failed" << window;
            return false;
        }
    }
    const QPoint inWindow = target->mapTo(window, point);
    QWidget* hit = deepestMouseTarget(window, inWindow);
    if (hit != target && !target->isAncestorOf(hit)) {
        qWarning() << "unexpected deepest hit" << hit << "instead of" << target << point;
        return false;
    }
    const QPoint global = hit->mapToGlobal(hit->mapFrom(window, inWindow));
    if (!headlessPlatform() && QApplication::widgetAt(global) != hit) {
        qWarning() << "native widgetAt mismatch" << QApplication::widgetAt(global) << hit;
        return false;
    }
    QTest::mouseClick(hit, Qt::LeftButton, Qt::NoModifier, hit->mapFromGlobal(global));
    QApplication::processEvents();
    return true;
}

// 悬停进入/离开：直接投递 Enter/Leave（QTest 合成鼠标移动不产生顶层 Enter）。
void sendEnterLeave(QWidget* widget, bool enter)
{
    QEvent event(enter ? QEvent::Enter : QEvent::Leave);
    QApplication::sendEvent(widget, &event);
    QApplication::processEvents();
}

// QTest::keyClicks 内部对每个字符做 toLatin1，中文会触发 qasciikey 断言。
// 非 ASCII 文本一律走真实输入法提交路径（QInputMethodEvent.commitString，
// 经 QLineEdit/EditorTextEdit 的 inputMethodEvent 处理器插入，绝不用 setText）。
bool commitText(QWidget* edit, const QString& text)
{
    if (!edit || !edit->isVisible() || !edit->hasFocus()) return false;
    QInputMethodEvent event;
    event.setCommitString(text);
    QApplication::sendEvent(edit, &event);
    QApplication::processEvents();
    return true;
}

// 原始指针拖动：真实 Press/Move/Release 事件序列（Move 携带按下的左键，
// QTest::mouseMove 不带按键，无法驱动按住拖动的控件）。
void sendDrag(QWidget* target, const QPoint& fromLocal, const QPoint& toLocal)
{
    const QPoint fromGlobal = target->mapToGlobal(fromLocal);
    const QPoint toGlobal = target->mapToGlobal(toLocal);
    QMouseEvent press(QEvent::MouseButtonPress, fromLocal, fromGlobal,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &press);
    QMouseEvent move(QEvent::MouseMove, toLocal, toGlobal,
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &move);
    QApplication::processEvents();
    QMouseEvent release(QEvent::MouseButtonRelease, toLocal, toGlobal,
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &release);
    QApplication::processEvents();
}

QLineEdit* titleEdit(TaskDetailWidget* detail)
{
    return detail ? detail->findChild<QLineEdit*>(QStringLiteral("TaskTitle")) : nullptr;
}
MarkdownEditor* bodyEditor(TaskDetailWidget* detail)
{
    return detail ? detail->findChild<MarkdownEditor*>() : nullptr;
}
QTextEdit* bodyEdit(TaskDetailWidget* detail)
{
    auto* editor = bodyEditor(detail);
    return editor ? editor->textEditor() : nullptr;
}
QScrollArea* detailScroll(TaskDetailWidget* detail)
{
    return detail ? detail->findChild<QScrollArea*>(QStringLiteral("DetailScroll")) : nullptr;
}

// 点击正文视口内真实有文字的位置获得焦点；不以 setFocus 代替真实命中。
bool focusBody(TaskDetailWidget* detail)
{
    QTextEdit* body = bodyEdit(detail);
    if (!body || !body->isVisible()) return false;
    QTextCursor cursor(body->document());
    QPoint point = body->cursorRect(cursor).center();
    point = QPoint(qMin(point.x() + 8, body->viewport()->width() - 4),
                   qMin(point.y(), body->viewport()->height() - 2));
    return clickWidget(body->viewport(), point) && waitUntil([body]() { return body->hasFocus(); });
}
bool focusTitle(TaskDetailWidget* detail)
{
    QLineEdit* title = titleEdit(detail);
    if (!title) return false;
    auto* window = title->window();
    window->activateWindow();
    if (headlessPlatform()) QApplication::setActiveWindow(window);
    if (!QTest::qWaitForWindowActive(window)) return false;
    if (auto* scroll = detailScroll(detail)) scroll->ensureWidgetVisible(title, 0, 4);
    return clickWidget(title) && waitUntil([title]() { return title->hasFocus(); });
}

// 轮询等待弹出菜单可见；断言全部留在回调外，带看门狗防止嵌套事件循环卡死。
PopupMenu* openMenuByClick(NoteWidget& note, QToolButton* button, const char* objectName)
{
    if (!button || !button->isVisible() || !button->isEnabled()) return nullptr;
    QElapsedTimer elapsed;
    elapsed.start();
    QPointer<PopupMenu> found;
    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, &poll, [&]() {
        auto* menu = note.findChild<PopupMenu*>(QString::fromLatin1(objectName));
        if (menu && menu->isVisible()) {
            poll.stop();
            found = menu;
        } else if (elapsed.elapsed() > 2000) {
            poll.stop();
            if (QWidget* popup = QApplication::activePopupWidget()) popup->close();
        }
    });
    poll.start();
    QTest::mouseClick(button, Qt::LeftButton);
    while (poll.isActive() && elapsed.elapsed() < 2200) QTest::qWait(5);
    QApplication::processEvents();
    return found;
}

// 用真实键盘在菜单（含子菜单）里逐级选中并触发；路径为动作文本序列。
bool chooseMenuPath(QMenu* menu, const QStringList& path)
{
    if (!menu || !menu->isVisible() || path.isEmpty()) return false;
    QMenu* current = menu;
    for (int step = 0; step < path.size(); ++step) {
        QAction* found = nullptr;
        for (QAction* action : current->actions()) {
            if (action && !action->isSeparator() && action->text() == path.at(step)
                && (step + 1 < path.size() ? action->menu() != nullptr : action->isEnabled())) {
                found = action;
                break;
            }
        }
        if (!found) return false;
        current->setActiveAction(found);
        QApplication::processEvents();
        if (step + 1 < path.size()) {
            QTest::keyClick(current, Qt::Key_Right);
            QApplication::processEvents();
            if (!found->menu() || !found->menu()->isVisible()) return false;
            current = found->menu();
        } else {
            QTest::keyClick(current, Qt::Key_Return);
        }
    }
    QApplication::processEvents();
    return true;
}

QStringList collectActionTexts(const QMenu* menu)
{
    QStringList texts;
    if (!menu) return texts;
    for (QAction* action : menu->actions()) {
        if (!action || action->isSeparator()) continue;
        texts.append(action->text());
        if (QMenu* child = action->menu()) texts.append(collectActionTexts(child));
    }
    return texts;
}

QImage renderWidgetToImage(QWidget* widget)
{
    const qreal dpr = widget->devicePixelRatioF();
    QImage image(qCeil(widget->width() * dpr), qCeil(widget->height() * dpr),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        widget->render(&painter);
    }
    return image;
}

QColor logicalPixel(const QImage& image, qreal dpr, int x, int y)
{
    return image.pixelColor(qRound(x * dpr), qRound(y * dpr));
}

int colorDistance(const QColor& a, const QColor& b)
{
    return qMax(qAbs(a.red() - b.red()),
                qMax(qAbs(a.green() - b.green()), qAbs(a.blue() - b.blue())));
}

} // namespace

class DesktopNotesTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // 几何与静息态
    void chromeGeometryQuietControlsAndResizeHitboxes();
    void hoverRevealsTinyControlsAndFooterChipStays();
    // 小控件与菜单
    void colorMenuFromTinyButtonChangesProjectionColor_data();
    void colorMenuFromTinyButtonChangesProjectionColor();
    void pinTogglesNoteLayerKeepsTaskPinnedAt();
    void closeKeepsTaskAndRemovesProjection();
    void moreMenuLabelsActionsAndSignals();
    void menuDeleteTrashesTask();
    void completionOnlyFromMenuActionOrShortcut();
    // 共享文本会话
    void alternatingTitleBodyEditsMirrorImmediately();
    void blankTitleKeepsRawMirrorBodySavesFinishBlocked();
    void markdownFormatsCheckboxPasteSingleUndoMirror();
    void independentWidthsKeepOwnDocumentLayout();
    void remoteEditPreservesTitleSelectionAndBodyScroll();
    void remoteEditPreservesPartialLinkCaptionSelection();
    void imePreeditLocalCancelCommitAndRemoteRebase();
    void imeCommitWithNextPreeditMirrorsImmediately();
    // 回收站与恢复
    void trashedThenRestoredViewBindsSessionAndSaves();
    // 字段提交与投影
    void dateChipCommitMirrorsBothViews_data();
    void dateChipCommitMirrorsBothViews();
    void resizeAreaDragResizesPaperAndPersists();
    void titleBarDragSnapsPaperToPeersAndScreen();
    // 自绘
    void paperRenderingCornersBorderShadowBothThemes();
    void paperPaletteAndMarkupStayFixed();

private:
    QString dbPath() const { return m_dir->filePath(QStringLiteral("desktopnotes.db")); }
    QString seedProjectedTask(const QString& title, const QString& body, const QRect& paper);
    NoteWidget* openProjection(NoteManager& notes, const QString& id);
    QRect seedPaper(int slot) const;

    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<QTemporaryDir> m_settingsDir;
    std::unique_ptr<TaskStore> m_store;
    QByteArray m_originalData;
    QByteArray m_originalBrand;
    bool m_hadData = false;
    bool m_hadBrand = false;
    QSettings::Format m_originalFormat = QSettings::NativeFormat;
};

void DesktopNotesTest::initTestCase()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    AppStyle::apply(*qobject_cast<QApplication*>(qApp));
    disableNativeWindowShadows();
    m_hadData = qEnvironmentVariableIsSet("KYLINLIST_DATA_DIR");
    m_originalData = qgetenv("KYLINLIST_DATA_DIR");
    m_hadBrand = qEnvironmentVariableIsSet("KYLINLIST_BRANDING_DIR");
    m_originalBrand = qgetenv("KYLINLIST_BRANDING_DIR");
    m_originalFormat = QSettings::defaultFormat();
    m_settingsDir.reset(new QTemporaryDir);
    QVERIFY(m_settingsDir->isValid());
    // 设置与品牌都指向临时目录：合成环境，不碰真实用户数据。
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir->path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                       m_settingsDir->path() + QStringLiteral("/system"));
    qputenv("KYLINLIST_BRANDING_DIR", m_settingsDir->path().toUtf8());
    Branding::load();
}

void DesktopNotesTest::cleanupTestCase()
{
    QSettings::setDefaultFormat(m_originalFormat);
    if (m_hadData) qputenv("KYLINLIST_DATA_DIR", m_originalData);
    else qunsetenv("KYLINLIST_DATA_DIR");
    if (m_hadBrand) qputenv("KYLINLIST_BRANDING_DIR", m_originalBrand);
    else qunsetenv("KYLINLIST_BRANDING_DIR");
}

void DesktopNotesTest::init()
{
    m_dir.reset(new QTemporaryDir);
    QVERIFY(m_dir->isValid());
    qputenv("KYLINLIST_DATA_DIR", m_dir->path().toUtf8());
    ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
    QString error;
    QVERIFY2(Database::instance().openAt(dbPath(), &error), qPrintable(error));
    m_store.reset(new TaskStore(Database::instance().connection()));
    QVERIFY2(m_store->load(&error), qPrintable(error));
}

void DesktopNotesTest::cleanup()
{
    if (QWidget* popup = QApplication::activePopupWidget()) popup->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    m_store.reset();
    Database::instance().close();
    m_dir.reset();
}

QRect DesktopNotesTest::seedPaper(int slot) const
{
    const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
    const int width = qMin(300, qMax(240, avail.width() - 400));
    const int height = qMin(280, qMax(220, avail.height() - 300));
    return QRect(avail.left() + 80 + slot * 60, avail.top() + 60 + slot * 40, width, height);
}

QString DesktopNotesTest::seedProjectedTask(const QString& title, const QString& body,
                                            const QRect& paper)
{
    const QString id = m_store->createTask(title);
    if (id.isEmpty()) return QString();
    if (!body.isEmpty()) {
        Entity entity = m_store->entity(id);
        entity.body = body;
        if (!m_store->saveTask(entity)) return QString();
    }
    if (!m_store->saveDesktopState(id, NoteColor::Yellow, windowForPaper(paper), 0,
                                   NoteLayer::Normal, true)) return QString();
    return id;
}

NoteWidget* DesktopNotesTest::openProjection(NoteManager& notes, const QString& id)
{
    if (!notes.restoreAll()) return nullptr;
    NoteWidget* note = notes.note(id);
    if (!note) return nullptr;
    note->show();
    note->raise();
    note->activateWindow();
    if (headlessPlatform()) QApplication::setActiveWindow(note);
    if (!QTest::qWaitForWindowExposed(note)) return nullptr;
    QApplication::processEvents();
    return note;
}

// ---------------------------------------------------------------------------
// 几何与静息态
// ---------------------------------------------------------------------------

// 共享阴影边距24、纸面圆角、内容水平边距14、20px 顶部拖动条、26px 底栏、
// 八个命中区围绕可见纸面边缘；静息态小控件全部隐藏、日期胶囊常显；
// 角落命中区不得抢走底栏胶囊/菜单的点击。
void DesktopNotesTest::chromeGeometryQuietControlsAndResizeHitboxes()
{
    const QRect paper = seedPaper(0);
    const QString id = seedProjectedTask(QStringLiteral("几何便签"), QStringLiteral("几何正文"), paper);
    QVERIFY(!id.isEmpty());
    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    QVERIFY(notes.note(id) == note);
    const qreal expectedDpr = qEnvironmentVariable("KYLINLIST_EXPECT_DPR").toDouble();
    if (expectedDpr > 0)
        QVERIFY2(qAbs(note->devicePixelRatioF() - expectedDpr) < 0.01,
                 qPrintable(QStringLiteral("note DPR=%1 expected=%2")
                     .arg(note->devicePixelRatioF()).arg(expectedDpr)));

    // 窗口几何与纸面几何互为阴影边距换算。
    QCOMPARE(note->window(), static_cast<QWidget*>(note));
    QCOMPARE(note->paperGeometry(), paper);
    QCOMPARE(note->geometry(), windowForPaper(paper));
    const QRect localPaper = localPaperRect(*note);
    QCOMPARE(localPaper.size(), paper.size());

    // 顶部 20px 拖动条，纸面左上角起步；标题区在 20px 拖动条之下。
    auto* titleBar = note->findChild<QWidget*>(QStringLiteral("TitleBar"));
    QVERIFY(titleBar);
    QCOMPARE(titleBar->height(), 20);
    QCOMPARE(titleBar->mapTo(note, QPoint()), localPaper.topLeft());
    QCOMPARE(titleBar->width(), localPaper.width());
    auto* titleHost = note->findChild<QWidget*>(QStringLiteral("TaskTitleHost"));
    QVERIFY(titleHost);
    QVERIFY(titleHost->mapTo(note, QPoint()).y() >= titleBar->mapTo(note, QPoint()).y() + 20);
    QCOMPARE(titleHost->layout()->contentsMargins().left(), 14);
    QCOMPARE(titleHost->layout()->contentsMargins().right(), 14);

    // 嵌入的详情为桌面模式：纸面内水平边距14、26px 底栏贴纸面底缘。
    TaskDetailWidget* detail = note->detail();
    QVERIFY(detail);
    QVERIFY(detail->findChild<QCheckBox*>(QStringLiteral("TaskCompleted")));
    QVERIFY(detail->findChild<QCheckBox*>(QStringLiteral("TaskCompleted"))->isHidden());
    auto* editor = bodyEditor(detail);
    QVERIFY(editor);
    QCOMPARE(editor->layout()->contentsMargins().left(), 14);
    QCOMPARE(editor->layout()->contentsMargins().right(), 14);
    auto* footer = note->findChild<QFrame*>(QStringLiteral("DetailFooter"));
    QVERIFY(footer);
    QCOMPARE(footer->height(), 26);
    QCOMPARE(footer->mapTo(note, QPoint()).y() + footer->height(), localPaper.bottom() + 1);
    QCOMPARE(footer->width(), localPaper.width());

    // 日期胶囊常显、20px 高、caption 级字体；DetailMore 20x20、悬停才可见。
    auto* dateChip = note->findChild<QPushButton*>(QStringLiteral("DateChip"));
    QVERIFY(dateChip);
    QVERIFY(dateChip->isVisible());
    QCOMPARE(dateChip->height(), 20);
    QCOMPARE(dateChip->font().pointSizeF(), ds::qfont(ds::FontScene::Caption).pointSizeF());
    QCOMPARE(dateChip->font().family(), ds::qfont(ds::FontScene::Caption).family());
    auto* more = note->findChild<QToolButton*>(QStringLiteral("DetailMore"));
    QVERIFY(more);
    QCOMPARE(more->size(), QSize(20, 20));
    QVERIFY(!more->isVisible());

    // 三个 18x18 小控件静息隐藏；没有已删除的旧控件类别。
    const char* const tiny[] = {"ColorButton", "NotePinButton", "CloseButton"};
    for (const char* name : tiny) {
        auto* button = note->findChild<QToolButton*>(QString::fromLatin1(name));
        QVERIFY2(button, name);
        QCOMPARE(button->size(), QSize(18, 18));
        QVERIFY2(!button->isVisible(), name);
    }
    QVERIFY(!note->findChild<QObject*>(QStringLiteral("NoteComplete")));
    QVERIFY(!note->findChild<QObject*>(QStringLiteral("NoteEditButton")));
    QVERIFY(!note->findChild<QObject*>(QStringLiteral("NotePreview")));
    QVERIFY(note->findChildren<ResizeGrip*>().isEmpty());

    // 八个透明命中区：名字齐全、几何围绕可见纸面边缘、原生平台全局命中可核。
    const int left = localPaper.left(), top = localPaper.top();
    const int right = localPaper.right(), bottom = localPaper.bottom();
    const int width = localPaper.width(), height = localPaper.height();
    struct AreaSpec { const char* name; QRect rect; };
    const AreaSpec areas[] = {
        {"NoteResizeLeft", QRect(left - 3, top + 7, 6, qMax(1, height - 14))},
        {"NoteResizeRight", QRect(right - 2, top + 7, 6, qMax(1, height - 14))},
        {"NoteResizeTop", QRect(left + 7, top - 3, qMax(1, width - 14), 6)},
        {"NoteResizeBottom", QRect(left + 7, bottom - 2, qMax(1, width - 14), 6)},
        {"NoteResizeTopLeft", QRect(left - 3, top - 3, 10, 10)},
        {"NoteResizeTopRight", QRect(right - 6, top - 3, 10, 10)},
        {"NoteResizeBottomLeft", QRect(left - 3, bottom - 6, 10, 10)},
        {"NoteResizeBottomRight", QRect(right - 6, bottom - 6, 10, 10)}
    };
    for (const AreaSpec& spec : areas) {
        auto* area = note->findChild<QWidget*>(QString::fromLatin1(spec.name));
        QVERIFY2(area, spec.name);
        QVERIFY2(area->isVisible(), spec.name);
        QVERIFY2(area->width() > 0 && area->height() > 0, spec.name);
        QCOMPARE(area->geometry(), spec.rect);
        // 命中区中心确实由该区接收（窗口内最深命中），原生平台再核对全局命中。
        QWidget* hit = deepestMouseTarget(note, area->mapTo(note, area->rect().center()));
        QVERIFY2(hit == area, spec.name);
        if (!headlessPlatform())
            QVERIFY2(QApplication::widgetAt(area->mapToGlobal(area->rect().center())) == area,
                     spec.name);
    }

    // 角落命中区不得抢走底栏日期胶囊与更多按钮的点击（只核命中，不真开菜单）。
    QWidget* chipHit = deepestMouseTarget(note, dateChip->mapTo(note, dateChip->rect().center()));
    QVERIFY(chipHit == dateChip);
    sendEnterLeave(note, true);
    QWidget* moreHit = deepestMouseTarget(note, more->mapTo(note, more->rect().center()));
    QVERIFY(moreHit == more);
    if (!headlessPlatform()) {
        QVERIFY(QApplication::widgetAt(dateChip->mapToGlobal(dateChip->rect().center())) == dateChip);
        QVERIFY(QApplication::widgetAt(more->mapToGlobal(more->rect().center())) == more);
    }
    sendEnterLeave(note, false);
}

// 进入便签显示三个小控件与 DetailMore；离开后全部隐藏；日期胶囊始终可见。
void DesktopNotesTest::hoverRevealsTinyControlsAndFooterChipStays()
{
    const QString id = seedProjectedTask(QStringLiteral("悬停便签"), QStringLiteral("悬停正文"),
                                         seedPaper(1));
    QVERIFY(!id.isEmpty());
    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    auto* dateChip = note->findChild<QPushButton*>(QStringLiteral("DateChip"));
    auto* more = note->findChild<QToolButton*>(QStringLiteral("DetailMore"));
    auto* color = note->findChild<QToolButton*>(QStringLiteral("ColorButton"));
    auto* pin = note->findChild<QToolButton*>(QStringLiteral("NotePinButton"));
    auto* close = note->findChild<QToolButton*>(QStringLiteral("CloseButton"));
    QVERIFY(dateChip && more && color && pin && close);

    QVERIFY(dateChip->isVisible());
    QVERIFY(!more->isVisible() && !color->isVisible() && !pin->isVisible() && !close->isVisible());

    sendEnterLeave(note, true);
    QVERIFY(color->isVisible() && pin->isVisible() && close->isVisible() && more->isVisible());
    QVERIFY(dateChip->isVisible());

    sendEnterLeave(note, false);
    QVERIFY(!color->isVisible() && !pin->isVisible() && !close->isVisible() && !more->isVisible());
    QVERIFY(dateChip->isVisible());
}

// ---------------------------------------------------------------------------
// 小控件与菜单
// ---------------------------------------------------------------------------

// 18x18 颜色按钮真实可点：打开 NoteColorMenu，选中后只改投影外观并落桌面状态。
void DesktopNotesTest::colorMenuFromTinyButtonChangesProjectionColor_data()
{
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
}

void DesktopNotesTest::colorMenuFromTinyButtonChangesProjectionColor()
{
    QFETCH(bool, dark);
    auto& theme = ThemeManager::instance();
    const auto previous = theme.preference();
    auto restoreTheme = qScopeGuard([&]() { theme.setPreference(previous); });
    theme.setPreference(dark ? ThemeManager::Preference::Dark : ThemeManager::Preference::Light);
    const QString id = seedProjectedTask(QStringLiteral("颜色便签"), QString(), seedPaper(0));
    QVERIFY(!id.isEmpty());
    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    sendEnterLeave(note, true);
    auto* color = note->findChild<QToolButton*>(QStringLiteral("ColorButton"));
    QVERIFY(color && color->isVisible());

    PopupMenu* menu = openMenuByClick(*note, color, "NoteColorMenu");
    QVERIFY(menu);
    QCOMPARE(menu->actions().size(), ColorCount);
    const QColor paperBg = ds::color(ds::ColorToken::MenuBg, ColorTheme::paperTheme());
    const QColor hoverBg = ds::color(ds::ColorToken::MenuHover, ColorTheme::paperTheme());
    QAction* active = menu->actions().at(colorToInt(NoteColor::Green));
    menu->setActiveAction(active);
    const QRect row = menu->actionGeometry(active);
    QVERIFY(row.left() - PopupShadowMargin >= 8);
    QVERIFY(menu->width() - PopupShadowMargin - row.right() - 1 >= 8);
    const QImage image = renderWidgetToImage(menu);
    const qreal dpr = image.devicePixelRatio();
    const int y = row.center().y();
    QCOMPARE(logicalPixel(image, dpr, PopupShadowMargin + 4, y), paperBg);
    QCOMPARE(logicalPixel(image, dpr, menu->width() - PopupShadowMargin - 5, y), paperBg);
    QCOMPARE(logicalPixel(image, dpr, row.left() + 8, y), hoverBg);
    QCOMPARE(logicalPixel(image, dpr, row.left() + 21, y), QColor(ColorTheme::spec(NoteColor::Green).body));
    // 真实键盘导航到第 4 项（Pink）：无选中项时首个 Down 才选中第 1 项。
    menu->setActiveAction(nullptr);
    for (int step = 0; step < 4; ++step) QTest::keyClick(menu, Qt::Key_Down);
    QVERIFY2(menu->activeAction()
             && menu->activeAction()->text() == ColorTheme::spec(NoteColor::Pink).label,
             qPrintable(QStringLiteral("active=%1 expected=%2")
                        .arg(menu->activeAction() ? menu->activeAction()->text()
                                                  : QStringLiteral("<none>"),
                             ColorTheme::spec(NoteColor::Pink).label)));
    QTest::keyClick(menu, Qt::Key_Return);
    QApplication::processEvents();

    QCOMPARE(note->snapshot().color, NoteColor::Pink);
    QVERIFY(note->desktopDirty());
    QVERIFY(note->flush());
    QCOMPARE(m_store->desktopNote(id).color, NoteColor::Pink);
    // 只改外观：任务本体与文本不受影响。
    QCOMPARE(m_store->entity(id).title, QStringLiteral("颜色便签"));
    QVERIFY(m_store->entity(id).body.isEmpty());
}

// 置顶按钮只切换 NoteLayer（投影层），不改任务 pinnedAt。
void DesktopNotesTest::pinTogglesNoteLayerKeepsTaskPinnedAt()
{
    const QString id = seedProjectedTask(QStringLiteral("置顶便签"), QString(), seedPaper(0));
    QVERIFY(!id.isEmpty());
    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    QCOMPARE(note->layer(), NoteLayer::Normal);
    const QDateTime pinned = QDateTime(QDate(2026, 10, 6), QTime(9, 15), Qt::UTC);
    QVERIFY(m_store->setPinnedAt(id, pinned));

    sendEnterLeave(note, true);
    auto* pin = note->findChild<QToolButton*>(QStringLiteral("NotePinButton"));
    QVERIFY(pin && pin->isVisible());
    QVERIFY(clickWidget(pin));
    QCOMPARE(note->layer(), NoteLayer::OnTop);
    QVERIFY(pin->isChecked());
    // 任务置顶时间原样保留。
    QCOMPARE(m_store->entity(id).pinnedAt, pinned);
    QVERIFY(note->desktopDirty());
    QVERIFY(note->flush());
    QCOMPARE(m_store->desktopNote(id).layer, NoteLayer::OnTop);
    QCOMPARE(m_store->entity(id).pinnedAt, pinned);

    QVERIFY(clickWidget(pin));
    QCOMPARE(note->layer(), NoteLayer::Normal);
    QVERIFY(note->flush());
    QCOMPARE(m_store->desktopNote(id).layer, NoteLayer::Normal);
    QCOMPARE(m_store->entity(id).pinnedAt, pinned);
}

// 关闭只移除桌面投影：open=false、窗口消失、任务本体完好。
void DesktopNotesTest::closeKeepsTaskAndRemovesProjection()
{
    const QString id = seedProjectedTask(QStringLiteral("关闭便签"), QStringLiteral("关闭正文"),
                                         seedPaper(0));
    QVERIFY(!id.isEmpty());
    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    QPointer<NoteWidget> guard(note);
    const QRect moved = seedPaper(2);
    note->setPaperGeometry(moved);
    QCOMPARE(note->paperGeometry(), moved);

    sendEnterLeave(note, true);
    auto* close = note->findChild<QToolButton*>(QStringLiteral("CloseButton"));
    QVERIFY(close && close->isVisible());
    QVERIFY(clickWidget(close));

    QTRY_COMPARE(notes.count(), 0);
    // 便签窗口经 deleteLater 移除：排空 DeferredDelete 后再断言真实析构，
    // 不要求产品改为同步删除。
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    QVERIFY(!guard);
    const Entity task = m_store->entity(id);
    QVERIFY(!task.id.isEmpty());
    QVERIFY(!task.deleted);
    QCOMPARE(task.title, QStringLiteral("关闭便签"));
    QCOMPARE(task.body, QStringLiteral("关闭正文"));
    const DesktopNote desktop = m_store->desktopNote(id);
    QVERIFY(!desktop.id.isEmpty());
    QVERIFY(!desktop.open);
    QCOMPARE(desktop.geometry, windowForPaper(moved)); // 关闭前保存最终位置
}

// 便签菜单：标签集合与信号；没有“开始专注”；设置子菜单含层级与应用设置。
void DesktopNotesTest::moreMenuLabelsActionsAndSignals()
{
    const QString id = seedProjectedTask(QStringLiteral("菜单便签"), QString(), seedPaper(0));
    QVERIFY(!id.isEmpty());
    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    sendEnterLeave(note, true);
    auto* more = note->findChild<QToolButton*>(QStringLiteral("DetailMore"));
    QVERIFY(more && more->isVisible());

    QSignalSpy newNoteSpy(note, &NoteWidget::newNoteRequested);
    QSignalSpy arrangeSpy(note, &NoteWidget::arrangeRequested);
    QSignalSpy openSpy(note, &NoteWidget::taskOpenRequested);
    QSignalSpy managerOpenSpy(&notes, &NoteManager::taskOpenRequested);
    // 菜单关闭后小控件随悬停状态隐藏，重开前重新进入悬停。
    const auto reopenMoreMenu = [&]() {
        sendEnterLeave(note, false);
        sendEnterLeave(note, true);
        return openMenuByClick(*note, more, "NoteMoreMenu");
    };

    PopupMenu* menu = reopenMoreMenu();
    QVERIFY(menu);
    QStringList topLevel;
    for (QAction* action : menu->actions())
        if (!action->isSeparator()) topLevel.append(action->text());
    QCOMPARE(topLevel, QStringList({QStringLiteral("新建"), QStringLiteral("完成"),
                                    QStringLiteral("排列"), QStringLiteral("打开主窗口"),
                                    QStringLiteral("设置"), QStringLiteral("删除")}));
    const QStringList allTexts = collectActionTexts(menu);
    QVERIFY(!allTexts.contains(QStringLiteral("开始专注")));
    QVERIFY(allTexts.contains(QStringLiteral("横向排列")));
    QVERIFY(allTexts.contains(QStringLiteral("纵向排列")));
    QVERIFY(allTexts.contains(layerLabel(NoteLayer::Normal)));
    QVERIFY(allTexts.contains(layerLabel(NoteLayer::OnTop)));
    QVERIFY(allTexts.contains(layerLabel(NoteLayer::OnDesktop)));
    QVERIFY(allTexts.contains(QStringLiteral("应用设置…")));

    // 新建：真实创建第二个桌面便签任务、显示窗口并保持桌面状态打开。
    const int tasksBefore = m_store->entities().size();
    menu = reopenMoreMenu();
    QVERIFY(menu);
    QVERIFY(chooseMenuPath(menu, {QStringLiteral("新建")}));
    QTRY_VERIFY(newNoteSpy.count() == 1);
    QTRY_COMPARE(notes.count(), 2);
    QCOMPARE(m_store->entities().size(), tasksBefore + 1);
    QString createdId;
    for (const Entity& entity : m_store->entities())
        if (entity.title == QStringLiteral("新建桌面便签")) createdId = entity.id;
    QVERIFY(!createdId.isEmpty());
    NoteWidget* created = notes.note(createdId);
    QVERIFY(created);
    QVERIFY(created->isVisible());
    QVERIFY(!m_store->entity(createdId).deleted);
    QVERIFY(m_store->desktopNote(createdId).open);

    // 排列 -> 纵向排列：真实把两枚便签排成一列，尺寸不变、贴工作区。
    QVERIFY(arrangeSpy.isEmpty());
    menu = reopenMoreMenu();
    QVERIFY(menu);
    QVERIFY(chooseMenuPath(menu, {QStringLiteral("排列"), QStringLiteral("纵向排列")}));
    QTRY_VERIFY(arrangeSpy.count() == 1);
    QCOMPARE(arrangeSpy.first().at(0).toBool(), true);
    const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
    const QSize keptSize = note->paperGeometry().size();
    const QSize createdSize = created->paperGeometry().size();
    QList<QRect> papers;
    papers << note->paperGeometry() << created->paperGeometry();
    std::sort(papers.begin(), papers.end(), [](const QRect& a, const QRect& b) {
        return a.top() == b.top() ? a.left() < b.left() : a.top() < b.top();
    });
    QCOMPARE(note->paperGeometry().size(), keptSize);          // 排布绝不改尺寸
    QCOMPARE(created->paperGeometry().size(), createdSize);
    QCOMPARE(papers.at(0).topLeft(), avail.topLeft()); // 列首贴工作区左上角
    QCOMPARE(papers.at(1).x(), avail.left());          // 同一列
    if (papers.at(0).height() + papers.at(1).height() + 8 <= avail.height())
        QCOMPARE(papers.at(1).y(), papers.at(0).y() + papers.at(0).height() + 8);
    QVERIFY(avail.contains(papers.at(0)));
    QVERIFY(avail.contains(papers.at(1)));

    // 打开主窗口：发导航信号。
    menu = reopenMoreMenu();
    QVERIFY(menu);
    QVERIFY(chooseMenuPath(menu, {QStringLiteral("打开主窗口")}));
    QTRY_VERIFY(openSpy.count() == 1);
    QTRY_VERIFY(managerOpenSpy.count() == 1);
    QCOMPARE(openSpy.first().at(0).toString(), id);

    // 设置 -> 始终置顶：切换层级，不动任务置顶。
    menu = reopenMoreMenu();
    QVERIFY(menu);
    QVERIFY(chooseMenuPath(menu, {QStringLiteral("设置"), layerLabel(NoteLayer::OnTop)}));
    QTRY_COMPARE(note->layer(), NoteLayer::OnTop);

    // 设置 -> 应用设置…：打开设置对话框（本测试随即关闭，不做截图）。
    menu = reopenMoreMenu();
    QVERIFY(menu);
    SettingsDialog* dialog = nullptr;
    QVERIFY(chooseMenuPath(menu, {QStringLiteral("设置"), QStringLiteral("应用设置…")}));
    if (waitUntil([&]() {
            for (QWidget* widget : QApplication::topLevelWidgets())
                if ((dialog = qobject_cast<SettingsDialog*>(widget)) && dialog->isVisible()) return true;
            return false;
        })) {
        dialog->close();
        QApplication::processEvents();
    } else {
        QFAIL("应用设置… 未打开设置对话框");
    }
}

// 菜单“删除”回收任务：投影消失、任务进入回收站、可恢复。
void DesktopNotesTest::menuDeleteTrashesTask()
{
    const QString id = seedProjectedTask(QStringLiteral("删除便签"), QStringLiteral("删除正文"),
                                         seedPaper(0));
    QVERIFY(!id.isEmpty());
    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    sendEnterLeave(note, true);
    auto* more = note->findChild<QToolButton*>(QStringLiteral("DetailMore"));
    QVERIFY(more && more->isVisible());
    PopupMenu* menu = openMenuByClick(*note, more, "NoteMoreMenu");
    QVERIFY(menu);
    QVERIFY(chooseMenuPath(menu, {QStringLiteral("删除")}));

    QTRY_COMPARE(notes.count(), 0);
    QVERIFY(m_store->entity(id).deleted);
    QVERIFY(!m_store->entity(id).completed);
    QVERIFY(m_store->restoreTask(id));
    QVERIFY(!m_store->entity(id).deleted);
    QCOMPARE(m_store->entity(id).body, QStringLiteral("删除正文"));
}

// 完成只有菜单动作与 Ctrl+M 两个入口；完成后投影消失但任务未删除。
void DesktopNotesTest::completionOnlyFromMenuActionOrShortcut()
{
    // Ctrl+M 路径。
    const QString shortcutId = seedProjectedTask(QStringLiteral("快捷完成"), QString(), seedPaper(0));
    QVERIFY(!shortcutId.isEmpty());
    NoteManager shortcutNotes(m_store.get());
    NoteWidget* note = openProjection(shortcutNotes, shortcutId);
    QVERIFY(note);
    // 静息态没有任何可见的完成控件。
    for (QCheckBox* check : note->findChildren<QCheckBox*>())
        QVERIFY(!check->isVisible());
    QVERIFY(focusBody(note->detail()));
    QTest::keyClick(bodyEdit(note->detail()), Qt::Key_M, Qt::ControlModifier);
    QTRY_COMPARE(shortcutNotes.count(), 0);
    QVERIFY(m_store->entity(shortcutId).completed);
    QVERIFY(!m_store->entity(shortcutId).deleted);
    QVERIFY(!m_store->desktopNote(shortcutId).id.isEmpty());

    // 菜单“完成”路径。
    const QString menuId = seedProjectedTask(QStringLiteral("菜单完成"), QString(), seedPaper(0));
    QVERIFY(!menuId.isEmpty());
    NoteManager menuNotes(m_store.get());
    note = openProjection(menuNotes, menuId);
    QVERIFY(note);
    sendEnterLeave(note, true);
    auto* more = note->findChild<QToolButton*>(QStringLiteral("DetailMore"));
    QVERIFY(more && more->isVisible());
    PopupMenu* menu = openMenuByClick(*note, more, "NoteMoreMenu");
    QVERIFY(menu);
    QVERIFY(chooseMenuPath(menu, {QStringLiteral("完成")}));
    QTRY_COMPARE(menuNotes.count(), 0);
    QVERIFY(m_store->entity(menuId).completed);
    QVERIFY(!m_store->entity(menuId).deleted);
}

// ---------------------------------------------------------------------------
// 共享文本会话
// ---------------------------------------------------------------------------

// 双视图交替编辑标题/正文：每次击键后权威会话与另一视图立即镜像，
// 切换视图前数据库仍是基线；全部操作先于任何防抖/flush 完成。
void DesktopNotesTest::alternatingTitleBodyEditsMirrorImmediately()
{
    const QString id = seedProjectedTask(QStringLiteral("同步标题"), QStringLiteral("同步正文"),
                                         seedPaper(0));
    QVERIFY(!id.isEmpty());
    TaskDetailWidget main(m_store.get());
    main.resize(560, 700);
    main.show();
    QVERIFY(main.setTask(id));
    if (headlessPlatform()) QApplication::setActiveWindow(&main);
    QVERIFY(QTest::qWaitForWindowExposed(&main));

    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    TaskDetailWidget* projection = note->detail();
    QVERIFY(projection);
    EntityTextSession* session = m_store->textSession(id);
    QVERIFY(session);
    QVERIFY(session == m_store->textSession(id)); // 同一实体共享同一会话

    QLineEdit* mainTitle = titleEdit(&main);
    QLineEdit* noteTitle = titleEdit(projection);
    MarkdownEditor* mainEditor = bodyEditor(&main);
    MarkdownEditor* noteEditor = bodyEditor(projection);
    QVERIFY(mainTitle && noteTitle && mainEditor && noteEditor);
    const QString baselineTitle = QStringLiteral("同步标题");
    const QString baselineBody = QStringLiteral("同步正文");
    QCOMPARE(dbTitle(id), baselineTitle);
    QCOMPARE(dbBody(id), baselineBody);
    const qint64 seedRevision = dbRevision(id);

    // 标题：便签内击键，主窗口立即镜像；焦点未离开，数据库仍是基线。
    QVERIFY(focusTitle(projection));
    QTest::keyClick(noteTitle, Qt::Key_End);
    QTest::keyClicks(noteTitle, QStringLiteral("a"));
    QApplication::processEvents();
    QCOMPARE(session->title(), QStringLiteral("同步标题a"));
    QCOMPARE(noteTitle->text(), QStringLiteral("同步标题a"));
    QCOMPARE(mainTitle->text(), QStringLiteral("同步标题a"));
    QCOMPARE(dbTitle(id), baselineTitle);
    QCOMPARE(dbRevision(id), seedRevision); // 镜像从未依赖持久化

    // 焦点切入便签正文：另一视图结束共享会话，脏标题被有意落盘（+1 次提交）。
    QVERIFY(focusBody(projection));
    QTRY_COMPARE(dbTitle(id), QStringLiteral("同步标题a"));
    const qint64 afterTitleWrite = dbRevision(id);
    QCOMPARE(afterTitleWrite, seedRevision + 1);
    QCOMPARE(dbBody(id), baselineBody);

    QTest::keyClick(bodyEdit(projection), Qt::Key_End, Qt::ControlModifier);
    QTest::keyClicks(bodyEdit(projection), QStringLiteral("b"));
    QApplication::processEvents();
    QCOMPARE(session->body(), QStringLiteral("同步正文b"));
    QCOMPARE(noteEditor->content(), QStringLiteral("同步正文b"));
    QCOMPARE(mainEditor->content(), QStringLiteral("同步正文b"));
    QCOMPARE(dbBody(id), baselineBody);       // 焦点未再切换：正文不落盘
    QCOMPARE(dbRevision(id), afterTitleWrite);
    QVERIFY(note->descriptionDirty());
    QVERIFY(!note->isSaving());

    // 切到主窗口：便签失焦，脏正文随会话结束落盘（+1 次提交）。
    QVERIFY(focusTitle(&main));
    QTRY_COMPARE(dbBody(id), QStringLiteral("同步正文b"));
    const qint64 afterBodyWrite = dbRevision(id);
    QCOMPARE(afterBodyWrite, seedRevision + 2);
    QTest::keyClick(mainTitle, Qt::Key_Home);
    QTest::keyClicks(mainTitle, QStringLiteral("M"));
    QApplication::processEvents();
    QCOMPARE(session->title(), QStringLiteral("M同步标题a"));
    QCOMPARE(noteTitle->text(), QStringLiteral("M同步标题a"));
    QCOMPARE(mainTitle->text(), QStringLiteral("M同步标题a"));
    QCOMPARE(dbRevision(id), afterBodyWrite); // 输入本身不落盘

    // 切回便签正文：主窗口侧会话结束，标题再落盘一次（+1 次提交）。
    QVERIFY(focusBody(projection));
    QTRY_COMPARE(dbTitle(id), QStringLiteral("M同步标题a"));
    const qint64 afterSecondTitleWrite = dbRevision(id);
    QCOMPARE(afterSecondTitleWrite, seedRevision + 3);
    QTest::keyClick(bodyEdit(projection), Qt::Key_End, Qt::ControlModifier);
    QTest::keyClicks(bodyEdit(projection), QStringLiteral("c"));
    QApplication::processEvents();
    QCOMPARE(session->body(), QStringLiteral("同步正文bc"));
    QCOMPARE(mainEditor->content(), QStringLiteral("同步正文bc"));
    QCOMPARE(dbRevision(id), afterSecondTitleWrite);

    QVERIFY(note->finishEditing());           // 收尾提交正文（+1 次提交）
    QCOMPARE(dbTitle(id), QStringLiteral("M同步标题a"));
    QCOMPARE(dbBody(id), QStringLiteral("同步正文bc"));
    QCOMPARE(dbRevision(id), seedRevision + 4);
    QVERIFY(!session->isDirty());
    QCOMPARE(note->snapshot().title, QStringLiteral("M同步标题a"));
    QCOMPARE(note->snapshot().body, QStringLiteral("同步正文bc"));
}

// 空白标题：原始空格镜像、正文照常可保存、finish 保持阻塞；修正后恢复。
void DesktopNotesTest::blankTitleKeepsRawMirrorBodySavesFinishBlocked()
{
    const QString id = seedProjectedTask(QStringLiteral("空白标题"), QStringLiteral("空白正文"),
                                         seedPaper(0));
    QVERIFY(!id.isEmpty());
    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    TaskDetailWidget* projection = note->detail();
    EntityTextSession* session = m_store->textSession(id);
    QVERIFY(projection && session);
    QLineEdit* title = titleEdit(projection);

    QVERIFY(focusTitle(projection));
    QTest::keyClick(title, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClicks(title, QStringLiteral("   "));
    QApplication::processEvents();
    QCOMPARE(session->title(), QStringLiteral("   ")); // 原始空格镜像
    QCOMPARE(title->text(), QStringLiteral("   "));
    QVERIFY(session->isDirty());

    QVERIFY(focusBody(projection));
    QTest::keyClick(bodyEdit(projection), Qt::Key_End, Qt::ControlModifier);
    QTest::keyClicks(bodyEdit(projection), QStringLiteral("x"));
    QApplication::processEvents();
    QCOMPARE(session->body(), QStringLiteral("空白正文x"));

    note->flush(); // 正文可提交并落库；标题草稿保留
    QCOMPARE(dbBody(id), QStringLiteral("空白正文x"));
    QCOMPARE(dbTitle(id), QStringLiteral("空白标题")); // 标题未被覆盖
    QCOMPARE(session->title(), QStringLiteral("   ")); // 草稿原样保留
    QVERIFY(note->textRejected());
    QVERIFY(!note->finishEditing()); // 空白标题阻止结束
    QCOMPARE(notes.count(), 1);      // 窗口保留

    QVERIFY(focusTitle(projection));
    QTest::keyClick(title, Qt::Key_A, Qt::ControlModifier);
    QVERIFY(commitText(title, QStringLiteral("修正标题"))); // 中文走真实输入法提交
    QApplication::processEvents();
    QVERIFY(note->finishEditing());
    QCOMPARE(dbTitle(id), QStringLiteral("修正标题"));
    QVERIFY(!note->textRejected());
}

// 加粗/任务清单勾选/粘贴各算一步撤销；每步都镜像到另一视图，单次 Ctrl+Z 整体回退。
void DesktopNotesTest::markdownFormatsCheckboxPasteSingleUndoMirror()
{
    const QString id = seedProjectedTask(QStringLiteral("格式便签"), QStringLiteral("格式目标行"),
                                         seedPaper(0));
    QVERIFY(!id.isEmpty());
    TaskDetailWidget main(m_store.get());
    main.resize(560, 700);
    main.show();
    QVERIFY(main.setTask(id));
    QVERIFY(QTest::qWaitForWindowExposed(&main));

    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    TaskDetailWidget* projection = note->detail();
    MarkdownEditor* noteEditor = bodyEditor(projection);
    MarkdownEditor* mainEditor = bodyEditor(&main);
    QTextEdit* noteBody = bodyEdit(projection);
    QVERIFY(noteEditor && mainEditor && noteBody);
    EntityTextSession* session = m_store->textSession(id);
    QVERIFY(session);

    // 加粗：真实键盘全选后触发格式动作（延续真实选择状态）。
    QVERIFY(focusBody(projection));
    QTest::keyClick(noteBody, Qt::Key_A, Qt::ControlModifier);
    noteEditor->applyFormat(MarkdownEditor::FormatAction::Bold);
    QApplication::processEvents();
    QCOMPARE(noteEditor->content(), QStringLiteral("**格式目标行**"));
    QCOMPARE(mainEditor->content(), QStringLiteral("**格式目标行**"));
    QCOMPARE(session->body(), QStringLiteral("**格式目标行**"));
    QTest::keyClick(noteBody, Qt::Key_Z, Qt::ControlModifier); // 一次撤销整体回退
    QApplication::processEvents();
    QCOMPARE(noteEditor->content(), QStringLiteral("格式目标行"));
    QCOMPARE(mainEditor->content(), QStringLiteral("格式目标行"));

    // 任务清单勾选：一行前缀一次加/一次撤。
    noteEditor->applyBlockStyle(MarkdownEditor::BlockStyle::TaskList);
    QApplication::processEvents();
    QVERIFY(noteEditor->content().startsWith(QStringLiteral("- [ ] ")));
    QCOMPARE(mainEditor->content(), noteEditor->content());
    QTest::keyClick(noteBody, Qt::Key_Z, Qt::ControlModifier);
    QApplication::processEvents();
    QCOMPARE(noteEditor->content(), QStringLiteral("格式目标行"));
    QCOMPARE(mainEditor->content(), QStringLiteral("格式目标行"));

    // 粘贴：整段一次落、一次撤销整体移除；两侧镜像。
    QTest::keyClick(noteBody, Qt::Key_End, Qt::ControlModifier);
    QApplication::clipboard()->setText(QStringLiteral("粘贴的整段内容"));
    QTest::keyClick(noteBody, Qt::Key_V, Qt::ControlModifier);
    QApplication::processEvents();
    QCOMPARE(noteEditor->content(), QStringLiteral("格式目标行粘贴的整段内容"));
    QCOMPARE(mainEditor->content(), QStringLiteral("格式目标行粘贴的整段内容"));
    QTest::keyClick(noteBody, Qt::Key_Z, Qt::ControlModifier);
    QApplication::processEvents();
    QCOMPARE(noteEditor->content(), QStringLiteral("格式目标行"));
    QCOMPARE(mainEditor->content(), QStringLiteral("格式目标行"));

    // 重做一步：粘贴内容恢复，两视图同步。
    QTest::keyClick(noteBody, Qt::Key_Y, Qt::ControlModifier);
    QApplication::processEvents();
    QCOMPARE(noteEditor->content(), QStringLiteral("格式目标行粘贴的整段内容"));
    QCOMPARE(mainEditor->content(), QStringLiteral("格式目标行粘贴的整段内容"));

    QVERIFY(note->finishEditing());
    QCOMPARE(dbBody(id), QStringLiteral("格式目标行粘贴的整段内容"));
}

// 两视图宽度独立：同一段长文本各自按自身视口排版。
void DesktopNotesTest::independentWidthsKeepOwnDocumentLayout()
{
    const QString id = seedProjectedTask(QStringLiteral("宽度便签"), QString(), seedPaper(0));
    QVERIFY(!id.isEmpty());
    TaskDetailWidget main(m_store.get());
    main.resize(600, 720);
    main.show();
    QVERIFY(main.setTask(id));
    QVERIFY(QTest::qWaitForWindowExposed(&main));

    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    TaskDetailWidget* projection = note->detail();
    MarkdownEditor* noteEditor = bodyEditor(projection);
    MarkdownEditor* mainEditor = bodyEditor(&main);
    QTextEdit* noteBody = bodyEdit(projection);
    QVERIFY(noteEditor && mainEditor && noteBody);
    QVERIFY(mainEditor->textEditor()->viewport()->width()
            - noteEditor->textEditor()->viewport()->width() >= 100);

    QVERIFY(focusBody(projection));
    QTest::keyClicks(noteBody, QStringLiteral(
        "shared line of synthetic body text that must wrap differently at two widths"));
    QApplication::processEvents();
    QCOMPARE(mainEditor->content(), noteEditor->content());

    // 各自的文档排版宽度都贴合自己的视口，而不是贴对方。
    const qreal noteDocWidth = noteEditor->textEditor()->document()->documentLayout()->documentSize().width();
    const qreal mainDocWidth = mainEditor->textEditor()->document()->documentLayout()->documentSize().width();
    QVERIFY2(qAbs(noteDocWidth - noteEditor->textEditor()->viewport()->width()) <= 12,
             qPrintable(QStringLiteral("note doc %1 viewport %2")
                        .arg(noteDocWidth).arg(noteEditor->textEditor()->viewport()->width())));
    QVERIFY2(qAbs(mainDocWidth - mainEditor->textEditor()->viewport()->width()) <= 12,
             qPrintable(QStringLiteral("main doc %1 viewport %2")
                        .arg(mainDocWidth).arg(mainEditor->textEditor()->viewport()->width())));
    // 窄视图里同一块必然折行更多。
    const QTextBlock noteBlock = noteBody->document()->firstBlock();
    const QTextBlock mainBlock = mainEditor->textEditor()->document()->firstBlock();
    QVERIFY(noteBlock.layout() && mainBlock.layout());
    QVERIFY(noteBlock.layout()->lineCount() > mainBlock.layout()->lineCount());
}

// 远端编辑落在光标/选区之前：标题正向/反向选区保持，正文滚动条稳定。
void DesktopNotesTest::remoteEditPreservesTitleSelectionAndBodyScroll()
{
    QStringList lines;
    for (int line = 0; line < 30; ++line)
        lines.append(QStringLiteral("第%1行：足够长的合成正文内容，用来把详情滚动条撑出范围。").arg(line));
    const QString longBody = lines.join(QLatin1Char('\n'));
    const QString id = seedProjectedTask(QStringLiteral("选区标题"), longBody, seedPaper(0));
    QVERIFY(!id.isEmpty());
    TaskDetailWidget main(m_store.get());
    main.resize(560, 700);
    main.show();
    QVERIFY(main.setTask(id));
    QVERIFY(QTest::qWaitForWindowExposed(&main));

    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    TaskDetailWidget* projection = note->detail();
    QLineEdit* noteTitle = titleEdit(projection);
    QLineEdit* mainTitle = titleEdit(&main);
    QTextEdit* noteBody = bodyEdit(projection);
    QScrollArea* noteScroll = detailScroll(projection);
    QVERIFY(noteTitle && mainTitle && noteBody && noteScroll);

    // 正文滚动稳定：便签正文滚到中部，远端在文首插入一行，滚动值不变。
    QVERIFY(focusBody(projection));
    noteScroll->verticalScrollBar()->setValue(noteScroll->verticalScrollBar()->maximum() / 2);
    QApplication::processEvents();
    const int scrollBefore = noteScroll->verticalScrollBar()->value();
    QVERIFY(scrollBefore > 0);
    QVERIFY(focusBody(&main));
    QTest::keyClick(bodyEdit(&main), Qt::Key_Home, Qt::ControlModifier);
    QTest::keyClicks(bodyEdit(&main), QStringLiteral("head"));
    QApplication::processEvents();
    QCOMPARE(noteScroll->verticalScrollBar()->value(), scrollBefore);
    QVERIFY(noteBody->document()->toPlainText().startsWith(QStringLiteral("head")));

    // 标题正向选区（锚点在左）：远端在行首插入，选区内容与方向保持。
    QVERIFY(focusTitle(projection));
    QTest::keyClick(noteTitle, Qt::Key_A, Qt::ControlModifier);
    const QString selected = noteTitle->selectedText();
    QCOMPARE(selected, QStringLiteral("选区标题"));
    QVERIFY(noteTitle->cursorPosition() == noteTitle->selectionStart() + selected.size());
    QVERIFY(focusTitle(&main));
    QTest::keyClick(mainTitle, Qt::Key_Home);
    QTest::keyClicks(mainTitle, QStringLiteral("Q"));
    QApplication::processEvents();
    QCOMPARE(noteTitle->text(), QStringLiteral("Q选区标题"));
    QCOMPARE(noteTitle->selectedText(), selected);
    QVERIFY(noteTitle->cursorPosition() == noteTitle->selectionStart() + selected.size());

    // 标题反向选区（光标在左、锚点在右）：同样保持方向与内容。
    QVERIFY(focusTitle(projection));
    QTest::keyClick(noteTitle, Qt::Key_End);
    QTest::keyClick(noteTitle, Qt::Key_Home, Qt::ShiftModifier);
    QCOMPARE(noteTitle->selectedText(), noteTitle->text());
    QVERIFY(noteTitle->cursorPosition() == noteTitle->selectionStart());
    QVERIFY(focusTitle(&main));
    QTest::keyClick(mainTitle, Qt::Key_Home);
    QTest::keyClicks(mainTitle, QStringLiteral("W"));
    QApplication::processEvents();
    QCOMPARE(noteTitle->text(), QStringLiteral("WQ选区标题"));
    QCOMPARE(noteTitle->selectedText(), QStringLiteral("Q选区标题"));
    QCOMPARE(noteTitle->selectionStart(), 1);
    QVERIFY(noteTitle->cursorPosition() == noteTitle->selectionStart());

    QVERIFY(note->finishEditing());
    QCOMPARE(m_store->entity(id).title, QStringLiteral("WQ选区标题"));
    QVERIFY(m_store->entity(id).body.startsWith(QStringLiteral("head")));
}

// 渲染态链接标题的部分选区/光标：sourceSelection 对链接是原子的，
// 因此用真实 QTextCursor 位置选取 "el"；远端改其它段落后选区与方向保持精确。
void DesktopNotesTest::remoteEditPreservesPartialLinkCaptionSelection()
{
    const QString body = QStringLiteral("第一段正文\n\n[hello](https://example.com)\n\n第三段正文");
    const QString id = seedProjectedTask(QStringLiteral("链接选区"), body, seedPaper(0));
    QVERIFY(!id.isEmpty());
    TaskDetailWidget main(m_store.get());
    main.resize(560, 700);
    main.show();
    QVERIFY(main.setTask(id));
    QVERIFY(QTest::qWaitForWindowExposed(&main));

    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    QTextEdit* mainBody = bodyEdit(&main);
    QTextEdit* noteBody = bodyEdit(note->detail());
    QVERIFY(mainBody && noteBody);

    const auto captionStart = [](QTextEdit* edit) -> int {
        const QTextCursor found = edit->document()->find(QStringLiteral("hello"));
        return found.isNull() ? -1 : found.selectionStart();
    };
    const auto setCursorRange = [mainBody](int first, int second) {
        QTextCursor cursor(mainBody->document());
        cursor.setPosition(first);
        cursor.setPosition(second, QTextCursor::KeepAnchor);
        mainBody->setTextCursor(cursor);
    };

    // 正向部分选区：选中标题中的 "el"。
    int start = captionStart(mainBody);
    QVERIFY(start >= 0);
    setCursorRange(start + 1, start + 3);
    QCOMPARE(mainBody->textCursor().selectedText(), QStringLiteral("el"));
    QVERIFY(mainBody->textCursor().anchor() < mainBody->textCursor().position());
    // 远端（便签）在其它段落（文首）插入：映射后选区仍是同一处 "el"。
    QVERIFY(focusBody(note->detail()));
    QTest::keyClick(noteBody, Qt::Key_Home, Qt::ControlModifier);
    QTest::keyClicks(noteBody, QStringLiteral("R"));
    QApplication::processEvents();
    QCOMPARE(mainBody->textCursor().selectedText(), QStringLiteral("el"));
    const int shifted = captionStart(mainBody);
    QVERIFY(shifted >= 0);
    QCOMPARE(mainBody->textCursor().selectionStart(), shifted + 1);
    QCOMPARE(mainBody->textCursor().selectionEnd(), shifted + 3);
    QVERIFY(mainBody->textCursor().anchor() < mainBody->textCursor().position());

    // 反向部分选区：方向同样保持。
    setCursorRange(shifted + 3, shifted + 1);
    QCOMPARE(mainBody->textCursor().selectedText(), QStringLiteral("el"));
    QVERIFY(mainBody->textCursor().anchor() > mainBody->textCursor().position());
    QVERIFY(focusBody(note->detail()));
    QTest::keyClick(noteBody, Qt::Key_Home, Qt::ControlModifier);
    QTest::keyClicks(noteBody, QStringLiteral("S"));
    QApplication::processEvents();
    const int shiftedAgain = captionStart(mainBody);
    QVERIFY(shiftedAgain >= 0);
    QCOMPARE(mainBody->textCursor().selectedText(), QStringLiteral("el"));
    QCOMPARE(mainBody->textCursor().selectionStart(), shiftedAgain + 1);
    QCOMPARE(mainBody->textCursor().selectionEnd(), shiftedAgain + 3);
    QVERIFY(mainBody->textCursor().anchor() > mainBody->textCursor().position());

    // 光标停在标题中间：远端改动后位置精确映射、仍落在链接标题内。
    setCursorRange(shiftedAgain + 2, shiftedAgain + 2);
    QVERIFY(mainBody->textCursor().selectedText().isEmpty());
    QVERIFY(focusBody(note->detail()));
    QTest::keyClick(noteBody, Qt::Key_Home, Qt::ControlModifier);
    QTest::keyClicks(noteBody, QStringLiteral("T"));
    QApplication::processEvents();
    const int shiftedMore = captionStart(mainBody);
    QVERIFY(shiftedMore >= 0);
    QCOMPARE(mainBody->textCursor().position(), shiftedMore + 2);
    QCOMPARE(mainBody->textCursor().charFormat().anchorHref(),
             QStringLiteral("https://example.com"));

    QVERIFY(note->finishEditing());
    QVERIFY(m_store->entity(id).body.startsWith(QStringLiteral("TSR")));

    setCursorRange(0, 3);
    QVERIFY(focusBody(note->detail()));
    QTest::keyClick(noteBody, Qt::Key_Home, Qt::ControlModifier);
    for (int index = 0; index < 3; ++index) QTest::keyClick(noteBody, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClicks(noteBody, QStringLiteral("UV"));
    QVERIFY(!mainBody->textCursor().hasSelection());
    QCOMPARE(mainBody->textCursor().position(), 2);

    auto* mainTitle = titleEdit(&main);
    auto* noteTitle = titleEdit(note->detail());
    QVERIFY(mainTitle && noteTitle);
    mainTitle->setSelection(0, 2);
    QVERIFY(focusTitle(note->detail()));
    noteTitle->setSelection(0, 2);
    QTest::keyClicks(noteTitle, QStringLiteral("V"));
    QVERIFY(!mainTitle->hasSelectedText());
    QCOMPARE(mainTitle->cursorPosition(), 1);
    QVERIFY(note->finishEditing());
    QVERIFY(m_store->entity(id).body.startsWith(QStringLiteral("UV")));
    QCOMPARE(m_store->entity(id).title, QStringLiteral("V选区"));
}

// 视图在任务处于回收站时打开（无文本会话），恢复后必须绑定会话并真实可编辑落库。
void DesktopNotesTest::trashedThenRestoredViewBindsSessionAndSaves()
{
    const QString id = seedProjectedTask(QStringLiteral("回收站视图"), QStringLiteral("回收站正文"),
                                         seedPaper(0));
    QVERIFY(!id.isEmpty());
    QVERIFY(m_store->trashTask(id));
    QVERIFY(!m_store->textSession(id)); // 回收站目标不建会话

    TaskDetailWidget main(m_store.get());
    main.resize(560, 700);
    main.show();
    QVERIFY(main.setTask(id)); // 视图在已回收状态打开：只读、无会话
    QVERIFY(!titleEdit(&main)->text().isEmpty());
    QApplication::processEvents();

    QVERIFY(m_store->restoreTask(id));
    QApplication::processEvents();
    EntityTextSession* session = m_store->textSession(id);
    QVERIFY(session);

    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    QCOMPARE(notes.count(), 1);

    // 恢复后的旧视图必须已绑定会话：真实输入立即进入权威草稿并镜像到便签。
    QVERIFY(focusTitle(&main));
    QTest::keyClick(titleEdit(&main), Qt::Key_A, Qt::ControlModifier);
    QVERIFY(commitText(titleEdit(&main), QStringLiteral("恢复标题"))); // 中文走输入法提交
    QApplication::processEvents();
    QCOMPARE(session->title(), QStringLiteral("恢复标题"));
    QCOMPARE(titleEdit(note->detail())->text(), QStringLiteral("恢复标题"));

    QVERIFY(focusBody(&main));
    QTest::keyClick(bodyEdit(&main), Qt::Key_End, Qt::ControlModifier);
    QVERIFY(commitText(bodyEdit(&main), QStringLiteral("+恢复")));
    QApplication::processEvents();
    QCOMPARE(session->body(), QStringLiteral("回收站正文+恢复"));
    QCOMPARE(bodyEditor(note->detail())->content(), QStringLiteral("回收站正文+恢复"));

    QVERIFY(note->finishEditing());
    QCOMPARE(dbTitle(id), QStringLiteral("恢复标题"));
    QCOMPARE(dbBody(id), QStringLiteral("回收站正文+恢复"));
}

// IME：预编辑只留在本地视图；合成期间远端不重叠修改被推迟；取消/提交后重放。
void DesktopNotesTest::imePreeditLocalCancelCommitAndRemoteRebase()
{
    const QString seed = QStringLiteral("甲乙\n丙丁");
    const QString id = seedProjectedTask(QStringLiteral("输入法便签"), seed, seedPaper(0));
    QVERIFY(!id.isEmpty());
    TaskDetailWidget main(m_store.get());
    main.resize(560, 700);
    main.show();
    QVERIFY(main.setTask(id));
    QVERIFY(QTest::qWaitForWindowExposed(&main));

    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    TaskDetailWidget* projection = note->detail();
    MarkdownEditor* noteEditor = bodyEditor(projection);
    MarkdownEditor* mainEditor = bodyEditor(&main);
    QTextEdit* noteBody = bodyEdit(projection);
    QVERIFY(noteEditor && mainEditor && noteBody);
    EntityTextSession* session = m_store->textSession(id);
    QVERIFY(session);

    // 进入预编辑：内容、会话、另一视图与数据库都保持基线。
    QVERIFY(focusBody(projection));
    QTest::keyClick(noteBody, Qt::Key_End, Qt::ControlModifier);
    QInputMethodEvent preedit(QStringLiteral("pin"), {});
    QApplication::sendEvent(noteBody, &preedit);
    QApplication::processEvents();
    QVERIFY(noteEditor->isComposing());
    QCOMPARE(noteEditor->content(), seed);
    QCOMPARE(session->body(), seed);
    QCOMPARE(mainEditor->content(), seed);

    // 合成期间远端（主窗口侧来源）在文首插入：合成视图推迟投影。
    const QString remoteBody = QStringLiteral("Z") + seed;
    QVERIFY(session->updateBody(remoteBody, &main));
    QApplication::processEvents();
    QCOMPARE(session->body(), remoteBody);
    QCOMPARE(mainEditor->content(), remoteBody);
    QCOMPARE(noteEditor->content(), seed); // 预编辑视图暂不刷新
    QVERIFY(noteEditor->isComposing());

    // 取消合成：预编辑消失，被推迟的远端修改按不重叠差异重放。
    QInputMethodEvent cancel;
    QApplication::sendEvent(noteBody, &cancel);
    QApplication::processEvents();
    QVERIFY(!noteEditor->isComposing());
    QTRY_COMPARE(noteEditor->content(), remoteBody);
    QVERIFY(!noteEditor->content().contains(QStringLiteral("pin")));
    QCOMPARE(dbBody(id), seed); // 尚未持久化

    // 提交合成：提交文本进入内容并即时镜像。
    QTest::keyClick(noteBody, Qt::Key_End, Qt::ControlModifier);
    QInputMethodEvent commit;
    commit.setCommitString(QStringLiteral("完成"));
    QApplication::sendEvent(noteBody, &commit);
    QApplication::processEvents();
    QTRY_COMPARE(noteEditor->content(), remoteBody + QStringLiteral("完成"));
    QCOMPARE(mainEditor->content(), remoteBody + QStringLiteral("完成"));
    QCOMPARE(session->body(), remoteBody + QStringLiteral("完成"));
    QCOMPARE(dbBody(id), seed);
    QVERIFY(note->finishEditing());
    QCOMPARE(dbBody(id), remoteBody + QStringLiteral("完成"));
}

// 同一事件携带提交串与下一段预编辑（IME 常见形态）：
// 提交内容立即镜像，预编辑字符串绝不落入任何一侧或数据库。
void DesktopNotesTest::imeCommitWithNextPreeditMirrorsImmediately()
{
    const QString seed = QStringLiteral("基础正文");
    const QString id = seedProjectedTask(QStringLiteral("连续输入法"), seed, seedPaper(0));
    QVERIFY(!id.isEmpty());
    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    TaskDetailWidget* projection = note->detail();
    MarkdownEditor* noteEditor = bodyEditor(projection);
    QTextEdit* noteBody = bodyEdit(projection);
    QVERIFY(noteEditor && noteBody);
    EntityTextSession* session = m_store->textSession(id);
    QVERIFY(session);

    QVERIFY(focusBody(projection));
    QTest::keyClick(noteBody, Qt::Key_End, Qt::ControlModifier);
    QInputMethodEvent commitWithPreedit(QStringLiteral("hao"), {});
    commitWithPreedit.setCommitString(QStringLiteral("你"));
    QApplication::sendEvent(noteBody, &commitWithPreedit);
    QApplication::processEvents();

    QVERIFY(noteEditor->isComposing()); // 新预编辑仍在合成
    QTRY_VERIFY(session->body().contains(QStringLiteral("你"))); // 提交立即进入权威草稿
    QTRY_VERIFY(noteEditor->content().contains(QStringLiteral("你")));
    QVERIFY(!session->body().contains(QStringLiteral("hao"))); // 预编辑绝不落草稿
    QVERIFY(!noteEditor->content().contains(QStringLiteral("hao")));
    QCOMPARE(dbBody(id), seed); // 未持久化

    // 结束合成后内容保留提交串、没有预编辑串，可正常落盘。
    QInputMethodEvent done;
    QApplication::sendEvent(noteBody, &done);
    QApplication::processEvents();
    QVERIFY(!noteEditor->isComposing());
    QCOMPARE(noteEditor->content(), seed + QStringLiteral("你"));
    QVERIFY(note->finishEditing());
    QCOMPARE(dbBody(id), seed + QStringLiteral("你"));
    QVERIFY(!dbBody(id).contains(QStringLiteral("hao")));
}

// ---------------------------------------------------------------------------
// 字段提交与投影
// ---------------------------------------------------------------------------

// 底栏日期胶囊打开完整的共享弹层，快捷选择后两视图都刷新。
void DesktopNotesTest::dateChipCommitMirrorsBothViews_data()
{
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
}

void DesktopNotesTest::dateChipCommitMirrorsBothViews()
{
    QFETCH(bool, dark);
    auto& theme = ThemeManager::instance();
    const auto previous = theme.preference();
    auto restoreTheme = qScopeGuard([&]() { theme.setPreference(previous); });
    theme.setPreference(dark ? ThemeManager::Preference::Dark : ThemeManager::Preference::Light);
    const QDate today = toAppTime(appNow()).date();
    const QDate day = today.addDays(1);
    const QString id = seedProjectedTask(QStringLiteral("日期便签"), QString(), seedPaper(0));
    QVERIFY(!id.isEmpty());
    TaskDetailWidget main(m_store.get());
    main.resize(560, 700);
    main.show();
    QVERIFY(main.setTask(id));
    QVERIFY(QTest::qWaitForWindowExposed(&main));

    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    auto* noteChip = note->findChild<QPushButton*>(QStringLiteral("DateChip"));
    auto* mainChip = main.findChild<QPushButton*>(QStringLiteral("DateChip"));
    QVERIFY(noteChip && mainChip);
    QVERIFY(noteChip->isVisible()); // 日期胶囊不依赖悬停
    QCOMPARE(noteChip->text(), QStringLiteral("设置日期"));

    QVERIFY(clickWidget(noteChip, QPoint(noteChip->width() / 2, noteChip->height() / 2)));
    DatePickerPopup* picker = nullptr;
    QVERIFY(waitUntil([&]() {
        picker = note->findChild<DatePickerPopup*>();
        return picker && picker->isVisible();
    }));
    auto* calendar = picker->findChild<DateCalendarPanel*>();
    QVERIFY(calendar);
    const auto quickButtons = picker->findChildren<QToolButton*>(QStringLiteral("DatePickerQuick"));
    const auto navButtons = calendar->findChildren<QToolButton*>(QStringLiteral("DatePickerNav"));
    QCOMPARE(quickButtons.size(), 4);
    QCOMPARE(navButtons.size(), 3);
    const QImage popupImage = renderWidgetToImage(picker);
    const qreal dpr = popupImage.devicePixelRatio();
    const QColor ink = theme.color(ds::ColorToken::TextSecondary);
    for (QToolButton* button : quickButtons + navButtons) {
        QVERIFY(button->isVisible() && button->isEnabled());
        const QRect center(button->mapTo(picker, QPoint(3, 3)), button->size() - QSize(6, 6));
        int inkPixels = 0;
        for (int y = center.top(); y <= center.bottom(); ++y) {
            for (int x = center.left(); x <= center.right(); ++x) {
                const QColor pixel = logicalPixel(popupImage, dpr, x, y);
                if (pixel.alpha() > 240 && colorDistance(pixel, ink) < 40) ++inkPixels;
            }
        }
        QVERIFY2(inkPixels >= 6, qPrintable(QStringLiteral("%1 %2: %3 pixels")
            .arg(button->objectName(), button->accessibleName()).arg(inkPixels)));
    }

    auto* confirm = picker->findChild<QPushButton*>(QStringLiteral("DatePickerConfirm"));
    QVERIFY(confirm && confirm->isVisible());
    const QPoint confirmLeft = confirm->mapTo(picker, QPoint(10, confirm->height() / 2));
    const QColor confirmFill = logicalPixel(popupImage, dpr, confirmLeft.x(), confirmLeft.y());
    QCOMPARE(confirmFill, theme.color(ds::ColorToken::Primary));

    const QDate month = calendar->visibleMonth();
    QTest::mouseClick(navButtons.at(0), Qt::LeftButton);
    QCOMPARE(calendar->visibleMonth(), month.addMonths(-1));
    QTest::mouseClick(navButtons.at(2), Qt::LeftButton);
    QTest::mouseClick(navButtons.at(2), Qt::LeftButton);
    QCOMPARE(calendar->visibleMonth(), month.addMonths(1));
    QTest::mouseClick(navButtons.at(1), Qt::LeftButton);
    QVERIFY(picker->isVisible());
    QCOMPARE(calendar->visibleMonth(), month);
    QCOMPARE(picker->allDayDate(), today);

    auto* tomorrow = quickButtons.at(1);
    QCOMPARE(tomorrow->accessibleName(), QStringLiteral("明天"));
    QTest::mouseClick(tomorrow, Qt::LeftButton);
    QApplication::processEvents();

    QTRY_COMPARE(m_store->entity(id).allDayDate, day);
    QVERIFY(!m_store->entity(id).dueAt.isValid());
    QTRY_VERIFY(noteChip->text() != QStringLiteral("设置日期"));
    QTRY_VERIFY(mainChip->text() != QStringLiteral("设置日期"));
    const QString format = day.year() == today.year() ? QStringLiteral("M月d日") : QStringLiteral("yyyy/MM/dd");
    const QString label = day.toString(format) + QStringLiteral(", 明天");
    QCOMPARE(noteChip->text(), label);
    QCOMPARE(mainChip->text(), label);
}

// 命中区真实拖动改变纸面尺寸；桌面状态在 flush 后持久化。
void DesktopNotesTest::resizeAreaDragResizesPaperAndPersists()
{
    const QString id = seedProjectedTask(QStringLiteral("缩放便签"), QString(), seedPaper(0));
    QVERIFY(!id.isEmpty());
    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    const QRect paperBefore = note->paperGeometry();
    QVERIFY(!note->desktopDirty());

    // 右缘 +30：宽度增加，位置不动。
    auto* right = note->findChild<QWidget*>(QStringLiteral("NoteResizeRight"));
    QVERIFY(right && right->isVisible() && right->width() > 0 && right->height() > 0);
    const QPoint rightCenter = QRect(right->rect()).center();
    sendDrag(right, rightCenter, rightCenter + QPoint(30, 0));
    QCOMPARE(note->paperGeometry(), paperBefore.adjusted(0, 0, 30, 0));
    QVERIFY(note->desktopDirty());

    // 右下角 +20/+24：双向同时放大。
    auto* corner = note->findChild<QWidget*>(QStringLiteral("NoteResizeBottomRight"));
    QVERIFY(corner && corner->isVisible());
    const QPoint cornerCenter = QRect(corner->rect()).center();
    const QRect grew = note->paperGeometry();
    sendDrag(corner, cornerCenter, cornerCenter + QPoint(20, 24));
    QCOMPARE(note->paperGeometry(), grew.adjusted(0, 0, 20, 24));

    // 左缘大幅内拖：宽度收敛到最小纸面 220，右缘与高度不动。
    auto* left = note->findChild<QWidget*>(QStringLiteral("NoteResizeLeft"));
    QVERIFY(left && left->isVisible());
    const QPoint leftCenter = QRect(left->rect()).center();
    const QRect beforeShrink = note->paperGeometry();
    sendDrag(left, leftCenter, leftCenter + QPoint(600, 0));
    QCOMPARE(note->paperGeometry().width(), 220);
    QCOMPARE(note->paperGeometry().height(), beforeShrink.height());
    QCOMPARE(note->paperGeometry().x(), beforeShrink.x() + beforeShrink.width() - 220);

    // 原生平台：八个命中区与拖动条中心全局命中核验（逐行执行，避免整窗遮挡误判）。
    if (!headlessPlatform()) {
        const char* const names[] = {"NoteResizeLeft", "NoteResizeRight", "NoteResizeTop",
                                     "NoteResizeBottom", "NoteResizeTopLeft", "NoteResizeTopRight",
                                     "NoteResizeBottomLeft", "NoteResizeBottomRight"};
        for (const char* name : names) {
            auto* area = note->findChild<QWidget*>(QString::fromLatin1(name));
            QVERIFY2(area && QApplication::widgetAt(area->mapToGlobal(area->rect().center())) == area,
                     name);
        }
        auto* titleBar = note->findChild<QWidget*>(QStringLiteral("TitleBar"));
        QVERIFY(titleBar);
        QVERIFY(QApplication::widgetAt(titleBar->mapToGlobal(QPoint(30, 10))) == titleBar);
    }

    QVERIFY(note->flush());
    QCOMPARE(m_store->desktopNote(id).geometry, note->geometry());
    QCOMPARE(m_store->desktopNote(id).geometry.adjusted(PopupShadowMargin, PopupShadowMargin,
                                                        -PopupShadowMargin, -PopupShadowMargin),
             note->paperGeometry());
}

// 标题条原始拖动走 NoteManager 安装的真实吸附提供者：
// 纸面贴齐相邻便签（留 8px 间距）与工作区左缘，且绝不动邻居。
void DesktopNotesTest::titleBarDragSnapsPaperToPeersAndScreen()
{
    const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
    const int anchorX = avail.left() + qMax<int>(320, avail.width() - 480);
    const QRect paperA(anchorX, avail.top() + 60, 300, 260);
    const QRect paperB(anchorX - 290, avail.top() + 80, 260, 240);
    const QString peerId = seedProjectedTask(QStringLiteral("邻居便签"), QString(), paperB);
    const QString id = seedProjectedTask(QStringLiteral("吸附便签"), QString(), paperA);
    QVERIFY(!peerId.isEmpty() && !id.isEmpty());
    NoteManager notes(m_store.get());
    NoteWidget* peer = openProjection(notes, peerId);
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(peer && note);
    QCOMPARE(note->paperGeometry(), paperA);
    QCOMPARE(peer->paperGeometry(), paperB);

    auto* titleBar = note->findChild<QWidget*>(QStringLiteral("TitleBar"));
    QVERIFY(titleBar && titleBar->isVisible());
    // 原始指针拖动：落点比“留 8px 间距”的位置多 3px，真实提供者阈值内吸回间距位。
    const int targetX = peer->paperGeometry().x() + peer->paperGeometry().width() + 8;
    const int dx = (targetX + 3) - paperA.x();
    sendDrag(titleBar, QPoint(40, 10), QPoint(40 + dx, 10));
    QCOMPARE(note->paperGeometry().x(), targetX);            // 吸附到 peer + gap 8
    QVERIFY(note->paperGeometry().x() != paperA.x() + dx);   // 结果不同于原始落点
    QCOMPARE(note->paperGeometry().y(), paperA.y());
    QCOMPARE(peer->paperGeometry(), paperB);                 // 拖动绝不移动邻居
    QVERIFY(note->desktopDirty());

    // 再拖到工作区左缘附近：纸面左缘贴齐 availableGeometry 左缘。
    const int rawX = avail.left() + 4;
    const int toLeftEdge = rawX - note->paperGeometry().x();
    sendDrag(titleBar, QPoint(40, 10), QPoint(40 + toLeftEdge, 10));
    QCOMPARE(note->paperGeometry().x(), avail.left());
    QVERIFY(note->paperGeometry().x() != rawX);
    QCOMPARE(note->paperGeometry().y(), paperA.y());
    QCOMPARE(peer->paperGeometry(), paperB);

    QVERIFY(note->flush());
    QCOMPARE(m_store->desktopNote(id).geometry, note->geometry());
}

// ---------------------------------------------------------------------------
// 自绘
// ---------------------------------------------------------------------------

// 内存渲染（浅色+深色）：圆角裁掉纸面直角、唯一一圈边框、外缘阴影半透明。
void DesktopNotesTest::paperRenderingCornersBorderShadowBothThemes()
{
    Entity presentation;
    presentation.title = QStringLiteral("渲染便签");
    presentation.color = NoteColor::Yellow;
    const QRect paper(80, 80, 300, 260);
    presentation.geometry = windowForPaper(paper);
    NoteWidget note(presentation);
    note.show();
    if (headlessPlatform()) QApplication::setActiveWindow(&note);
    QVERIFY(QTest::qWaitForWindowExposed(&note));
    QApplication::processEvents();

    const QColor body(ds::raw::L_NoteYellow);
    const auto assertRendering = [&]() {
        const qreal dpr = note.devicePixelRatioF();
        const QImage image = renderWidgetToImage(&note);
        // 纸面中心：不透明的纸面色。
        const QColor center = logicalPixel(image, dpr, note.width() / 2, note.height() / 2);
        QCOMPARE(center.alpha(), 255);
        QVERIFY2(colorDistance(center, body) <= 10,
                 qPrintable(QStringLiteral("center %1 body %2").arg(center.name(), body.name())));
        // 圆角：纸面直角像素不属于纸面本体（只剩阴影）。
        const QColor corner = logicalPixel(image, dpr, PopupShadowMargin, PopupShadowMargin);
        QVERIFY2(corner.alpha() < 128,
                 qPrintable(QStringLiteral("corner alpha %1").arg(corner.alpha())));
        // 唯一一圈边框：左缘中点处有一条与纸面色不同的 1-2px 描边，往内即纸面本色。
        const int midY = note.height() / 2;
        int borderPixels = 0;
        for (int x = PopupShadowMargin; x <= PopupShadowMargin + 4; ++x) {
            const QColor pixel = logicalPixel(image, dpr, x, midY);
            if (pixel.alpha() > 100 && colorDistance(pixel, body) > 25) ++borderPixels;
        }
        QVERIFY2(borderPixels >= 1 && borderPixels <= 2,
                 qPrintable(QStringLiteral("border band %1 px").arg(borderPixels)));
        const QColor inside = logicalPixel(image, dpr, PopupShadowMargin + 7, midY);
        QVERIFY2(colorDistance(inside, body) <= 12,
                 qPrintable(QStringLiteral("inside %1 body %2").arg(inside.name(), body.name())));
        // 外缘透明带：阴影半透明、绝无任何不透明像素，纸面下方确有阴影。
        int maxShadowAlpha = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const bool outsidePaper = x < qRound(PopupShadowMargin * dpr)
                    || y < qRound(PopupShadowMargin * dpr)
                    || x >= qRound((note.width() - PopupShadowMargin) * dpr)
                    || y >= qRound((note.height() - PopupShadowMargin) * dpr);
                if (!outsidePaper) continue;
                const int alpha = image.pixelColor(x, y).alpha();
                QVERIFY2(alpha < 255, qPrintable(QStringLiteral("opaque margin at %1,%2 alpha %3").arg(x).arg(y).arg(alpha)));
                maxShadowAlpha = qMax(maxShadowAlpha, alpha);
            }
        QVERIFY2(maxShadowAlpha > 4 && maxShadowAlpha < 255,
                 qPrintable(QStringLiteral("shadow max alpha %1").arg(maxShadowAlpha)));
    };

    assertRendering();
    ThemeManager::instance().setPreference(ThemeManager::Preference::Dark);
    QApplication::processEvents();
    QApplication::processEvents();
    assertRendering();
    ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
    QApplication::processEvents();
}

void DesktopNotesTest::paperPaletteAndMarkupStayFixed()
{
    const QRgb papers[] = {ds::raw::L_NoteYellow, ds::raw::L_NoteBlue, ds::raw::L_NoteGreen,
                           ds::raw::L_NotePink, ds::raw::L_NotePurple, ds::raw::White};
    const auto verifyPalette = [&]() {
        for (int i = 0; i < ColorCount; ++i) {
            const auto spec = ColorTheme::spec(colorFromInt(i));
            QCOMPARE(QColor(spec.body), QColor(papers[i]));
            QCOMPARE(QColor(spec.text), QColor(ds::raw::L_Text));
            QCOMPARE(QColor(spec.barText), QColor(ds::raw::L_TextSecondary));
            QCOMPARE(QColor(spec.selection), QColor(ds::raw::PrimarySoft));
        }
    };
    const QString source = QStringLiteral("`code`\n<mark data-color=\"blue\">mark</mark>\n> quote");
    const QString id = seedProjectedTask(QStringLiteral("双色主题"), source, seedPaper(0));
    QVERIFY(!id.isEmpty());
    const auto verifyEditor = [](NoteWidget* note) {
        auto* editor = note->findChild<MarkdownEditor*>();
        QVERIFY(editor);
        auto* body = editor->textEditor();
        QCOMPARE(body->palette().color(QPalette::Text), QColor(ds::raw::L_Text));
        QCOMPARE(body->palette().color(QPalette::Highlight), QColor(ds::raw::PrimarySoft));
        for (const auto token : {ds::ColorToken::Field, ds::ColorToken::HighlightBlue}) {
            const QString word = token == ds::ColorToken::Field ? QStringLiteral("code") : QStringLiteral("mark");
            const auto fragment = body->document()->find(word);
            QVERIFY(!fragment.isNull());
            QCOMPARE(fragment.charFormat().background().color(), ds::color(token, ds::Theme::Light));
        }
    };
    NoteManager notes(m_store.get());
    NoteWidget* note = openProjection(notes, id);
    QVERIFY(note);
    verifyPalette();
    verifyEditor(note);

    ThemeManager::instance().setPreference(ThemeManager::Preference::Dark);
    QApplication::processEvents();
    verifyPalette();
    verifyEditor(note);
    const QString coldId = seedProjectedTask(QStringLiteral("深色主题中新建"), source, seedPaper(1));
    NoteWidget* cold = openProjection(notes, coldId);
    QVERIFY(cold);
    verifyEditor(cold);
    ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
}

QTEST_MAIN(DesktopNotesTest)
#include "tst_desktopnotes.moc"
