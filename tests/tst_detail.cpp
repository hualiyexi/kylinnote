#include <cstdio>
#include <functional>
#include <memory>

#include <QAbstractButton>
#include <QAbstractTextDocumentLayout>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFrame>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFragment>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QWindow>
#include <QtMath>
#include <QtTest>

#include "app/Settings.h"
#include "app/TaskStore.h"
#include "db/Database.h"
#include "domain/TaskGrouping.h"
#include "domain/ViewSpec.h"
#include "platform/SystemShadow.h"
#include "ui/ActivityTimelineWidget.h"
#include "ui/AppIcons.h"
#include "ui/AppStyle.h"
#include "ui/AttachmentWidget.h"
#include "ui/DisplaySettingsDialog.h"
#include "ui/DatePickerPopup.h"
#include "ui/MarkdownEditor.h"
#include "ui/NavigationListWidget.h"
#include "ui/PopupMenu.h"
#include "ui/TaskDetailWidget.h"
#include "ui/TaskDelegate.h"
#include "ui/ToastIsland.h"
#include "ui/DetailPopups.h"
#include "ui/TaskListModel.h"
#include "ui/TaskListView.h"
#include "ui/design/ThemeManager.h"
#include "ui/design/TypeSystem.h"
#include "ui/TaskPanel.h"

using namespace kl;

namespace {
// Qt 5.12 Windows 的 QMessageBox SC_CLOSE Show 钩子会解引用 offscreen 的空 native interface。
// 仅把真实消息框交互分层为 native-only；不吞 Qt Show、不伪造按钮或业务结果。
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

// 不把事件强行发给已知 editor：先核对实际命中的最深子控件，再由该控件收点击。
// 原生平台额外核对全局 widgetAt，防止覆盖详情/别的窗口挡住 viewport。
bool viewportIsActualHit(QWidget& window, QTextEdit* body, const QPoint& point) {
    if (!body || window.childAt(point) != body->viewport()) return false;
    const QString platform = QGuiApplication::platformName();
    return platform == QLatin1String("offscreen") || platform == QLatin1String("minimal")
        || QApplication::widgetAt(window.mapToGlobal(point)) == body->viewport();
}

bool clickActualChild(QWidget& window, const QPoint& point) {
    // QTest 合成点击不负责原生窗口激活；offscreen 的临时格式浮层关闭后也不恢复激活。
    // 只激活本 fixture 自己的顶层窗，绝不预设 editor/title 的控件焦点。
    if (!window.isActiveWindow()) {
        window.activateWindow();
        if (!QTest::qWaitForWindowActive(&window)) return false;
    }
    QWidget* hit = window.childAt(point);
    if (!hit) return false;
    QTest::mouseClick(hit, Qt::LeftButton, Qt::NoModifier, hit->mapFrom(&window, point));
    return true;
}

QRect inkBounds(const QImage& source, const QColor& background = QColor()) {
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    QRect bounds;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            const int contrast = background.isValid()
                ? qMax(qAbs(pixel.red() - background.red()),
                    qMax(qAbs(pixel.green() - background.green()), qAbs(pixel.blue() - background.blue())))
                : pixel.alpha();
            if (pixel.alpha() > 40 && contrast > 40)
                bounds = bounds.united(QRect(x, y, 1, 1));
        }
    }
    return bounds;
}

// 只在内存渲染真实 viewport，不抓桌面/不落盘。文字、布局与像素分别采集，避免
// content()==source 掩盖“文档存在、viewport被压扁/裁掉/文字透明”的回归。
struct BodyViewportProbe {
    QRect editorRect, textEditRect, viewportRect, visibleRect, firstCaret, currentCaret, ink;
    QRectF firstBlockRect, firstLineRect;
    QPointF textLayoutPosition;
    QSizeF documentSize;
    QString documentText, currentPage;
    QColor foreground, background;
    int inkPixels = 0, textContrast = 0, logicalInkWidth = 0;
    int blocks = 0, laidOutLines = 0, unlaidOutTextBlocks = 0;
    int minimumTextHeight = 0, maximumTextHeight = 0, innerScroll = 0;
    bool visible = false, currentEditor = false;

    QString describe() const {
        QString message; QDebug out(&message);
        out.nospace() << "editor=" << editorRect << " textEdit=" << textEditRect
            << " viewport=" << viewportRect << " visible=" << visibleRect
            << " page=" << currentPage << " visibleEditor=" << visible << '/' << currentEditor
            << " document=" << documentSize << " text=" << documentText
            << " block=" << firstBlockRect << " layoutPos=" << textLayoutPosition << " line=" << firstLineRect
            << " blocks/lines/unlaidOut=" << blocks << '/' << laidOutLines << '/' << unlaidOutTextBlocks
            << " firstCaret=" << firstCaret << " currentCaret=" << currentCaret
            << " min/max=" << minimumTextHeight << '/' << maximumTextHeight << " innerScroll=" << innerScroll
            << " fg=" << foreground << " bg=" << background << " contrast=" << textContrast
            << " ink=" << ink << " inkPixels=" << inkPixels << " logicalInkWidth=" << logicalInkWidth;
        return message;
    }

    static BodyViewportProbe capture(MarkdownEditor* editor) {
        BodyViewportProbe result;
        auto* text = editor->textEditor(); auto* viewport = text->viewport();
        auto* stack = editor->findChild<QStackedWidget*>();
        result.editorRect = editor->geometry(); result.textEditRect = text->geometry();
        result.viewportRect = viewport->rect(); result.visibleRect = viewport->visibleRegion().boundingRect();
        result.visible = text->isVisible(); result.currentEditor = stack && stack->currentWidget() == text;
        result.currentPage = stack && stack->currentWidget() ? stack->currentWidget()->objectName() : QStringLiteral("<none>");
        result.minimumTextHeight = text->minimumHeight(); result.maximumTextHeight = text->maximumHeight();
        result.innerScroll = text->verticalScrollBar()->value();
        result.documentText = text->document()->toPlainText();
        result.documentSize = text->document()->documentLayout()->documentSize();
        const QTextBlock first = text->document()->firstBlock();
        result.firstBlockRect = text->document()->documentLayout()->blockBoundingRect(first);
        if (first.layout()) {
            result.textLayoutPosition = first.layout()->position();
            if (first.layout()->lineCount() > 0) result.firstLineRect = first.layout()->lineAt(0).naturalTextRect();
        }
        for (QTextBlock block = first; block.isValid(); block = block.next()) {
            ++result.blocks;
            text->document()->documentLayout()->blockBoundingRect(block);
            const int lines = block.layout() ? block.layout()->lineCount() : 0;
            result.laidOutLines += lines;
            if (!block.text().isEmpty() && lines == 0) ++result.unlaidOutTextBlocks;
        }
        QTextCursor start(text->document()); start.movePosition(QTextCursor::Start);
        result.firstCaret = text->cursorRect(start); result.currentCaret = text->cursorRect();
        result.foreground = text->palette().color(QPalette::Text);
        for (QTextBlock::iterator it = first.begin(); !it.atEnd(); ++it) {
            const QBrush brush = it.fragment().charFormat().foreground();
            if (brush.style() != Qt::NoBrush) { result.foreground = brush.color(); break; }
        }
        result.background = ThemeManager::instance().color(ds::ColorToken::Surface);
        if (viewport->width() > 0 && viewport->height() > 0) {
            const qreal dpr = viewport->devicePixelRatioF();
            QImage canvas(qCeil(viewport->width() * dpr), qCeil(viewport->height() * dpr), QImage::Format_ARGB32_Premultiplied);
            canvas.setDevicePixelRatio(dpr); canvas.fill(result.background);
            { QPainter painter(&canvas); viewport->render(&painter); }
            result.background = canvas.pixelColor(canvas.width() - 1, canvas.height() - 1);
            result.ink = inkBounds(canvas, result.background);
            result.logicalInkWidth = qRound(result.ink.width() / dpr);
            for (int y = 0; y < canvas.height(); ++y) for (int x = 0; x < canvas.width(); ++x) {
                const QColor pixel = canvas.pixelColor(x, y);
                const int contrast = qMax(qAbs(pixel.red() - result.background.red()),
                    qMax(qAbs(pixel.green() - result.background.green()), qAbs(pixel.blue() - result.background.blue())));
                if (pixel.alpha() > 40 && contrast > 40) ++result.inkPixels;
            }
        }
        result.textContrast = qMax(qAbs(result.foreground.red() - result.background.red()),
            qMax(qAbs(result.foreground.green() - result.background.green()), qAbs(result.foreground.blue() - result.background.blue())));
        return result;
    }
};

// All assertions stay outside timer callbacks. Every exec() path has a watchdog,
// including missing menus/dialogs, so an assertion cannot strand a nested loop.
bool withMenu(QWidget& owner, QToolButton* button, const QString& objectName,
              const std::function<void(PopupMenu*)>& inspect) {
    if (!button || !button->isVisible() || !button->isEnabled()) return false;
    bool handled = false;
    QElapsedTimer elapsed; elapsed.start();
    QTimer poll; poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, &poll, [&]() {
        auto* menu = owner.findChild<PopupMenu*>(objectName);
        if (menu && menu->isVisible()) {
            poll.stop(); handled = true;
            QPointer<PopupMenu> guard(menu);
            inspect(menu);
            if (guard && guard->isVisible()) guard->close();
        } else if (elapsed.elapsed() > 2000) {
            poll.stop();
            if (QWidget* popup = QApplication::activePopupWidget()) popup->close();
        }
    });
    poll.start(); QTest::mouseClick(button, Qt::LeftButton);
    while (poll.isActive() && elapsed.elapsed() < 2200) QTest::qWait(5);
    return handled;
}

bool keyboardChoose(QMenu* menu, const QString& objectName) {
    if (!menu || !menu->isVisible()) return false;
    QList<QAction*> selectable;
    for (QAction* action : menu->actions())
        if (action->isVisible() && action->isEnabled() && !action->isSeparator()) selectable.append(action);
    int target = -1;
    for (int index = 0; index < selectable.size(); ++index)
        if (selectable.at(index)->objectName() == objectName) target = index;
    if (target < 0) return false;
    // Qt 5.12 的菜单 Home 行为随平台样式而异；从无选中项用 Down 走真实导航。
    menu->setActiveAction(nullptr);
    QTest::keyClick(menu, Qt::Key_Down);
    for (int index = 0; index < target; ++index) QTest::keyClick(menu, Qt::Key_Down);
    if (menu->activeAction() != selectable.at(target)) {
        qWarning() << "menu keyboard target mismatch" << menu->objectName() << objectName
                   << (menu->activeAction() ? menu->activeAction()->objectName() : QStringLiteral("<none>"));
        return false;
    }
    QTest::keyClick(menu, Qt::Key_Return);
    return true;
}

bool detailCommand(TaskDetailWidget& detail, const QString& name) {
    bool chosen = false;
    const bool opened = withMenu(detail, detail.findChild<QToolButton*>(QStringLiteral("DetailMore")),
        QStringLiteral("DetailMoreMenu"), [&](PopupMenu* menu) { chosen = keyboardChoose(menu, name); });
    return opened && chosen;
}

struct GroupMenuSnapshot {
    int parentCardWidth = 0, childCardWidth = 0;
    QStringList names, texts;
    QVector<int> modes;
    QString checked, trailing;
    bool childVisible = false;
};

bool chooseGrouping(TaskPanel& panel, GroupMode mode, GroupMenuSnapshot* snapshot = nullptr) {
    bool chosen = false;
    const bool opened = withMenu(panel, panel.findChild<QToolButton*>(QStringLiteral("WorkspaceMore")),
        QStringLiteral("WorkspaceMenu"), [&](PopupMenu* menu) {
            auto* child = menu->findChild<PopupMenu*>(QStringLiteral("GroupingMenu"));
            if (!child) return;
            menu->setActiveAction(nullptr);
            QTest::keyClick(menu, Qt::Key_Down);
            QTest::keyClick(menu, Qt::Key_Right);
            QApplication::processEvents();
            if (snapshot) {
                snapshot->parentCardWidth = menu->width() - 2 * PopupMenu::shadowMargin();
                snapshot->childCardWidth = child->width() - 2 * PopupMenu::shadowMargin();
                snapshot->childVisible = child->isVisible();
                snapshot->trailing = child->menuAction()->property("trailingText").toString();
                for (QAction* action : menu->actions()) snapshot->texts.append(action->text());
                for (QAction* action : child->actions()) {
                    snapshot->names.append(action->objectName());
                    snapshot->texts.append(action->text());
                    snapshot->modes.append(action->property("groupingMode").toInt());
                    if (action->isChecked()) snapshot->checked = action->objectName();
                }
            }
            chosen = keyboardChoose(child, QStringLiteral("GroupMode_") + groupModeName(mode));
        });
    return opened && chosen;
}

bool navigate(TaskPanel& panel, ViewScope scope, const QString& identity = QString()) {
    auto* navigation = panel.findChild<QListWidget*>(QStringLiteral("Navigation"));
    if (!navigation) return false;
    for (int row = 0; row < navigation->count(); ++row) {
        QListWidgetItem* item = navigation->item(row);
        if (item->data(NavigationDelegate::SectionRole).toBool()
            || item->data(Qt::UserRole).toInt() != static_cast<int>(scope)) continue;
        if (scope == ViewScope::List && item->data(Qt::UserRole + 1).toString() != identity) continue;
        if (scope == ViewScope::Folder && item->data(NavigationDelegate::FolderIdRole).toString() != identity) continue;
        navigation->setCurrentItem(item);
        QApplication::processEvents();
        return true;
    }
    return false;
}

TaskListModel* panelModel(TaskPanel& panel) {
    auto* view = panel.findChild<TaskListView*>(QStringLiteral("TaskList"));
    return view ? qobject_cast<TaskListModel*>(view->model()) : nullptr;
}

QString groupingKey(ViewScope scope, const QString& identity = QStringLiteral("all")) {
    return QStringLiteral("grouping/%1/%2").arg(static_cast<int>(scope)).arg(identity);
}

struct DisplayResult {
    bool opened = false, checkboxFound = false, initial = false, final = false;
    int completedSignals = 0, legacySignals = 0;
};

DisplayResult toggleCompletedSetting(TaskPanel& panel) {
    DisplayResult result;
    withMenu(panel, panel.findChild<QToolButton*>(QStringLiteral("WorkspaceMore")),
        QStringLiteral("WorkspaceMenu"), [&](PopupMenu* menu) {
            QElapsedTimer elapsed; elapsed.start();
            QTimer poll; poll.setInterval(5);
            QObject::connect(&poll, &QTimer::timeout, &poll, [&]() {
                auto* dialog = panel.findChild<DisplaySettingsDialog*>();
                if (dialog && dialog->isVisible()) {
                    poll.stop(); result.opened = true;
                    auto* checkbox = dialog->findChild<QCheckBox*>(QStringLiteral("ShowCompletedEntities"));
                    result.checkboxFound = checkbox;
                    if (checkbox) {
                        QSignalSpy completed(dialog, &DisplaySettingsDialog::completedVisibilityChanged);
                        QSignalSpy legacy(dialog, &DisplaySettingsDialog::optionsChanged);
                        result.initial = checkbox->isChecked();
                        checkbox->setFocus(); QTest::keyClick(checkbox, Qt::Key_Space);
                        result.final = checkbox->isChecked();
                        result.completedSignals = completed.count(); result.legacySignals = legacy.count();
                    }
                    dialog->accept();
                } else if (elapsed.elapsed() > 2000) {
                    poll.stop();
                    if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
                }
            });
            poll.start();
            if (!keyboardChoose(menu, QStringLiteral("DisplaySettings"))) poll.stop();
        });
    return result;
}

QString makeFile(const QString& root, const QString& name, const QByteArray& contents) {
    const QString path = root + QLatin1Char('/') + name;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()) return QString();
    return path;
}

QList<QAbstractButton*> visibleAttachmentRows(AttachmentWidget* attachments) {
    QList<QAbstractButton*> result;
    if (!attachments) return result;
    for (auto* row : attachments->findChildren<QAbstractButton*>(QStringLiteral("AttachmentRow")))
        if (!row->isHidden()) result.append(row);
    return result;
}

QWidget* detailDropTarget(TaskDetailWidget& detail, const QString& name) {
    if (name == QLatin1String("body")) {
        auto* editor = detail.findChild<MarkdownEditor*>();
        return editor ? editor->textEditor()->viewport() : nullptr;
    }
    if (name == QLatin1String("body-scroll")) {
        auto* scroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        return scroll ? scroll->viewport() : nullptr;
    }
    if (name == QLatin1String("attachment-pane"))
        return detail.findChild<QWidget*>(QStringLiteral("DetailAttachmentPane"));
    if (name == QLatin1String("attachment-viewport")) {
        auto* scroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailAttachmentScroll"));
        return scroll ? scroll->viewport() : nullptr;
    }
    const auto rows = visibleAttachmentRows(detail.findChild<AttachmentWidget*>());
    if (rows.isEmpty()) return nullptr;
    if (name == QLatin1String("attachment-row")) return rows.first();
    if (name == QLatin1String("attachment-actions"))
        return rows.first()->findChild<QToolButton*>(QStringLiteral("AttachmentActions"));
    return nullptr;
}

struct DropResult {
    bool entered = false, moved = false, dropped = false, hint = false;
    Qt::DropAction action = Qt::IgnoreAction;
};

DropResult dropFiles(TaskDetailWidget& detail, QWidget* target, const QStringList& paths,
                     Qt::DropActions actions = Qt::CopyAction | Qt::MoveAction) {
    DropResult result;
    if (!target) return result;
    QMimeData mime; QList<QUrl> urls;
    for (const QString& path : paths) urls.append(QUrl::fromLocalFile(path));
    mime.setUrls(urls);
    const QPoint point(qMin(20, target->width() - 1), qMin(12, target->height() - 1));
    QDragEnterEvent enter(point, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &enter); result.entered = enter.isAccepted();
    QDragMoveEvent move(point, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &move); result.moved = move.isAccepted();
    auto* hint = detail.findChild<QLabel*>(QStringLiteral("AttachmentDropHint"));
    result.hint = hint && hint->isVisible();
    QDropEvent drop(QPointF(point), actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &drop); result.dropped = drop.isAccepted(); result.action = drop.dropAction();
    return result;
}
class DetailEventCounter : public QObject {
public:
    bool eventFilter(QObject*, QEvent*) override {
        ++count;
        return false;
    }
    int count = 0;
};
} // namespace

class DetailTest : public QObject {

