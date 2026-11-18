// 常驻富文本 MarkdownEditor 重构验证：正文始终保持渲染态（源码标记不落入文档），
// 块标记（标题字号、列表 bullet、复选框、引用条）由编辑器独立绘制/维护，源码模型
// 仍以 editor.content() 的 Markdown 文本为准。覆盖：
//   1 typedHeadingMarkerRestylesEmptyBlockImmediately      真实键入 "# " 当场成标题
//   2 initialFormatsSurviveNavigationAndFocusCyclesWithoutUndo 初始多格式 + 往返/失焦稳定
//   3 closedInlineMarkersConvertInstantlyAndPersist        键入闭合行内标记即时转换
//   4 realCheckboxClicksToggleBidirectionallyWithStatePreserved 真实复选框点击
//   5 enterContinuesListsAndHeadingsFallToBody             回车续项/退出
//   6 backspaceAtBlockStartRestoresParagraph               块起始退格还原正文
//   7 richTextEditsKeepLegalSourceAndRendering             富文本内部编辑保持合法源码
//   8 imePreeditNeverPersistsOrReflows                     IME 预编辑不保存不重排
//   9 multiLineMarkdownPasteMatchesReopen                  多行 Markdown 粘贴与重开一致
//  10 checkboxStateLivesInBodyTextOnly                     检查项只存正文，不依赖实体
// 回归 slots（源码偏移映射/样式保留 bug 修复验证）：
//  11 linkLabelEditingDeletesSingleCharacter               链接标签内单字删除 + 边界整链原子删除
//  12 inlineStyleInsideLinkLabelSurvivesTyping             链接标签内行内样式编辑保留
//  13 codeSpanBackticksSurviveEditing                      代码 span 内反引号编辑不丢失/字面化规范化
//  14 richCopyPasteRoundTripsViaSourceMime                 源码私有 MIME 复制粘贴保格式
//  15 checkboxClickJoinsTypingUndoChain                    键入→点选→键入的撤销合组逆序
//  16 imePreeditOnEmptyMarkerBlocksKeepsMarkerAnchored     空列表行首字 IME 预编辑时前置标记不随光标横移
// 断言只用内存像素/布局/文本，不保存人工验收截图。

#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QInputMethodEvent>
#include <QLineEdit>
#include <QPainter>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QtTest>

#include "app/TaskStore.h"
#include "db/Database.h"
#include "platform/SystemShadow.h"
#include "ui/AppStyle.h"
#include "ui/EditorTextEdit.h"
#include "ui/Markdown.h"
#include "ui/MarkdownDocument.h"
#include "ui/MarkdownEditor.h"
#include "ui/TaskDetailWidget.h"
#include "ui/design/ThemeManager.h"

using namespace kl;

namespace {

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
    if (!QTest::qWaitForWindowExposed(&window) || !QTest::qWaitForWindowActive(&window)) return false;
    if (qEnvironmentVariableIsSet("KYLINLIST_EXPECT_DPR")) {
        const qreal expected = qEnvironmentVariable("KYLINLIST_EXPECT_DPR").toDouble();
        if (qAbs(window.devicePixelRatioF() - expected) > 0.01) {
            qWarning() << "Unexpected editor device pixel ratio" << window.devicePixelRatioF() << expected;
            return false;
        }
    }
    return true;
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

bool focusBody(MarkdownEditor& editor) {
    QTextEdit* body = editor.textEditor();
    QWidget* viewport = body->viewport();
    if (!body->isVisible() || viewport->width() < 60 || viewport->height() < 40) return false;
    // The blank tail is a real body hit, not an anchor or a synthetic focus call.
    if (!clickActual(viewport, QPoint(viewport->width() - 12, viewport->height() - 12))) return false;
    return waitUntil([body]() { return body->hasFocus(); });
}

// 用真实键盘把聚焦正文的光标移到目标逻辑行（Ctrl+Home 后逐行 Down）。
bool keyboardToLine(MarkdownEditor& editor, int line) {
    QTextEdit* body = editor.textEditor();
    if (!body->hasFocus()) return false;
    QTest::keyClick(body, Qt::Key_Home, Qt::ControlModifier);
    for (int step = 0; step < line; ++step) QTest::keyClick(body, Qt::Key_Down);
    return waitUntil([body, line]() { return body->textCursor().blockNumber() == line; });
}

// IME 提交文本走真实 inputMethodEvent 通道（仅对可见且真实聚焦的正文）。
bool commitText(QWidget* edit, const QString& text) {
    if (!edit || !edit->isVisible() || !edit->hasFocus()) return false;
    QInputMethodEvent event;
    event.setCommitString(text);
    QApplication::sendEvent(edit, &event);
    QApplication::processEvents();
    return true;
}

EditorTextEdit* bodyEdit(MarkdownEditor& editor) {
    return qobject_cast<EditorTextEdit*>(editor.textEditor());
}

const char MarkdownMime[] = "application/x-kylinlist-markdown";

// 用真实文档光标布置插入点：直接对 QTextDocument 位置 setTextCursor，
// 不经 sourceSelection/selectSource 的源码偏移映射（本轮回归 bug 正在该映射）。
bool setDocumentCaret(MarkdownEditor& editor, int position) {
    QTextEdit* body = editor.textEditor();
    if (!body->isVisible() || !body->hasFocus()) return false;
    QTextDocument* doc = body->document();
    if (position < 0 || position > doc->characterCount() - 1) return false;
    QTextCursor cursor(doc);
    cursor.setPosition(position);
    body->setTextCursor(cursor);
    return body->textCursor().position() == position;
}

QTextBlock blockWithText(QTextDocument* doc, const QString& text) {
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next())
        if (block.text() == text) return block;
    return QTextBlock();
}

// 在文档片段中定位渲染文本，并校验其字符格式（片段按样式边界切分）。
bool spanCheck(QTextDocument* doc, const QString& text,
               const std::function<bool(const QTextCharFormat&)>& predicate) {
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || !fragment.text().contains(text)) continue;
            if (predicate(fragment.charFormat())) return true;
        }
    }
    return false;
}

// 光标所在块末字符的格式（即“下一次继续键入将继承的格式”）。
QTextCharFormat blockTailFormat(QTextEdit* body) {
    QTextCursor cursor = body->textCursor();
    cursor.movePosition(QTextCursor::StartOfBlock);
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    return cursor.charFormat();
}

qreal bodyPointSize(QTextDocument* doc) { return doc->defaultFont().pointSizeF(); }

// 内存渲染墨迹断言：不抓桌面、不存截图；只确认正文真的画出了内容。
int paintedInk(QWidget* viewport) {
    if (viewport->width() <= 0 || viewport->height() <= 0) return 0;
    const QColor background = ThemeManager::instance().color(ds::ColorToken::Surface);
    QImage image(viewport->size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(background);
    QPainter painter(&image);
    viewport->render(&painter);
    painter.end();
    int ink = 0;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
        const QColor pixel = image.pixelColor(x, y);
        const int contrast = qMax(qAbs(pixel.red() - background.red()),
            qMax(qAbs(pixel.green() - background.green()), qAbs(pixel.blue() - background.blue())));
        if (pixel.alpha() > 40 && contrast > 40) ++ink;
    }
    return ink;
}

} // namespace

class InstantMarkdownTest : public QObject {
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
        QVERIFY2(Database::instance().openAt(m_dir->path() + QStringLiteral("/instantmarkdown.db"), &error), qPrintable(error));
        m_store.reset(new TaskStore(Database::instance().connection()));
        QVERIFY2(m_store->load(&error), qPrintable(error));
        m_sourceId = m_store->createNote(QStringLiteral("即时渲染测试来源"));
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

    // ---------------------------------------------------------------- 1
    // 空块上真实键入 "# "：当场获得标题字号/字重/行高，光标仍在该行，源码 '#' 不露出；
    // 继续键入的每个字符都继承标题格式，源码完整保留 "# " 前缀。
    void typedHeadingMarkerRestylesEmptyBlockImmediately() {
        MarkdownEditor editor;
        configure(editor, QString());
        QVERIFY(expose(editor));
        QVERIFY(focusBody(editor));
        QTextEdit* body = editor.textEditor();
        QTextDocument* doc = body->document();
        QTest::keyClick(body, Qt::Key_Home, Qt::ControlModifier);  // 光标回块 0 起始
        const qreal bodyPs = bodyPointSize(doc);
        const int bodyCaretHeight = body->cursorRect().height();
        QVERIFY2(bodyCaretHeight > 0, "空正文块也必须有正的行高基线");

        QTest::keyClicks(body, QStringLiteral("# "));
        QCOMPARE(body->textCursor().blockNumber(), 0);                       // 光标仍该行
        QCOMPARE(editor.content(), QStringLiteral("# "));                    // 源码保留标记
        QVERIFY2(!body->toPlainText().contains(QLatin1Char('#')),
                 "标题标记不得以源码形式露出");
        const QTextCharFormat caretFormat = body->textCursor().charFormat();
        QVERIFY2(caretFormat.fontWeight() == QFont::Bold && caretFormat.fontPointSize() > bodyPs,
                 "空块在键入 \"# \" 后必须当场获得标题字重与更大字号");
        QVERIFY2(body->cursorRect().height() > bodyCaretHeight + 2,
                 "空标题块的行高必须当场大于正文行高");

        // 继续键入（ASCII 真实键击）：每个字符当场继承标题格式。
        for (const QChar letter : QStringLiteral("AB")) {
            QTest::keyClicks(body, QString(letter));
            const QTextCharFormat tail = blockTailFormat(body);
            QVERIFY2(tail.fontWeight() == QFont::Bold && tail.fontPointSize() > bodyPs,
                     "继续键入的每个字符都必须当场继承标题格式");
            QCOMPARE(body->textCursor().blockNumber(), 0);
            QVERIFY(!body->toPlainText().contains(QLatin1Char('#')));
            QVERIFY(body->cursorRect().height() > bodyCaretHeight + 2);
        }
        QCOMPARE(editor.content(), QStringLiteral("# AB"));
        QCOMPARE(body->toPlainText(), QStringLiteral("AB"));
        QVERIFY2(paintedInk(body->viewport()) > 40, "标题墨迹必须真实绘制");
    }

