#include <cstdio>
#include <memory>

#include <QAction>
#include <QApplication>
#include <QGuiApplication>
#include <QLayout>
#include <QImage>
#include <QMenu>
#include <QPainter>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextEdit>
#include <QToolButton>
#include <QVariant>
#include <QWidgetAction>
#include <QtTest>

#include "app/TaskStore.h"
#include "db/Database.h"
#include "platform/SystemShadow.h"
#include "ui/AppStyle.h"
#include "ui/FormatToolbar.h"
#include "ui/MarkdownEditor.h"
#include "ui/PopupMenu.h"
#include "ui/PopupShadow.h"
#include "ui/TaskDetailWidget.h"
#include "ui/design/ThemeManager.h"

using namespace kl;

namespace {
using Action = MarkdownEditor::FormatAction;
using BlockStyle = MarkdownEditor::BlockStyle;

bool expose(QWidget& widget) {
    const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
    widget.move(available.topLeft() + QPoint(40, 30));
    widget.show();
    widget.activateWindow();
    return QTest::qWaitForWindowExposed(&widget) && QTest::qWaitForWindowActive(&widget);
}

QRect globalRect(const QWidget* widget) {
    return QRect(widget->mapToGlobal(QPoint()), widget->size());
}

QRect menuCard(const PopupMenu* menu) {
    return globalRect(menu).adjusted(PopupShadowMargin, PopupShadowMargin,
                                      -PopupShadowMargin, -PopupShadowMargin);
}

QRect detailBounds(TaskDetailWidget& detail) {
    auto* pane = detail.findChild<QWidget*>(QStringLiteral("TaskDetailHost"));
    if (!pane) return QRect();
    QRect bounds = globalRect(pane).intersected(QGuiApplication::primaryScreen()->availableGeometry());
    bounds.adjust(16, 16, -16, -16);
    if (auto* footer = detail.findChild<QWidget*>(QStringLiteral("DetailFooter")))
        if (footer->isVisible()) bounds.setBottom(qMin(bounds.bottom(), footer->mapToGlobal(QPoint()).y() - 6));
    return bounds;
}

QList<QAction*> visibleActions(QMenu* menu) {
    QList<QAction*> result;
    for (QAction* action : menu->actions()) if (action->isVisible()) result.append(action);
    return result;
}

QAction* formatAction(QMenu* menu, Action kind) {
    if (!menu) return nullptr;
    for (QAction* action : menu->actions())
        if (action->isVisible() && action->property("formatAction").isValid() &&
            action->property("formatAction").toInt() == static_cast<int>(kind)) return action;
    return nullptr;
}

QAction* blockAction(QMenu* menu, BlockStyle kind) {
    if (!menu) return nullptr;
    for (QAction* action : menu->actions())
        if (action->isVisible() && action->property("blockStyle").isValid() &&
            action->property("blockStyle").toInt() == static_cast<int>(kind)) return action;
    return nullptr;
}

QToolButton* formatButton(QWidget* card, Action kind) {
    if (!card) return nullptr;
    for (QToolButton* button : card->findChildren<QToolButton*>())
        if (button->property("formatAction").isValid() &&
            button->property("formatAction").toInt() == static_cast<int>(kind)) return button;
    return nullptr;
}

bool clickButton(QToolButton* button) {
    if (!button || !button->isVisible() || !button->isEnabled()) return false;
    QTest::mouseClick(button, Qt::LeftButton, Qt::NoModifier, button->rect().center());
    return true;
}

bool clickAction(PopupMenu* menu, QAction* action) {
    if (!menu || !menu->isVisible() || !action || !action->isVisible() || !action->isEnabled()) return false;
    const QRect row = menu->actionGeometry(action);
    if (row.isEmpty() || !menu->rect().contains(row.center())) return false;
    QTest::mouseClick(menu, Qt::LeftButton, Qt::NoModifier, row.center());
    return true;
}

FormatPanel* openFooter(TaskDetailWidget& detail) {
    if (!clickButton(detail.findChild<QToolButton*>(QStringLiteral("FooterFormat")))) return nullptr;
    return detail.findChild<FormatPanel*>(QStringLiteral("FormatPanel"));
}

void selectSource(MarkdownEditor* editor, int anchor, int position) {
    editor->textEditor()->setFocus();
    editor->selectSource(anchor, position);
    editor->textEditor()->ensureCursorVisible();
    QApplication::processEvents();
}

QImage renderSurface(QWidget* surface) {
    QImage image(surface->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    // 只核对组件自身的绘制结果；这不是对原生窗口合成或人工截图的验收。
    surface->render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
    return image;
}

QRect inkBounds(const QImage& image) {
    QRect result;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(image.pixel(x, y)) > 40) result = result.united(QRect(x, y, 1, 1));
    return result;
}
}  // namespace