    Q_OBJECT
private slots:
    void initTestCase() {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        AppStyle::apply(*qobject_cast<QApplication*>(qApp));
        disableNativeWindowShadows();
    }
    void init() {
        m_dir.reset(new QTemporaryDir); QVERIFY(m_dir->isValid());
        qputenv("KYLINLIST_DATA_DIR", m_dir->path().toUtf8());
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_dir->path() + QStringLiteral("/settings"));
        for (const QString& key : {QStringLiteral("all"), QStringLiteral("today"), QStringLiteral("tomorrow"),
                                   QStringLiteral("next7days"), QStringLiteral("inbox"), QStringLiteral("completed"),
                                   QStringLiteral("abandoned"), QStringLiteral("trash")})
            Settings::instance().setSmartListVisibility(key, Settings::SmartListVisibility::Show);
        ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
        QString error;
        QVERIFY2(Database::instance().openAt(m_dir->path() + QStringLiteral("/detail.db"), &error), qPrintable(error));
        m_store.reset(new TaskStore(Database::instance().connection()));
        QVERIFY2(m_store->load(&error), qPrintable(error));
    }
    void cleanup() {
        m_store.reset(); Database::instance().close(); m_dir.reset();
    }
    void realControlsOnlyAndTwoFooterActions() {
        const QString id = m_store->createTask(QStringLiteral("真实详情"));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 680); detail.show(); QVERIFY(detail.setTask(id));
        for (const QString& name : {QStringLiteral("SaveTask"), QStringLiteral("ConvertKind"),
                                    QStringLiteral("ActivityLog"), QStringLiteral("RestoreTask"), QStringLiteral("TrashTask"),
                                    QStringLiteral("TaskPriority"), QStringLiteral("TaskPriorityHost"), QStringLiteral("TaskPinned"),
                                    QStringLiteral("TaskPinnedAt"), QStringLiteral("TaskGroup"), QStringLiteral("TaskScheduleHost"),
                                    QStringLiteral("startEnabled"), QStringLiteral("dueEnabled"), QStringLiteral("reminderEnabled"),
                                    QStringLiteral("recurrence"), QStringLiteral("TaskChecklist"), QStringLiteral("ChecklistInput"),
                                    QStringLiteral("AddChecklist"), QStringLiteral("FooterComment"), QStringLiteral("NoteChecklist")})
            QVERIFY2(!detail.findChild<QObject*>(name), qPrintable(name));
        QVERIFY(detail.findChildren<QDateTimeEdit*>().isEmpty());
        QVERIFY(detail.findChildren<QComboBox*>().isEmpty());
        const auto checks = detail.findChildren<QCheckBox*>(); QCOMPARE(checks.size(), 1);
        QCOMPARE(checks.first()->objectName(), QStringLiteral("TaskCompleted"));
        auto* footer = detail.findChild<QFrame*>(QStringLiteral("DetailFooter")); QVERIFY(footer);
        const auto buttons = footer->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly);
        QCOMPARE(buttons.size(), 3); // 所属清单 + 仅两个右侧动作。
        QStringList actions;
        for (QToolButton* button : buttons) {
            QVERIFY(button->isVisible());
            if (button->objectName() == QLatin1String("GroupChip")) continue;
            actions.append(button->objectName());
            QCOMPARE(button->size(), QSize(30, 30)); QCOMPARE(button->iconSize(), QSize(18, 18));
        }
        QCOMPARE(actions, QStringList({QStringLiteral("FooterFormat"), QStringLiteral("DetailMore")}));
        QCOMPARE(AppIcons::iconName(IconId::Format), QStringLiteral("td-footer-markdown"));
        QCOMPARE(AppIcons::iconName(IconId::MoreTitlebar), QStringLiteral("more-titlebar"));
        auto* format = footer->findChild<QToolButton*>(QStringLiteral("FooterFormat"));
        auto* more = footer->findChild<QToolButton*>(QStringLiteral("DetailMore"));
        QVERIFY(format && more);
        QCOMPARE(format->icon().pixmap(18, 18).toImage(), AppIcons::icon(IconId::Format, 18).pixmap(18, 18).toImage());
        QCOMPARE(more->icon().pixmap(18, 18).toImage(), AppIcons::icon(IconId::MoreTitlebar, 18).pixmap(18, 18).toImage());
        QCOMPARE(static_cast<int>(MarkdownEditor::FormatAction::Link), 8);
        QVERIFY(!detail.findChild<QToolButton*>(QStringLiteral("format8"))); // 已删除的旧检查项快捷按钮不复活。
        QTest::mouseClick(format, Qt::LeftButton);
        auto* panel = detail.findChild<QFrame*>(QStringLiteral("FormatPanel")); QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible()); QVERIFY(!panel->findChild<QObject*>(QStringLiteral("FormatChecklist")));
        panel->hide();
        const QString note = m_store->createNote(QStringLiteral("笔记也没有兼容控件"));
        QVERIFY(detail.setTask(note)); QVERIFY(checks.first()->isHidden());
        QVERIFY(!detail.findChild<QWidget*>(QStringLiteral("NoteChecklist")));
    }
    // GroupChip 现为局部复合按钮（图标与文本自绘，不再由 QStyle 分别摆放）：
    // 用内存渲染的真实墨迹验证默认图标/emoji/中英混排/长名省略的墨迹中线。
    void groupChipCompositeKeepsBodyFontInkMidlineAndClick_data() {
        QTest::addColumn<QString>("name"); QTest::addColumn<QString>("icon"); QTest::addColumn<bool>("elided");
        QTest::newRow("inbox-default") << QString() << QString() << false;
        QTest::newRow("builtin-mixed-name") << QStringLiteral("项目Alpha 甲乙") << QString() << false;
        QTest::newRow("emoji-icon") << QStringLiteral("清单") << QStringLiteral("🦄") << false;
        QTest::newRow("long-name-elided") << QStringLiteral("非常长的清单名称用来验证省略号截断后的墨迹中线对齐") << QString() << true;
    }
    void groupChipCompositeKeepsBodyFontInkMidlineAndClick() {
        QFETCH(QString, name); QFETCH(QString, icon); QFETCH(bool, elided);
        const QString listId = name.isEmpty() ? QString()
            : m_store->createList(name, QString(), EntityKind::Task, icon);
        QVERIFY2(name.isEmpty() || !listId.isEmpty(), "测试清单创建失败");
        const QString id = m_store->createTask(QStringLiteral("清单墨迹任务"));
        QVERIFY(!id.isEmpty());
        if (!listId.isEmpty()) QVERIFY(m_store->moveEntityToList(id, listId));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 680); detail.show(); QVERIFY(detail.setTask(id));
        auto* chip = detail.findChild<QToolButton*>(QStringLiteral("GroupChip"));
        QVERIFY(chip); QVERIFY(chip->isVisible());
        // 保留的构成：18px 图标、正文级字体与字号、文本在图标旁。
        QCOMPARE(chip->iconSize(), QSize(18, 18));
        QCOMPARE(chip->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
        QCOMPARE(chip->accessibleName(), QStringLiteral("所属清单"));
        QCOMPARE(chip->font().pointSizeF(), ds::qfont(ds::FontScene::Body).pointSizeF());
        QCOMPARE(chip->font().family(), ds::qfont(ds::FontScene::Body).family());
        QCOMPARE(chip->text(), name.isEmpty() ? QStringLiteral("收集箱") : name);
        QVERIFY(!chip->icon().isNull());
        QTest::mouseMove(&detail, QPoint(1, 1)); QApplication::processEvents();  // 静息态渲染，避开悬停底色
        const qreal dpr = chip->devicePixelRatioF();
        QImage image(qCeil(chip->width() * dpr), qCeil(chip->height() * dpr), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        image.setDevicePixelRatio(dpr);
        {
            QPainter painter(&image);
            chip->render(&painter);
        }
        // 图标区（左 6px 边距 + 18px 字形）与文本区分别实测墨迹中线。
        const auto regionInk = [&image, dpr](qreal xFromLogical, qreal xToLogical) {
            QRect bounds;
            const int x0 = qCeil(xFromLogical * dpr);
            const int x1 = qMin(image.width(), qCeil(xToLogical * dpr));
            for (int y = 0; y < image.height(); ++y) for (int x = x0; x < x1; ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (pixel.alpha() > 40 && qGray(pixel.red(), pixel.green(), pixel.blue()) < 235)
                    bounds = bounds.united(QRect(x, y, 1, 1));
            }
            return bounds;
        };
        const QRect iconInk = regionInk(0, 26);
        const QRect labelInk = regionInk(26, chip->width());
        QVERIFY2(!iconInk.isEmpty() && !labelInk.isEmpty(), "复合按钮必须同时绘制图标与文本墨迹");
        QVERIFY2(iconInk.height() <= qCeil(19 * dpr), "图标墨迹不得超出 18px 字形区");
        const qreal centerY = chip->rect().center().y();
        const qreal iconMid = (iconInk.top() + iconInk.bottom()) / 2.0 / dpr;
        const qreal labelMid = (labelInk.top() + labelInk.bottom()) / 2.0 / dpr;
        QVERIFY2(qAbs(labelMid - centerY) <= 1.0,
            qPrintable(QStringLiteral("文本墨迹中线 %1，按钮中线 %2").arg(labelMid).arg(centerY)));
        const qreal inkTolerance = icon.isEmpty() ? 1.0 : 2.0;
        QVERIFY2(qAbs(iconMid - labelMid) <= inkTolerance,
            qPrintable(QStringLiteral("图标墨迹中线 %1，文本墨迹中线 %2").arg(iconMid).arg(labelMid)));
        QVERIFY2(labelMid >= iconMid - inkTolerance,
            qPrintable(QStringLiteral("文本不得高于图标：图标中线 %1，文本中线 %2").arg(iconMid).arg(labelMid)));
        if (elided) {
            QCOMPARE(chip->width(), chip->maximumWidth());
            const int labelWidth = chip->width() - 12 - 18 - 4;  // 内容左右边距 - 图标 - 间距
            const QString shown = chip->fontMetrics().elidedText(chip->text(), Qt::ElideRight, labelWidth);
            QVERIFY(shown.contains(QChar(0x2026)));
            // 整字移除后可能留下不足一个字的余宽，不能要求省略号硬贴裁剪边缘。
            const qreal expectedRight = 6 + 18 + 4 + chip->fontMetrics().tightBoundingRect(shown).right();
            QVERIFY2(qAbs(labelInk.right() / dpr - expectedRight) <= 2.0,
                     "省略后的完整文字墨迹应与同字体参考一致，且不被裁掉");
            QVERIFY(labelInk.right() / dpr < chip->width() - 5);
        } else {
            QVERIFY(chip->width() < chip->maximumWidth());  // 未超宽不强制截断
        }
        // 点击逻辑保留：真实点击复合按钮打开所属清单菜单。
        QVERIFY(clickActualChild(detail, rectIn(chip, &detail).center()));
        QWidget* menu = detail.findChild<QWidget*>(QStringLiteral("GroupSelectMenu"));
        QTRY_VERIFY(menu && menu->isVisible());
        menu->close();
        QTRY_VERIFY(!menu->isVisible());
    }
    void dateChipCompositeKeepsActualLabelInkMidline() {
        const QString id = m_store->createTask(QStringLiteral("顶部日期墨迹任务"));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 680); detail.show(); QVERIFY(detail.setTask(id));
        auto* chip = detail.findChild<QPushButton*>(QStringLiteral("DateChip")); QVERIFY(chip);
        QCOMPARE(chip->text(), QStringLiteral("设置日期"));
        QTest::mouseMove(&detail, QPoint(1, 1)); QApplication::processEvents();
        const qreal dpr = chip->devicePixelRatioF();
        QImage image(qCeil(chip->width() * dpr), qCeil(chip->height() * dpr),
                     QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent); image.setDevicePixelRatio(dpr);
        { QPainter painter(&image); chip->render(&painter); }
        const auto regionInk = [&image, dpr](qreal from, qreal to) {
            QRect bounds;
            for (int y = 0; y < image.height(); ++y)
                for (int x = qFloor(from * dpr); x < qMin(image.width(), qCeil(to * dpr)); ++x) {
                    const QColor pixel = image.pixelColor(x, y);
                    if (pixel.alpha() > 40 && qGray(pixel.red(), pixel.green(), pixel.blue()) < 235)
                        bounds = bounds.united(QRect(x, y, 1, 1));
                }
            return bounds;
        };
        const QRect iconInk = regionInk(0, 26), labelInk = regionInk(28, chip->width());
        QVERIFY(!iconInk.isEmpty() && !labelInk.isEmpty());
        const qreal iconMid = (iconInk.top() + iconInk.bottom()) / 2.0 / dpr;
        const qreal labelMid = (labelInk.top() + labelInk.bottom()) / 2.0 / dpr;
        QVERIFY2(qAbs(iconMid - labelMid) <= 1.0,
                 qPrintable(QStringLiteral("日期图标中线 %1，文字中线 %2").arg(iconMid).arg(labelMid)));
        QVERIFY2(labelMid >= iconMid - 1.0, "顶部日期文字不得高于图标");
    }

    void narrowDateChipPaintsBothDateAndTime_data() {
        QTest::addColumn<bool>("overlay");
        QTest::addColumn<bool>("range");
        QTest::newRow("docked-timed") << false << false;
        QTest::newRow("overlay-timed") << true << false;
        QTest::newRow("docked-overdue-range") << false << true;
        QTest::newRow("overlay-overdue-range") << true << true;
    }

    void narrowDateChipPaintsBothDateAndTime() {
        QFETCH(bool, overlay);
        QFETCH(bool, range);
        const QString id = m_store->createTask(QStringLiteral("窄栏日期渲染"));
        QVERIFY(!id.isEmpty());
        const QDate today = toAppTime(appNow()).date();
        const QDate end = range ? QDate(2002, 1, 1) : today.addDays(1);
        const QDate start(2001, 12, 31);
        const auto at = [](const QDate& day, const QTime& time) {
            return fromAppTime(QDateTime(day, time, Qt::LocalTime));
        };
        ScheduleValue schedule;
        schedule.hasScheduleRange = range;
        if (range) schedule.startAt = at(start, QTime(9, 22));
        schedule.dueAt = at(end, QTime(17, 49));
        QVERIFY(m_store->saveSchedule(m_store->entity(id), schedule));

        TaskDetailWidget detail(m_store.get());
        detail.setOverlayMode(overlay);
        detail.resize(260, 620);
        detail.show();
        QVERIFY(detail.setTask(id));
        QVERIFY(QTest::qWaitForWindowExposed(&detail));
        auto* chip = detail.findChild<QPushButton*>(QStringLiteral("DateChip"));
        auto* header = detail.findChild<QFrame*>(QStringLiteral("DetailHeader"));
        auto* priority = detail.findChild<QToolButton*>(QStringLiteral("DetailPriority"));
        QVERIFY(chip && header && priority);
        QCOMPARE(detail.width(), 260);
        QTRY_VERIFY(chip->isVisible() && header->rect().contains(rectIn(chip, header)));
        if (range || overlay) QTRY_VERIFY(rectIn(chip, header).top() > rectIn(priority, header).bottom());
        if (range) QTRY_VERIFY(chip->height() > 30);
        const QString endDate = end.toString(end.year() == today.year() ? QStringLiteral("M月d日")
                                                                         : QStringLiteral("yyyy/MM/dd"));
        QVERIFY(chip->text().contains(endDate + QStringLiteral(", 17:49")));
        if (range) {
            QVERIFY(chip->text().contains(start.toString(QStringLiteral("yyyy/MM/dd")) + QStringLiteral(", 09:22 - ")));
            QVERIFY(chip->text().contains(QStringLiteral("延期")));
        }

        const auto labelInk = [chip]() {
            const qreal dpr = chip->devicePixelRatioF();
            QImage image(qCeil(chip->width() * dpr), qCeil(chip->height() * dpr),
                         QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(dpr);
            image.fill(Qt::transparent);
            {
                QPainter painter(&image);
                chip->render(&painter);
            }
            const int left = qCeil(28 * dpr); // 4px 内缩 + 20px 图标 + 4px 文字间隔
            return image.copy(left, 0, image.width() - left, image.height());
        };
        const QImage before = labelInk();
        // 改变最后一位：若只保存原始文字但绘制时仍省略时刻，图像不会变。
        schedule.dueAt = at(end, QTime(17, 48));
        QVERIFY(m_store->saveSchedule(m_store->entity(id), schedule));
        QTRY_VERIFY(chip->text().contains(QStringLiteral("17:48")));
        QCOMPARE(labelInk().size(), before.size());
        QVERIFY(labelInk() != before);
        if (range) {
            const QImage beforeStartChange = labelInk();
            schedule.startAt = at(start, QTime(9, 23));
            QVERIFY(m_store->saveSchedule(m_store->entity(id), schedule));
            QTRY_VERIFY(chip->text().contains(QStringLiteral("09:23")));
            QCOMPARE(labelInk().size(), beforeStartChange.size());
            QVERIFY(labelInk() != beforeStartChange);
        }
        // 宽栏复原要求日期与其他控件同排；字体不同，固定 560px 未必真能容纳整段文字。
        detail.resize(qMax(560, chip->sizeHint().width() + 180), 620);
        QTRY_COMPARE(chip->height(), 30);
        QTRY_COMPARE(rectIn(chip, header).center().y(), rectIn(priority, header).center().y());
        QVERIFY(chip->width() >= chip->sizeHint().width());
    }

    void typographyUsesApprovedPointSizes() {
        QCOMPARE(ds::pointSize(ds::TypeScale::Caption), 9.0);
        QCOMPARE(ds::pointSize(ds::TypeScale::Menu), 9.75);
        QCOMPARE(ds::pointSize(ds::TypeScale::Body), 10.5);
        QCOMPARE(ds::qfont(ds::FontScene::Caption).pointSizeF(), 9.0);
        QCOMPARE(ds::qfont(ds::FontScene::Menu).pointSizeF(), 9.75);
        QCOMPARE(ds::qfont(ds::FontScene::Body).pointSizeF(), 10.5);
#ifdef Q_OS_WIN
        QCOMPARE(ds::fontFamilies().first(), QStringLiteral("Microsoft YaHei UI"));
        if (QFontDatabase().families().contains(QStringLiteral("Microsoft YaHei UI")))
            QCOMPARE(ds::qfont(ds::FontScene::Body).family(), QStringLiteral("Microsoft YaHei UI"));
#endif
    }
    void nonEmptyBodyWithAttachmentHasVisibleInkAndRealCaret_data() {
        QTest::addColumn<QString>("source"); QTest::addColumn<QString>("rendered");
        QTest::newRow("plain-english") << QStringLiteral("Plain English body") << QStringLiteral("Plain English body");
        QTest::newRow("plain-chinese") << QStringLiteral("20合成普通正文") << QStringLiteral("20合成普通正文");
        QTest::newRow("plain-mixed") << QStringLiteral("20合成普通正文 Test body") << QStringLiteral("20合成普通正文 Test body");
        QTest::newRow("plain-multiline") << QStringLiteral("First plain line\n第二行合成正文\nLast English line")
            << QStringLiteral("First plain line\n第二行合成正文\nLast English line");
        QTest::newRow("formatted-multiline") << QStringLiteral("**Bold 合成正文**\nSecond _italic_ line")
            << QStringLiteral("Bold 合成正文\nSecond italic line");
    }
    void nonEmptyBodyWithAttachmentHasVisibleInkAndRealCaret() {
        QFETCH(QString, source); QFETCH(QString, rendered);
        const QString id = m_store->createTask(QStringLiteral("正文真实绘制回归"));
        Entity entity = m_store->entity(id); entity.body = source; QVERIFY(m_store->saveTask(entity));
        const QString file = makeFile(m_dir->path(), QStringLiteral("body-paint.txt"), QByteArray("synthetic attachment"));
        QVERIFY(!file.isEmpty()); QVERIFY(!m_store->importAttachment(id, file).id.isEmpty());
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("kylinlist"), QStringLiteral("panel"));
        settings.setValue(QStringLiteral("showDetails"), true); settings.setValue(QStringLiteral("detailPopup"), true); settings.sync();
        TaskPanel panel(m_store.get()); panel.resize(1240, 760); panel.show(); panel.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QVERIFY(QTest::qWaitForWindowActive(&panel));
        auto* view = panel.findChild<TaskListView*>(QStringLiteral("TaskList"));
        auto* detail = panel.findChild<TaskDetailWidget*>();
        auto* model = view ? qobject_cast<TaskListModel*>(view->model()) : nullptr;
        QVERIFY(view && detail && model);
        const QModelIndex index = model->indexForTask(id); QVERIFY(index.isValid());
        QCOMPARE(index.data(TaskListModel::DetailRole).toString(), QString(rendered).replace(QLatin1Char('\n'), QLatin1Char(' ')));
        QVERIFY(index.data(TaskListModel::DetailsVisibleRole).toBool());
        view->scrollTo(index);
        const QRect row = view->visualRect(index);
        const QPoint rowPoint = view->viewport()->mapTo(&panel, QPoint(row.left() + 100, row.center().y()));
        QCOMPARE(panel.childAt(rowPoint), view->viewport());
        // 先让本测试窗口真正暴露、可命中，再由点击加载正文；不等待正文失败自行恢复。
        QTRY_COMPARE(QApplication::widgetAt(panel.mapToGlobal(rowPoint)), view->viewport());
        QVERIFY(clickActualChild(panel, rowPoint)); QCOMPARE(detail->entityId(), id);
        auto* title = detail->findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* editor = detail->findChild<MarkdownEditor*>(); QVERIFY(title && editor);
        QCOMPARE(title->text(), entity.title); QCOMPARE(editor->content(), source);
        auto* body = editor->textEditor();
        const BodyViewportProbe initial = BodyViewportProbe::capture(editor);
        qInfo().noquote() << "first TaskPanel selection:" << initial.describe();
        QVERIFY2(initial.visible && initial.currentEditor, qPrintable(initial.describe()));
        QCOMPARE(initial.documentText, rendered);
        QVERIFY2(initial.unlaidOutTextBlocks == 0, qPrintable(initial.describe()));
        QVERIFY2(initial.viewportRect.height() >= body->fontMetrics().height(), qPrintable(initial.describe()));
        QVERIFY2(initial.firstLineRect.height() > 0 && initial.viewportRect.contains(initial.firstCaret), qPrintable(initial.describe()));
        QVERIFY2(initial.foreground.alpha() >= 200 && initial.textContrast >= 80, qPrintable(initial.describe()));
        QVERIFY2(initial.inkPixels >= 32 && initial.logicalInkWidth >= 12, qPrintable(initial.describe()));
        QTest::qWait(30); // 先断言首帧，再确认稳定后仍有字；不以等待代替失败的首帧。
        const BodyViewportProbe settled = BodyViewportProbe::capture(editor);
        QVERIFY2(settled.unlaidOutTextBlocks == 0 && settled.viewportRect.contains(settled.firstCaret)
            && settled.inkPixels >= 32 && settled.logicalInkWidth >= 12, qPrintable(settled.describe()));
        QTextCursor middle(body->document()); middle.setPosition(2);
        const QPoint bodyPoint = body->viewport()->mapTo(&panel, body->cursorRect(middle).center());
        QVERIFY2(viewportIsActualHit(panel, body, bodyPoint), qPrintable(initial.describe()));
        QVERIFY(clickActualChild(panel, bodyPoint)); QVERIFY(body->hasFocus());
        const BodyViewportProbe focused = BodyViewportProbe::capture(editor);
        qInfo().noquote() << "after real body click:" << focused.describe();
        QVERIFY2(focused.unlaidOutTextBlocks == 0 && focused.viewportRect.contains(focused.firstCaret)
            && focused.viewportRect.contains(focused.currentCaret) && focused.inkPixels >= 32
            && focused.logicalInkWidth >= 12, qPrintable(focused.describe()));
        QTest::qWait(20);
        const BodyViewportProbe focusSettled = BodyViewportProbe::capture(editor);
        QVERIFY2(focusSettled.unlaidOutTextBlocks == 0 && focusSettled.viewportRect.contains(focusSettled.currentCaret)
            && focusSettled.inkPixels >= 32 && focusSettled.logicalInkWidth >= 12, qPrintable(focusSettled.describe()));
        QTest::keyClick(body, Qt::Key_End, Qt::ControlModifier);
        QCOMPARE(body->textCursor().position(), body->document()->characterCount() - 1);
        const int endPosition = body->textCursor().position();
        QTest::keyClick(body, Qt::Key_Left); QCOMPARE(body->textCursor().position(), endPosition - 1);
        QTest::keyClick(body, Qt::Key_Right); QCOMPARE(body->textCursor().position(), endPosition);
        QTest::keyClicks(body, QStringLiteral(" edited"));
        const QString expected = source + QStringLiteral(" edited");
        QTest::keyClick(body, Qt::Key_S, Qt::ControlModifier); QTRY_COMPARE(m_store->entity(id).body, expected);
        QCOMPARE(editor->content(), expected); QCOMPARE(m_store->attachments(id).size(), 1);
        const BodyViewportProbe saved = BodyViewportProbe::capture(editor);
        QVERIFY2(saved.unlaidOutTextBlocks == 0 && saved.viewportRect.contains(saved.firstCaret)
            && saved.viewportRect.contains(saved.currentCaret) && saved.inkPixels >= 32
            && saved.logicalInkWidth >= 12, qPrintable(saved.describe()));
    }
    void nonEmptyBodySurvivesPanelSelectionAndReparenting_data() {
        QTest::addColumn<bool>("initialOverlay"); QTest::addColumn<QString>("focusName");
        QTest::newRow("docked-title") << false << QStringLiteral("TaskTitle");
        QTest::newRow("docked-quick-add") << false << QStringLiteral("QuickAdd");
        QTest::newRow("overlay-title") << true << QStringLiteral("TaskTitle");
        QTest::newRow("overlay-quick-add") << true << QStringLiteral("QuickAdd");
    }
    void nonEmptyBodySurvivesPanelSelectionAndReparenting() {
        QFETCH(bool, initialOverlay); QFETCH(QString, focusName);
        const QString source = QStringLiteral("20合成普通正文 Test body");
        const QString existing = m_store->createTask(QStringLiteral("已有正文与附件"));
        const QString empty = m_store->createTask(QStringLiteral("空正文切换目标"));
        Entity entity = m_store->entity(existing); entity.body = source; QVERIFY(m_store->saveTask(entity));
        const QString file = makeFile(m_dir->path(), QStringLiteral("switch-body.txt"), QByteArray("synthetic switch attachment"));
        QVERIFY(!file.isEmpty()); QVERIFY(!m_store->importAttachment(existing, file).id.isEmpty());
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("kylinlist"), QStringLiteral("panel"));
        settings.setValue(QStringLiteral("showDetails"), true); settings.setValue(QStringLiteral("detailPopup"), true); settings.sync();
        TaskPanel panel(m_store.get()); panel.resize(initialOverlay ? 900 : 1240, 760); panel.show(); panel.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QVERIFY(QTest::qWaitForWindowActive(&panel));
        auto* view = panel.findChild<TaskListView*>(QStringLiteral("TaskList"));
        auto* detail = panel.findChild<TaskDetailWidget*>();
        auto* frame = panel.findChild<QFrame*>(QStringLiteral("DetailFrame"));
        auto* model = view ? qobject_cast<TaskListModel*>(view->model()) : nullptr;
        QVERIFY(view && detail && frame && model);
        auto* editor = detail->findChild<MarkdownEditor*>();
        auto* attachments = detail->findChild<AttachmentWidget*>();
        auto* scroll = detail->findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        auto* pane = detail->findChild<QWidget*>(QStringLiteral("DetailAttachmentPane"));
        auto* attachmentScroll = detail->findChild<QScrollArea*>(QStringLiteral("DetailAttachmentScroll"));
        auto* footer = detail->findChild<QFrame*>(QStringLiteral("DetailFooter"));
        auto* focusField = panel.findChild<QLineEdit*>(focusName);
        QVERIFY(editor && attachments && scroll && pane && attachmentScroll && footer && focusField);
        auto* body = editor->textEditor();
        const auto bottomPaneIsBounded = [&]() {
            return pane->isVisible() && !scroll->isAncestorOf(pane)
                && pane->geometry().bottom() + 1 == footer->geometry().top()
                && pane->height() <= (scroll->height() + pane->height()) / 3
                && attachmentScroll->verticalScrollBar()->maximum() == 0
                && body->viewport()->height() > scroll->viewport()->height() / 2;
        };
        const auto selectThroughList = [&](const QString& id) {
            const QModelIndex index = model->indexForTask(id); if (!index.isValid()) return false;
            view->scrollTo(index); const QRect row = view->visualRect(index);
            const QPoint point = view->viewport()->mapTo(&panel, QPoint(row.left() + 100, row.center().y()));
            if (panel.childAt(point) != view->viewport()) return false;
            const QString platform = QGuiApplication::platformName();
            if (platform != QLatin1String("offscreen") && platform != QLatin1String("minimal")
                && QApplication::widgetAt(panel.mapToGlobal(point)) != view->viewport()) return false;
            return clickActualChild(panel, point) && detail->entityId() == id;
        };
        const auto clickFocusField = [&]() {
            const QPoint point = focusField->mapTo(&panel, QPoint(8, focusField->height() / 2));
            return panel.childAt(point) == focusField && clickActualChild(panel, point) && focusField->hasFocus();
        };
        const auto hasRealBodyInk = [&](const BodyViewportProbe& probe) {
            return probe.visible && probe.currentEditor && probe.documentText == source
                && probe.unlaidOutTextBlocks == 0 && probe.firstLineRect.height() > 0
                && probe.viewportRect.contains(probe.firstCaret) && probe.foreground.alpha() >= 200
                && probe.textContrast >= 80 && probe.inkPixels >= 32 && probe.logicalInkWidth >= 12;
        };
        QTRY_VERIFY(selectThroughList(existing)); QTRY_COMPARE(frame->property("overlay").toBool(), initialOverlay);
        QTRY_VERIFY(bottomPaneIsBounded());
        QCOMPARE(model->indexForTask(existing).data(TaskListModel::DetailRole).toString(), source);
        QCOMPARE(editor->content(), source); QVERIFY(editor->flowLayout());
        QVERIFY(editor->fillAvailableHeight()); QVERIFY(editor->blockControlsEnabled());
        BodyViewportProbe probe = BodyViewportProbe::capture(editor);
        QVERIFY2(hasRealBodyInk(probe), qPrintable(QStringLiteral("initial: ") + probe.describe()));
        QVERIFY(clickFocusField()); QVERIFY(!body->hasFocus());
        probe = BodyViewportProbe::capture(editor);
        QVERIFY2(hasRealBodyInk(probe), qPrintable(QStringLiteral("title/QuickAdd focus: ") + probe.describe()));

        QTRY_VERIFY(selectThroughList(empty)); QVERIFY(editor->fillAvailableHeight()); QVERIFY(editor->content().isEmpty());
        QTRY_VERIFY(body->viewport()->height() > scroll->viewport()->height() / 2);
        QVERIFY(attachments->isHidden()); QVERIFY(pane->isHidden()); QVERIFY(clickFocusField());
        panel.resize(initialOverlay ? 1240 : 900, 760);
        QTRY_COMPARE(frame->property("overlay").toBool(), !initialOverlay);
        QVERIFY(editor->fillAvailableHeight());
        QTRY_VERIFY(body->viewport()->height() > scroll->viewport()->height() / 2);

        QTRY_VERIFY(selectThroughList(existing)); QVERIFY(editor->fillAvailableHeight());
        QCOMPARE(editor->content(), source); QTRY_VERIFY(attachments->isVisible());
        QTRY_VERIFY(bottomPaneIsBounded());
        probe = BodyViewportProbe::capture(editor);
        QVERIFY2(hasRealBodyInk(probe), qPrintable(QStringLiteral("return after empty/reparent: ") + probe.describe()));
        QTest::qWait(30); probe = BodyViewportProbe::capture(editor);
        QVERIFY2(hasRealBodyInk(probe), qPrintable(QStringLiteral("settled return: ") + probe.describe()));
        panel.resize(initialOverlay ? 900 : 1240, 760);
        QTRY_COMPARE(frame->property("overlay").toBool(), initialOverlay);
        QTRY_VERIFY(bottomPaneIsBounded());
        probe = BodyViewportProbe::capture(editor);
        QVERIFY2(hasRealBodyInk(probe), qPrintable(QStringLiteral("redock/overlay again: ") + probe.describe()));
        QVERIFY(clickFocusField());
        QTextCursor middle(body->document()); middle.setPosition(2);
        const QPoint point = body->viewport()->mapTo(&panel, body->cursorRect(middle).center());
        QVERIFY2(viewportIsActualHit(panel, body, point), qPrintable(probe.describe()));
        QVERIFY(clickActualChild(panel, point)); QVERIFY(body->hasFocus());
        probe = BodyViewportProbe::capture(editor);
        QVERIFY2(hasRealBodyInk(probe) && probe.viewportRect.contains(probe.currentCaret), qPrintable(probe.describe()));
        QTest::keyClick(body, Qt::Key_End, Qt::ControlModifier); QCOMPARE(body->textCursor().position(), source.size());
        QTest::keyClick(body, Qt::Key_Left); QCOMPARE(body->textCursor().position(), source.size() - 1);
        QTest::keyClick(body, Qt::Key_Right); QTest::keyClicks(body, QStringLiteral(" switched"));
        const QString expected = source + QStringLiteral(" switched");
        QTest::keyClick(body, Qt::Key_S, Qt::ControlModifier); QTRY_COMPARE(m_store->entity(existing).body, expected);
        QCOMPARE(editor->content(), expected); QCOMPARE(m_store->attachments(existing).size(), 1);
        QVERIFY(m_store->entity(empty).body.isEmpty());
        probe = BodyViewportProbe::capture(editor);
        QVERIFY2(probe.unlaidOutTextBlocks == 0 && probe.viewportRect.contains(probe.currentCaret)
            && probe.inkPixels >= 32 && probe.logicalInkWidth >= 12, qPrintable(probe.describe()));
    }
    void bottomAttachmentPaneAndContinuousBody_data() {
        QTest::addColumn<QString>("bodyKind"); QTest::addColumn<int>("attachmentCount");
        for (const QString& body : {QStringLiteral("empty"), QStringLiteral("short"), QStringLiteral("long")})
            for (int count : {0, 1, 3, 7})
                QTest::newRow(qPrintable(body + QStringLiteral("-%1-files").arg(count))) << body << count;
    }
    void bottomAttachmentPaneAndContinuousBody() {
        QFETCH(QString, bodyKind); QFETCH(int, attachmentCount);
        const QString id = m_store->createTask(QStringLiteral("底栏与连续正文"));
        Entity entity = m_store->entity(id);
        if (bodyKind == QLatin1String("short")) entity.body = QStringLiteral("Short editable body");
        if (bodyKind == QLatin1String("long")) {
            QStringList lines;
            for (int line = 0; line < 60; ++line)
                lines.append(QStringLiteral("Paragraph %1: synthetic detail content wraps and stays editable at narrow widths.").arg(line));
            entity.body = lines.join(QLatin1Char('\n'));
        }
        QVERIFY(m_store->saveTask(entity));
        for (int index = 0; index < attachmentCount; ++index) {
            const QString file = makeFile(m_dir->path(), QStringLiteral("pane-%1.txt").arg(index),
                QByteArray("synthetic-pane-") + QByteArray::number(index));
            QVERIFY(!file.isEmpty()); QVERIFY(!m_store->importAttachment(id, file).id.isEmpty());
        }
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 840); detail.show(); QVERIFY(detail.setTask(id));
        detail.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&detail));
        auto* scroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        auto* form = detail.findChild<QWidget*>(QStringLiteral("TaskDetailForm"));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* pane = detail.findChild<QWidget*>(QStringLiteral("DetailAttachmentPane"));
        auto* attachmentScroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailAttachmentScroll"));
        auto* attachments = detail.findChild<AttachmentWidget*>();
        auto* footer = detail.findChild<QFrame*>(QStringLiteral("DetailFooter"));
        auto* divider = detail.findChild<QFrame*>(QStringLiteral("DetailAttachmentDivider"));
        auto* attachmentHeader = detail.findChild<QWidget*>(QStringLiteral("DetailAttachmentHeader"));
        auto* count = detail.findChild<QLabel*>(QStringLiteral("DetailAttachmentCount"));
        auto* add = detail.findChild<QToolButton*>(QStringLiteral("DetailAttachmentAdd"));
        QVERIFY(scroll && form && editor && pane && attachmentScroll && attachments && footer && divider && attachmentHeader && count && add);
        auto* body = editor->textEditor();
        QVERIFY(editor->flowLayout()); QVERIFY(editor->fillAvailableHeight()); QVERIFY(editor->blockControlsEnabled());
        QCOMPARE(form->layout()->contentsMargins().left(), 0); QCOMPARE(form->layout()->contentsMargins().right(), 0);
        QTRY_COMPARE(rectIn(editor, form).left(), 0); QTRY_COMPARE(editor->width(), form->width());
        QTRY_COMPARE(rectIn(body->viewport(), editor).left(), 24);
        QTRY_COMPARE(editor->width() - rectIn(body->viewport(), editor).right() - 1, 24);
        for (int index = 0; index < form->layout()->count(); ++index)
            QVERIFY2(!form->layout()->itemAt(index)->spacerItem(), "The body form must not end in an inert blank stretch");
        QCOMPARE(footer->height(), 48); QVERIFY(!form->isAncestorOf(pane)); QVERIFY(!form->isAncestorOf(attachments));
        QCOMPARE(attachmentScroll->widget(), static_cast<QWidget*>(attachments));
        QTRY_COMPARE(count->text(), QString::number(attachmentCount));
        if (attachmentCount == 0) {
            QVERIFY(pane->isHidden()); QVERIFY(attachments->isHidden());
            QTRY_COMPARE(scroll->geometry().bottom() + 1, footer->geometry().top());
        } else {
            QTRY_VERIFY(pane->isVisible()); QTRY_VERIFY(attachments->isVisible());
            QTRY_COMPARE(pane->geometry().bottom() + 1, footer->geometry().top());
            QTRY_COMPARE(scroll->geometry().bottom() + 1, pane->geometry().top());
            QTRY_VERIFY(pane->height() <= (scroll->height() + pane->height()) / 3);
            const int visibleCards = qMin(attachmentCount, 3);
            const int naturalPane = divider->height() + attachmentHeader->height()
                + pane->layout()->contentsMargins().top() + pane->layout()->contentsMargins().bottom()
                + visibleCards * 56 + (visibleCards - 1) * 6;
            QTRY_COMPARE(pane->height(), qMin(naturalPane, (scroll->height() + pane->height()) / 3));
            QCOMPARE(divider->height(), 1); QCOMPARE(rectIn(divider, pane).left(), 24);
            QCOMPARE(pane->width() - rectIn(divider, pane).right() - 1, 24);
            QCOMPARE(add->size(), QSize(24, 24)); QCOMPARE(add->iconSize(), QSize(16, 16));
            QCOMPARE(detail.findChild<QLabel*>(QStringLiteral("DetailAttachmentTitle"))->text(), QStringLiteral("附件"));
            const auto rows = visibleAttachmentRows(attachments); QCOMPARE(rows.size(), attachmentCount);
            for (auto* row : rows) QCOMPARE(row->height(), 56);
            const int cardsHeight = attachmentCount * 56 + (attachmentCount - 1) * 6;
            QTRY_COMPARE(attachmentScroll->verticalScrollBar()->maximum(),
                         qMax(0, cardsHeight - attachmentScroll->viewport()->height()));
            // A native screen may constrain the requested 840px window at high DPI.
            // The one-third pane budget still applies, so even three cards may scroll.
            if (cardsHeight <= attachmentScroll->viewport()->height()) {
                QTRY_COMPARE(attachmentScroll->verticalScrollBar()->maximum(), 0);
                for (auto* row : rows) QVERIFY(attachmentScroll->viewport()->rect().contains(rectIn(row, attachmentScroll->viewport())));
            } else {
                QTRY_VERIFY(attachmentScroll->verticalScrollBar()->maximum() > 0);
                const QRect editorBefore = editor->geometry(), paneBefore = pane->geometry(), footerBefore = footer->geometry();
                const int bodyScroll = scroll->verticalScrollBar()->value();
                const int firstRowY = rows.first()->mapTo(attachmentScroll->viewport(), QPoint()).y();
                attachmentScroll->verticalScrollBar()->setValue(attachmentScroll->verticalScrollBar()->maximum());
                QApplication::processEvents();
                QVERIFY(rows.first()->mapTo(attachmentScroll->viewport(), QPoint()).y() < firstRowY);
                QCOMPARE(editor->geometry(), editorBefore); QCOMPARE(pane->geometry(), paneBefore);
                QCOMPARE(footer->geometry(), footerBefore); QCOMPARE(scroll->verticalScrollBar()->value(), bodyScroll);
                QVERIFY(attachmentScroll->viewport()->rect().contains(rectIn(rows.last(), attachmentScroll->viewport())));
            }
            QTRY_COMPARE(attachmentScroll->horizontalScrollBar()->maximum(), 0);
        }
        QCOMPARE(body->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        QTRY_COMPARE(body->verticalScrollBar()->maximum(), 0); QTRY_COMPARE(body->horizontalScrollBar()->maximum(), 0);
        QTRY_COMPARE(scroll->horizontalScrollBar()->maximum(), 0); QCOMPARE(editor->content(), entity.body);
        if (!entity.body.isEmpty()) {
            const BodyViewportProbe probe = BodyViewportProbe::capture(editor);
            QVERIFY2(probe.visible && probe.currentEditor && probe.unlaidOutTextBlocks == 0
                && probe.firstLineRect.height() > 0 && probe.viewportRect.contains(probe.firstCaret)
                && probe.textContrast >= 80 && probe.inkPixels >= 32 && probe.logicalInkWidth >= 12, qPrintable(probe.describe()));
        }
        if (bodyKind == QLatin1String("long")) {
            QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > scroll->viewport()->height());
            auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle")); QVERIFY(title);
            const int titleY = title->mapTo(scroll->viewport(), QPoint()).y();
            const int editorY = editor->mapTo(scroll->viewport(), QPoint()).y();
            const QRect paneBefore = pane->geometry(), footerBefore = footer->geometry();
            const int attachmentOffset = attachmentScroll->verticalScrollBar()->value();
            scroll->verticalScrollBar()->setValue(160); QApplication::processEvents();
            QCOMPARE(title->mapTo(scroll->viewport(), QPoint()).y(), titleY - 160);
            QCOMPARE(editor->mapTo(scroll->viewport(), QPoint()).y(), editorY - 160);
            QCOMPARE(pane->geometry(), paneBefore); QCOMPARE(footer->geometry(), footerBefore);
            QCOMPARE(attachmentScroll->verticalScrollBar()->value(), attachmentOffset);
        } else {
            QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
            QTRY_VERIFY(body->viewport()->height() > scroll->viewport()->height() / 2);
            const QPoint lower = body->viewport()->mapTo(&detail, QPoint(12, body->viewport()->height() - 6));
            QVERIFY(viewportIsActualHit(detail, body, lower)); QVERIFY(clickActualChild(detail, lower)); QTRY_VERIFY(body->hasFocus());
            const int filledHeight = editor->height();
            QTest::keyClick(body, Qt::Key_End, Qt::ControlModifier); QTest::keyClicks(body, QStringLiteral(" appended"));
            QVERIFY(detail.finishEditing()); QCOMPARE(m_store->entity(id).body, entity.body + QStringLiteral(" appended"));
            QTRY_COMPARE(editor->height(), filledHeight); QVERIFY(viewportIsActualHit(detail, body, lower));
            QVERIFY(body->viewport()->rect().contains(body->cursorRect()));
            QCOMPARE(m_store->attachments(id).size(), attachmentCount);
        }
    }
    void attachmentPaneRebudgetsOnResizeAndLastFileRemoval() {
        const QString id = m_store->createTask(QStringLiteral("底栏随宿主重算"));
        for (int index = 0; index < 7; ++index) {
            const QString source = makeFile(m_dir->path(), QStringLiteral("resize-%1.txt").arg(index),
                QByteArray("synthetic-resize-") + QByteArray::number(index));
            QVERIFY(!source.isEmpty()); QVERIFY(!m_store->importAttachment(id, source).id.isEmpty());
        }
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 840); detail.show(); QVERIFY(detail.setTask(id));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* scroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        auto* attachmentScroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailAttachmentScroll"));
        auto* pane = detail.findChild<QWidget*>(QStringLiteral("DetailAttachmentPane"));
        auto* footer = detail.findChild<QFrame*>(QStringLiteral("DetailFooter"));
        QVERIFY(editor && scroll && attachmentScroll && pane && footer);
        QTRY_VERIFY(pane->height() > 180); const int tallHeight = pane->height();
        detail.resize(360, 560);
        QTRY_VERIFY(pane->height() < tallHeight);
        QTRY_VERIFY(pane->height() <= (scroll->height() + pane->height()) / 3);
        QTRY_VERIFY(editor->textEditor()->viewport()->height() >= 120);
        QTRY_COMPARE(pane->geometry().bottom() + 1, footer->geometry().top());
        QTRY_VERIFY(attachmentScroll->verticalScrollBar()->maximum() > 0);
        detail.resize(360, 330);
        QTRY_VERIFY(editor->textEditor()->viewport()->height() >= 120);
        QTRY_VERIFY(pane->height() <= (scroll->height() + pane->height()) / 3);
        QTRY_COMPARE(pane->geometry().bottom() + 1, footer->geometry().top());
        detail.resize(480, 840); QTRY_COMPARE(pane->height(), tallHeight);
        const auto items = m_store->attachments(id);
        for (const Attachment& attachment : items) QVERIFY(m_store->removeAttachment(id, attachment.id));
        QTRY_VERIFY(pane->isHidden()); QTRY_VERIFY(detail.findChild<AttachmentWidget*>()->isHidden());
        QTRY_COMPARE(scroll->geometry().bottom() + 1, footer->geometry().top());
        QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
        QTRY_VERIFY(editor->textEditor()->viewport()->height() > scroll->viewport()->height() / 2);
        QVERIFY(editor->content().isEmpty()); QVERIFY(m_store->entity(id).body.isEmpty());
    }
    void titleLongBodyAndActivityScrollTogetherAboveAttachments() {
        const QString id = m_store->createTask(QStringLiteral("正文动态共享外层滚动"));
        Entity entity = m_store->entity(id); QStringList lines;
        for (int index = 0; index < 60; ++index) lines.append(QStringLiteral("Synthetic paragraph %1 with editable detail text.").arg(index));
        entity.body = lines.join(QLatin1Char('\n')); QVERIFY(m_store->saveTask(entity));
        const QString source = makeFile(m_dir->path(), QStringLiteral("activity-pane.txt"), QByteArray("synthetic attachment"));
        QVERIFY(!source.isEmpty()); QVERIFY(!m_store->importAttachment(id, source).id.isEmpty());
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 700); detail.show(); QVERIFY(detail.setTask(id));
        auto* scroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* activity = detail.findChild<ActivityTimelineWidget*>();
        auto* pane = detail.findChild<QWidget*>(QStringLiteral("DetailAttachmentPane"));
        auto* footer = detail.findChild<QFrame*>(QStringLiteral("DetailFooter"));
        auto* attachmentScroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailAttachmentScroll"));
        QVERIFY(scroll && title && editor && activity && pane && footer && attachmentScroll);
        QVERIFY(detail.showActivity()); QTRY_VERIFY(activity->isVisible());
        QTRY_VERIFY(scroll->verticalScrollBar()->value() > 0); // showActivity 的延后 ensureVisible 已完成。
        scroll->verticalScrollBar()->setValue(0); QApplication::processEvents();
        const int titleY = title->mapTo(scroll->viewport(), QPoint()).y();
        const int editorY = editor->mapTo(scroll->viewport(), QPoint()).y();
        const int activityY = activity->mapTo(scroll->viewport(), QPoint()).y();
        const QRect paneBefore = pane->geometry(), footerBefore = footer->geometry();
        scroll->verticalScrollBar()->setValue(160); QApplication::processEvents();
        QCOMPARE(title->mapTo(scroll->viewport(), QPoint()).y(), titleY - 160);
        QCOMPARE(editor->mapTo(scroll->viewport(), QPoint()).y(), editorY - 160);
        QCOMPARE(activity->mapTo(scroll->viewport(), QPoint()).y(), activityY - 160);
        QCOMPARE(pane->geometry(), paneBefore); QCOMPARE(footer->geometry(), footerBefore);
        QCOMPARE(attachmentScroll->verticalScrollBar()->value(), 0);
        QCOMPARE(editor->textEditor()->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        QCOMPARE(editor->textEditor()->verticalScrollBar()->maximum(), 0);
        QCOMPARE(editor->content(), entity.body); QCOMPARE(m_store->entity(id).body, entity.body);
    }
    void emptyBodyViewportOwnsWhitespaceAndAcceptsMouseInput_data() {
        QTest::addColumn<bool>("overlay");
        QTest::newRow("standalone") << false;
        QTest::newRow("narrow-overlay") << true;
    }
    void emptyBodyViewportOwnsWhitespaceAndAcceptsMouseInput() {
        QFETCH(bool, overlay);
        const QString id = m_store->createTask(QStringLiteral("空正文真实命中"));
        std::unique_ptr<QWidget> window;
        TaskDetailWidget* detail = nullptr;
        if (overlay) {
            auto* panel = new TaskPanel(m_store.get());
            window.reset(panel); panel->resize(900, 700); panel->showTask(id);
            detail = panel->findChild<TaskDetailWidget*>();
            auto* frame = panel->findChild<QFrame*>(QStringLiteral("DetailFrame"));
            QVERIFY(detail && frame); QTRY_VERIFY(frame->property("overlay").toBool());
            QVERIFY(frame->width() <= 480);
        } else {
            detail = new TaskDetailWidget(m_store.get()); window.reset(detail);
            detail->resize(480, 700); detail->show(); QVERIFY(detail->setTask(id));
        }
        window->activateWindow(); QVERIFY(QTest::qWaitForWindowActive(window.get()));
        auto* title = detail->findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* editor = detail->findChild<MarkdownEditor*>();
        auto* scroll = detail->findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        auto* form = detail->findChild<QWidget*>(QStringLiteral("TaskDetailForm"));
        QVERIFY(title && editor && scroll && form);
        auto* body = editor->textEditor();
        QVERIFY(editor->flowLayout()); QVERIFY(editor->fillAvailableHeight());
        QVERIFY(editor->blockControlsEnabled()); QVERIFY(editor->content().isEmpty());
        QTRY_VERIFY(body->viewport()->height() > scroll->viewport()->height() / 2);
        QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
        const int expandedHeight = editor->height();
        const auto lowerWhitespace = [&]() {
            const int x = body->viewport()->mapTo(scroll->viewport(), QPoint()).x() + 12;
            const int y = scroll->viewport()->height() - form->layout()->contentsMargins().bottom() - 8;
            return scroll->viewport()->mapTo(window.get(), QPoint(x, y));
        };
        QVERIFY(clickActualChild(*window, title->mapTo(window.get(), title->rect().center())));
        QTRY_VERIFY(title->hasFocus()); QVERIFY(!body->hasFocus());
        QPoint point = lowerWhitespace();
        QTRY_VERIFY(viewportIsActualHit(*window, body, point));
        QTest::mouseMove(body->viewport(), body->viewport()->mapFrom(window.get(), point));
        QTest::qWait(20);
        QVERIFY2(title->hasFocus(), "Hovering empty body whitespace must not steal title focus");
        QVERIFY(!body->hasFocus());
        QVERIFY(clickActualChild(*window, point));
        QTRY_VERIFY(body->hasFocus()); QCOMPARE(QApplication::focusWidget(), static_cast<QWidget*>(body));
        // FocusIn 会触发 Markdown 行渲染；尚未键入时绝不能把空 viewport 又缩回自然高度。
        QTest::qWait(20); QCOMPARE(editor->height(), expandedHeight);
        QVERIFY(viewportIsActualHit(*window, body, point)); QVERIFY(editor->content().isEmpty());
        QCOMPARE(body->viewport()->cursor().shape(), Qt::IBeamCursor);
        QVERIFY(!body->cursorRect().isEmpty()); QVERIFY(body->viewport()->rect().intersects(body->cursorRect()));
        QTest::keyClicks(body, QStringLiteral("Entered through real whitespace"));
        QTRY_COMPARE(editor->content(), QStringLiteral("Entered through real whitespace"));
        QVERIFY(detail->finishEditing());
        QCOMPARE(m_store->entity(id).body, QStringLiteral("Entered through real whitespace"));
        QTRY_COMPARE(editor->height(), expandedHeight);
        QVERIFY(viewportIsActualHit(*window, body, point));
        QCOMPARE(body->verticalScrollBarPolicy(), Qt::ScrollBarAlwaysOff);
        QTRY_COMPARE(body->verticalScrollBar()->maximum(), 0);
        QTRY_COMPARE(body->horizontalScrollBar()->maximum(), 0);
        QTRY_COMPARE(scroll->horizontalScrollBar()->maximum(), 0);

        // 经键盘清空，而非 setPlainText；空 -> 非空 -> 空必须恢复同一个可点击 viewport。
        QTest::keyClick(body, Qt::Key_A, Qt::ControlModifier); QTest::keyClick(body, Qt::Key_Backspace);
        QTRY_VERIFY(editor->content().isEmpty()); QVERIFY(detail->finishEditing());
        QCOMPARE(m_store->entity(id).body, QString());
        QTRY_COMPARE(editor->height(), expandedHeight);
        QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
        QTRY_VERIFY(window->childAt(title->mapTo(window.get(), title->rect().center())) == title);
        QVERIFY(clickActualChild(*window, title->mapTo(window.get(), title->rect().center())));
        QTRY_VERIFY2(title->hasFocus(), qPrintable(QStringLiteral("after clear: focus=%1, popup=%2, scroll=%3/%4, titleY=%5, editor=%6")
            .arg(QApplication::focusWidget() ? QApplication::focusWidget()->objectName() : QStringLiteral("<none>"))
            .arg(QApplication::activePopupWidget() ? QApplication::activePopupWidget()->objectName() : QStringLiteral("<none>"))
            .arg(scroll->verticalScrollBar()->value()).arg(scroll->verticalScrollBar()->maximum())
            .arg(rectIn(title, window.get()).top()).arg(editor->height())));
        point = lowerWhitespace(); QTRY_VERIFY(viewportIsActualHit(*window, body, point));
        QVERIFY(clickActualChild(*window, point)); QTRY_VERIFY(body->hasFocus());
        QTest::keyClicks(body, QStringLiteral("Still editable after clearing"));
        QVERIFY(detail->finishEditing());
        QCOMPARE(m_store->entity(id).body, QStringLiteral("Still editable after clearing"));
    }
    void emptyBodyWithBottomAttachmentsStaysContinuouslyEditable() {
        const QString id = m_store->createTask(QStringLiteral("空正文与底部附件"));
        const QString source = makeFile(m_dir->path(), QStringLiteral("empty-body.txt"), QByteArray("synthetic attachment"));
        QVERIFY(!source.isEmpty()); QVERIFY(!m_store->importAttachment(id, source).id.isEmpty());
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 700); detail.show(); QVERIFY(detail.setTask(id));
        detail.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&detail));
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* attachments = detail.findChild<AttachmentWidget*>();
        auto* scroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        auto* form = detail.findChild<QWidget*>(QStringLiteral("TaskDetailForm"));
        auto* pane = detail.findChild<QWidget*>(QStringLiteral("DetailAttachmentPane"));
        auto* footer = detail.findChild<QFrame*>(QStringLiteral("DetailFooter"));
        QVERIFY(title && editor && attachments && scroll && form && pane && footer);
        auto* body = editor->textEditor();
        QTRY_VERIFY(attachments->isVisible()); QTRY_VERIFY(pane->isVisible()); QTRY_VERIFY(pane->height() > 56);
        QVERIFY(editor->fillAvailableHeight()); QVERIFY(!form->isAncestorOf(attachments));
        QTRY_VERIFY(body->viewport()->height() > scroll->viewport()->height() / 2);
        QTRY_COMPARE(pane->geometry().bottom() + 1, footer->geometry().top());
        const int filledHeight = editor->height(); const QRect paneBefore = pane->geometry();
        QVERIFY(clickActualChild(detail, title->mapTo(&detail, title->rect().center())));
        QTRY_VERIFY(title->hasFocus());
        const QPoint point = body->viewport()->mapTo(&detail, QPoint(12, body->viewport()->height() - 4));
        QTRY_VERIFY(viewportIsActualHit(detail, body, point));
        QVERIFY(clickActualChild(detail, point)); QTRY_VERIFY(body->hasFocus());
        QTest::qWait(20); QCOMPARE(editor->height(), filledHeight);
        QCOMPARE(body->viewport()->cursor().shape(), Qt::IBeamCursor);
        QVERIFY(body->viewport()->rect().intersects(body->cursorRect()));
        QTest::keyClicks(body, QStringLiteral("Body above attachment")); QVERIFY(detail.finishEditing());
        QCOMPARE(m_store->entity(id).body, QStringLiteral("Body above attachment"));
        QCOMPARE(m_store->attachments(id).size(), 1);
        QTRY_COMPARE(editor->height(), filledHeight); QCOMPARE(pane->geometry(), paneBefore);
        QVERIFY(viewportIsActualHit(detail, body, point));
        const BodyViewportProbe typed = BodyViewportProbe::capture(editor);
        QVERIFY2(typed.unlaidOutTextBlocks == 0 && typed.viewportRect.contains(typed.currentCaret)
            && typed.inkPixels >= 32 && typed.logicalInkWidth >= 12, qPrintable(typed.describe()));
        QTest::keyClick(body, Qt::Key_A, Qt::ControlModifier); QTest::keyClick(body, Qt::Key_Backspace);
        QVERIFY(detail.finishEditing()); QTRY_VERIFY(editor->content().isEmpty());
        QTRY_COMPARE(editor->height(), filledHeight); QCOMPARE(pane->geometry(), paneBefore);
        QVERIFY(viewportIsActualHit(detail, body, point));
        QTRY_COMPARE(body->verticalScrollBar()->maximum(), 0);
        QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
    }
    void continuousTaskTitleEditsDoNotRefreshWholeDetail() {
        const QString id = m_store->createTask(QStringLiteral("快速创建后的标题"));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 700); detail.show();
        QVERIFY(detail.setTask(id));
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* info = detail.findChild<QToolButton*>(QStringLiteral("DetailPriority"));
        auto* date = detail.findChild<QPushButton*>(QStringLiteral("DateChip"));
        auto* group = detail.findChild<QToolButton*>(QStringLiteral("GroupChip"));
        auto* parentLink = detail.findChild<QPushButton*>(QStringLiteral("ParentTaskLink"));
        QVERIFY(title && info && date && group && parentLink);
        QCOMPARE(info->toolTip(), QStringLiteral("优先级"));

        DetailEventCounter infoEvents, dateEvents, groupEvents, parentEvents;
        info->installEventFilter(&infoEvents); date->installEventFilter(&dateEvents);
        group->installEventFilter(&groupEvents); parentLink->installEventFilter(&parentEvents);
        title->setText(QStringLiteral("持续输入的新标题"));
        QApplication::processEvents();
        const int baseline = infoEvents.count + dateEvents.count + groupEvents.count + parentEvents.count;
        for (int i = 0; i < 12; ++i) emit title->textEdited(title->text());
        QCOMPARE(infoEvents.count + dateEvents.count + groupEvents.count + parentEvents.count, baseline);
        QCOMPARE(info->toolTip(), QStringLiteral("优先级"));

        title->clear(); emit title->textEdited(title->text());
        QVERIFY(!detail.finishEditing());
        auto* status = detail.findChild<QLabel*>(QStringLiteral("DetailStatus")); QVERIFY(status);
        QVERIFY(status->isVisible()); QCOMPARE(info->toolTip(), status->text());
        title->setText(QStringLiteral("恢复有效标题")); emit title->textEdited(title->text());
        QVERIFY(status->isHidden()); QCOMPARE(info->toolTip(), QStringLiteral("优先级"));

        const QString note = m_store->createNote(QStringLiteral("切换实体仍刷新信息按钮"));
        QVERIFY(detail.setTask(note));
        auto* noteInfo = detail.findChild<QToolButton*>(QStringLiteral("DetailInfo")); QVERIFY(noteInfo);
        QCOMPARE(noteInfo->toolTip(), QStringLiteral("信息"));
        QVERIFY(detail.setTask(id));
        info = detail.findChild<QToolButton*>(QStringLiteral("DetailPriority")); QVERIFY(info);
        QCOMPARE(info->toolTip(), QStringLiteral("优先级"));
    }

    void continuousBodyReactsToBottomPaneActivityAndValidationStrip() {
        const QString id = m_store->createTask(QStringLiteral("连续正文布局重算"));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 700); detail.show(); QVERIFY(detail.setTask(id));
        detail.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* attachments = detail.findChild<AttachmentWidget*>();
        auto* activity = detail.findChild<ActivityTimelineWidget*>();
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* status = detail.findChild<QLabel*>(QStringLiteral("DetailStatus"));
        auto* scroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        auto* form = detail.findChild<QWidget*>(QStringLiteral("TaskDetailForm"));
        auto* pane = detail.findChild<QWidget*>(QStringLiteral("DetailAttachmentPane"));
        auto* statusHost = detail.findChild<QWidget*>(QStringLiteral("DetailStatusHost"));
        QVERIFY(editor && attachments && activity && title && status && scroll && form && pane && statusHost);
        QVERIFY(editor->fillAvailableHeight()); QTRY_VERIFY(editor->height() > scroll->viewport()->height() / 2);
        const int filledHeight = editor->height();
        const QString source = makeFile(m_dir->path(), QStringLiteral("layout-change.txt"), QByteArray("synthetic"));
        QVERIFY(!source.isEmpty()); const Attachment attachment = m_store->importAttachment(id, source);
        QVERIFY(!attachment.id.isEmpty()); QTRY_VERIFY(attachments->isVisible()); QTRY_VERIFY(pane->isVisible());
        QTRY_VERIFY(pane->height() > 56); QTRY_COMPARE(editor->height() + pane->height(), filledHeight);
        QTRY_VERIFY(editor->textEditor()->viewport()->height() >= 120);
        QVERIFY(m_store->removeAttachment(id, attachment.id)); QTRY_VERIFY(attachments->isHidden()); QTRY_VERIFY(pane->isHidden());
        QTRY_COMPARE(editor->height(), filledHeight);

        QVERIFY(detail.showActivity()); QTRY_VERIFY(activity->isVisible()); QVERIFY(form->isAncestorOf(activity));
        QTRY_VERIFY(editor->height() < filledHeight); QVERIFY(editor->fillAvailableHeight());
        const auto activityGap = [&]() { return rectIn(activity, form).top() - rectIn(editor, form).bottom() - 1; };
        QTRY_VERIFY2(activityGap() >= 0 && activityGap() <= 12,
            qPrintable(QStringLiteral("activity gap=%1 editorY=%2 height=%3 activityY=%4")
                .arg(activityGap()).arg(rectIn(editor, form).top()).arg(editor->height()).arg(rectIn(activity, form).top())));
        QCOMPARE(rectIn(activity, form).left(), 24);
        QCOMPARE(form->width() - rectIn(activity, form).right() - 1, 24);
        auto* close = activity->findChild<QToolButton*>(QStringLiteral("ActivityTimelineClose")); QVERIFY(close);
        scroll->ensureWidgetVisible(close, 0, 0); QApplication::processEvents();
        QVERIFY(clickActualChild(detail, close->mapTo(&detail, close->rect().center())));
        QTRY_VERIFY(activity->isHidden()); QTRY_COMPARE(editor->height(), filledHeight);
        QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);

        QVERIFY(clickActualChild(detail, title->mapTo(&detail, title->rect().center())));
        QTRY_VERIFY(title->hasFocus());
        QTest::keyClick(title, Qt::Key_A, Qt::ControlModifier); QTest::keyClick(title, Qt::Key_Backspace);
        QVERIFY(!detail.finishEditing()); QTRY_VERIFY(status->isVisible()); QTRY_VERIFY(statusHost->isVisible());
        QVERIFY(status->text().contains(QStringLiteral("标题不能为空")));
        QVERIFY(!form->isAncestorOf(status)); QVERIFY(!scroll->isAncestorOf(statusHost));
        QTRY_VERIFY(editor->height() < filledHeight); QTRY_VERIFY(editor->textEditor()->viewport()->height() >= 120);
        QTRY_COMPARE(scroll->geometry().bottom() + 1, statusHost->geometry().top());
        QVERIFY(clickActualChild(detail, status->mapTo(&detail, status->rect().center())));
        QVERIFY(!editor->textEditor()->hasFocus());
        QVERIFY(clickActualChild(detail, title->mapTo(&detail, title->rect().center())));
        QTest::keyClicks(title, QStringLiteral("Fixed title")); QVERIFY(detail.finishEditing());
        QTRY_VERIFY(status->isHidden()); QTRY_VERIFY(statusHost->isHidden()); QTRY_COMPARE(editor->height(), filledHeight);
        QCOMPARE(m_store->entity(id).title, QStringLiteral("Fixed title"));
        QVERIFY(m_store->entity(id).body.isEmpty());
    }
    void deletedBodyAndFormMarginsNeverForwardBackgroundClicks() {
        const QString id = m_store->createTask(QStringLiteral("只读与空白命中边界"));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 700); detail.show(); QVERIFY(detail.setTask(id));
        detail.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* form = detail.findChild<QWidget*>(QStringLiteral("TaskDetailForm"));
        auto* scroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        QVERIFY(editor && title && form && scroll); auto* body = editor->textEditor();
        QVERIFY(clickActualChild(detail, title->mapTo(&detail, title->rect().center())));
        QTRY_VERIFY(title->hasFocus());
        // 左右 24px 现在属于编辑器的真实 block gutter；表单留白只在标题上方的 12px。
        const QPoint marginPoint = form->mapTo(&detail, QPoint(form->width() - 2, 2));
        QWidget* marginHit = detail.childAt(marginPoint);
        QCOMPARE(marginHit, form); QVERIFY(!editor->isAncestorOf(marginHit));
        QVERIFY(marginHit != body && marginHit != body->viewport());
        QVERIFY(clickActualChild(detail, marginPoint));
        QVERIFY2(!body->hasFocus(), "The real form top margin must not forward clicks into the editor");
        QVERIFY(editor->content().isEmpty());

        QVERIFY(m_store->trashTask(id)); QTRY_VERIFY(!body->isEnabled()); QVERIFY(title->isReadOnly());
        QVERIFY(editor->fillAvailableHeight());
        QTRY_VERIFY(editor->height() > scroll->viewport()->height() / 2);
        const QPoint bodyPoint = body->viewport()->mapTo(&detail, QPoint(12, body->viewport()->height() / 2));
        QTRY_VERIFY(viewportIsActualHit(detail, body, bodyPoint));
        QVERIFY(clickActualChild(detail, bodyPoint)); QVERIFY(!body->hasFocus());
        QTest::keyClicks(body, QStringLiteral("must not persist"));
        QCOMPARE(editor->content(), QString()); QCOMPARE(m_store->entity(id).body, QString());
        const QPoint background = form->mapTo(&detail, QPoint(form->width() - 2, 2));
        QVERIFY(detail.childAt(background) != body->viewport());
        QVERIFY(clickActualChild(detail, background)); QVERIFY(!body->hasFocus());
        QVERIFY(detail.finishEditing()); QVERIFY(m_store->entity(id).deleted); QVERIFY(m_store->entity(id).body.isEmpty());
    }
    void attachmentPaneNeverOwnsOverlayBackgroundOrEditorClicks() {
        const QString first = m_store->createTask(QStringLiteral("覆盖详情附件边界"));
        const QString second = m_store->createTask(QStringLiteral("同正文无附件目标"));
        const QString sourceBody = QStringLiteral("Identical visible body");
        for (const QString& id : {first, second}) {
            Entity entity = m_store->entity(id); entity.body = sourceBody; QVERIFY(m_store->saveTask(entity));
        }
        const QString source = makeFile(m_dir->path(), QStringLiteral("overlay-card.txt"), QByteArray("synthetic overlay attachment"));
        QVERIFY(!source.isEmpty()); QVERIFY(!m_store->importAttachment(first, source).id.isEmpty());
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("kylinlist"), QStringLiteral("panel"));
        settings.setValue(QStringLiteral("showDetails"), true); settings.setValue(QStringLiteral("detailPopup"), true); settings.sync();
        TaskPanel panel(m_store.get()); panel.resize(900, 700); panel.showTask(first); panel.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&panel));
        auto* detail = panel.findChild<TaskDetailWidget*>();
        auto* frame = panel.findChild<QFrame*>(QStringLiteral("DetailFrame"));
        auto* quickAdd = panel.findChild<QLineEdit*>(QStringLiteral("QuickAdd"));
        QVERIFY(detail && frame && quickAdd); QTRY_VERIFY(frame->property("overlay").toBool());
        auto* pane = detail->findChild<QWidget*>(QStringLiteral("DetailAttachmentPane"));
        auto* divider = detail->findChild<QFrame*>(QStringLiteral("DetailAttachmentDivider"));
        auto* title = detail->findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* editor = detail->findChild<MarkdownEditor*>();
        auto* format = detail->findChild<QToolButton*>(QStringLiteral("FooterFormat"));
        QVERIFY(pane && divider && title && editor && format); auto* body = editor->textEditor();
        QTRY_VERIFY(pane->isVisible()); QTRY_VERIFY(pane->height() > 56); const int withPane = editor->height();
        QVERIFY(clickActualChild(panel, title->mapTo(&panel, title->rect().center()))); QVERIFY(title->hasFocus());
        const QPoint dividerPoint = divider->mapTo(&panel, divider->rect().center());
        QVERIFY(panel.childAt(dividerPoint) != body->viewport()); QVERIFY(clickActualChild(panel, dividerPoint));
        QVERIFY(!body->hasFocus()); QCOMPARE(editor->content(), sourceBody);
        auto* paneTitle = detail->findChild<QLabel*>(QStringLiteral("DetailAttachmentTitle")); QVERIFY(paneTitle);
        QVERIFY(clickActualChild(panel, paneTitle->mapTo(&panel, paneTitle->rect().center()))); QVERIFY(!body->hasFocus());
        const QPoint outside = quickAdd->mapTo(&panel, QPoint(8, quickAdd->height() / 2));
        QTRY_COMPARE(panel.childAt(outside), static_cast<QWidget*>(quickAdd));
        QVERIFY(!rectIn(frame, &panel).contains(outside));
        QVERIFY(clickActualChild(panel, outside)); QTRY_VERIFY(quickAdd->hasFocus()); QVERIFY(!body->hasFocus());
        dropFiles(*detail, quickAdd, {source});
        QCOMPARE(m_store->attachments(first).size(), 1); QVERIFY(m_store->attachments(second).isEmpty());
        QVERIFY(!detail->findChild<QLabel*>(QStringLiteral("AttachmentDropHint"))->isVisible());
        QCOMPARE(editor->content(), sourceBody); quickAdd->clear();
        QTest::mouseClick(format, Qt::LeftButton);
        auto* popup = detail->findChild<QFrame*>(QStringLiteral("FormatPanel")); QVERIFY(popup); QTRY_VERIFY(popup->isVisible());
        QVERIFY(editor->ownsPopup(popup)); const quint64 generation = editor->contextGeneration();
        panel.showTask(second); QTRY_COMPARE(detail->entityId(), second); QTRY_VERIFY(popup->isHidden());
        QVERIFY(editor->contextGeneration() != generation); QTRY_VERIFY(pane->isHidden());
        QTRY_VERIFY(editor->height() > withPane); QCOMPARE(editor->content(), sourceBody);
        const BodyViewportProbe probe = BodyViewportProbe::capture(editor);
        QVERIFY2(probe.visible && probe.currentEditor && probe.unlaidOutTextBlocks == 0
            && probe.viewportRect.contains(probe.firstCaret) && probe.inkPixels >= 32
            && probe.logicalInkWidth >= 12 && probe.textContrast >= 80, qPrintable(probe.describe()));
        const QPoint lower = body->viewport()->mapTo(&panel, QPoint(12, body->viewport()->height() - 6));
        QVERIFY(viewportIsActualHit(panel, body, lower)); QVERIFY(clickActualChild(panel, lower)); QVERIFY(body->hasFocus());
        QTest::keyClick(body, Qt::Key_End, Qt::ControlModifier); QTest::keyClicks(body, QStringLiteral(" current"));
        QVERIFY(detail->finishEditing()); QCOMPARE(m_store->entity(second).body, sourceBody + QStringLiteral(" current"));
        QCOMPARE(m_store->entity(first).body, sourceBody); QCOMPARE(m_store->attachments(first).size(), 1);
    }
    void readOnlyBottomPanePreservesFileActionsAndCollapsesOnSwitch() {
        const QString id = m_store->createTask(QStringLiteral("只读附件仍可另存"));
        const QString other = m_store->createTask(QStringLiteral("无附件可编辑目标"));
        const QString source = makeFile(m_dir->path(), QStringLiteral("readonly-card.txt"), QByteArray("synthetic readonly attachment"));
        QVERIFY(!source.isEmpty()); QVERIFY(!m_store->importAttachment(id, source).id.isEmpty()); QVERIFY(m_store->trashTask(id));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 700); detail.show(); QVERIFY(detail.setTask(id));
        auto* pane = detail.findChild<QWidget*>(QStringLiteral("DetailAttachmentPane"));
        auto* attachments = detail.findChild<AttachmentWidget*>();
        auto* add = detail.findChild<QToolButton*>(QStringLiteral("DetailAttachmentAdd"));
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* status = detail.findChild<QLabel*>(QStringLiteral("DetailStatus"));
        QVERIFY(pane && attachments && add && title && editor && status);
        QTRY_VERIFY(pane->isVisible()); QVERIFY(!add->isEnabled()); QVERIFY(title->isReadOnly()); QVERIFY(!editor->isEnabled());
        QVERIFY(status->isVisible()); auto rows = visibleAttachmentRows(attachments); QCOMPARE(rows.size(), 1);
        auto* open = rows.first()->findChild<QAction*>(QStringLiteral("OpenAttachment"));
        auto* save = rows.first()->findChild<QAction*>(QStringLiteral("SaveAttachmentAs"));
        auto* remove = rows.first()->findChild<QAction*>(QStringLiteral("RemoveAttachment"));
        QVERIFY(open && save && remove); QVERIFY(open->isEnabled()); QVERIFY(save->isEnabled());
        QVERIFY(!remove->isVisible()); QVERIFY(!remove->isEnabled());
        QVERIFY(m_store->restoreTask(id)); QTRY_VERIFY(add->isEnabled()); QTRY_VERIFY(editor->isEnabled());
        rows = visibleAttachmentRows(attachments); QCOMPARE(rows.size(), 1);
        remove = rows.first()->findChild<QAction*>(QStringLiteral("RemoveAttachment")); QVERIFY(remove);
        QVERIFY(remove->isVisible()); QVERIFY(remove->isEnabled());
        QVERIFY(detail.setTask(other)); QTRY_VERIFY(pane->isHidden()); QVERIFY(status->isHidden());
        QVERIFY(!title->isReadOnly()); QVERIFY(editor->isEnabled()); QVERIFY(editor->fillAvailableHeight());
        QCOMPARE(m_store->attachments(id).size(), 1); QVERIFY(m_store->attachments(other).isEmpty());
    }
    void selectedTagsMergeWithLatestEntityAndSurviveTokenRemoval() {
        const QString id = m_store->createTask(QStringLiteral("标签只作增量关联"));
        Entity entity = m_store->entity(id); entity.body = QStringLiteral("Initial #Existing");
        entity.tags = QStringList({QStringLiteral("Existing"), QStringLiteral("Keep")}); QVERIFY(m_store->saveTask(entity));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 700); detail.show(); QVERIFY(detail.setTask(id));
        detail.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&detail));
        auto* editor = detail.findChild<MarkdownEditor*>(); QVERIFY(editor); auto* body = editor->textEditor();
        const QPoint point = body->viewport()->mapTo(&detail, QPoint(12, body->viewport()->height() - 6));
        QVERIFY(viewportIsActualHit(detail, body, point)); QVERIFY(clickActualChild(detail, point)); QVERIFY(body->hasFocus());
        QTest::keyClick(body, Qt::Key_End, Qt::ControlModifier); QTest::keyClicks(body, QStringLiteral(" pending"));
        const QString pending = editor->content();
        Entity concurrent = m_store->entity(id); concurrent.tags.append(QStringLiteral("Concurrent"));
        QVERIFY(m_store->saveTask(concurrent)); QCOMPARE(editor->content(), pending);
        // 直接驱动编辑器的公开信号，单测详情的 merge/flush 边界；补全选择另有真实键盘用例。
        QVERIFY(QMetaObject::invokeMethod(editor, "tagSelected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral(" New "))));
        QCOMPARE(m_store->entity(id).body, pending); QCOMPARE(editor->content(), pending);
        QStringList tags = m_store->entity(id).tags; QCOMPARE(tags.size(), 4);
        for (const QString& name : {QStringLiteral("Existing"), QStringLiteral("Keep"), QStringLiteral("Concurrent"), QStringLiteral("New")})
            QVERIFY2(tags.contains(name, Qt::CaseInsensitive), qPrintable(name));
        QSignalSpy saved(m_store.get(), &TaskStore::taskChanged);
        QVERIFY(QMetaObject::invokeMethod(editor, "tagSelected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral("nEW"))));
        QVERIFY(QMetaObject::invokeMethod(editor, "tagSelected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral("   "))));
        QCOMPARE(saved.count(), 0); QCOMPARE(m_store->entity(id).tags.size(), 4);
        QVERIFY(body->hasFocus());
        QTest::keyClick(body, Qt::Key_A, Qt::ControlModifier); QTest::keyClicks(body, QStringLiteral("Token removed, tags retained"));
        QVERIFY(detail.finishEditing()); QCOMPARE(m_store->entity(id).body, QStringLiteral("Token removed, tags retained"));
        tags = m_store->entity(id).tags; QCOMPARE(tags.size(), 4);
        QVERIFY(tags.contains(QStringLiteral("Existing"), Qt::CaseInsensitive)); QVERIFY(tags.contains(QStringLiteral("New"), Qt::CaseInsensitive));
        const BodyViewportProbe probe = BodyViewportProbe::capture(editor);
        QVERIFY2(probe.unlaidOutTextBlocks == 0 && probe.viewportRect.contains(probe.currentCaret)
            && probe.inkPixels >= 32 && probe.logicalInkWidth >= 12, qPrintable(probe.describe()));
        TaskStore reloaded(Database::instance().connection()); QVERIFY(reloaded.load());
        QCOMPARE(reloaded.entity(id).body, QStringLiteral("Token removed, tags retained")); QCOMPARE(reloaded.entity(id).tags.size(), 4);
    }
    void selectedTagsRevalidateAfterFlushSignals_data() {
        QTest::addColumn<bool>("deleteDuringFlush");
        QTest::newRow("merge-concurrent-tags") << false;
        QTest::newRow("became-read-only") << true;
    }
    void selectedTagsRevalidateAfterFlushSignals() {
        QFETCH(bool, deleteDuringFlush);
        const QString id = m_store->createTask(QStringLiteral("提交后重读标签权限"));
        Entity entity = m_store->entity(id); entity.tags = QStringList({QStringLiteral("Keep")}); QVERIFY(m_store->saveTask(entity));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 700); detail.show(); QVERIFY(detail.setTask(id));
        detail.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&detail));
        auto* editor = detail.findChild<MarkdownEditor*>(); QVERIFY(editor); auto* body = editor->textEditor();
        const QPoint point = body->viewport()->mapTo(&detail, QPoint(12, body->viewport()->height() - 6));
        QVERIFY(viewportIsActualHit(detail, body, point)); QVERIFY(clickActualChild(detail, point)); QVERIFY(body->hasFocus());
        QTest::keyClicks(body, QStringLiteral("Pending body survives the flush"));
        bool mutated = false, mutationSucceeded = false;
        const auto connection = connect(m_store.get(), &TaskStore::taskChanged, &detail, [&](const QString& changedId) {
            if (changedId != id || mutated) return;
            mutated = true;
            if (deleteDuringFlush) mutationSucceeded = m_store->trashTask(id);
            else {
                Entity current = m_store->entity(id); current.tags.append(QStringLiteral("During flush"));
                mutationSucceeded = m_store->saveTask(current);
            }
        });
        QVERIFY(QMetaObject::invokeMethod(editor, "tagSelected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral("Selected"))));
        disconnect(connection); QVERIFY(mutated); QVERIFY(mutationSucceeded);
        const Entity after = m_store->entity(id); QCOMPARE(after.body, QStringLiteral("Pending body survives the flush"));
        QCOMPARE(editor->content(), after.body); QVERIFY(after.tags.contains(QStringLiteral("Keep")));
        if (deleteDuringFlush) {
            QVERIFY(after.deleted); QCOMPARE(after.tags.size(), 1); QVERIFY(!after.tags.contains(QStringLiteral("Selected")));
            QVERIFY(!editor->isEnabled()); QVERIFY(detail.findChild<QLabel*>(QStringLiteral("DetailStatus"))->isVisible());
        } else {
            QVERIFY(!after.deleted); QCOMPARE(after.tags.size(), 3);
            QVERIFY(after.tags.contains(QStringLiteral("During flush"))); QVERIFY(after.tags.contains(QStringLiteral("Selected")));
        }
    }
    void invalidAndReadOnlyDetailsDoNotAttachSelectedTags() {
        const QString id = m_store->createTask(QStringLiteral("标签前也校验标题"));
        const QString other = m_store->createTask(QStringLiteral("切换目标"));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 700); detail.show(); QVERIFY(detail.setTask(id));
        detail.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* status = detail.findChild<QLabel*>(QStringLiteral("DetailStatus")); QVERIFY(editor && title && status);
        detail.focusTitle(); QTest::keyClick(title, Qt::Key_Backspace); QVERIFY(title->text().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(editor, "tagSelected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral("Rejected"))));
        QVERIFY(m_store->entity(id).tags.isEmpty()); QVERIFY(status->isVisible());
        QVERIFY(status->text().contains(QStringLiteral("标题不能为空"))); QVERIFY(!detail.setTask(other)); QCOMPARE(detail.entityId(), id);
        QTest::keyClicks(title, QStringLiteral("Valid again")); QVERIFY(detail.finishEditing());
        QVERIFY(m_store->trashTask(id)); QTRY_VERIFY(!editor->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(editor, "tagSelected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral("Read only"))));
        QVERIFY(m_store->entity(id).tags.isEmpty()); QVERIFY(m_store->entity(id).deleted);
        QVERIFY(detail.setTask(other));
        QVERIFY(QMetaObject::invokeMethod(editor, "tagSelected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral("Current"))));
        QVERIFY(m_store->entity(other).tags.contains(QStringLiteral("Current"))); QVERIFY(m_store->entity(id).tags.isEmpty());
        QVERIFY(detail.setTask(QString()));
        QVERIFY(QMetaObject::invokeMethod(editor, "tagSelected", Qt::DirectConnection, Q_ARG(QString, QStringLiteral("No entity"))));
        QCOMPARE(m_store->entity(other).tags.size(), 1); QVERIFY(m_store->entity(id).tags.isEmpty());
    }
    void editorOwnedPopupKeepsOneTextSessionAndConsumesEscape() {
        const QString id = m_store->createTask(QStringLiteral("浮层不切断正文会话"));
        Entity entity = m_store->entity(id); entity.body = QStringLiteral("Start"); QVERIFY(m_store->saveTask(entity));
        QWidget window; auto* layout = new QVBoxLayout(&window);
        TaskDetailWidget detail(m_store.get(), &window); layout->addWidget(&detail, 1);
        QLineEdit outside(&window); outside.setObjectName(QStringLiteral("OutsideDetailFocus")); layout->addWidget(&outside);
        window.resize(800, 760); window.show(); QVERIFY(detail.setTask(id)); window.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* format = detail.findChild<QToolButton*>(QStringLiteral("FooterFormat")); QVERIFY(editor && format);
        auto* body = editor->textEditor();
        const auto bodyEvents = [&]() {
            QVector<EntityEvent> result;
            for (const EntityEvent& event : m_store->events(id))
                if (event.type == QLatin1String("changed") && event.field == QLatin1String("body")) result.append(event);
            return result;
        };
        const int before = bodyEvents().size();
        const QPoint point = body->viewport()->mapTo(&window, QPoint(12, body->viewport()->height() - 6));
        QVERIFY(viewportIsActualHit(window, body, point)); QVERIFY(clickActualChild(window, point)); QVERIFY(body->hasFocus());
        QTest::keyClick(body, Qt::Key_End, Qt::ControlModifier); QTest::keyClicks(body, QStringLiteral(" one"));
        QVERIFY(detail.flush()); QCOMPARE(m_store->entity(id).body, QStringLiteral("Start one")); QCOMPARE(bodyEvents().size(), before);
        QSignalSpy focusMode(&detail, &TaskDetailWidget::focusModeRequested), closed(&detail, &TaskDetailWidget::closeRequested);
        QVERIFY(QMetaObject::invokeMethod(editor, "expandRequested", Qt::DirectConnection));
        QCOMPARE(focusMode.count(), 1); QVERIFY(focusMode.first().at(0).toBool()); focusMode.clear();
        QTest::mouseClick(format, Qt::LeftButton);
        auto* popup = detail.findChild<QFrame*>(QStringLiteral("FormatPanel")); QVERIFY(popup); QTRY_VERIFY(popup->isVisible());
        QVERIFY(editor->ownsPopup(popup)); QTest::qWait(30);
        QCOMPARE(bodyEvents().size(), before); QCOMPARE(editor->content(), QStringLiteral("Start one"));
        QTest::keyClick(popup, Qt::Key_Escape); QTRY_VERIFY(popup->isHidden());
        QCOMPARE(focusMode.count(), 0); QCOMPARE(closed.count(), 0);
        QVERIFY(clickActualChild(window, point)); QVERIFY(body->hasFocus());
        QTest::keyClick(body, Qt::Key_Escape); QCOMPARE(focusMode.count(), 1); QVERIFY(!focusMode.first().at(0).toBool());
        QTest::keyClick(body, Qt::Key_End, Qt::ControlModifier); QTest::keyClicks(body, QStringLiteral(" two"));
        QVERIFY(detail.flush()); QCOMPARE(bodyEvents().size(), before);
        QVERIFY(clickActualChild(window, outside.mapTo(&window, outside.rect().center()))); QTRY_VERIFY(outside.hasFocus());
        QTRY_COMPARE(bodyEvents().size(), before + 1);
        QCOMPARE(bodyEvents().last().newValue.toString(), QStringLiteral("Start one two"));
        QCOMPARE(m_store->entity(id).body, QStringLiteral("Start one two")); QCOMPARE(editor->content(), QStringLiteral("Start one two"));
        const BodyViewportProbe probe = BodyViewportProbe::capture(editor);
        QVERIFY2(probe.unlaidOutTextBlocks == 0 && probe.viewportRect.contains(probe.currentCaret)
            && probe.inkPixels >= 32 && probe.logicalInkWidth >= 12, qPrintable(probe.describe()));
    }
    void completionAndCalendarHaveComparableVisibleInk_data() {
        QTest::addColumn<bool>("dark"); QTest::addColumn<bool>("checked");
        QTest::newRow("light-active") << false << false;
        QTest::newRow("dark-active") << true << false;
        QTest::newRow("light-completed") << false << true;
        QTest::newRow("dark-completed") << true << true;
    }
    void completionAndCalendarHaveComparableVisibleInk() {
        QFETCH(bool, dark); QFETCH(bool, checked);
        ThemeManager::instance().setPreference(dark ? ThemeManager::Preference::Dark : ThemeManager::Preference::Light);
        const QString id = m_store->createTask(QStringLiteral("头栏图标墨迹"));
        if (checked) QVERIFY(m_store->setCompleted(id, true));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 680); detail.show(); QVERIFY(detail.setTask(id));
        auto* completion = detail.findChild<QCheckBox*>(QStringLiteral("TaskCompleted"));
        auto* date = detail.findChild<QPushButton*>(QStringLiteral("DateChip"));
        auto* separator = detail.findChild<QFrame*>(QStringLiteral("DetailDateSeparator"));
        auto* overlaySeparator = detail.findChild<QFrame*>(QStringLiteral("DetailHeaderSeparator"));
        QVERIFY(completion && date && separator && overlaySeparator);
        QCOMPARE(completion->size(), QSize(30, 30)); QCOMPARE(date->iconSize(), QSize(20, 20));
        QCOMPARE(AppIcons::iconName(IconId::DetailCalendar), QStringLiteral("date-unselected"));
        QCOMPARE(date->icon().pixmap(20, 20).toImage(), AppIcons::icon(IconId::DetailCalendar, 20).pixmap(20, 20).toImage());
        QVERIFY(separator->isVisible()); QCOMPARE(separator->size(), QSize(1, 14));
        QVERIFY(rectIn(separator, &detail).left() > rectIn(completion, &detail).right());
        QVERIFY(rectIn(separator, &detail).right() < rectIn(date, &detail).left());
        QVERIFY(!overlaySeparator->isVisible());
        QTest::mouseMove(&detail, QPoint(1, 1)); QApplication::processEvents();
        const QImage painted = completion->grab().toImage();
        const qreal dpr = painted.devicePixelRatio();
        const QRect completionInk = inkBounds(painted, painted.pixelColor(0, 0));
        const QImage calendar = date->icon().pixmap(20, 20).toImage();
        const QRect calendarInk = inkBounds(calendar);
        QVERIFY(!completionInk.isEmpty() && !calendarInk.isEmpty());
        const qreal completionWidth = completionInk.width() / dpr;
        const qreal completionHeight = completionInk.height() / dpr;
        const qreal calendarWidth = calendarInk.width() / calendar.devicePixelRatio();
        const qreal calendarHeight = calendarInk.height() / calendar.devicePixelRatio();
        QVERIFY2(qAbs(completionWidth - calendarWidth) <= 2.0,
            qPrintable(QStringLiteral("completion ink %1, calendar ink %2").arg(completionWidth).arg(calendarWidth)));
        QVERIFY(qAbs(completionHeight - calendarHeight) <= 2.0);
        QVERIFY(completionWidth >= 14.0 && completionWidth <= 19.0);
        detail.setOverlayMode(true); QVERIFY(separator->isVisible()); QVERIFY(overlaySeparator->isVisible());
        detail.setOverlayMode(false); QVERIFY(separator->isVisible()); QVERIFY(!overlaySeparator->isVisible());
        // 命中区比字形大：边角点击也必须切换完成状态。
        QTest::mouseClick(completion, Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
        QCOMPARE(m_store->entity(id).completed, !checked);
    }
    void parentTitleAndBodyShareTextOrigin() {
        const QString parentId = m_store->createTask(QStringLiteral("MMMM Parent"));
        const QString childId = m_store->createSubtask(QStringLiteral("MMMM Title"), parentId);
        Entity child = m_store->entity(childId); child.body = QStringLiteral("MMMM Body"); QVERIFY(m_store->saveTask(child));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 680); detail.show(); QVERIFY(detail.setTask(childId));
        auto* parent = detail.findChild<QPushButton*>(QStringLiteral("ParentTaskLink"));
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* editor = detail.findChild<MarkdownEditor*>();
        QVERIFY(parent && title && editor); QVERIFY(parent->isVisible());
        auto* body = editor->textEditor();
        QCOMPARE(title->textMargins().top(), 0); QCOMPARE(title->textMargins().right(), 0);
        QCOMPARE(title->textMargins().bottom(), 0); QCOMPARE(body->document()->documentMargin(), 0.0);
        QCOMPARE(rectIn(parent, &detail).left(), rectIn(title, &detail).left());
        QCOMPARE(rectIn(title, &detail).left(), rectIn(body->viewport(), &detail).left());
        title->deselect(); title->setCursorPosition(0); title->clearFocus(); body->clearFocus();
        QApplication::processEvents();
        QStyleOptionButton parentStyle; parentStyle.initFrom(parent); parentStyle.rect = parent->rect();
        parentStyle.features = QStyleOptionButton::Flat; parentStyle.text = parent->text();
        const QRect parentText = parent->style()->subElementRect(QStyle::SE_PushButtonContents, &parentStyle, parent);
        const qreal parentOrigin = parent->mapTo(&detail, parentText.topLeft()).x();
        // QLineEdit 的 IME cursor rect 宽 10、居中于真实 caret；不是控件外框。
        const QRectF titleCaret = title->inputMethodQuery(Qt::ImCursorRectangle).toRectF();
        const qreal titleOrigin = title->mapTo(&detail, QPoint()).x() + titleCaret.center().x();
        QTextCursor bodyStart(body->document()); bodyStart.setPosition(0);
        const QTextLayout* layout = body->document()->firstBlock().layout();
        QVERIFY(layout && layout->lineCount() > 0);
        const qreal bodyOrigin = body->viewport()->mapTo(&detail, QPoint()).x()
            + layout->position().x() + layout->lineAt(0).cursorToX(0);
        QCOMPARE(bodyOrigin, parentOrigin);
        QCOMPARE(qreal(body->viewport()->mapTo(&detail, body->cursorRect(bodyStart).topLeft()).x()), bodyOrigin);
        // 验证真实文字起点：宿主需抵消 Qt 5.12 QLineEdit 的内部 2px inset。
        QCOMPARE(titleOrigin, parentOrigin);
        QVector<int> origins;
        for (QWidget* widget : {static_cast<QWidget*>(parent), static_cast<QWidget*>(title), body->viewport()}) {
            const QImage image = widget->grab().toImage();
            const QRect ink = inkBounds(image, image.pixelColor(0, 0)); QVERIFY(!ink.isEmpty());
            origins.append(rectIn(widget, &detail).left() + qRound(ink.left() / image.devicePixelRatio()));
        }
        // 字号不同会改变 sidebearing；在两平台均核对可见字形，不只核对容器。
        QVERIFY2(qAbs(origins.at(0) - origins.at(1)) <= 2 && qAbs(origins.at(1) - origins.at(2)) <= 2,
            qPrintable(QStringLiteral("parent/title/body ink origins: %1/%2/%3").arg(origins.at(0)).arg(origins.at(1)).arg(origins.at(2))));
        QSignalSpy opened(&detail, &TaskDetailWidget::internalLinkRequested);
        QTest::mouseClick(parent, Qt::LeftButton); QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.first().at(0).toString(), parentId);
    }
    void localFilesDropCopiesIntoAttachments_data() {
        QTest::addColumn<QString>("targetName"); QTest::addColumn<int>("fileCount");
        for (const QString& target : {QStringLiteral("body"), QStringLiteral("body-scroll"),
                                     QStringLiteral("attachment-pane"), QStringLiteral("attachment-viewport"),
                                     QStringLiteral("attachment-row"), QStringLiteral("attachment-actions")}) {
            QTest::newRow(qPrintable(target + QStringLiteral("-single"))) << target << 1;
            QTest::newRow(qPrintable(target + QStringLiteral("-multiple"))) << target << 3;
        }
    }
    void localFilesDropCopiesIntoAttachments() {
        QFETCH(QString, targetName); QFETCH(int, fileCount);
        const QString id = m_store->createTask(QStringLiteral("拖入附件"));
        Entity entity = m_store->entity(id); entity.body = QStringLiteral("正文不插入 file URL"); QVERIFY(m_store->saveTask(entity));
        const int initialCount = targetName.startsWith(QLatin1String("attachment-")) ? 1 : 0;
        if (initialCount) {
            const QString seed = makeFile(m_dir->path(), QStringLiteral("existing.txt"), QByteArray("synthetic existing card"));
            QVERIFY(!seed.isEmpty()); QVERIFY(!m_store->importAttachment(id, seed).id.isEmpty());
        }
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 680); detail.show(); QVERIFY(detail.setTask(id));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* scroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailScroll")); QVERIFY(editor && scroll);
        auto* body = editor->textEditor(); body->setPlainText(QStringLiteral("尚未保存的正文"));
        const QString pending = editor->content();
        QStringList files;
        for (int index = 0; index < fileCount; ++index) {
            const QString file = makeFile(m_dir->path(), QStringLiteral("附件 %1.txt").arg(index), QByteArray("fixture-") + QByteArray::number(index));
            QVERIFY(!file.isEmpty()); files.append(file);
        }
        QWidget* target = detailDropTarget(detail, targetName); QVERIFY(target); QTRY_VERIFY(target->isVisible());
        QSignalSpy imported(m_store.get(), &TaskStore::attachmentsChanged);
        const DropResult drop = dropFiles(detail, target, files);
        QVERIFY(drop.entered); QVERIFY(drop.moved); QVERIFY(drop.hint); QVERIFY(drop.dropped);
        QCOMPARE(drop.action, Qt::CopyAction); QCOMPARE(m_store->attachments(id).size(), initialCount + fileCount);
        QCOMPARE(imported.count(), fileCount);
        QCOMPARE(m_store->entity(id).body, pending); QCOMPARE(editor->content(), pending);
        QVERIFY(!editor->content().contains(QStringLiteral("file:")));
        QVERIFY(!detail.findChild<QLabel*>(QStringLiteral("AttachmentDropHint"))->isVisible());
        QTRY_VERIFY(detail.findChild<QWidget*>(QStringLiteral("DetailAttachmentPane"))->isVisible());
        for (const QString& source : files) QVERIFY(QFileInfo::exists(source));
        for (const Attachment& attachment : m_store->attachments(id)) {
            const QString managed = m_store->attachmentFilePath(attachment);
            QVERIFY(QFileInfo::exists(managed)); QVERIFY(!files.contains(managed));
            QFile source(m_dir->path() + QLatin1Char('/') + attachment.originalName), copy(managed);
            QVERIFY(source.open(QIODevice::ReadOnly)); QVERIFY(copy.open(QIODevice::ReadOnly));
            QCOMPARE(copy.readAll(), source.readAll());
        }
        TaskStore reloaded(Database::instance().connection()); QVERIFY(reloaded.load());
        QCOMPARE(reloaded.attachments(id).size(), initialCount + fileCount); QCOMPARE(reloaded.entity(id).body, pending);
    }
    void externalFileUrisDropThroughWindow_data() {
        QTest::addColumn<QString>("format");
        QTest::addColumn<bool>("xcbMove");
        QTest::newRow("uri-list") << QStringLiteral("text/uri-list") << false;
        QTest::newRow("peony-copy") << QStringLiteral("peony-qt/encoded-uris") << false;
        if (QGuiApplication::platformName() == QLatin1String("xcb")) {
            QTest::newRow("uri-list-xcb-move") << QStringLiteral("text/uri-list") << true;
            QTest::newRow("peony-xcb-move") << QStringLiteral("peony-qt/encoded-uris") << true;
        }
    }
    void externalFileUrisDropThroughWindow() {
        QFETCH(QString, format); QFETCH(bool, xcbMove);
        const QString id = m_store->createTask(QStringLiteral("外部文件拖入"));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 680);
        QVERIFY(detail.setTask(id));
        detail.show();
        QVERIFY(QTest::qWaitForWindowExposed(&detail));
        auto* window = detail.windowHandle();
        auto* target = detailDropTarget(detail, QStringLiteral("body"));
        QVERIFY(window && target && target->isVisible());
        const QPoint pos = target->mapTo(&detail, QPoint(20, 12));

        QMimeData mime;
        mime.setData(format, QByteArray()); // 原生选择的格式先可见，数据直到落下才读取。
        const Qt::DropAction offered = xcbMove ? Qt::MoveAction : Qt::CopyAction;
        const Qt::DropAction result = xcbMove ? Qt::IgnoreAction : Qt::CopyAction;
        QDragEnterEvent enter(pos, offered, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(window, &enter);
        QVERIFY(enter.isAccepted()); QCOMPARE(enter.dropAction(), result);
        QDragMoveEvent move(pos, offered, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(window, &move);
        QVERIFY(move.isAccepted()); QCOMPARE(move.dropAction(), result);

        const QString local = makeFile(m_dir->path(), QStringLiteral("中文 附件 #1.txt"), QByteArray("fixture"));
        QVERIFY(!local.isEmpty());
        const QByteArray uri = QUrl::fromLocalFile(local).toEncoded();
        const QByteArray separator = format == QLatin1String("text/uri-list") ? QByteArray("\r\n") : QByteArray(" ");
        mime.setData(format, uri + separator + uri + separator);
        QDropEvent drop(pos, result, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(window, &drop);
        QVERIFY(drop.isAccepted()); QCOMPARE(drop.dropAction(), result);
        const auto attachments = m_store->attachments(id);
        QCOMPARE(attachments.size(), 1);
        QCOMPARE(attachments.first().originalName, QFileInfo(local).fileName());
        QVERIFY(QFileInfo::exists(local));
        QVERIFY(m_store->entity(id).body.isEmpty());
        QVERIFY(!detail.findChild<QLabel*>(QStringLiteral("AttachmentDropHint"))->isVisible());
    }
    void failedPresaveAndDeletedDetailsRejectFileDrops_data() {
        QTest::addColumn<QString>("reason"); QTest::addColumn<QString>("targetName");
        for (const QString& reason : {QStringLiteral("empty-title"), QStringLiteral("deleted"), QStringLiteral("move-only")})
            for (const QString& target : {QStringLiteral("body"), QStringLiteral("body-scroll"),
                                         QStringLiteral("attachment-viewport"), QStringLiteral("attachment-row")})
                QTest::newRow(qPrintable(reason + QLatin1Char('-') + target)) << reason << target;
    }
    void failedPresaveAndDeletedDetailsRejectFileDrops() {
        QFETCH(QString, reason); QFETCH(QString, targetName);
        const QString id = m_store->createTask(QStringLiteral("保留的标题"));
        const int initialCount = targetName.startsWith(QLatin1String("attachment-")) ? 1 : 0;
        if (initialCount) {
            const QString seed = makeFile(m_dir->path(), QStringLiteral("retained.txt"), QByteArray("retained card"));
            QVERIFY(!seed.isEmpty()); QVERIFY(!m_store->importAttachment(id, seed).id.isEmpty());
        }
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 680); detail.show(); QVERIFY(detail.setTask(id));
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle"));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* scroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailScroll")); QVERIFY(title && editor && scroll);
        if (reason == QLatin1String("empty-title")) {
            detail.activateWindow(); detail.focusTitle(); QTest::keyClick(title, Qt::Key_Backspace);
            QCOMPARE(title->text(), QString());
        } else if (reason == QLatin1String("deleted")) QVERIFY(m_store->trashTask(id));
        const QString source = makeFile(m_dir->path(), QStringLiteral("must-stay.txt"), QByteArray("not imported"));
        QVERIFY(!source.isEmpty()); QSignalSpy imported(m_store.get(), &TaskStore::attachmentsChanged);
        QWidget* target = detailDropTarget(detail, targetName); QVERIFY(target); QTRY_VERIFY(target->isVisible());
        const DropResult drop = dropFiles(detail, target, {source},
            reason == QLatin1String("move-only") ? Qt::DropActions(Qt::MoveAction)
                                                : Qt::DropActions(Qt::CopyAction | Qt::MoveAction));
        QVERIFY(!drop.dropped); QCOMPARE(imported.count(), 0); QCOMPARE(m_store->attachments(id).size(), initialCount);
        QVERIFY(QFileInfo::exists(source)); QVERIFY(m_store->entity(id).body.isEmpty()); QVERIFY(editor->content().isEmpty());
        QVERIFY(!detail.findChild<QLabel*>(QStringLiteral("AttachmentDropHint"))->isVisible());
        if (reason == QLatin1String("empty-title")) {
            QVERIFY(drop.entered); QVERIFY(drop.hint);
            auto* status = detail.findChild<QLabel*>(QStringLiteral("DetailStatus")); QVERIFY(status);
            QVERIFY(status->isVisible()); QVERIFY(status->text().contains(QStringLiteral("标题不能为空")));
            QCOMPARE(m_store->entity(id).title, QStringLiteral("保留的标题"));
            QVERIFY(!detail.finishEditing());
            QTest::keyClicks(title, QStringLiteral("Fixed title")); QVERIFY(detail.finishEditing());
        } else { QVERIFY(!drop.entered); QVERIFY(!drop.hint); }
    }
    void dragLeaveClearsHintAndOtherWindowsDoNotImport() {
        const QString id = m_store->createTask(QStringLiteral("限定本详情窗口"));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 680); detail.show(); QVERIFY(detail.setTask(id));
        auto* body = detail.findChild<MarkdownEditor*>()->textEditor();
        auto* hint = detail.findChild<QLabel*>(QStringLiteral("AttachmentDropHint")); QVERIFY(hint);
        const QString source = makeFile(m_dir->path(), QStringLiteral("scoped.txt"), QByteArray("fixture")); QVERIFY(!source.isEmpty());
        QMimeData mime; mime.setUrls({QUrl::fromLocalFile(source)});
        QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(body->viewport(), &enter); QVERIFY(enter.isAccepted()); QVERIFY(hint->isVisible());
        QDragLeaveEvent leave; QApplication::sendEvent(body->viewport(), &leave); QVERIFY(!hint->isVisible());
        QTextEdit otherWindow(&detail); otherWindow.setWindowFlag(Qt::Tool); otherWindow.resize(300, 200); otherWindow.show();
        QVERIFY(otherWindow.window() != detail.window());
        dropFiles(detail, otherWindow.viewport(), {source});
        QVERIFY(m_store->attachments(id).isEmpty()); QVERIFY(!hint->isVisible());
        TaskDetailWidget second(m_store.get()); second.resize(480, 680); second.show();
        const QString secondId = m_store->createTask(QStringLiteral("另一个详情")); QVERIFY(second.setTask(secondId));
        auto* secondBody = second.findChild<MarkdownEditor*>()->textEditor();
        QVERIFY(dropFiles(second, secondBody->viewport(), {source}).dropped);
        QCOMPARE(m_store->attachments(secondId).size(), 1); QVERIFY(m_store->attachments(id).isEmpty());
    }
    void sharedImporterReturnsSuccessfulCount() {
        const QString id = m_store->createTask(QStringLiteral("共享附件成功导入"));
        AttachmentWidget attachments(m_store.get()); attachments.setTask(id);
        const QString first = makeFile(m_dir->path(), QStringLiteral("first.txt"), QByteArray("first"));
        const QString second = makeFile(m_dir->path(), QStringLiteral("second.txt"), QByteArray("second"));
        QVERIFY(!first.isEmpty() && !second.isEmpty());
        QCOMPARE(attachments.importPaths({first, second}), 2);
        QCOMPARE(attachments.importPaths({}), 0);
        QCOMPARE(m_store->attachments(id).size(), 2);
        QVERIFY(QFileInfo::exists(first)); QVERIFY(QFileInfo::exists(second));
        attachments.setReadOnly(true); QCOMPARE(attachments.importPaths({first}), 0);
        QCOMPARE(m_store->attachments(id).size(), 2);
    }
    void sharedImporterStopsAtFailureAndClosesWarning() {
        if (messageBoxRequiresNativePlatform()) QSKIP(MessageBoxNativeOnlyReason);
        const QString id = m_store->createTask(QStringLiteral("共享附件导入"));
        AttachmentWidget attachments(m_store.get()); attachments.setTask(id); attachments.show();
        const QString first = makeFile(m_dir->path(), QStringLiteral("first.txt"), QByteArray("first"));
        const QString second = makeFile(m_dir->path(), QStringLiteral("second.txt"), QByteArray("second"));
        QVERIFY(!first.isEmpty() && !second.isEmpty());
        int warnings = 0; QStringList warningTexts;
        QTimer dismiss; dismiss.setInterval(5);
        connect(&dismiss, &QTimer::timeout, &dismiss, [&]() {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                ++warnings; warningTexts.append(box->text()); box->done(QMessageBox::Ok);
            }
        });
        dismiss.start();
        const int count = attachments.importPaths({first, m_dir->path() + QStringLiteral("/missing.txt"), second});
        dismiss.stop();
        QCOMPARE(count, 1); QCOMPARE(warnings, 1); QVERIFY(!warningTexts.first().isEmpty());
        QCOMPARE(m_store->attachments(id).size(), 1); // 共享导入器遇到首个失败立即停止，返回此前成功数。
        QCOMPARE(attachments.importPaths({second}), 1); QCOMPARE(m_store->attachments(id).size(), 2);
        QVERIFY(QFileInfo::exists(first)); QVERIFY(QFileInfo::exists(second));
        attachments.setReadOnly(true); QCOMPARE(attachments.importPaths({first}), 0);
        QCOMPARE(m_store->attachments(id).size(), 2);
    }
    void detailMoreMenuOrderAndGeometry_data() {
        QTest::addColumn<QString>("state"); QTest::addColumn<QStringList>("expected");
        const QStringList tail{QStringLiteral("uploadAttachment"), QString(), QStringLiteral("viewActivity"),
            QStringLiteral("clone"), QStringLiteral("convertKind"), QString(), QStringLiteral("delete")};
        QTest::newRow("active") << QStringLiteral("active")
            << (QStringList{QStringLiteral("addSubtask"), QStringLiteral("linkParent"), QStringLiteral("togglePin"), QStringLiteral("abandon")} + tail);
        QTest::newRow("completed") << QStringLiteral("completed") << (QStringList{QStringLiteral("reactivate")} + tail);
        QTest::newRow("abandoned") << QStringLiteral("abandoned") << (QStringList{QStringLiteral("reactivate")} + tail);
        QTest::newRow("note") << QStringLiteral("note") << (QStringList{QStringLiteral("togglePin")} + tail);
        QTest::newRow("deleted") << QStringLiteral("deleted")
            << QStringList{QStringLiteral("restore"), QStringLiteral("viewActivity"), QString(), QStringLiteral("deletePermanently")};
    }
    void detailMoreMenuOrderAndGeometry() {
        QFETCH(QString, state); QFETCH(QStringList, expected);
        const QString id = state == QLatin1String("note") ? m_store->createNote(QStringLiteral("菜单笔记"))
                                                          : m_store->createTask(QStringLiteral("菜单任务"));
        if (state == QLatin1String("completed")) QVERIFY(m_store->setCompleted(id, true));
        if (state == QLatin1String("abandoned")) QVERIFY(m_store->abandonTask(id));
        if (state == QLatin1String("deleted")) QVERIFY(m_store->trashTask(id));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 760); detail.show(); QVERIFY(detail.setTask(id));
        QStringList actual, badActions; QVector<int> heights, separatorHeights; QVector<QSize> icons;
        int width = 0; bool popupFlags = false, transparent = false, emptyMask = false, rounded = false;
        bool closedWithEscape = false;
        QVERIFY(withMenu(detail, detail.findChild<QToolButton*>(QStringLiteral("DetailMore")),
            QStringLiteral("DetailMoreMenu"), [&](PopupMenu* menu) {
                width = menu->width() - 2 * PopupMenu::shadowMargin();
                popupFlags = menu->windowFlags().testFlag(Qt::Popup) && menu->windowFlags().testFlag(Qt::NoDropShadowWindowHint);
                transparent = menu->testAttribute(Qt::WA_TranslucentBackground); emptyMask = menu->mask().isEmpty();
                for (QAction* action : menu->actions()) {
                    actual.append(action->isSeparator() ? QString() : action->objectName());
                    if (action->isSeparator()) separatorHeights.append(menu->actionGeometry(action).height());
                    else {
                        heights.append(menu->actionGeometry(action).height()); icons.append(action->icon().actualSize(QSize(18, 18)));
                        if (!action->isEnabled() || !action->isVisible() || action->text().isEmpty()
                            || qobject_cast<QWidgetAction*>(action)) badActions.append(action->objectName());
                    }
                }
                const QImage image = menu->grab().toImage(); const qreal dpr = image.devicePixelRatio();
                const auto alphaAt = [&](int x, int y) { return image.pixelColor(qRound(x * dpr), qRound(y * dpr)).alpha(); };
                const int margin = PopupMenu::shadowMargin();
                // 12px 圆角：角点不是不透明直角，12px 内切点已进入完整卡片。
                rounded = alphaAt(margin, margin) < 100 && alphaAt(margin + 12, margin + 2) > 240
                    && alphaAt(margin + 2, margin + 12) > 240;
                QTest::keyClick(menu, Qt::Key_Escape); closedWithEscape = !menu->isVisible();
            }));
        QCOMPARE(actual, expected); QVERIFY(badActions.isEmpty()); QCOMPARE(width, 164);
        QVERIFY(popupFlags && transparent && emptyMask && rounded); QVERIFY(closedWithEscape);
        for (int height : heights) QCOMPARE(height, 34);
        for (int height : separatorHeights) QCOMPARE(height, 9);
        for (const QSize& icon : icons) QCOMPARE(icon, QSize(18, 18));
    }
    void detailCommandsOperateByKeyboardWithoutPlaceholderActions() {
        const QString id = m_store->createTask(QStringLiteral("键盘详情命令"));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 760); detail.show(); QVERIFY(detail.setTask(id));
        auto* title = detail.findChild<QLineEdit*>(QStringLiteral("TaskTitle")); QVERIFY(title);
        detail.activateWindow(); detail.focusTitle(); QTest::keyClicks(title, QStringLiteral("Saved before command"));
        QVERIFY(detailCommand(detail, QStringLiteral("togglePin")));
        QCOMPARE(m_store->entity(id).title, QStringLiteral("Saved before command")); QVERIFY(m_store->entity(id).pinnedAt.isValid());
        QVERIFY(detailCommand(detail, QStringLiteral("abandon"))); QCOMPARE(m_store->entity(id).status, TaskStatus::Abandoned);
        QVERIFY(detailCommand(detail, QStringLiteral("reactivate"))); QCOMPARE(m_store->entity(id).status, TaskStatus::Active);
        QVERIFY(detailCommand(detail, QStringLiteral("viewActivity")));
        auto* activity = detail.findChild<ActivityTimelineWidget*>(); QVERIFY(activity && activity->isVisible());
        QVERIFY(detailCommand(detail, QStringLiteral("delete"))); QVERIFY(m_store->entity(id).deleted);
        QVERIFY(detailCommand(detail, QStringLiteral("restore"))); QVERIFY(!m_store->entity(id).deleted);
    }
    void detailPriorityAndInfoButtonsSplitByKind() {
        // 任务：头部按钮是 DetailPriority（旗标 + 优先级菜单）；笔记：保持 DetailInfo（信息弹层）。
        const QString task = m_store->createTask(QStringLiteral("优先级任务"));
        Entity withPriority = m_store->entity(task);
        withPriority.priority = 2;
        QVERIFY(m_store->saveTask(withPriority));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 760); detail.show(); QVERIFY(detail.setTask(task));
        QVERIFY(!detail.findChild<QToolButton*>(QStringLiteral("DetailInfo")));
        auto* priority = detail.findChild<QToolButton*>(QStringLiteral("DetailPriority"));
        QVERIFY(priority && priority->isVisible());
        QCOMPARE(priority->accessibleName(), QStringLiteral("设置优先级"));
        QCOMPARE(priority->icon().pixmap(QSize(18, 18)).toImage(),
                 AppIcons::icon(IconId::FlagMedium, 18).pixmap(18, 18).toImage());

        QStringList names; QString checked; bool entityMenu = false;
        QVERIFY(withMenu(detail, priority, QStringLiteral("PriorityMenu"), [&](PopupMenu* menu) {
            entityMenu = menu->property("entityMenu").toBool();
            for (QAction* action : menu->actions()) {
                names.append(action->objectName());
                if (action->isChecked()) checked = action->objectName();
            }
        }));
        QVERIFY(entityMenu);
        QCOMPARE(names, QStringList({QStringLiteral("setPriorityHigh"), QStringLiteral("setPriorityMedium"),
                                     QStringLiteral("setPriorityLow"), QStringLiteral("setPriorityNone")}));
        QCOMPARE(checked, QStringLiteral("setPriorityMedium"));  // 当前优先级被勾选

        QVERIFY(withMenu(detail, priority, QStringLiteral("PriorityMenu"), [&](PopupMenu* menu) {
            keyboardChoose(menu, QStringLiteral("setPriorityHigh"));
        }));
        QCOMPARE(m_store->entity(task).priority, 3);
        QTRY_COMPARE(priority->icon().pixmap(QSize(18, 18)).toImage(),
                     AppIcons::icon(IconId::FlagHigh, 18).pixmap(18, 18).toImage());

        // 笔记：DetailInfo + 信息弹层；日期入口保留，独立提醒状态按钮移除。
        const QString note = m_store->createNote(QStringLiteral("信息笔记"));
        QVERIFY(!m_store->addReminder(note, appNow().addDays(1)).id.isEmpty());
        QVERIFY(detail.setTask(note));
        QVERIFY(!detail.findChild<QToolButton*>(QStringLiteral("DetailPriority")));
        auto* info = detail.findChild<QToolButton*>(QStringLiteral("DetailInfo"));
        QVERIFY(info && info->isVisible());
        QCOMPARE(info->accessibleName(), QStringLiteral("信息"));
        auto* dateChip = detail.findChild<QPushButton*>(QStringLiteral("DateChip"));
        QVERIFY(dateChip && dateChip->isVisible());
        QVERIFY(!detail.findChild<QToolButton*>(QStringLiteral("DetailReminder")));
        QCOMPARE(m_store->entity(note).reminders.size(), 1);
        QTest::mouseClick(dateChip, Qt::LeftButton);
        DatePickerPopup* picker = nullptr;
        QTRY_VERIFY((picker = detail.findChild<DatePickerPopup*>()) && picker->isVisible());
        bool hasReminderEntry = false;
        for (QPushButton* entry : picker->findChildren<QPushButton*>(QStringLiteral("DatePickerEntry")))
            if (entry->accessibleName() == QStringLiteral("提醒")) hasReminderEntry = true;
        QVERIFY(hasReminderEntry);
        picker->reject();
        QTest::mouseClick(info, Qt::LeftButton);
        auto* popup = detail.findChild<EntityInfoPopup*>(QStringLiteral("EntityInfoPopup"));
        QTRY_VERIFY(popup && popup->isVisible());
        popup->hide();
    }

    void detailDateChipUsesStableInfoAndOverdueInk() {
        // 日期芯片配色只随状态切换：未排期=次要灰、已排期=Info 蓝、逾期=Overdue 红。
        const QDate today = toAppTime(appNow()).date();
        const QString id = m_store->createTask(QStringLiteral("日期芯片配色"));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 760); detail.show(); QVERIFY(detail.setTask(id));
        auto* chip = detail.findChild<QPushButton*>(QStringLiteral("DateChip")); QVERIFY(chip);
        const auto chipIcon = [&chip]() {
            return chip->icon().pixmap(QSize(20, 20)).toImage();
        };
        QCOMPARE(chipIcon(), AppIcons::icon(IconId::DetailCalendar, ds::ColorToken::TextSecondary, 20)
                     .pixmap(20, 20).toImage());

        Entity future = m_store->entity(id);
        future.allDayDate = today.addDays(3);
        QVERIFY(m_store->saveTask(future));
        QTRY_COMPARE(chipIcon(), AppIcons::icon(IconId::DetailCalendar, ds::ColorToken::Info, 20)
                                      .pixmap(20, 20).toImage());
        QVERIFY(chip->property("hasSchedule").toBool());
        QVERIFY(!chip->property("overdue").toBool());

        Entity late = m_store->entity(id);
        late.allDayDate = today.addDays(-3);
        QVERIFY(m_store->saveTask(late));
        QTRY_COMPARE(chipIcon(), AppIcons::icon(IconId::DetailCalendar, ds::ColorToken::Overdue, 20)
                                      .pixmap(20, 20).toImage());
        QVERIFY(chip->property("overdue").toBool());

        // 完成后不再逾期红：回到 Info。
        QVERIFY(m_store->setCompleted(id, true));
        QTRY_COMPARE(chipIcon(), AppIcons::icon(IconId::DetailCalendar, ds::ColorToken::Info, 20)
                                      .pixmap(20, 20).toImage());
        QVERIFY(!chip->property("overdue").toBool());
    }

    void detailConversionRunsWithoutConfirmationAndShowsToast() {
        // 类型转换不再弹确认框：直接执行，成功后宿主窗口显示「已转换」Toast。
        const QString id = m_store->createTask(QStringLiteral("免确认类型转换"));
        Entity entity = m_store->entity(id); entity.body = QStringLiteral("转换保留正文"); QVERIFY(m_store->saveTask(entity));
        TaskDetailWidget detail(m_store.get()); detail.resize(480, 760); detail.show(); QVERIFY(detail.setTask(id));
        int unexpectedDialogs = 0;
        QTimer watch; watch.setInterval(5);
        connect(&watch, &QTimer::timeout, &watch, [&]() {
            if (qobject_cast<QMessageBox*>(QApplication::activeModalWidget())
                || qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                ++unexpectedDialogs;
        });
        watch.start(); QVERIFY(detailCommand(detail, QStringLiteral("convertKind"))); watch.stop();
        QCOMPARE(unexpectedDialogs, 0);
        QCOMPARE(m_store->entity(id).kind, EntityKind::Note);
        QCOMPARE(m_store->entity(id).body, entity.body);
        QCOMPARE(m_store->entity(id).title, entity.title);
        auto* toast = detail.window()->findChild<ToastIsland*>(QStringLiteral("EntityCommandToast"),
                                                               Qt::FindDirectChildrenOnly);
        QVERIFY(toast && toast->isVisible());
        QCOMPARE(toast->findChild<QLabel*>(QStringLiteral("ToastText"))->text(), QStringLiteral("已转换"));
        toast->hide();
    }
    void successfulDeleteSignalsAndClosesOverlay_data() {
        QTest::addColumn<bool>("permanent");
        QTest::newRow("trash") << false;
        QTest::newRow("permanent") << true;
    }
    void successfulDeleteSignalsAndClosesOverlay() {
        QFETCH(bool, permanent);
        if (permanent && messageBoxRequiresNativePlatform()) QSKIP(MessageBoxNativeOnlyReason);
        const QString id = m_store->createTask(QStringLiteral("移除后关闭覆盖详情"));
        if (permanent) QVERIFY(m_store->trashTask(id));
        TaskPanel panel(m_store.get()); panel.resize(900, 700); panel.showTask(id);
        auto* detail = panel.findChild<TaskDetailWidget*>();
        auto* frame = panel.findChild<QFrame*>(QStringLiteral("DetailFrame"));
        QVERIFY(detail && frame); QTRY_VERIFY(frame->isVisible()); QVERIFY(frame->property("overlay").toBool());
        QSignalSpy removed(detail, &TaskDetailWidget::taskRemoved);
        int confirmations = 0;
        QTimer confirm; confirm.setInterval(5);
        connect(&confirm, &QTimer::timeout, &confirm, [&]() {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                ++confirmations;
                if (auto* yes = box->button(QMessageBox::Yes)) yes->click();
            }
        });
        if (permanent) confirm.start();
        const bool chosen = detailCommand(*detail, permanent ? QStringLiteral("deletePermanently") : QStringLiteral("delete"));
        confirm.stop(); QVERIFY(chosen);
        QCOMPARE(confirmations, permanent ? 1 : 0);
        QCOMPARE(removed.count(), 1); QCOMPARE(removed.first().at(0).toString(), id);
        QTRY_VERIFY(!frame->isVisible());
        if (permanent) QVERIFY(m_store->entity(id).id.isEmpty());
        else QVERIFY(m_store->entity(id).deleted);
    }
    void focusedPanelCanBeDestroyedWithoutClearingFocus_data() {
        QTest::addColumn<QString>("focusTarget");
        QTest::newRow("quick-add-focused") << QStringLiteral("QuickAdd");
        QTest::newRow("task-list-focused") << QStringLiteral("TaskList");
    }
    void focusedPanelCanBeDestroyedWithoutClearingFocus() {
        QFETCH(QString, focusTarget);
        const QString id = m_store->createTask(QStringLiteral("持焦点销毁窗口"));
        std::unique_ptr<TaskPanel> owner(new TaskPanel(m_store.get()));
        auto* panel = owner.get();
        panel->resize(1240, 820); panel->showTask(id); panel->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(panel));
        auto* target = panel->findChild<QWidget*>(focusTarget); QVERIFY(target);
        target->setFocus(); QTRY_VERIFY(target->hasFocus());
        QPointer<TaskPanel> lifetime(panel); QSignalSpy destroyed(panel, &QObject::destroyed);
        // 不提前 hide/clearFocus：真实析构必须能处理子控件 clearFocus 引发的 qApp 信号。
        owner.reset(); // 同步 delete；断言提前退出也由 RAII 保证销毁，不泄漏持焦点窗口。
        QVERIFY(lifetime.isNull()); QCOMPARE(destroyed.count(), 1);
        QCOMPARE(m_store->entity(id).title, QStringLiteral("持焦点销毁窗口"));
    }
    void groupingMenuSwitchesAndRestoresPerScopePreferences() {
        const QString folder = m_store->createFolder(QStringLiteral("分组偏好文件夹"));
        const QString first = m_store->createList(QStringLiteral("偏好清单一"), folder);
        const QString second = m_store->createList(QStringLiteral("偏好清单二"), folder);
        const QString id = m_store->createTask(QStringLiteral("分组不改实体"), first);
        Entity entity = m_store->entity(id); entity.body = QStringLiteral("body"); entity.priority = 2;
        entity.allDayDate = toAppTime(appNow()).date(); entity.sortOrder = 7; QVERIFY(m_store->saveTask(entity));
        const int eventCount = m_store->events(id).size();
        QSignalSpy changed(m_store.get(), &TaskStore::taskChanged), events(m_store.get(), &TaskStore::eventsChanged);
        {
            TaskPanel panel(m_store.get()); panel.resize(1240, 820); panel.show();
            auto* model = panelModel(panel); QVERIFY(model); QCOMPARE(model->filter().grouping, GroupMode::Time);
            GroupMenuSnapshot all;
            QVERIFY(chooseGrouping(panel, GroupMode::Priority, &all));
            QCOMPARE(all.parentCardWidth, 202); QCOMPARE(all.childCardWidth, 164); QVERIFY(all.childVisible);
            QCOMPARE(all.names, QStringList({QStringLiteral("GroupMode_list"), QStringLiteral("GroupMode_time"),
                QStringLiteral("GroupMode_created"), QStringLiteral("GroupMode_priority"), QStringLiteral("GroupMode_none")}));
            QCOMPARE(all.modes, QVector<int>({0, 1, 2, 3, 4}));
            QCOMPARE(all.checked, QStringLiteral("GroupMode_time")); QCOMPARE(all.trailing, QStringLiteral("时间"));
            for (const QString& text : all.texts) { QVERIFY(!text.contains(QStringLiteral("排序"))); QVERIFY(!text.contains(QStringLiteral("顺序"))); }
            QCOMPARE(model->filter().grouping, GroupMode::Priority);
            QSettings prefs(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("kylinlist"), QStringLiteral("panel"));
            prefs.sync(); QCOMPARE(prefs.value(groupingKey(ViewScope::AllLists)).toString(), QStringLiteral("priority"));
            QVERIFY(navigate(panel, ViewScope::List, first)); QCOMPARE(model->filter().grouping, GroupMode::Time);
            GroupMenuSnapshot single; QVERIFY(chooseGrouping(panel, GroupMode::None, &single));
            QVERIFY(!single.names.contains(QStringLiteral("GroupMode_list"))); QCOMPARE(single.names.size(), 4);
            QCOMPARE(model->filter().grouping, GroupMode::None);
            prefs.sync(); QCOMPARE(prefs.value(groupingKey(ViewScope::List, first)).toString(), QStringLiteral("none"));
            QVERIFY(navigate(panel, ViewScope::List, second)); QCOMPARE(model->filter().grouping, GroupMode::Time);
            QVERIFY(chooseGrouping(panel, GroupMode::Created)); QCOMPARE(model->filter().grouping, GroupMode::Created);
            QVERIFY(navigate(panel, ViewScope::Folder, folder)); QCOMPARE(model->filter().grouping, GroupMode::List);
            QVERIFY(chooseGrouping(panel, GroupMode::Time));
            QVERIFY(navigate(panel, ViewScope::AllLists)); QCOMPARE(model->filter().grouping, GroupMode::Priority);
            QVERIFY(!panel.findChild<QComboBox*>(QStringLiteral("ViewRenderer")));
        }
        {
            TaskPanel restored(m_store.get()); restored.resize(1240, 820); restored.show();
            auto* model = panelModel(restored); QVERIFY(model); QCOMPARE(model->filter().grouping, GroupMode::Priority);
            QVERIFY(navigate(restored, ViewScope::List, first)); QCOMPARE(model->filter().grouping, GroupMode::None);
            QVERIFY(navigate(restored, ViewScope::List, second)); QCOMPARE(model->filter().grouping, GroupMode::Created);
            QVERIFY(navigate(restored, ViewScope::Folder, folder)); QCOMPARE(model->filter().grouping, GroupMode::Time);
        }
        QCOMPARE(changed.count(), 0); QCOMPARE(events.count(), 0); QCOMPARE(m_store->events(id).size(), eventCount);
        const Entity after = m_store->entity(id);
        QCOMPARE(after.title, entity.title); QCOMPARE(after.body, entity.body); QCOMPARE(after.listId, entity.listId);
        QCOMPARE(after.priority, entity.priority); QCOMPARE(after.allDayDate, entity.allDayDate); QCOMPARE(after.sortOrder, entity.sortOrder);
    }
    void noGroupingKeepsOrderAndDisplayCheckboxControlsTerminalFooters() {
        const QDate today = toAppTime(appNow()).date();
        const QString slow = m_store->createTask(QStringLiteral("顺序七"));
        const QString later = m_store->createTask(QStringLiteral("顺序一较晚"));
        const QString earlier = m_store->createTask(QStringLiteral("顺序一较早"));
        for (const QString& id : {slow, later, earlier}) {
            Entity entity = m_store->entity(id); entity.sortOrder = id == slow ? 7 : 1;
            entity.allDayDate = id == later ? today.addDays(3) : today; QVERIFY(m_store->saveTask(entity));
        }
        const QString pinned = m_store->createTask(QStringLiteral("置顶先于普通")); QVERIFY(m_store->setPinnedAt(pinned, appNow()));
        const QString note = m_store->createNote(QStringLiteral("笔记殿后"));
        const QString completed = m_store->createTask(QStringLiteral("终态完成")); QVERIFY(m_store->setCompleted(completed, true));
        const QString abandoned = m_store->createTask(QStringLiteral("终态放弃")); QVERIFY(m_store->abandonTask(abandoned));
        TaskPanel panel(m_store.get()); panel.resize(1240, 820); panel.show(); auto* model = panelModel(panel); QVERIFY(model);
        QVERIFY(chooseGrouping(panel, GroupMode::None));
        QStringList flat;
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex index = model->index(row, 0);
            if (index.data(TaskListModel::SectionKeyRole).toString() != QLatin1String("none")) continue;
            QVERIFY(!index.data(TaskListModel::HeaderRole).toBool()); flat.append(model->entityId(index));
        }
        QCOMPARE(flat, QStringList({earlier, later, slow}));
        QVERIFY(model->indexForTask(pinned).row() < model->indexForTask(earlier).row());
        QVERIFY(model->indexForTask(note).row() > model->indexForTask(slow).row());
        QVERIFY(!model->indexForTask(completed).isValid()); QVERIFY(!model->indexForTask(abandoned).isValid());
        const DisplayResult show = toggleCompletedSetting(panel);
        QVERIFY(show.opened && show.checkboxFound); QVERIFY(!show.initial && show.final);
        QCOMPARE(show.completedSignals, 1); QCOMPARE(show.legacySignals, 0); QVERIFY(model->filter().showCompleted);
        const QModelIndex completedIndex = model->indexForTask(completed), abandonedIndex = model->indexForTask(abandoned);
        QVERIFY(completedIndex.isValid() && abandonedIndex.isValid());
        QCOMPARE(completedIndex.data(TaskListModel::SectionKeyRole).toString(), QStringLiteral("completed"));
        QCOMPARE(abandonedIndex.data(TaskListModel::SectionKeyRole).toString(), QStringLiteral("abandoned"));
        QVERIFY(model->indexForTask(note).row() < completedIndex.row()); QVERIFY(completedIndex.row() < abandonedIndex.row());
        const DisplayResult hide = toggleCompletedSetting(panel);
        QVERIFY(hide.opened && hide.checkboxFound); QVERIFY(hide.initial && !hide.final);
        QCOMPARE(hide.completedSignals, 1); QCOMPARE(hide.legacySignals, 0); QVERIFY(!model->filter().showCompleted);
        QVERIFY(!model->indexForTask(completed).isValid()); QVERIFY(!model->indexForTask(abandoned).isValid());
    }
private:
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<TaskStore> m_store;
};

QTEST_MAIN(DetailTest)
#include "tst_detail.moc"