    // ---------------------------------------------------------------- 2
    // 初始多格式文档：点击/键盘往返、失焦再聚焦后，格式、文档高度与下方块位置
    // 全部不变，且失焦往返既不产生 undo，也不吞掉已有 undo。
    void initialFormatsSurviveNavigationAndFocusCyclesWithoutUndo() {
        const QString inlineLine = QStringLiteral(
            "**粗体**与*斜体*和~~删除~~加<u>下划线</u>或<mark data-color=\"yellow\">高亮</mark>与`代码`");
        const QString source = QStringLiteral("# 一级标题\n## 二级标题\n") + inlineLine
            + QStringLiteral("\n> 引用文字\n- 无序项\n1. 有序项\n- [ ] 未选任务");
        const QString expectedPlain = QStringLiteral(
            "一级标题\n二级标题\n粗体与斜体和删除加下划线或高亮与代码\n引用文字\n无序项\n有序项\n未选任务");
        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        auto* away = new QLineEdit(&window);
        away->setObjectName(QStringLiteral("FocusAway"));
        MarkdownEditor editor(&window);
        configure(editor, source);
        layout->addWidget(&editor);
        layout->addWidget(away);
        window.resize(760, 640);
        QVERIFY(expose(window));
        QVERIFY(focusBody(editor));
        QTextEdit* body = editor.textEditor();
        QTextDocument* doc = body->document();

        const auto verifyFormats = [&]() {
            QCOMPARE(doc->blockCount(), 7);
            QCOMPARE(doc->toPlainText(), expectedPlain);   // 无 #、bullet、1. 、▏、☐/☑、标记漏出
            const qreal ps = bodyPointSize(doc);
            qreal heading1 = 0, heading2 = 0;
            QVERIFY2(spanCheck(doc, QStringLiteral("一级标题"), [&](const QTextCharFormat& f) {
                heading1 = f.fontPointSize();
                return f.fontWeight() == QFont::Bold && f.fontPointSize() > ps;
            }), "一级标题必须加粗且字号大于正文");
            QVERIFY2(spanCheck(doc, QStringLiteral("二级标题"), [&](const QTextCharFormat& f) {
                heading2 = f.fontPointSize();
                return f.fontWeight() == QFont::Bold && f.fontPointSize() > ps;
            }), "二级标题必须加粗且字号大于正文");
            QVERIFY2(heading1 > heading2, "一级标题字号必须大于二级标题");
            QVERIFY2(spanCheck(doc, QStringLiteral("粗体"), [](const QTextCharFormat& f) {
                return f.fontWeight() == QFont::Bold; }), "粗体段必须保持加粗");
            QVERIFY2(spanCheck(doc, QStringLiteral("斜体"), [](const QTextCharFormat& f) {
                return f.fontItalic(); }), "斜体段必须保持斜体");
            QVERIFY2(spanCheck(doc, QStringLiteral("删除"), [](const QTextCharFormat& f) {
                return f.fontStrikeOut(); }), "删除线段必须保持删除线");
            QVERIFY2(spanCheck(doc, QStringLiteral("下划线"), [](const QTextCharFormat& f) {
                return f.fontUnderline(); }), "下划线段必须保持下划线");
            const QColor highlight = Markdown::highlightColor(QStringLiteral("yellow"));
            QVERIFY2(spanCheck(doc, QStringLiteral("高亮"), [&highlight](const QTextCharFormat& f) {
                return f.background().style() == Qt::SolidPattern && f.background().color() == highlight;
            }), "高亮段必须保持黄色底色");
            QVERIFY2(spanCheck(doc, QStringLiteral("代码"), [](const QTextCharFormat& f) {
                return f.background().style() == Qt::SolidPattern;
            }), "行内代码必须保持代码底色");
            QVERIFY2(spanCheck(doc, QStringLiteral("引用文字"), [](const QTextCharFormat& f) {
                return f.fontItalic(); }), "引用文字必须保持引用样式");
        };
        verifyFormats();
        QVERIFY2(paintedInk(body->viewport()) > 200, "多格式文档必须有真实墨迹");

        // 键盘往返：跨全部块移动、建立与折叠选区、翻页，格式不得漂移。
        QTest::keyClick(body, Qt::Key_End, Qt::ControlModifier);
        QTest::keyClick(body, Qt::Key_Home, Qt::ControlModifier);
        for (int step = 0; step < doc->blockCount(); ++step) QTest::keyClick(body, Qt::Key_Down);
        QTest::keyClick(body, Qt::Key_End);
        QTest::keyClick(body, Qt::Key_Home, Qt::ShiftModifier);
        QVERIFY(body->textCursor().hasSelection());
        QTest::keyClick(body, Qt::Key_Down);  // 折叠选区
        QTest::keyClick(body, Qt::Key_PageDown);
        QTest::keyClick(body, Qt::Key_PageUp);
        verifyFormats();

        // 点击往返：逐块点击真实视口位置。
        for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
            const QRect caret = body->cursorRect(QTextCursor(block));
            QVERIFY2(clickActual(body->viewport(), caret.center()),
                qPrintable(QStringLiteral("块 %1 必须可真实点击").arg(block.blockNumber())));
            verifyFormats();
        }

        // 失焦再聚焦：格式、文档高度、下方块位置不变，且不产生 undo。
        QVERIFY2(!doc->isUndoAvailable(), "加载与导航不得产生可撤销步骤");
        QAbstractTextDocumentLayout* documentLayout = doc->documentLayout();
        const QTextBlock lastBlock = doc->lastBlock();
        const qreal heightBefore = documentLayout->documentSize().height();
        const qreal lastTopBefore = documentLayout->blockBoundingRect(lastBlock).top();
        QVERIFY(clickActual(away));
        QVERIFY(waitUntil([away]() { return away->hasFocus(); }));
        QVERIFY(focusBody(editor));
        verifyFormats();
        QVERIFY2(qAbs(documentLayout->documentSize().height() - heightBefore) <= 0.5,
            "失焦再聚焦不得改变文档高度");
        QVERIFY2(qAbs(documentLayout->blockBoundingRect(doc->lastBlock()).top() - lastTopBefore) <= 0.5,
            "失焦再聚焦不得改变下方块位置");
        QVERIFY2(!doc->isUndoAvailable(), "失焦再聚焦不得产生可撤销步骤");
        QTest::keyClick(body, Qt::Key_Z, Qt::ControlModifier);   // 无 undo 可用时应为空操作
        QCOMPARE(editor.content(), source);

        // 真实编辑产生 undo 后，失焦往返不得吞掉它。
        QTest::keyClick(body, Qt::Key_End, Qt::ControlModifier);
        QTest::keyClicks(body, QStringLiteral("X"));
        QTRY_COMPARE(editor.content(), source + QStringLiteral("X"));
        QVERIFY(doc->isUndoAvailable());
        QVERIFY(clickActual(away));
        QVERIFY(waitUntil([away]() { return away->hasFocus(); }));
        QVERIFY(focusBody(editor));
        QVERIFY2(doc->isUndoAvailable(), "失焦再聚焦不得破坏已有 undo");
        QTest::keyClick(body, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(editor.content(), source);
    }

    // ---------------------------------------------------------------- 3
    // 键入闭合的 **bold** / *italic* / ~~strike~~ / `code`：当行即时转换，标记不露出，
    // 源码完整保留；继续输入与重选不丢已转换格式。标记用真实键盘，样本文字走 IME
    // 提交通道（QTest 的 ASCII 键盘助手无法键入中文，见 tst_editorinteraction 先例）。
    void closedInlineMarkersConvertInstantlyAndPersist() {
        struct Case {
            const char* name;
            QString opening;   // 前半个标记（真实键入）
            QString sample;    // 样本文本（IME 提交，同为真实输入通道）
            QString closing;   // 后半个标记（真实键入）
            QString span;      // 转换后的渲染文本
            QString source;    // 键入完成后的源码
            std::function<bool(const QTextCharFormat&)> check;
        };
        const std::vector<Case> cases = {
            {"bold", QStringLiteral("**"), QStringLiteral("加粗"), QStringLiteral("**"),
                QStringLiteral("加粗"), QStringLiteral("**加粗**"),
                [](const QTextCharFormat& f) { return f.fontWeight() == QFont::Bold; }},
            {"italic", QStringLiteral("*"), QStringLiteral("斜体"), QStringLiteral("*"),
                QStringLiteral("斜体"), QStringLiteral("*斜体*"),
                [](const QTextCharFormat& f) { return f.fontItalic(); }},
            {"strike", QStringLiteral("~~"), QStringLiteral("删除"), QStringLiteral("~~"),
                QStringLiteral("删除"), QStringLiteral("~~删除~~"),
                [](const QTextCharFormat& f) { return f.fontStrikeOut(); }},
            {"code", QStringLiteral("`"), QStringLiteral("代码"), QStringLiteral("`"),
                QStringLiteral("代码"), QStringLiteral("`代码`"),
                [](const QTextCharFormat& f) { return f.background().style() == Qt::SolidPattern; }},
        };
        for (const Case& item : cases) {
            MarkdownEditor editor;
            configure(editor, QString());
            QVERIFY(expose(editor));
            QVERIFY(focusBody(editor));
            QTextEdit* body = editor.textEditor();
            QTextDocument* doc = body->document();
            QTest::keyClicks(body, item.opening);
            QVERIFY(commitText(body, item.sample));
            QTest::keyClicks(body, item.closing);
            // 闭合最后半个标记的瞬间即完成转换，无需失焦。
            QTRY_COMPARE(editor.content(), item.source);
            QVERIFY2(waitUntil([&]() { return !doc->toPlainText().contains(QLatin1Char('*'))
                && !doc->toPlainText().contains(QLatin1String("~~"))
                && !doc->toPlainText().contains(QLatin1Char('`')); }),
                "行内标记不得以源码形式露出");
            QVERIFY2(spanCheck(doc, item.span, item.check),
                qPrintable(QStringLiteral("%1 转换后必须立即携带对应格式").arg(item.name)));
            QString span = item.span;   // 后续在段内插入字符时同步更新
            // 闭合标记之后的继续输入：新输入是普通正文，已转换段不丢格式。
            QVERIFY(commitText(body, QStringLiteral("后缀")));
            QTRY_COMPARE(editor.content(), item.source + QStringLiteral("后缀"));
            QVERIFY2(spanCheck(doc, span, item.check),
                qPrintable(QStringLiteral("%1 继续输入后不得丢格式").arg(item.name)));
            for (const QChar tail : QStringLiteral("后缀")) {
                QVERIFY2(!spanCheck(doc, QString(tail), item.check),
                    qPrintable(QStringLiteral("%1 闭合标记后的新输入应为普通正文").arg(item.name)));
            }
            // 边界区分：移回已有样式段内部继续加字，新字符仍为该样式（与上一步的普通正文相对）。
            QTest::keyClick(body, Qt::Key_Home);
            QTest::keyClick(body, Qt::Key_Right);   // 样本第 1 字之后（段内部）
            QTest::keyClicks(body, QStringLiteral("Q"));
            QString expected = item.source;
            expected.insert(item.opening.size() + 1, QLatin1Char('Q'));
            QTRY_COMPARE(editor.content(), expected + QStringLiteral("后缀"));
            span.insert(1, QLatin1Char('Q'));
            QVERIFY2(spanCheck(doc, QStringLiteral("Q"), item.check),
                qPrintable(QStringLiteral("%1 样式段内部继续加字必须保持该样式").arg(item.name)));
            QCOMPARE(doc->toPlainText(), span + QStringLiteral("后缀"));
            // 重选（全选/折叠/左右穿越）不丢格式、不露出源码。
            QTest::keyClick(body, Qt::Key_A, Qt::ControlModifier);
            QVERIFY(body->textCursor().hasSelection());
            QVERIFY2(spanCheck(doc, span, item.check),
                qPrintable(QStringLiteral("%1 建立选区时不得丢格式").arg(item.name)));
            QTest::keyClick(body, Qt::Key_Right);   // 折叠选区
            QTest::keyClick(body, Qt::Key_Home);
            for (int step = 0; step < span.size() + 2; ++step) QTest::keyClick(body, Qt::Key_Right);
            for (int step = 0; step < span.size() + 2; ++step) QTest::keyClick(body, Qt::Key_Left);
            QVERIFY2(spanCheck(doc, span, item.check),
                qPrintable(QStringLiteral("%1 光标穿越后不得丢格式").arg(item.name)));
            QCOMPARE(doc->toPlainText(), span + QStringLiteral("后缀"));
        }
    }