// 使用真实可见的详情入口、选区卡片和 PopupMenu，不访问已移除的旧工具栏按钮。
class EditorToolsTest : public QObject {
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
        AppStyle::apply(*qobject_cast<QApplication*>(qApp));
        disableNativeWindowShadows();
    }

    void init() {
        m_dir.reset(new QTemporaryDir);
        QVERIFY(m_dir->isValid());
        qputenv("KYLINLIST_DATA_DIR", m_dir->path().toUtf8());
        ThemeManager::instance().setPreference(ThemeManager::Preference::Light);
        QString error;
        QVERIFY2(Database::instance().openAt(m_dir->path() + QStringLiteral("/editortools.db"), &error), qPrintable(error));
        m_store.reset(new TaskStore(Database::instance().connection()));
        QVERIFY2(m_store->load(&error), qPrintable(error));
    }

    void cleanup() {
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

    void sharedShadowIsPremultipliedContinuousAndOffset() {
        QCOMPARE(PopupShadowMargin, 24);
        const QSize size(160, 120);
        const QRectF card(20, 20, 120, 80);
        const QColor color = ThemeManager::instance().color(ds::ColorToken::Shadow);
        const QImage shadow = popupShadow(size, card, 12, color);
        QCOMPARE(shadow.size(), size);
        QCOMPARE(shadow.format(), QImage::Format_ARGB32_Premultiplied);
        int peak = 0;
        for (int y = 0; y < shadow.height(); ++y) {
            for (int x = 0; x < shadow.width(); ++x) {
                const QRgb pixel = shadow.pixel(x, y);
                peak = qMax(peak, qAlpha(pixel));
                QVERIFY(qRed(pixel) <= qAlpha(pixel));
                QVERIFY(qGreen(pixel) <= qAlpha(pixel));
                QVERIFY(qBlue(pixel) <= qAlpha(pixel));
            }
        }
        QVERIFY(peak >= 49 && peak <= 52); // 0.20 的同一峰值，没有重复叠加的描边。
        QVERIFY(qAlpha(shadow.pixel(80, 108)) > qAlpha(shadow.pixel(80, 12)));
        QVERIFY(qAlpha(shadow.pixel(16, 60)) > 0);
        for (int x = 1; x < shadow.width(); ++x)
            QVERIFY(qAbs(qAlpha(shadow.pixel(x, 12)) - qAlpha(shadow.pixel(x - 1, 12))) <= 3);
        QVERIFY(popupShadow(QSize(), card, 12, color).isNull());
    }

    void selectionCardHasBalancedRealButtonAndGlyphPadding() {
        const QString id = createTask(QStringLiteral("Select this plain body"));
        QVERIFY(!id.isEmpty());
        TaskDetailWidget detail(m_store.get());
        detail.resize(460, 640);
        QVERIFY(detail.setTask(id));
        QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* bubble = detail.findChild<SelectionBubble*>();
        QVERIFY(editor && bubble);
        selectSource(editor, 0, 6);
        QTRY_VERIFY(bubble->isVisible());
        auto* card = bubble->findChild<QWidget*>(QStringLiteral("SelectionBubbleCard"));
        QVERIFY(card && card->layout());
        QCOMPARE(card->height(), 32);
        QCOMPARE(card->layout()->contentsMargins(), QMargins(4, 4, 4, 4));
        QCOMPARE(bubble->layout()->contentsMargins(), QMargins(24, 24, 24, 24));
        QVERIFY(!card->graphicsEffect());
        QVERIFY(bubble->testAttribute(Qt::WA_TranslucentBackground));
        QVERIFY(bubble->windowFlags().testFlag(Qt::NoDropShadowWindowHint));
        QVERIFY(bubble->mask().isEmpty());
        const auto buttons = card->findChildren<QToolButton*>();
        QCOMPARE(buttons.size(), 9); // 文本类型 + 七个主动作 + 更多，全部都是真实入口。
        QVector<int> actual;
        for (QToolButton* button : buttons) {
            QVERIFY(button->isVisible());
            QCOMPARE(button->height(), 24);
            QCOMPARE(button->iconSize(), QSize(18, 18));
            const QRect rect(button->mapTo(card, QPoint()), button->size());
            QCOMPARE(rect.top(), 4);
            QCOMPARE(card->height() - rect.bottom() - 1, 4);
            if (button->property("formatAction").isValid()) actual.append(button->property("formatAction").toInt());
        }
        const QVector<int> expected = {int(Action::Bold), int(Action::Italic), int(Action::Strike),
            int(Action::Underline), int(Action::Highlight), int(Action::Code), int(Action::Link)};
        QCOMPARE(actual, expected);
        auto* bold = formatButton(card, Action::Bold);
        QVERIFY(bold);
        const int iconTop = bold->mapTo(card, QPoint()).y() + (bold->height() - bold->iconSize().height()) / 2;
        QCOMPARE(iconTop, 7);
        QCOMPARE(card->height() - iconTop - bold->iconSize().height(), 7);
        const QImage glyph = bold->icon().pixmap(18, 18).toImage().scaled(18, 18);
        const QRect ink = inkBounds(glyph);
        QVERIFY(!ink.isEmpty());
        const int topRoom = iconTop + ink.top();
        const int bottomRoom = card->height() - iconTop - ink.bottom() - 1;
        QVERIFY(topRoom >= 7 && bottomRoom >= 7);
        QVERIFY(qAbs(topRoom - bottomRoom) <= 4);
    }

    void selectionMoreOpensOrderedListMenuNotAnotherHorizontalPanel() {
        const QString id = createTask(QStringLiteral("A selected body"));
        TaskDetailWidget detail(m_store.get());
        detail.resize(460, 640);
        QVERIFY(detail.setTask(id));
        QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* bubble = detail.findChild<SelectionBubble*>();
        QVERIFY(editor && bubble);
        selectSource(editor, 2, 10);
        QTRY_VERIFY(bubble->isVisible());
        QVERIFY(clickButton(bubble->findChild<QToolButton*>(QStringLiteral("BubbleMoreButton"))));
        auto* menu = bubble->findChild<PopupMenu*>(QStringLiteral("SelectionOverflowMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->isVisible());
        QVERIFY(bubble->isVisible());
        QVERIFY(detail.findChildren<FormatPanel*>().isEmpty());
        QVERIFY(menu->property("entityMenu").toBool());
        QCOMPARE(menu->property("markdownEditor").value<QObject*>(), static_cast<QObject*>(editor));
        QVERIFY(editor->ownsPopup(menu));
        QCOMPARE(menu->findChildren<QWidgetAction*>().size(), 0);
        const auto actions = visibleActions(menu);
        QCOMPARE(actions.size(), 7);
        QCOMPARE(actions.at(0)->text(), QStringLiteral("添加图片或附件"));
        const QVector<Action> order = {Action::Link, Action::EntityReference, Action::Quote, Action::Code};
        for (int i = 0; i < order.size(); ++i) {
            QCOMPARE(actions.at(i + 1)->property("formatAction").toInt(), int(order.at(i)));
            QCOMPARE(actions.at(i + 1)->text(), editor->formatLabel(order.at(i)));
        }
        QCOMPARE(actions.at(2)->text(), QStringLiteral("关联任务/笔记"));
        QVERIFY(actions.at(5)->isSeparator());
        QVERIFY(actions.at(5)->text().isEmpty());
        QCOMPARE(actions.at(6)->property("formatAction").toInt(), int(Action::ClearFormat));
        for (QAction* action : actions) {
            if (action->isSeparator()) continue;
            const QRect row = menu->actionGeometry(action);
            QCOMPARE(row.height(), 34);
            QCOMPARE(row.left() - PopupShadowMargin, 4);
            QCOMPARE(action->icon().actualSize(QSize(18, 18)), QSize(18, 18));
        }
        QVERIFY(menuCard(menu).height() > 6 * 32);
        QVERIFY(detailBounds(detail).contains(menuCard(menu)));
        QTest::keyClick(menu, Qt::Key_Escape);
        QTRY_VERIFY(!menu->isVisible());
    }

    void footerUsesCompactCardAndSharedOverflowAndPalette() {
        const QString id = createTask(QStringLiteral("A plain body"));
        TaskDetailWidget detail(m_store.get());
        detail.resize(460, 640);
        QVERIFY(detail.setTask(id));
        QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        QVERIFY(editor);
        selectSource(editor, 2, 7);
        auto* panel = openFooter(detail);
        QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible());
        auto* card = panel->findChild<QWidget*>(QStringLiteral("FormatPanelCard"));
        QVERIFY(card);
        QCOMPARE(card->size(), QSize(304, 36));
        QVERIFY(!card->graphicsEffect());
        QVERIFY(panel->testAttribute(Qt::WA_TranslucentBackground));
        QVERIFY(panel->windowFlags().testFlag(Qt::NoDropShadowWindowHint));
        QVERIFY(panel->mask().isEmpty());
        QVERIFY(editor->ownsPopup(panel));
        auto* overflow = panel->findChild<PopupMenu*>(QStringLiteral("FormatOverflowMenu"));
        auto* palette = panel->findChild<PopupMenu*>(QStringLiteral("FormatHighlightPalette"));
        QVERIFY(overflow && palette);
        QVERIFY(overflow->property("entityMenu").toBool() && palette->property("entityMenu").toBool());
        QVERIFY(clickButton(panel->findChild<QToolButton*>(QStringLiteral("FormatMore"))));
        QTRY_VERIFY(overflow->isVisible());
        QCOMPARE(visibleActions(overflow).size(), 7);
        QCOMPARE(visibleActions(overflow).first()->text(), QStringLiteral("添加图片或附件"));
        QVERIFY(visibleActions(overflow).at(5)->isSeparator());
        QTest::keyClick(overflow, Qt::Key_Escape);
        QTRY_VERIFY(!overflow->isVisible());
        QTRY_VERIFY(panel->isVisible());
        QVERIFY(clickButton(panel->findChild<QToolButton*>(QStringLiteral("FormatHighlightColors"))));
        QTRY_VERIFY(palette->isVisible());
        QCOMPARE(visibleActions(palette).size(), 5);
        for (QAction* swatch : visibleActions(palette)) {
            QVERIFY(swatch->isCheckable());
            QCOMPARE(palette->actionGeometry(swatch).height(), 34);
        }
        QVERIFY(editor->ownsPopup(palette));
        QVERIFY(panel->isVisible());
        QTest::keyClick(palette, Qt::Key_Escape);
        panel->hide();
    }

    void surfacesFollowThemeWithoutOpaqueCornerOrGraphicsEffect_data() {
        QTest::addColumn<bool>("footer");
        QTest::newRow("selection") << false;
        QTest::newRow("footer") << true;
    }
    void surfacesFollowThemeWithoutOpaqueCornerOrGraphicsEffect() {
        QFETCH(bool, footer);
        const QString id = createTask(QStringLiteral("Themed body"));
        TaskDetailWidget detail(m_store.get());
        detail.resize(460, 640);
        QVERIFY(detail.setTask(id));
        QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        QVERIFY(editor);
        selectSource(editor, 0, 6);
        QWidget* surface = footer ? static_cast<QWidget*>(openFooter(detail))
            : static_cast<QWidget*>(detail.findChild<SelectionBubble*>());
        QVERIFY(surface);
        QTRY_VERIFY(surface->isVisible());
        auto* card = surface->findChild<QWidget*>(footer ? QStringLiteral("FormatPanelCard") : QStringLiteral("SelectionBubbleCard"));
        QVERIFY(card);
        QColor previous;
        for (auto preference : {ThemeManager::Preference::Light, ThemeManager::Preference::Dark}) {
            ThemeManager::instance().setPreference(preference);
            QApplication::processEvents();
            const QImage image = renderSurface(surface);
            const QPoint sample(card->x() + 2, card->y() + card->height() / 2);
            const QColor expected = ThemeManager::instance().color(footer ? ds::ColorToken::Surface : ds::ColorToken::MenuBg);
            QCOMPARE(image.pixelColor(sample), expected);
            QVERIFY(qAlpha(image.pixel(card->x() + 1, card->y() + 5)) < 200);
            QVERIFY(!card->graphicsEffect());
            if (previous.isValid()) QVERIFY(previous != expected);
            previous = expected;
        }
        surface->hide();
    }

    void selectionSurvivesOwnMenuFocusAndAppliesToSourceOffsets() {
        const QString source = QStringLiteral("**already** plain <u>under</u> ending");
        const QString id = createTask(source);
        TaskDetailWidget detail(m_store.get());
        detail.resize(460, 640);
        QVERIFY(detail.setTask(id));
        QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* bubble = detail.findChild<SelectionBubble*>();
        QVERIFY(editor && bubble);
        const int start = source.indexOf(QStringLiteral("plain"));
        selectSource(editor, start + 5, start); // 反向选区也必须保存源码锚点方向。
        QTRY_VERIFY(bubble->isVisible());
        const auto saved = editor->sourceSelection();
        const quint64 generation = editor->contextGeneration();
        QVERIFY(clickButton(bubble->findChild<QToolButton*>(QStringLiteral("BubbleMoreButton"))));
        auto* menu = bubble->findChild<PopupMenu*>(QStringLiteral("SelectionOverflowMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->isVisible());
        QApplication::processEvents();
        QVERIFY(bubble->isVisible());
        QCOMPARE(editor->sourceSelection().first, saved.first);
        QCOMPARE(editor->sourceSelection().second, saved.second);
        QCOMPARE(editor->selectedSource(), QStringLiteral("plain"));
        QTest::keyClick(menu, Qt::Key_Escape);
        QTRY_VERIFY(!menu->isVisible());
        QTRY_VERIFY(bubble->isVisible());
        auto* card = bubble->findChild<QWidget*>(QStringLiteral("SelectionBubbleCard"));
        QVERIFY(clickButton(formatButton(card, Action::Bold)));
        QTRY_COMPARE(editor->content(), QStringLiteral("**already** **plain** <u>under</u> ending"));
        QCOMPARE(editor->selectedSource(), QStringLiteral("plain"));
        QCOMPARE(editor->contextGeneration(), generation);
    }

    void footerPaletteAndContinuousActionsRetainRenderedSelection() {
        const QString source = QStringLiteral("**already** plain <u>under</u> ending");
        const QString id = createTask(source);
        TaskDetailWidget detail(m_store.get());
        detail.resize(460, 640);
        QVERIFY(detail.setTask(id));
        QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        QVERIFY(editor);
        const int start = source.indexOf(QStringLiteral("plain"));
        selectSource(editor, start, start + 5);
        const quint64 generation = editor->contextGeneration();
        auto* panel = openFooter(detail);
        QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible());
        QVERIFY(clickButton(panel->findChild<QToolButton*>(QStringLiteral("FormatHighlightColors"))));
        auto* palette = panel->findChild<PopupMenu*>(QStringLiteral("FormatHighlightPalette"));
        QVERIFY(palette);
        QTRY_VERIFY(palette->isVisible());
        QCOMPARE(editor->selectedSource(), QStringLiteral("plain"));
        QVERIFY(panel->isVisible());
        auto* pink = palette->findChild<QAction*>(QStringLiteral("Highlight_pink"));
        QVERIFY(clickAction(palette, pink));
        QTRY_COMPARE(editor->content(), QStringLiteral("**already** <mark data-color=\"pink\">plain</mark> <u>under</u> ending"));
        QCOMPARE(editor->selectedSource(), QStringLiteral("plain"));
        QTRY_VERIFY(panel->isVisible());
        QVERIFY(clickButton(panel->findChild<QToolButton*>(QStringLiteral("FormatBold"))));
        QTRY_COMPARE(editor->content(), QStringLiteral("**already** <mark data-color=\"pink\">**plain**</mark> <u>under</u> ending"));
        QCOMPARE(editor->selectedSource(), QStringLiteral("plain"));
        QCOMPARE(editor->contextGeneration(), generation);
        panel->hide();
    }

    void textTypeIsRealListOfSupportedBlocks() {
        const QString id = createTask(QStringLiteral("Plain body"));
        TaskDetailWidget detail(m_store.get());
        detail.resize(460, 640);
        QVERIFY(detail.setTask(id));
        QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* bubble = detail.findChild<SelectionBubble*>();
        QVERIFY(editor && bubble);
        selectSource(editor, 0, 5);
        QTRY_VERIFY(bubble->isVisible());
        QVERIFY(clickButton(bubble->findChild<QToolButton*>(QStringLiteral("BubbleTextType"))));
        auto* menu = bubble->findChild<PopupMenu*>(QStringLiteral("SelectionBlockStyles"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->isVisible());
        const auto styles = editor->blockStyles();
        const auto actions = visibleActions(menu);
        QCOMPARE(actions.size(), 9);
        QCOMPARE(actions.size(), styles.size());
        for (int i = 0; i < styles.size(); ++i) {
            QCOMPARE(actions.at(i)->property("blockStyle").toInt(), int(styles.at(i)));
            QCOMPARE(actions.at(i)->text(), editor->blockStyleLabel(styles.at(i)));
            QVERIFY(!actions.at(i)->icon().isNull());
            QCOMPARE(menu->actionGeometry(actions.at(i)).height(), 34);
        }
        QVERIFY(blockAction(menu, BlockStyle::TaskList));
        QVERIFY(blockAction(menu, BlockStyle::HorizontalRule));
        QVERIFY(clickAction(menu, blockAction(menu, BlockStyle::Heading2)));
        QTRY_COMPARE(editor->content(), QStringLiteral("## Plain body"));
    }

    void oldFooterActionCannotWriteAfterContextGenerationChanges_data() {
        QTest::addColumn<QString>("change");
        QTest::newRow("content") << QStringLiteral("content");
        QTest::newRow("entity") << QStringLiteral("entity");
        QTest::newRow("store") << QStringLiteral("store");
    }
    void oldFooterActionCannotWriteAfterContextGenerationChanges() {
        QFETCH(QString, change);
        const QString id = createTask(QStringLiteral("Original body"));
        TaskDetailWidget detail(m_store.get());
        detail.resize(460, 640);
        QVERIFY(detail.setTask(id));
        QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        QVERIFY(editor);
        selectSource(editor, 0, 8);
        auto* panel = openFooter(detail);
        QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible());
        QVERIFY(clickButton(panel->findChild<QToolButton*>(QStringLiteral("FormatMore"))));
        auto* menu = panel->findChild<PopupMenu*>(QStringLiteral("FormatOverflowMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->isVisible());
        QAction* stale = formatAction(menu, Action::Quote);
        QVERIFY(stale);
        QSignalSpy applied(panel, &FormatPanel::formatApplied);
        const quint64 generation = editor->contextGeneration();
        if (change == QLatin1String("content")) editor->setContent(QStringLiteral("Replacement body"));
        else if (change == QLatin1String("entity")) editor->setEntityId(createTask(QStringLiteral("Other entity body")));
        else editor->setStore(nullptr);
        QVERIFY(editor->contextGeneration() > generation);
        const QString replacement = editor->content();
        // 故意派发此前真实打开的菜单动作，模拟迟到的回调，不制造不可见替身控件。
        stale->trigger();
        QApplication::processEvents();
        QCOMPARE(editor->content(), replacement);
        QCOMPARE(applied.count(), 0);
        QVERIFY(!panel->isVisible());
        QVERIFY(!menu->isVisible());
    }

    void oldSelectionActionCannotWriteToNewEntity() {
        const QString first = createTask(QStringLiteral("First selected body"));
        const QString second = createTask(QStringLiteral("Second untouched body"));
        TaskDetailWidget detail(m_store.get());
        detail.resize(460, 640);
        QVERIFY(detail.setTask(first));
        QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* bubble = detail.findChild<SelectionBubble*>();
        QVERIFY(editor && bubble);
        selectSource(editor, 0, 5);
        QTRY_VERIFY(bubble->isVisible());
        QVERIFY(clickButton(bubble->findChild<QToolButton*>(QStringLiteral("BubbleMoreButton"))));
        auto* menu = bubble->findChild<PopupMenu*>(QStringLiteral("SelectionOverflowMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->isVisible());
        QAction* stale = formatAction(menu, Action::Quote);
        QVERIFY(stale);
        const quint64 generation = editor->contextGeneration();
        QVERIFY(detail.setTask(second));
        QVERIFY(editor->contextGeneration() > generation);
        stale->trigger();
        QApplication::processEvents();
        QCOMPARE(editor->content(), QStringLiteral("Second untouched body"));
        QVERIFY(detail.flush());
        QCOMPARE(m_store->entity(second).body, QStringLiteral("Second untouched body"));
        QVERIFY(!bubble->isVisible());
        QVERIFY(!menu->isVisible());
    }

    void narrowSelectionCollapsesIntoSameMenuRatherThanDisappearing() {
        MarkdownEditor editor;
        SelectionBubble bubble(&editor, editor.textEditor());
        editor.setContent(QStringLiteral("word"));
        editor.resize(100, 420);
        QVERIFY(expose(editor));
        selectSource(&editor, 0, 1);
        QTRY_VERIFY(bubble.isVisible());
        auto* card = bubble.findChild<QWidget*>(QStringLiteral("SelectionBubbleCard"));
        auto* more = bubble.findChild<QToolButton*>(QStringLiteral("BubbleMoreButton"));
        QVERIFY(card && more);
        const QRect bounds = globalRect(&editor).adjusted(16, 16, -16, -16);
        QVERIFY(bounds.contains(globalRect(card)));
        QVERIFY(more->isVisible());
        QVERIFY(card->width() <= bounds.width());
        int hidden = 0;
        for (QToolButton* button : card->findChildren<QToolButton*>()) if (!button->isVisible()) ++hidden;
        QVERIFY(hidden > 0);
        QVERIFY(clickButton(more));
        auto* menu = bubble.findChild<PopupMenu*>(QStringLiteral("SelectionOverflowMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->isVisible());
        for (Action action : editor.primaryFormats()) QVERIFY(formatAction(menu, action));
        bool hasTextTypes = false;
        for (QAction* action : visibleActions(menu)) hasTextTypes |= action->menu() != nullptr;
        QVERIFY(hasTextTypes);
        QVERIFY(bounds.contains(menuCard(menu)));
        QTest::keyClick(menu, Qt::Key_Escape);
    }

    void narrowFooterKeepsCollapsedActionsInSharedMenuAndRestoresAtFullWidth() {
        const QString id = createTask(QStringLiteral("Responsive body"));
        TaskDetailWidget detail(m_store.get());
        detail.resize(250, 640);
        QVERIFY(detail.setTask(id));
        QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        QVERIFY(editor);
        selectSource(editor, 0, 10);
        auto* panel = openFooter(detail);
        QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible());
        auto* card = panel->findChild<QWidget*>(QStringLiteral("FormatPanelCard"));
        auto* strike = panel->findChild<QToolButton*>(QStringLiteral("FormatStrike"));
        QVERIFY(card && strike);
        QCOMPARE(card->height(), 36);
        QVERIFY(card->width() < 304);
        QVERIFY(detailBounds(detail).contains(globalRect(card)));
        QVERIFY(!strike->isVisible());
        QVERIFY(clickButton(panel->findChild<QToolButton*>(QStringLiteral("FormatMore"))));
        auto* menu = panel->findChild<PopupMenu*>(QStringLiteral("FormatOverflowMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->isVisible());
        QVERIFY(formatAction(menu, Action::Strike));
        QVERIFY(detailBounds(detail).contains(menuCard(menu)));
        QTest::keyClick(menu, Qt::Key_Escape);
        detail.resize(460, 640);
        QTRY_COMPARE(card->size(), QSize(304, 36));
        QVERIFY(strike->isVisible());
        panel->hide();
    }

    void selectionAndOwnMenuDismissWhenOuterScrollClipsSelection() {
        QStringList lines;
        for (int i = 0; i < 80; ++i) lines.append(QStringLiteral("Paragraph %1 has plain synthetic text.").arg(i));
        const QString source = lines.join(QLatin1Char('\n'));
        const QString id = createTask(source);
        TaskDetailWidget detail(m_store.get());
        detail.resize(360, 560);
        QVERIFY(detail.setTask(id));
        QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* bubble = detail.findChild<SelectionBubble*>();
        auto* scroll = detail.findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        QVERIFY(editor && bubble && scroll);
        selectSource(editor, 0, 9);
        scroll->verticalScrollBar()->setValue(0);
        QTRY_VERIFY(bubble->isVisible());
        auto* card = bubble->findChild<QWidget*>(QStringLiteral("SelectionBubbleCard"));
        QVERIFY(card);
        QVERIFY(detailBounds(detail).contains(globalRect(card)));
        QVERIFY(clickButton(bubble->findChild<QToolButton*>(QStringLiteral("BubbleMoreButton"))));
        auto* menu = bubble->findChild<PopupMenu*>(QStringLiteral("SelectionOverflowMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->isVisible());
        QVERIFY(scroll->verticalScrollBar()->maximum() > scroll->viewport()->height());
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QTRY_VERIFY(!bubble->isVisible());
        QTRY_VERIFY(!menu->isVisible());
        QCOMPARE(editor->content(), source);
    }

private:
    QString createTask(const QString& body) {
        const QString id = m_store->createTask(QStringLiteral("Synthetic editor tools task"));
        if (id.isEmpty()) return QString();
        Entity entity = m_store->entity(id);
        entity.body = body;
        return m_store->saveTask(entity) ? id : QString();
    }

    bool m_hadDataDir = false;
    QByteArray m_previousDataDir;
    QSettings::Format m_previousSettingsFormat = QSettings::NativeFormat;
    std::unique_ptr<QTemporaryDir> m_settingsDir;
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<TaskStore> m_store;
};

QTEST_MAIN(EditorToolsTest)
#include "tst_editortools.moc"