    // ---------------------------------------------------------------- 4
    // 真实复选框点击：多次双向切换、源码完整 - [x]/- [ ]、活动/非活动/空项/缩进/大小写 X，
    // 保留原光标/选区与页面位置，一次 undo/redo，点击文本仍可编辑、只读不可切换。
    void realCheckboxClicksToggleBidirectionallyWithStatePreserved() {
        QStringList lines;
        lines << QStringLiteral("- [ ] 未选任务")
              << QStringLiteral("- [x] 已选任务")
              << QStringLiteral("- [X] 大写项")
              << QStringLiteral("- [ ] ")
              << QStringLiteral("  - [ ] 缩进项")
              << QStringLiteral("- [ ] 活动项");
        for (int index = 6; index < 38; ++index)
            lines << QStringLiteral("填充 %1").arg(index, 2, 10, QLatin1Char('0'));
        lines << QStringLiteral("- [ ] 底部任务");
        MarkdownEditor editor;
        configure(editor, lines.join(QLatin1Char('\n')));
        QVERIFY(expose(editor));
        QVERIFY(focusBody(editor));
        QVERIFY(keyboardToLine(editor, 0));   // focusBody 把光标带到文末，回到顶部
        QTextEdit* body = editor.textEditor();
        QTextDocument* doc = body->document();
        auto* edit = bodyEdit(editor);
        QVERIFY2(edit, "正文必须是 EditorTextEdit（复选框几何的宿主）");

        // 所有任务块的复选框矩形都有效且在视口内（块文本不含 glyph/bullet）。
        const int taskLines[] = {0, 1, 2, 3, 4, 5};
        for (const int line : taskLines) {
            const QTextBlock block = doc->findBlockByNumber(line);
            QVERIFY2(block.isValid(), qPrintable(QStringLiteral("源码行 %1 必须对应文档块").arg(line)));
            const QRect box = edit->checkBoxRect(block);
            QVERIFY2(box.width() > 4 && box.height() > 4,
                qPrintable(QStringLiteral("行 %1 的复选框矩形必须有效：%2").arg(line).arg(box.width())));
            QVERIFY2(body->viewport()->rect().contains(box),
                qPrintable(QStringLiteral("行 %1 的复选框矩形必须位于视口内").arg(line)));
        }
        QVERIFY2(blockWithText(doc, QStringLiteral("未选任务")).isValid(),
            "任务块必须可按渲染文本（不含 glyph/bullet）定位");
        QVERIFY(!doc->toPlainText().contains(QChar(0x2610))
            && !doc->toPlainText().contains(QChar(0x2611)));

        const auto clickBox = [&](int line) {
            const QTextBlock block = doc->findBlockByNumber(line);
            return clickActual(body->viewport(), edit->checkBoxRect(block).center());
        };
        const auto contentLineMatches = [&](int line, const QString& pattern) {
            return waitUntil([&]() {
                const QStringList current = editor.content().split(QLatin1Char('\n'));
                return current.size() > line
                    && QRegularExpression(pattern).match(current.at(line)).hasMatch();
            });
        };

        // 未选任务：四次点击双向，源码逐步完整。
        QVERIFY(clickBox(0));
        QVERIFY2(contentLineMatches(0, QStringLiteral("^- \\[x\\] 未选任务$")), "点击后必须成为 - [x]");
        QVERIFY(clickBox(0));
        QVERIFY2(contentLineMatches(0, QStringLiteral("^- \\[ \\] 未选任务$")), "再点必须回到 - [ ]");
        QVERIFY(clickBox(0));
        QVERIFY2(contentLineMatches(0, QStringLiteral("^- \\[x\\] 未选任务$")), "第三次点击必须再次选中");
        QVERIFY(clickBox(0));
        QVERIFY2(contentLineMatches(0, QStringLiteral("^- \\[ \\] 未选任务$")), "第四次点击必须再次取消");
        QVERIFY2(waitUntil([body]() { return body->hasFocus(); }), "点击复选框后正文应持有焦点");

        // 已选任务：选中 → 取消。
        QVERIFY(clickBox(1));
        QVERIFY2(contentLineMatches(1, QStringLiteral("^- \\[ \\] 已选任务$")), "已选项点击后必须取消");
        QVERIFY(clickBox(1));
        QVERIFY2(contentLineMatches(1, QStringLiteral("^- \\[x\\] 已选任务$")), "取消后再点必须恢复选中");

        // 大写 X：视为选中，点击取消为规范 - [ ]；再次点击回到选中态（x/X 均合法）。
        QVERIFY(clickBox(2));
        QVERIFY2(contentLineMatches(2, QStringLiteral("^- \\[ \\] 大写项$")), "大写 X 项点击后必须取消");
        QVERIFY(clickBox(2));
        QVERIFY2(contentLineMatches(2, QStringLiteral("^- \\[[xX]\\] 大写项$")), "再次点击必须回到选中态");

        // 空项：内容为空也能切换，块文本保持为空。
        QVERIFY(clickBox(3));
        QVERIFY2(contentLineMatches(3, QStringLiteral("^- \\[x\\][ \\t]*$")), "空项也必须能选中");
        QVERIFY(doc->findBlockByNumber(3).text().isEmpty());
        QVERIFY(clickBox(3));
        QVERIFY2(contentLineMatches(3, QStringLiteral("^- \\[ \\][ \\t]*$")), "空项再点必须取消");

        // 缩进项：切换必须保留缩进。
        QVERIFY(clickBox(4));
        QVERIFY2(contentLineMatches(4, QStringLiteral("^  - \\[x\\] 缩进项$")), "缩进必须原样保留");
        QVERIFY(clickBox(4));
        QVERIFY2(contentLineMatches(4, QStringLiteral("^  - \\[ \\] 缩进项$")), "缩进取消同样保留缩进");

        // 活动行（光标所在行）：切换不移动光标、不改变源码偏移。
        QVERIFY(keyboardToLine(editor, 5));
        QTest::keyClick(body, Qt::Key_End);
        const QPair<int, int> caretBefore = editor.sourceSelection();
        QCOMPARE(caretBefore.first, caretBefore.second);
        const int scrollBefore = body->verticalScrollBar()->value();
        QVERIFY(clickBox(5));
        QVERIFY2(contentLineMatches(5, QStringLiteral("^- \\[x\\] 活动项$")), "活动行复选框必须可切换");
        QCOMPARE(editor.sourceSelection().first, caretBefore.first);
        QCOMPARE(editor.sourceSelection().second, caretBefore.second);
        QCOMPARE(body->textCursor().blockNumber(), 5);
        QCOMPARE(body->verticalScrollBar()->value(), scrollBefore);
        QVERIFY(clickBox(5));
        QVERIFY2(contentLineMatches(5, QStringLiteral("^- \\[ \\] 活动项$")), "活动行再点必须取消");

        // 非活动行的既有选区：点击别行复选框不吞选区、不改变页面位置。
        QVERIFY(keyboardToLine(editor, 1));
        QTest::keyClick(body, Qt::Key_End);
        QTest::keyClick(body, Qt::Key_Home, Qt::ShiftModifier);
        QVERIFY(body->textCursor().hasSelection());
        const QPair<int, int> selectionBefore = editor.sourceSelection();
        QVERIFY(clickBox(4));
        QVERIFY2(contentLineMatches(4, QStringLiteral("^  - \\[x\\] 缩进项$")), "非活动行也必须可切换");
        QCOMPARE(editor.sourceSelection().first, selectionBefore.first);
        QCOMPARE(editor.sourceSelection().second, selectionBefore.second);
        QCOMPARE(body->verticalScrollBar()->value(), scrollBefore);
        QVERIFY(clickBox(4));

        // 滚动到中部后点击：页面位置不得变化。
        QScrollBar* scroll = body->verticalScrollBar();
        QVERIFY2(scroll->maximum() > 0, "夹带填充行后正文必须可滚动");
        const int bottomLine = lines.size() - 1;
        scroll->setValue(scroll->maximum());
        QApplication::processEvents();
        const int scrolled = scroll->value();
        const QTextBlock bottomBlock = doc->findBlockByNumber(bottomLine);
        QVERIFY2(clickActual(body->viewport(), edit->checkBoxRect(bottomBlock).center()),
            "滚动后底部任务复选框必须可真实点击");
        QVERIFY2(contentLineMatches(bottomLine, QStringLiteral("^- \\[x\\] 底部任务$")),
            "底部任务点击后必须选中");
        QCOMPARE(scroll->value(), scrolled);
        QVERIFY(clickActual(body->viewport(), edit->checkBoxRect(doc->findBlockByNumber(bottomLine)).center()));
        QVERIFY2(contentLineMatches(bottomLine, QStringLiteral("^- \\[ \\] 底部任务$")), "底部任务再点必须取消");
        scroll->setValue(0);
        QApplication::processEvents();

        // 点击任务文本（复选框以外的区域）仍可编辑，且不动复选框状态。
        QVERIFY(keyboardToLine(editor, 0));
        const QString current = editor.content().split(QLatin1Char('\n')).at(0);
        QCOMPARE(current, QStringLiteral("- [ ] 未选任务"));
        const QTextBlock firstBlock = blockWithText(doc, QStringLiteral("未选任务"));
        const QPoint textPoint = body->cursorRect(QTextCursor(firstBlock)).center();
        QVERIFY2(!edit->checkBoxRect(firstBlock).contains(textPoint),
            "测试点击点必须落在文本区域而非复选框内");
        QVERIFY2(clickActual(body->viewport(), textPoint), "任务文本区域必须可真实点击");
        QVERIFY(body->hasFocus());
        QCOMPARE(body->textCursor().blockNumber(), 0);
        QTest::keyClick(body, Qt::Key_End);   // 落点列不定的兜底：End 后键入位置确定
        QTest::keyClicks(body, QStringLiteral("tail"));
        QVERIFY2(waitUntil([&]() {
            return editor.content().split(QLatin1Char('\n')).at(0)
                == QStringLiteral("- [ ] 未选任务tail");
        }), "点击文本后仍必须可编辑，且复选框状态不变");

        // 一次 undo/redo：单击切换是单个撤销步骤。
        QVERIFY(clickBox(0));
        QVERIFY2(contentLineMatches(0, QStringLiteral("^- \\[x\\] 未选任务tail$")), "撤销前必须先切换");
        QTest::keyClick(body, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY2(waitUntil([&]() {
            return editor.content().split(QLatin1Char('\n')).at(0)
                == QStringLiteral("- [ ] 未选任务tail");
        }), "一次 undo 必须完整撤销一次切换");
        QTest::keyClick(body, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY2(waitUntil([&]() {
            return editor.content().split(QLatin1Char('\n')).at(0)
                == QStringLiteral("- [x] 未选任务tail");
        }), "redo 必须重新应用一次切换");
        QTest::keyClick(body, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY2(waitUntil([&]() {
            return editor.content().split(QLatin1Char('\n')).at(0)
                == QStringLiteral("- [ ] 未选任务tail");
        }), "再次 undo 应回到未选状态");

        // 只读：复选框不得切换。
        body->setReadOnly(true);
        QVERIFY(clickBox(1));
        QCOMPARE(editor.content().split(QLatin1Char('\n')).at(1), QStringLiteral("- [x] 已选任务"));
        body->setReadOnly(false);
    }

    // ---------------------------------------------------------------- 5
    // 回车：标题后回正文；无序/有序/任务连续下一项且新任务未选；空项回车退出列表。
    void enterContinuesListsAndHeadingsFallToBody() {
        {
            // 标题行回车 → 新块为正文格式。
            MarkdownEditor editor;
            configure(editor, QStringLiteral("# 标题行"));
            QVERIFY(expose(editor));
            QVERIFY(focusBody(editor));
            QTextEdit* body = editor.textEditor();
            QTest::keyClick(body, Qt::Key_End);
            QTest::keyClick(body, Qt::Key_Return);
            QTRY_COMPARE(editor.content(), QStringLiteral("# 标题行\n"));
            QCOMPARE(body->textCursor().blockNumber(), 1);
            const QTextCharFormat fresh = body->textCursor().charFormat();
            QVERIFY2(fresh.fontWeight() != QFont::Bold && fresh.fontPointSize() <= bodyPointSize(body->document()),
                "标题后回车的新块必须是正文格式");
            QTest::keyClicks(body, QStringLiteral("body"));
            QTRY_COMPARE(editor.content(), QStringLiteral("# 标题行\nbody"));
            QCOMPARE(body->document()->findBlockByNumber(1).text(), QStringLiteral("body"));
            QVERIFY2(spanCheck(body->document(), QStringLiteral("body"), [](const QTextCharFormat& f) {
                return f.fontWeight() != QFont::Bold; }), "回车后的文字必须不是标题");
        }
        {
            // 无序续项。
            MarkdownEditor editor;
            configure(editor, QStringLiteral("- 首项"));
            QVERIFY(expose(editor));
            QVERIFY(focusBody(editor));
            QTextEdit* body = editor.textEditor();
            QTest::keyClick(body, Qt::Key_End);
            QTest::keyClick(body, Qt::Key_Return);
            QVERIFY2(waitUntil([&]() { return editor.content().startsWith(QStringLiteral("- 首项\n-")); }),
                "无序项回车必须续出下一项");
            QCOMPARE(body->textCursor().blockNumber(), 1);
            QTest::keyClicks(body, QStringLiteral("second"));
            QTRY_COMPARE(editor.content(), QStringLiteral("- 首项\n- second"));
            QCOMPARE(body->document()->findBlockByNumber(1).text(), QStringLiteral("second"));
        }
        {
            // 有序续项自动编号。
            MarkdownEditor editor;
            configure(editor, QStringLiteral("1. 甲项"));
            QVERIFY(expose(editor));
            QVERIFY(focusBody(editor));
            QTextEdit* body = editor.textEditor();
            QTest::keyClick(body, Qt::Key_End);
            QTest::keyClick(body, Qt::Key_Return);
            QVERIFY2(waitUntil([&]() { return editor.content().startsWith(QStringLiteral("1. 甲项\n2.")); }),
                "有序项回车必须续出下一编号");
            QTest::keyClicks(body, QStringLiteral("next"));
            QTRY_COMPARE(editor.content(), QStringLiteral("1. 甲项\n2. next"));
            QCOMPARE(body->document()->findBlockByNumber(1).text(), QStringLiteral("next"));
        }
        {
            // 任务续项：即使当前项已选，新任务也必须未选。
            MarkdownEditor editor;
            configure(editor, QStringLiteral("- [x] 已做"));
            QVERIFY(expose(editor));
            QVERIFY(focusBody(editor));
            QTextEdit* body = editor.textEditor();
            QTest::keyClick(body, Qt::Key_End);
            QTest::keyClick(body, Qt::Key_Return);
            QVERIFY2(waitUntil([&]() {
                return editor.content().startsWith(QStringLiteral("- [x] 已做\n- [ ]")); }),
                "任务项回车必须续出未选任务");
            QTest::keyClicks(body, QStringLiteral("todo"));
            QTRY_COMPARE(editor.content(), QStringLiteral("- [x] 已做\n- [ ] todo"));
            QCOMPARE(body->document()->findBlockByNumber(1).text(), QStringLiteral("todo"));
        }
        {
            // 空项回车退出列表：留下一个空正文段，随后键入的是正文。
            MarkdownEditor editor;
            configure(editor, QStringLiteral("- 首项\n- "));
            QVERIFY(expose(editor));
            QVERIFY(focusBody(editor));
            QVERIFY(keyboardToLine(editor, 1));
            QTest::keyClick(editor.textEditor(), Qt::Key_End);
            QTest::keyClick(editor.textEditor(), Qt::Key_Return);
            QTRY_COMPARE(editor.content(), QStringLiteral("- 首项\n"));
            QCOMPARE(editor.textEditor()->textCursor().blockNumber(), 1);
            QTest::keyClicks(editor.textEditor(), QStringLiteral("text"));
            QTRY_COMPARE(editor.content(), QStringLiteral("- 首项\ntext"));
            QCOMPARE(editor.textEditor()->document()->findBlockByNumber(1).text(), QStringLiteral("text"));
        }
    }

    // ---------------------------------------------------------------- 6
    // 块起始退格：还原为正文，文本保留、下方块不动。
    void backspaceAtBlockStartRestoresParagraph_data() {
        QTest::addColumn<QString>("marked");
        QTest::addColumn<QString>("expected");
        QTest::newRow("heading1") << QStringLiteral("# 目标文字\n下方行") << QStringLiteral("目标文字\n下方行");
        QTest::newRow("heading2") << QStringLiteral("## 目标文字\n下方行") << QStringLiteral("目标文字\n下方行");
        QTest::newRow("unordered") << QStringLiteral("- 目标文字\n下方行") << QStringLiteral("目标文字\n下方行");
        QTest::newRow("ordered") << QStringLiteral("1. 目标文字\n下方行") << QStringLiteral("目标文字\n下方行");
        QTest::newRow("quote") << QStringLiteral("> 目标文字\n下方行") << QStringLiteral("目标文字\n下方行");
        QTest::newRow("task-unchecked") << QStringLiteral("- [ ] 目标文字\n下方行") << QStringLiteral("目标文字\n下方行");
        QTest::newRow("task-checked") << QStringLiteral("- [x] 目标文字\n下方行") << QStringLiteral("目标文字\n下方行");
    }
    void backspaceAtBlockStartRestoresParagraph() {
        QFETCH(QString, marked);
        QFETCH(QString, expected);
        MarkdownEditor editor;
        configure(editor, marked);
        QVERIFY(expose(editor));
        QVERIFY(focusBody(editor));
        QTextEdit* body = editor.textEditor();
        QVERIFY(keyboardToLine(editor, 0));
        QTest::keyClick(body, Qt::Key_Home);   // 光标到块起始（标记不占文档位置）
        QTest::keyClick(body, Qt::Key_Backspace);
        QTRY_COMPARE(editor.content(), expected);
        QCOMPARE(body->textCursor().blockNumber(), 0);
        QCOMPARE(body->textCursor().positionInBlock(), 0);
        QCOMPARE(body->document()->findBlockByNumber(0).text(), QStringLiteral("目标文字"));
        QCOMPARE(body->document()->findBlockByNumber(1).text(), QStringLiteral("下方行"));
        QVERIFY2(spanCheck(body->document(), QStringLiteral("目标文字"), [&](const QTextCharFormat& f) {
            return f.fontWeight() != QFont::Bold && f.fontPointSize() <= bodyPointSize(body->document());
        }), "退格还原后的文字必须是正文格式");
    }

    // ---------------------------------------------------------------- 7
    // 富文本内部编辑：样式段内插入/删除、跨样式选区替换、跨段合并，均保持合法源码
    // 与渲染（用“同源重开等价”作为合法性判据）。
    void richTextEditsKeepLegalSourceAndRendering() {
        {
            // 样式段内插入：不丢样式、不动下方块。
            MarkdownEditor editor;
            configure(editor, QStringLiteral("**粗文**\n# 哨兵标题"));
            QVERIFY(expose(editor));
            QVERIFY(focusBody(editor));
            editor.selectSource(3, 3);   // 光标落在“粗”之后（源码偏移，仅布置光标）
            QTextEdit* body = editor.textEditor();
            QTextDocument* doc = body->document();
            QVERIFY(body->hasFocus());
            const qreal sentinelTopBefore = doc->documentLayout()->blockBoundingRect(doc->findBlockByNumber(1)).top();
            QTest::keyClicks(body, QStringLiteral("N"));
            QTRY_COMPARE(editor.content(), QStringLiteral("**粗N文**\n# 哨兵标题"));
            QCOMPARE(doc->toPlainText(), QStringLiteral("粗N文\n哨兵标题"));
            QVERIFY2(spanCheck(doc, QStringLiteral("粗N文"), [](const QTextCharFormat& f) {
                return f.fontWeight() == QFont::Bold; }), "段内插入必须继承加粗");
            QVERIFY2(qAbs(doc->documentLayout()->blockBoundingRect(doc->findBlockByNumber(1)).top()
                - sentinelTopBefore) <= 0.5, "段内插入不得移动下方块");
        }
        {
            // 样式段内删除：剩余文字保持样式。
            MarkdownEditor editor;
            configure(editor, QStringLiteral("**粗文**\n# 哨兵标题"));
            QVERIFY(expose(editor));
            QVERIFY(focusBody(editor));
            editor.selectSource(3, 3);   // “粗”之后，退格删除“粗”
            QTextEdit* body = editor.textEditor();
            QTextDocument* doc = body->document();
            QTest::keyClick(body, Qt::Key_Backspace);
            QTRY_COMPARE(editor.content(), QStringLiteral("**文**\n# 哨兵标题"));
            QVERIFY2(spanCheck(doc, QStringLiteral("文"), [](const QTextCharFormat& f) {
                return f.fontWeight() == QFont::Bold; }), "段内删除后剩余文字必须保持加粗");
        }
        {
            // 跨样式选区替换：选区起点在无样式文字上，替换后保留前方加粗段。
            MarkdownEditor editor;
            configure(editor, QStringLiteral("**粗甲**普通*斜乙*\n# 哨兵标题"));
            QVERIFY(expose(editor));
            QVERIFY(focusBody(editor));
            editor.selectSource(6, 6);   // “普通”之前
            QTextEdit* body = editor.textEditor();
            QTextDocument* doc = body->document();
            QTest::keyClick(body, Qt::Key_End, Qt::ShiftModifier);   // 选到行尾
            QVERIFY(body->textCursor().hasSelection());
            QTest::keyClicks(body, QStringLiteral("R"));
            QTRY_COMPARE(editor.content(), QStringLiteral("**粗甲**R\n# 哨兵标题"));
            QCOMPARE(doc->toPlainText(), QStringLiteral("粗甲R\n哨兵标题"));
            QVERIFY2(spanCheck(doc, QStringLiteral("粗甲"), [](const QTextCharFormat& f) {
                return f.fontWeight() == QFont::Bold; }), "跨样式替换不得丢失未选中的加粗段");
        }
        {
            // 跨段合并：加粗段与斜体段合并后源码合法（同源重开渲染等价）。
            MarkdownEditor editor;
            configure(editor, QStringLiteral("**甲乙**\n*丙丁*\n# 哨兵标题"));
            QVERIFY(expose(editor));
            QVERIFY(focusBody(editor));
            editor.selectSource(3, 3);   // “甲”之后
            QTextEdit* body = editor.textEditor();
            QTextDocument* doc = body->document();
            QTest::keyClick(body, Qt::Key_Down, Qt::ShiftModifier);   // 选到“丙”之后
            QVERIFY(body->textCursor().hasSelection());
            QTest::keyClick(body, Qt::Key_Backspace);
            QVERIFY2(waitUntil([&]() { return doc->blockCount() == 2; }), "跨段删除必须合并段落");
            QTRY_COMPARE(doc->toPlainText(), QStringLiteral("甲丁\n哨兵标题"));
            QVERIFY2(spanCheck(doc, QStringLiteral("甲"), [](const QTextCharFormat& f) {
                return f.fontWeight() == QFont::Bold; }), "合并后原加粗字必须保持加粗");
            QVERIFY2(spanCheck(doc, QStringLiteral("丁"), [](const QTextCharFormat& f) {
                return f.fontItalic(); }), "合并后原斜体字必须保持斜体");
            // 合法源码判据：以当前源码重开，渲染结果与现场一致。
            MarkdownEditor reopened;
            configure(reopened, editor.content());
            QCOMPARE(reopened.textEditor()->document()->toPlainText(), QStringLiteral("甲丁\n哨兵标题"));
            QVERIFY2(spanCheck(reopened.textEditor()->document(), QStringLiteral("甲"), [](const QTextCharFormat& f) {
                return f.fontWeight() == QFont::Bold; }), "重开后原加粗字必须保持加粗");
            QVERIFY2(spanCheck(reopened.textEditor()->document(), QStringLiteral("丁"), [](const QTextCharFormat& f) {
                return f.fontItalic(); }), "重开后原斜体字必须保持斜体");
        }
    }

    // ---------------------------------------------------------------- 8
    // IME：预编辑不落入源码、不触发重排；提交后格式保留且一步 undo；取消不丢格式。
    void imePreeditNeverPersistsOrReflows() {
        MarkdownEditor editor;
        configure(editor, QStringLiteral("**粗文**"));
        QVERIFY(expose(editor));
        QVERIFY(focusBody(editor));
        editor.selectSource(3, 3);   // “粗”之后（加粗段内部）
        QTextEdit* body = editor.textEditor();
        QTextDocument* doc = body->document();
        QVERIFY(body->hasFocus());
        const QRectF blockBefore = doc->documentLayout()->blockBoundingRect(doc->firstBlock());

        QInputMethodEvent preedit(QStringLiteral("拼音"), {});
        QApplication::sendEvent(body, &preedit);
        QApplication::processEvents();
        QVERIFY2(editor.isComposing(), "预编辑期间编辑器必须处于合成状态");
        QCOMPARE(editor.content(), QStringLiteral("**粗文**"));   // 预编辑不保存
        QCOMPARE(doc->toPlainText(), QStringLiteral("粗文"));
        const QRectF blockDuring = doc->documentLayout()->blockBoundingRect(doc->firstBlock());
        QVERIFY2(qAbs(blockDuring.top() - blockBefore.top()) <= 0.01
            && qAbs(blockDuring.height() - blockBefore.height()) <= 0.01,
            "预编辑不得触发块重排");

        QInputMethodEvent cancel;   // 空 preedit 结束合成（取消路径）
        QApplication::sendEvent(body, &cancel);
        QApplication::processEvents();
        QVERIFY2(!editor.isComposing(), "取消后必须退出合成状态");
        QCOMPARE(editor.content(), QStringLiteral("**粗文**"));
        QVERIFY2(spanCheck(doc, QStringLiteral("粗文"), [](const QTextCharFormat& f) {
            return f.fontWeight() == QFont::Bold; }), "取消合成不得丢格式");

        // 提交：格式保留 + 一步 undo。
        QInputMethodEvent again(QStringLiteral("再拼"), {});
        QApplication::sendEvent(body, &again);
        QVERIFY(commitText(body, QStringLiteral("字")));
        QTRY_COMPARE(editor.content(), QStringLiteral("**粗字文**"));
        QCOMPARE(doc->toPlainText(), QStringLiteral("粗字文"));
        QVERIFY2(spanCheck(doc, QStringLiteral("字"), [](const QTextCharFormat& f) {
            return f.fontWeight() == QFont::Bold; }), "提交文字必须继承加粗段格式");
        QTest::keyClick(body, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(editor.content(), QStringLiteral("**粗文**"));
        QCOMPARE(doc->toPlainText(), QStringLiteral("粗文"));
    }

    // ---------------------------------------------------------------- 9
    // 多行 Markdown 纯文本粘贴：焦点内即时生效，与同源重开的渲染一致。
    void multiLineMarkdownPasteMatchesReopen() {
        QClipboard* clipboard = QGuiApplication::clipboard();
        if (!clipboard) QSKIP("The active Qt platform provides no clipboard.");
        const QString source = QStringLiteral(
            "# 粘贴标题\n"
            "正文**加粗**与*斜体*文字\n"
            "- [ ] 未完成任务\n"
            "- [x] 已完成任务\n"
            "> 引用一行\n");
        m_touchedClipboard = true;
        clipboard->setText(QStringLiteral("Synthetic clipboard sentinel"));
        clipboard->setText(source);   // 纯文本，无编辑器私有 MIME

        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        MarkdownEditor pasted(&window);
        MarkdownEditor reopened(&window);
        configure(pasted, QString());
        configure(reopened, source);
        layout->addWidget(&pasted);
        layout->addWidget(&reopened);
        window.resize(760, 860);
        QVERIFY(expose(window));
        QVERIFY(focusBody(pasted));
        QTest::keyClick(pasted.textEditor(), Qt::Key_V, Qt::ControlModifier);
        QTRY_COMPARE(pasted.content(), source);

        QTextDocument* live = pasted.textEditor()->document();
        QTextDocument* fresh = reopened.textEditor()->document();
        QCOMPARE(live->blockCount(), fresh->blockCount());
        for (int index = 0; index < fresh->blockCount(); ++index) {
            QCOMPARE(live->findBlockByNumber(index).text(), fresh->findBlockByNumber(index).text());
        }
        QCOMPARE(live->toPlainText(), fresh->toPlainText());
        const qreal ps = bodyPointSize(live);
        qreal liveHeading = 0, freshHeading = 0;
        QVERIFY(spanCheck(live, QStringLiteral("粘贴标题"), [&](const QTextCharFormat& f) {
            liveHeading = f.fontPointSize();
            return f.fontWeight() == QFont::Bold && f.fontPointSize() > ps;
        }));
        QVERIFY(spanCheck(fresh, QStringLiteral("粘贴标题"), [&](const QTextCharFormat& f) {
            freshHeading = f.fontPointSize();
            return f.fontWeight() == QFont::Bold;
        }));
        QVERIFY2(qAbs(liveHeading - freshHeading) < 0.01, "粘贴与重开的标题字号必须一致");
        QVERIFY2(spanCheck(live, QStringLiteral("加粗"), [](const QTextCharFormat& f) {
            return f.fontWeight() == QFont::Bold; }), "粘贴的加粗必须即时生效");
        QVERIFY2(spanCheck(live, QStringLiteral("斜体"), [](const QTextCharFormat& f) {
            return f.fontItalic(); }), "粘贴的斜体必须即时生效");
        QVERIFY2(!live->toPlainText().contains(QLatin1String("- [ ]"))
            && !live->toPlainText().contains(QLatin1String("- [x]"))
            && !live->toPlainText().contains(QChar(0x2610))
            && !live->toPlainText().contains(QChar(0x2611)),
            "粘贴后的任务标记必须独立绘制，不落入正文");
        auto* edit = bodyEdit(pasted);
        QVERIFY2(edit, "粘贴后的正文必须是 EditorTextEdit");
        const QTextBlock pastedTask = blockWithText(live, QStringLiteral("未完成任务"));
        QVERIFY2(pastedTask.isValid(), "粘贴后的任务项必须可按渲染文本定位");
        QVERIFY2(edit->checkBoxRect(pastedTask).width() > 4,
            "粘贴后的任务项必须有有效复选框矩形");
        QVERIFY2(qAbs(live->documentLayout()->documentSize().height()
                - fresh->documentLayout()->documentSize().height()) <= 1.0,
            "粘贴与重开的文档高度必须一致");
        QVERIFY2(paintedInk(pasted.textEditor()->viewport()) > 100, "粘贴内容必须有真实墨迹");
        // 粘贴是单一 undo 事务：一次 Ctrl+Z 清空，一次 redo 完整恢复。
        QTest::keyClick(pasted.textEditor(), Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(pasted.content(), QString());
        QCOMPARE(live->toPlainText(), QString());
        QTest::keyClick(pasted.textEditor(), Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(pasted.content(), source);
        QCOMPARE(live->toPlainText(), fresh->toPlainText());
        QVERIFY2(spanCheck(live, QStringLiteral("加粗"), [](const QTextCharFormat& f) {
            return f.fontWeight() == QFont::Bold; }), "redo 恢复后粘贴的格式必须完整");
    }

    // ---------------------------------------------------------------- 10
    // 检查项内容只存正文源码：无 store/实体的编辑器同样可渲染可切换；
    // 有 store 时切换不创建/修改实体，落库后仍是正文文本。
    void checkboxStateLivesInBodyTextOnly() {
        {
            // 完全不设置 store 与实体：复选框照常渲染与切换。
            MarkdownEditor solo;
            solo.setBlockControlsEnabled(true);
            solo.setContent(QStringLiteral("- [ ] 甲项\n- [x] 乙项"));
            solo.resize(720, 540);
            QVERIFY(expose(solo));
            QVERIFY(focusBody(solo));
            QTextEdit* body = solo.textEditor();
            QTextDocument* doc = body->document();
            auto* edit = bodyEdit(solo);
            QVERIFY2(edit, "独立编辑器同样必须是 EditorTextEdit");
            QCOMPARE(doc->toPlainText(), QStringLiteral("甲项\n乙项"));
            const QTextBlock first = blockWithText(doc, QStringLiteral("甲项"));
            QVERIFY2(first.isValid(), "无实体时任务项也必须可按渲染文本定位");
            QVERIFY2(edit->checkBoxRect(first).width() > 4, "无实体时复选框矩形必须有效");
            QVERIFY2(clickActual(body->viewport(), edit->checkBoxRect(first).center()),
                "无实体时复选框必须可真实点击");
            QTRY_COMPARE(solo.content(), QStringLiteral("- [x] 甲项\n- [x] 乙项"));
            QVERIFY2(clickActual(body->viewport(),
                edit->checkBoxRect(blockWithText(doc, QStringLiteral("甲项"))).center()),
                "切换后的任务项复选框仍必须可真实点击");
            QTRY_COMPARE(solo.content(), QStringLiteral("- [ ] 甲项\n- [x] 乙项"));
        }
        {
            // 有 store：切换不增删实体；落库后状态保存在正文文本里。
            Entity source = m_store->entity(m_sourceId);
            source.body = QStringLiteral("- [ ] 存储项");
            QVERIFY(m_store->saveTask(source));
            const int entitiesBefore = m_store->entities().size();
            TaskDetailWidget detail(m_store.get());
            detail.resize(560, 660);
            QVERIFY(detail.setTask(m_sourceId));
            QVERIFY(expose(detail));
            auto* editor = detail.findChild<MarkdownEditor*>();
            QVERIFY(editor);
            QCOMPARE(editor->content(), QStringLiteral("- [ ] 存储项"));
            QVERIFY(focusBody(*editor));
            auto* edit = bodyEdit(*editor);
            QVERIFY(edit);
            QTextDocument* doc = editor->textEditor()->document();
            const QTextBlock item = blockWithText(doc, QStringLiteral("存储项"));
            QVERIFY2(item.isValid(), "详情正文中的任务项必须可按渲染文本定位");
            QVERIFY2(clickActual(editor->textEditor()->viewport(), edit->checkBoxRect(item).center()),
                "详情正文中的复选框必须可真实点击");
            QTRY_COMPARE(editor->content(), QStringLiteral("- [x] 存储项"));
            QCOMPARE(m_store->entities().size(), entitiesBefore);   // 切换不创建实体
            QVERIFY(detail.finishEditing());
            QCOMPARE(m_store->entity(m_sourceId).body, QStringLiteral("- [x] 存储项"));
            QCOMPARE(m_store->entities().size(), entitiesBefore);
        }
    }

    // ---------------------------------------------------------------- 11
    // 链接标签内单字删除（源码偏移映射回归）：真实文档光标 b|c 时 Backspace 只删 b，
    // a|bc 时 Delete 只删 b，两个链接的 href 都保留；边界（光标在整链源码首尾）仍原子
    // 删除整链（旧行为不回退）。
    void linkLabelEditingDeletesSingleCharacter() {
        {
            // Backspace 只删第二个标签里的 b。
            MarkdownEditor editor;
            configure(editor, QStringLiteral("[a](https://a.example)[bc](https://b.example)"));
            QVERIFY(expose(editor));
            QVERIFY(focusBody(editor));
            QTextEdit* body = editor.textEditor();
            QTextDocument* doc = body->document();
            QCOMPARE(doc->toPlainText(), QStringLiteral("abc"));
            QVERIFY2(setDocumentCaret(editor, 2), "b|c 必须用真实文档光标布置");
            QTest::keyClick(body, Qt::Key_Backspace);
            QTRY_COMPARE(editor.content(), QStringLiteral("[a](https://a.example)[c](https://b.example)"));
            QCOMPARE(doc->toPlainText(), QStringLiteral("ac"));
            QVERIFY2(spanCheck(doc, QStringLiteral("a"), [](const QTextCharFormat& f) {
                return f.anchorHref() == QStringLiteral("https://a.example");
            }), "前一个链接必须完整保留");
            QVERIFY2(spanCheck(doc, QStringLiteral("c"), [](const QTextCharFormat& f) {
                return f.anchorHref() == QStringLiteral("https://b.example");
            }), "删字后的后一个链接必须仍是链接");
        }
        {
            // Delete 只删 b。
            MarkdownEditor editor;
            configure(editor, QStringLiteral("[abc](https://example.com)"));
            QVERIFY(expose(editor));
            QVERIFY(focusBody(editor));
            QTextEdit* body = editor.textEditor();
            QTextDocument* doc = body->document();
            QCOMPARE(doc->toPlainText(), QStringLiteral("abc"));
            QVERIFY2(setDocumentCaret(editor, 1), "a|bc 必须用真实文档光标布置");
            QTest::keyClick(body, Qt::Key_Delete);
            QTRY_COMPARE(editor.content(), QStringLiteral("[ac](https://example.com)"));
            QCOMPARE(doc->toPlainText(), QStringLiteral("ac"));
            QVERIFY2(spanCheck(doc, QStringLiteral("ac"), [](const QTextCharFormat& f) {
                return f.anchorHref() == QStringLiteral("https://example.com");
            }), "Delete 删字后链接必须保留");
        }
        {
            // 边界：光标紧跟整链之后，Backspace 原子删除整链（旧行为保留）。
            MarkdownEditor editor;
            configure(editor, QStringLiteral("[a](https://a.example)尾字"));
            QVERIFY(expose(editor));
            QVERIFY(focusBody(editor));
            QTextEdit* body = editor.textEditor();
            QTextDocument* doc = body->document();
            QCOMPARE(doc->toPlainText(), QStringLiteral("a尾字"));
            QVERIFY2(setDocumentCaret(editor, 1), "整链之后的光标必须用真实文档光标布置");
            QTest::keyClick(body, Qt::Key_Backspace);
            QTRY_COMPARE(editor.content(), QStringLiteral("尾字"));
            QCOMPARE(doc->toPlainText(), QStringLiteral("尾字"));
            QVERIFY(!doc->toPlainText().contains(QLatin1Char('a')));
        }
        {
            // 边界：光标紧贴整链之前，Delete 原子删除整链（旧行为保留）。
            MarkdownEditor editor;
            configure(editor, QStringLiteral("头字[a](https://a.example)"));
            QVERIFY(expose(editor));
            QVERIFY(focusBody(editor));
            QTextEdit* body = editor.textEditor();
            QTextDocument* doc = body->document();
            QCOMPARE(doc->toPlainText(), QStringLiteral("头字a"));
            QVERIFY2(setDocumentCaret(editor, doc->find(QStringLiteral("a")).selectionStart()),
                "整链之前的光标必须用真实文档光标布置");
            QTest::keyClick(body, Qt::Key_Delete);
            QTRY_COMPARE(editor.content(), QStringLiteral("头字"));
            QCOMPARE(doc->toPlainText(), QStringLiteral("头字"));
            QVERIFY(!doc->toPlainText().contains(QLatin1Char('a')));
        }
    }

    // ---------------------------------------------------------------- 12
    // 链接标签内的行内样式在编辑时保留：粗体标签内 a|b 键入 X，源码仍是粗体标签
    // [**aXb**](url) 且显示带 Bold；跨样式标签（a 粗体 + bc 普通）改普通部分时
    // 未改动的粗体样式保留。
    void inlineStyleInsideLinkLabelSurvivesTyping() {
        {
            MarkdownEditor editor;
            configure(editor, QStringLiteral("[**ab**](https://example.com)"));
            QVERIFY(expose(editor));
            QVERIFY(focusBody(editor));
            QTextEdit* body = editor.textEditor();
            QTextDocument* doc = body->document();
            QCOMPARE(doc->toPlainText(), QStringLiteral("ab"));
            QVERIFY2(setDocumentCaret(editor, 1), "a|b 必须用真实文档光标布置");
            QTest::keyClicks(body, QStringLiteral("X"));
            QTRY_COMPARE(editor.content(), QStringLiteral("[**aXb**](https://example.com)"));
            QCOMPARE(body->textCursor().position(), 2);
            QCOMPARE(doc->toPlainText(), QStringLiteral("aXb"));
            QVERIFY2(spanCheck(doc, QStringLiteral("aXb"), [](const QTextCharFormat& f) {
                return f.fontWeight() == QFont::Bold
                    && f.anchorHref() == QStringLiteral("https://example.com");
            }), "标签内键入后必须仍是加粗链接文字");
        }
        {
            // 跨内行样式标签：a 加粗 + bc 普通；在 b|c 处键入 X，粗体 a 不受影响。
            MarkdownEditor editor;
            configure(editor, QStringLiteral("[**a**bc](https://example.com)"));
            QVERIFY(expose(editor));
            QVERIFY(focusBody(editor));
            QTextEdit* body = editor.textEditor();
            QTextDocument* doc = body->document();
            QCOMPARE(doc->toPlainText(), QStringLiteral("abc"));
            QVERIFY2(setDocumentCaret(editor, 2), "b|c 必须用真实文档光标布置");
            QTest::keyClicks(body, QStringLiteral("X"));
            QTRY_COMPARE(editor.content(), QStringLiteral("[**a**bXc](https://example.com)"));
            QCOMPARE(doc->toPlainText(), QStringLiteral("abXc"));
            QVERIFY2(spanCheck(doc, QStringLiteral("a"), [](const QTextCharFormat& f) {
                return f.fontWeight() == QFont::Bold
                    && f.anchorHref() == QStringLiteral("https://example.com");
            }), "未编辑的粗体部分必须保持加粗链接");
            QVERIFY2(!spanCheck(doc, QStringLiteral("bXc"), [](const QTextCharFormat& f) {
                return f.fontWeight() == QFont::Bold;
            }), "普通部分键入后不得变成粗体");
            QVERIFY2(spanCheck(doc, QStringLiteral("bXc"), [](const QTextCharFormat& f) {
                return f.anchorHref() == QStringLiteral("https://example.com");
            }), "标签内的普通部分仍必须是链接");
        }
    }

    // ---------------------------------------------------------------- 13
    // 代码 span 内成对反引号：合法源码 `a``b` z 初始显示 a``b z（两枚反引号是代码
    // 内容）；末尾追加字符源码不丢反引号；在代码内键入新反引号后仍合法（同源重开等价）。
    void codeSpanBackticksSurviveEditing() {
        MarkdownEditor editor;
        configure(editor, QStringLiteral("`a``b` z"));
        QVERIFY(expose(editor));
        QVERIFY(focusBody(editor));
        QTextEdit* body = editor.textEditor();
        QTextDocument* doc = body->document();
        QCOMPARE(doc->toPlainText(), QStringLiteral("a``b z"));
        QVERIFY2(spanCheck(doc, QStringLiteral("a``b"), [](const QTextCharFormat& f) {
            return f.background().style() == Qt::SolidPattern;
        }), "a``b 必须整体渲染为行内代码");
        // 末尾追加 X：源码仍完整保留两枚反引号。
        QTest::keyClick(body, Qt::Key_End);
        QTest::keyClicks(body, QStringLiteral("X"));
        QTRY_COMPARE(editor.content(), QStringLiteral("`a``b` zX"));
        QCOMPARE(doc->toPlainText(), QStringLiteral("a``b zX"));
        QVERIFY2(spanCheck(doc, QStringLiteral("a``b"), [](const QTextCharFormat& f) {
            return f.background().style() == Qt::SolidPattern;
        }), "追加字符后代码 span 不得丢反引号");
        // 代码内部键入新反引号：源码精确，且同源重开渲染等价（合法源码判据）。
        QVERIFY2(setDocumentCaret(editor, 1), "代码内部（a 之后）必须用真实文档光标布置");
        QTest::keyClicks(body, QStringLiteral("`"));
        QTRY_COMPARE(editor.content(), QStringLiteral("`a```b` zX"));
        MarkdownEditor reopened;
        configure(reopened, editor.content());
        QCOMPARE(reopened.textEditor()->document()->toPlainText(), doc->toPlainText());
        QVERIFY2(spanCheck(reopened.textEditor()->document(), QStringLiteral("```"), [](const QTextCharFormat& f) {
            return f.background().style() == Qt::SolidPattern;
        }), "键入新反引号后重开仍必须有代码背景（源码合法）");
        {
            // 简单代码 span 内容中键入新反引号：显示为字面反引号且 b 仍是代码；
            // writer 规范化为双反引号围栏 ``a`b``，同源重开等价。
            MarkdownEditor simple;
            configure(simple, QStringLiteral("`ab` z"));
            QVERIFY(expose(simple));
            QVERIFY(focusBody(simple));
            QTextEdit* simpleBody = simple.textEditor();
            QTextDocument* simpleDoc = simpleBody->document();
            QCOMPARE(simpleDoc->toPlainText(), QStringLiteral("ab z"));
            QTest::keyClick(simpleBody, Qt::Key_Home);
            QTest::keyClick(simpleBody, Qt::Key_Right);   // a|b（代码内容内部）
            QTest::keyClicks(simpleBody, QStringLiteral("`"));
            QTRY_COMPARE(simple.content(), QStringLiteral("``a`b`` z"));
            QCOMPARE(simpleDoc->toPlainText(), QStringLiteral("a`b z"));
            QVERIFY2(spanCheck(simpleDoc, QStringLiteral("a`b"), [](const QTextCharFormat& f) {
                return f.background().style() == Qt::SolidPattern;
            }), "键入的反引号必须字面落在代码 span 内，b 仍是代码");
            MarkdownEditor normalized;
            configure(normalized, simple.content());
            QCOMPARE(normalized.textEditor()->document()->toPlainText(), QStringLiteral("a`b z"));
            QVERIFY2(spanCheck(normalized.textEditor()->document(), QStringLiteral("a`b"), [](const QTextCharFormat& f) {
                return f.background().style() == Qt::SolidPattern;
            }), "规范化源码重开后必须渲染等价");
        }
    }

    // ---------------------------------------------------------------- 14
    // 源码私有 MIME 复制粘贴：全选复制同时提供 application/x-kylinlist-markdown 源码与
    // 纯文本渲染文本；粘贴到空编辑器保留源码与字符格式。示例：粗体、标题、下划线+高亮。
    void richCopyPasteRoundTripsViaSourceMime() {
        QClipboard* clipboard = QGuiApplication::clipboard();
        if (!clipboard) QSKIP("The active Qt platform provides no clipboard.");
        m_touchedClipboard = true;
        clipboard->setText(QStringLiteral("Synthetic clipboard sentinel"));

        QWidget window;
        auto* layout = new QVBoxLayout(&window);
        MarkdownEditor origin(&window);
        MarkdownEditor destination(&window);
        configure(origin, QString());
        configure(destination, QString());
        layout->addWidget(&origin);
        layout->addWidget(&destination);
        window.resize(760, 860);
        QVERIFY(expose(window));

        const auto copyAll = [&](MarkdownEditor& editor) -> bool {
            if (!focusBody(editor)) return false;
            QTest::keyClick(editor.textEditor(), Qt::Key_A, Qt::ControlModifier);
            QTest::keyClick(editor.textEditor(), Qt::Key_C, Qt::ControlModifier);
            QApplication::processEvents();
            return clipboard->mimeData() && clipboard->mimeData()->hasFormat(QLatin1String(MarkdownMime));
        };
        const auto pastedSource = [&]() {
            return QString::fromUtf8(clipboard->mimeData()->data(QLatin1String(MarkdownMime)));
        };
        const auto pasteInto = [&](MarkdownEditor& editor) {
            editor.setContent(QString());
            QVERIFY(focusBody(editor));
            QTest::keyClick(editor.textEditor(), Qt::Key_V, Qt::ControlModifier);
        };

        // 粗体：私有 MIME 是源码，纯文本通道仍是渲染文本 ab；粘贴保留 Bold。
        origin.setContent(QStringLiteral("**ab**"));
        QVERIFY2(copyAll(origin), "全选复制必须提供源码私有 MIME");
        QCOMPARE(pastedSource(), QStringLiteral("**ab**"));
        QCOMPARE(clipboard->text(), QStringLiteral("ab"));
        pasteInto(destination);
        QTRY_COMPARE(destination.content(), QStringLiteral("**ab**"));
        QVERIFY2(spanCheck(destination.textEditor()->document(), QStringLiteral("ab"), [](const QTextCharFormat& f) {
            return f.fontWeight() == QFont::Bold; }), "粘贴必须保留粗体字符格式");

        // 标题示例：复制粘贴后仍是标题（加粗 + 字号大于正文）。
        origin.setContent(QStringLiteral("# 标题文字"));
        QVERIFY(copyAll(origin));
        QCOMPARE(pastedSource(), QStringLiteral("# 标题文字"));
        QCOMPARE(clipboard->text(), QStringLiteral("标题文字"));
        pasteInto(destination);
        QTRY_COMPARE(destination.content(), QStringLiteral("# 标题文字"));
        QVERIFY2(spanCheck(destination.textEditor()->document(), QStringLiteral("标题文字"),
            [&destination](const QTextCharFormat& f) {
                return f.fontWeight() == QFont::Bold
                    && f.fontPointSize() > bodyPointSize(destination.textEditor()->document());
            }), "粘贴必须保留标题字号与字重");

        // 下划线 + 高亮示例：两种字符格式都随源码往返。
        const QString styled = QStringLiteral("<u>下线</u>与<mark data-color=\"yellow\">高亮</mark>");
        origin.setContent(styled);
        QVERIFY(copyAll(origin));
        QCOMPARE(pastedSource(), styled);
        pasteInto(destination);
        QTRY_COMPARE(destination.content(), styled);
        const QColor highlight = Markdown::highlightColor(QStringLiteral("yellow"));
        QVERIFY2(spanCheck(destination.textEditor()->document(), QStringLiteral("下线"), [](const QTextCharFormat& f) {
            return f.fontUnderline(); }), "粘贴必须保留下划线");
        QVERIFY2(spanCheck(destination.textEditor()->document(), QStringLiteral("高亮"),
            [&highlight](const QTextCharFormat& f) {
                return f.background().style() == Qt::SolidPattern && f.background().color() == highlight;
            }), "粘贴必须保留高亮底色");
    }

    // ---------------------------------------------------------------- 15
    // 撤销合组（typing 事务链）：行尾真实键入 b → 1000ms 内点击本行复选框（不做任何
    // 键盘导航以免打断 typing 链）→ 紧接键入 c。Ctrl+Z 依次只撤 c、再撤复选框切换、
    // 再撤 b，每步内容精确。
    void checkboxClickJoinsTypingUndoChain() {
        MarkdownEditor editor;
        configure(editor, QStringLiteral("- [ ] a"));
        QVERIFY(expose(editor));
        QVERIFY(focusBody(editor));
        QTextEdit* body = editor.textEditor();
        QTextDocument* doc = body->document();
        auto* edit = bodyEdit(editor);
        QVERIFY2(edit, "正文必须是 EditorTextEdit（复选框几何的宿主）");
        QVERIFY2(body->textCursor().atBlockEnd(), "聚焦后光标应在本行行尾");
        QTest::keyClicks(body, QStringLiteral("b"));
        QTRY_COMPARE(editor.content(), QStringLiteral("- [ ] ab"));
        // 1000ms 内直接点击本行复选框：无任何键盘导航，保持 typing 链延续。
        const QTextBlock block = doc->findBlockByNumber(0);
        QVERIFY2(clickActual(body->viewport(), edit->checkBoxRect(block).center()),
            "键入后本行复选框必须可立即真实点击");
        QVERIFY2(waitUntil([&]() { return editor.content() == QStringLiteral("- [x] ab"); }),
            "点击复选框必须当场切换为已选");
        QTest::keyClicks(body, QStringLiteral("c"));
        QTRY_COMPARE(editor.content(), QStringLiteral("- [x] abc"));
        // 撤销次序：c → 复选框切换 → b。
        QTest::keyClick(body, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY2(waitUntil([&]() { return editor.content() == QStringLiteral("- [x] ab"); }),
            "第一次 undo 只能撤销 c，复选框保持已选");
        QTest::keyClick(body, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY2(waitUntil([&]() { return editor.content() == QStringLiteral("- [ ] ab"); }),
            "第二次 undo 撤销复选框切换");
        QTest::keyClick(body, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY2(waitUntil([&]() { return editor.content() == QStringLiteral("- [ ] a"); }),
            "第三次 undo 撤销 b");
    }

    void checkboxClickKeepsDetailOuterScrollAndRemoteSelection() {
        QStringList lines{QStringLiteral("- [ ] 顶部任务")};
        for (int i = 0; i < 60; ++i) lines.append(QStringLiteral("填充正文 %1").arg(i));
        lines.append(QStringLiteral("- [ ] 底部任务"));
        Entity entity = m_store->entity(m_sourceId);
        entity.body = lines.join(QLatin1Char('\n'));
        QVERIFY(m_store->saveTask(entity));
        TaskDetailWidget detail(m_store.get());
        detail.resize(560, 660);
        QVERIFY(detail.setTask(m_sourceId));
        QVERIFY(expose(detail));
        auto* editor = detail.findChild<MarkdownEditor*>();
        auto* outer = detail.findChild<QScrollArea*>(QStringLiteral("DetailScroll"));
        QVERIFY(editor && outer);
        QTextEdit* body = editor->textEditor();
        outer->verticalScrollBar()->setValue(0);
        QApplication::processEvents();
        QVERIFY(clickActual(body->viewport(), body->cursorRect(QTextCursor(body->document()->firstBlock())).center()));
        QTRY_VERIFY(body->hasFocus());
        auto* edit = bodyEdit(*editor);
        QVERIFY(edit);
        QTest::keyClick(body, Qt::Key_Home, Qt::ControlModifier);
        QTest::keyClick(body, Qt::Key_End, Qt::ShiftModifier);
        QVERIFY(body->textCursor().hasSelection());
        const auto selection = editor->sourceSelection();
        QTest::qWait(30);
        QVERIFY(outer->verticalScrollBar()->maximum() > 0);
        outer->verticalScrollBar()->setValue(outer->verticalScrollBar()->maximum());
        QApplication::processEvents();
        const int scroll = outer->verticalScrollBar()->value();
        const QRect checkbox = edit->checkBoxRect(body->document()->lastBlock());
        const QPoint global = body->viewport()->mapToGlobal(checkbox.center());
        QVERIFY(outer->viewport()->rect().contains(outer->viewport()->mapFromGlobal(global)));
        QVERIFY(clickActual(body->viewport(), checkbox.center()));
        QTRY_VERIFY(editor->content().endsWith(QStringLiteral("- [x] 底部任务")));
        QTest::qWait(40);
        QCOMPARE(outer->verticalScrollBar()->value(), scroll);
        QCOMPARE(editor->sourceSelection(), selection);
        QVERIFY(body->textCursor().hasSelection());
        QVERIFY(detail.finishEditing());
    }

    // ---------------------------------------------------------------- 16
    // 空检查项/有序/无序/引用行在 IME 预编辑（首字尚未提交）时，前置框/序号/圆点/
    // 引用条不得跟随预输入光标横移（回归：标记曾以 cursorRect(QTextCursor(block))
    // 锚定，而空块 preedit 会把该锚点从正文起点向右推；长 preedit 换行时还会带标记
    // 纵向跳行）。期望不取自 caretRect（被测根因即 caretRect 随 preedit 变动）：基线在
    // 组成前用真实 cursorRect(block) 设定固定左侧裁剪带，右缘 = 正文起点 x 再收 2px，
    // 严格小于 preedit/闪烁 caret 所在列，防止期望值被 preedit 污染；逐像素比较内存
    // 渲染（不保存截图），两种主题数据组合。
    void imePreeditOnEmptyMarkerBlocksKeepsMarkerAnchored_data() {
        QTest::addColumn<QString>("source");
        QTest::addColumn<int>("line");
        QTest::addColumn<int>("kind");
        QTest::addColumn<bool>("dark");
        const QString ordered = QStringLiteral(
            "1. 甲\n2. 乙\n3. 丙\n4. 丁\n5. 戊\n6. 己\n7. 庚\n8. 辛\n9. 壬\n10. ");
        QTest::newRow("task-unchecked-light") << QStringLiteral("- [ ] ") << 0
            << static_cast<int>(MarkdownDocument::Kind::TaskList) << false;
        QTest::newRow("task-unchecked-dark") << QStringLiteral("- [ ] ") << 0
            << static_cast<int>(MarkdownDocument::Kind::TaskList) << true;
        QTest::newRow("task-checked-light") << QStringLiteral("- [x] ") << 0
            << static_cast<int>(MarkdownDocument::Kind::TaskList) << false;
        QTest::newRow("task-checked-dark") << QStringLiteral("- [x] ") << 0
            << static_cast<int>(MarkdownDocument::Kind::TaskList) << true;
        QTest::newRow("unordered-bullet-light") << QStringLiteral("- ") << 0
            << static_cast<int>(MarkdownDocument::Kind::UnorderedList) << false;
        QTest::newRow("unordered-bullet-dark") << QStringLiteral("- ") << 0
            << static_cast<int>(MarkdownDocument::Kind::UnorderedList) << true;
        QTest::newRow("ordered-multidigit-light") << ordered << 9
            << static_cast<int>(MarkdownDocument::Kind::OrderedList) << false;
        QTest::newRow("ordered-multidigit-dark") << ordered << 9
            << static_cast<int>(MarkdownDocument::Kind::OrderedList) << true;
        QTest::newRow("quote-light") << QStringLiteral("> ") << 0
            << static_cast<int>(MarkdownDocument::Kind::Quote) << false;
        QTest::newRow("quote-dark") << QStringLiteral("> ") << 0
            << static_cast<int>(MarkdownDocument::Kind::Quote) << true;
    }
    void imePreeditOnEmptyMarkerBlocksKeepsMarkerAnchored() {
        QFETCH(QString, source);
        QFETCH(int, line);
        QFETCH(int, kind);
        QFETCH(bool, dark);
        ThemeManager::instance().setPreference(dark
            ? ThemeManager::Preference::Dark : ThemeManager::Preference::Light);
        QApplication::processEvents();

        MarkdownEditor editor;
        configure(editor, source);
        QVERIFY(expose(editor));
        QVERIFY(focusBody(editor));   // 真实聚焦视口，标记先有初始墨迹
        QVERIFY(keyboardToLine(editor, line));
        QTextEdit* body = editor.textEditor();
        // The native empty-document placeholder disappears during composition;
        // exclude that unrelated text so the pixel comparison measures markers.
        body->setPlaceholderText(QString());
        QTextDocument* doc = body->document();
        auto* edit = bodyEdit(editor);
        QVERIFY2(edit, "正文必须是 EditorTextEdit（标记几何的宿主）");
        const QTextBlock block = doc->findBlockByNumber(line);
        QVERIFY2(block.isValid(), "目标行必须对应文档块");
        QVERIFY2(block.text().isEmpty(), "回归场景要求目标块为空（首字尚未提交）");
        QCOMPARE(body->textCursor().blockNumber(), line);
        QCOMPARE(body->textCursor().positionInBlock(), 0);
        QCOMPARE(int(MarkdownDocument::kind(block)), kind);
        if (kind == static_cast<int>(MarkdownDocument::Kind::TaskList)) {
            QCOMPARE(block.blockFormat().boolProperty(MarkdownDocument::CheckedProperty),
                source.startsWith(QStringLiteral("- [x]")));   // 两态任务行必须真有区分
        }

        // 基线：组成前的真实块锚点与标记几何。
        const QRect anchor = body->cursorRect(QTextCursor(block));
        QVERIFY2(anchor.left() > 6, "正文起点必须给左侧标记带留出宽度");
        QCOMPARE(body->cursorRect().left(), anchor.left());   // 空块光标与块锚点同列
        const QString sourceBefore = editor.content();
        const QRect band(0, anchor.top(), qMax(1, anchor.left() - 2), anchor.height());
        const QRect boxBefore = edit->checkBoxRect(block);
        if (kind == static_cast<int>(MarkdownDocument::Kind::TaskList))
            QVERIFY2(boxBefore.width() > 4 && boxBefore.height() > 4,
                "任务块基线复选框矩形必须有效");

        const auto renderBand = [&body, band]() {
            QImage image(body->viewport()->size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(ThemeManager::instance().color(ds::ColorToken::Surface));
            QPainter painter(&image);
            body->viewport()->render(&painter);
            painter.end();
            return image.copy(band);
        };
        const auto inkOf = [](const QImage& image) {
            const QColor background = ThemeManager::instance().color(ds::ColorToken::Surface);
            int ink = 0;
            for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                // 阈值须容纳引用条 Divider 令牌（浅/深主题与 Surface 的反差都低于 40）。
                const int contrast = qMax(qAbs(pixel.red() - background.red()),
                    qMax(qAbs(pixel.green() - background.green()), qAbs(pixel.blue() - background.blue())));
                if (pixel.alpha() > 40 && contrast >= 8) ++ink;
            }
            return ink;
        };
        const auto bandDiff = [](const QImage& left, const QImage& right) {
            int count = 0, minX = left.width(), maxX = -1;
            const int width = qMin(left.width(), right.width());
            const int height = qMin(left.height(), right.height());
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
                if (left.pixel(x, y) != right.pixel(x, y)) {
                    ++count;
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                }
            return QStringLiteral("%1 px differ (x %2..%3, size %4x%5 vs %6x%7)")
                .arg(count).arg(minX).arg(maxX).arg(left.width()).arg(left.height())
                .arg(right.width()).arg(right.height());
        };
        const QImage bandBefore = renderBand();
        QVERIFY2(inkOf(bandBefore) > 8,
            qPrintable(QStringLiteral("初始标记必须有可判墨迹：%1 px").arg(inkOf(bandBefore))));

        const auto markerBandUnchanged = [&](const QString& stage) {
            const QImage current = renderBand();
            QVERIFY2(current == bandBefore,
                qPrintable(QStringLiteral("%1：标记带像素漂移：%2")
                    .arg(stage, bandDiff(current, bandBefore))));
        };
        // 真实 QInputMethodEvent 通道，带 Cursor 属性模拟 IME。
        const auto sendPreedit = [&body](const QString& preedit, int cursor) {
            QList<QInputMethodEvent::Attribute> attributes;
            attributes.append(QInputMethodEvent::Attribute(
                QInputMethodEvent::Cursor, cursor, 1, QVariant(true)));
            QInputMethodEvent event(preedit, attributes);
            QApplication::sendEvent(body, &event);
            QApplication::processEvents();
        };
        const auto duringPreedit = [&](const QString& stage, const QString& preedit, bool wrapped) {
            QVERIFY2(editor.isComposing(),
                qPrintable(QStringLiteral("%1：必须处于合成状态").arg(stage)));
            QVERIFY2(!editor.content().contains(preedit),
                qPrintable(QStringLiteral("%1：源码不得包含 preedit").arg(stage)));
            QCOMPARE(editor.content(), sourceBefore);
            QVERIFY2(block.text().isEmpty(),
                qPrintable(QStringLiteral("%1：preedit 不得落入文档文本").arg(stage)));
            // 场景生效证明：真实 caret 必须真的动了（横移或换行下移）；
            // 只证场景，不定义标记期望（根因正是 caretRect 随 preedit 变动）。
            if (wrapped)
                QVERIFY2(body->cursorRect().top() > anchor.top(),
                    qPrintable(QStringLiteral("%1：长 preedit 必须真实换行下移 caret（%2 -> %3）")
                        .arg(stage).arg(anchor.top()).arg(body->cursorRect().top())));
            else
                QVERIFY2(body->cursorRect().left() > anchor.left(),
                    qPrintable(QStringLiteral("%1：preedit 必须真实横移 caret（%2 -> %3）")
                        .arg(stage).arg(anchor.left()).arg(body->cursorRect().left())));
            markerBandUnchanged(stage);
            QCOMPARE(int(MarkdownDocument::kind(block)), kind);
        };
        const auto boxUnchanged = [&]() {
            QCOMPARE(edit->checkBoxRect(block), boxBefore);   // 复选框几何不得随 preedit 变动
        };

        const QString longPreedit = QStringLiteral("renjianchangpinyin").repeated(9);
        sendPreedit(QStringLiteral("r"), 1);                  // 长度 1，光标在尾
        duringPreedit(QStringLiteral("len1"), QStringLiteral("r"), false);
        boxUnchanged();
        sendPreedit(QStringLiteral("renjian"), 7);            // 多字母，光标仍在尾
        duringPreedit(QStringLiteral("len7"), QStringLiteral("renjian"), false);
        boxUnchanged();
        sendPreedit(QStringLiteral("renjian"), 3);            // 同串，光标从尾移中间
        duringPreedit(QStringLiteral("cursorMid"), QStringLiteral("renjian"), false);
        boxUnchanged();
        sendPreedit(longPreedit, longPreedit.size());         // 超长换到第二可视行，标记不得跳行
        duringPreedit(QStringLiteral("wrapped"), longPreedit, true);
        boxUnchanged();

        // 取消：合成结束，标记仍锚同一处，源码/undo 不受影响。
        QInputMethodEvent cancel;
        QApplication::sendEvent(body, &cancel);
        QApplication::processEvents();
        QVERIFY2(!editor.isComposing(), "取消后必须退出合成状态");
        QCOMPARE(editor.content(), sourceBefore);
        QVERIFY2(qAbs(body->cursorRect().left() - anchor.left()) <= 1,
            "取消后 caret 应回到基线列");
        markerBandUnchanged(QStringLiteral("cancel"));
        boxUnchanged();
        QVERIFY2(!doc->isUndoAvailable(), "取消预编辑不得产生撤销步骤");

        // 提交首个汉字：源码精确落行、格式继承、标记与几何仍在基线锚点。
        sendPreedit(QStringLiteral("r"), 1);
        QVERIFY(editor.isComposing());
        QVERIFY(commitText(body, QStringLiteral("任")));
        QTRY_COMPARE(editor.content(), sourceBefore + QStringLiteral("任"));
        QCOMPARE(block.text(), QStringLiteral("任"));
        QVERIFY2(!editor.isComposing(), "提交后必须退出合成状态");
        markerBandUnchanged(QStringLiteral("commit"));
        boxUnchanged();
        QCOMPARE(int(MarkdownDocument::kind(block)), kind);
        if (kind == static_cast<int>(MarkdownDocument::Kind::Quote)) {
            QVERIFY2(spanCheck(doc, QStringLiteral("任"), [](const QTextCharFormat& f) {
                return f.fontItalic(); }), "引用行提交的首字必须继承引用斜体");
        } else {
            QVERIFY2(spanCheck(doc, QStringLiteral("任"), [&doc](const QTextCharFormat& f) {
                return f.fontWeight() != QFont::Bold && f.fontPointSize() <= bodyPointSize(doc);
            }), "列表行提交的首字必须保持正文格式");
        }

        // 一步 undo 回到空行：标记仍在同一锚点。
        QTest::keyClick(body, Qt::Key_Z, Qt::ControlModifier);
        QTRY_COMPARE(editor.content(), sourceBefore);
        QCOMPARE(block.text(), QString());
        markerBandUnchanged(QStringLiteral("undo"));
        boxUnchanged();
        QCOMPARE(int(MarkdownDocument::kind(block)), kind);
    }

private:
    void configure(MarkdownEditor& editor, const QString& source) {
        editor.setStore(m_store.get());
        editor.setEntityId(m_sourceId);
        editor.setBlockControlsEnabled(true);
        editor.setContent(source);
        editor.resize(720, 540);
    }
    bool m_hadDataDir = false, m_touchedClipboard = false;
    QByteArray m_previousDataDir;
    QSettings::Format m_previousSettingsFormat = QSettings::NativeFormat;
    std::unique_ptr<QTemporaryDir> m_settingsDir, m_dir;
    std::unique_ptr<TaskStore> m_store;
    QString m_sourceId;
};

QTEST_MAIN(InstantMarkdownTest)
#include "tst_instantmarkdown.moc"
