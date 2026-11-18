#include <cstdio>
#include <memory>

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QTextCursor>
#include <QTextDocument>
#include <QUrl>
#include <QtTest>

#include "app/TaskStore.h"
#include "db/Database.h"
#include "domain/InternalLink.h"
#include "ui/AppStyle.h"
#include "ui/EntityCommands.h"
#include "ui/EntityPicker.h"
#include "ui/Markdown.h"

using namespace kl;

namespace {
const QString FirstId = QStringLiteral("12345678-1234-4abc-8def-1234567890ab");
const QString SecondId = QStringLiteral("abcdef01-2345-4678-9abc-def012345678");

QString textOf(const QString& html) {
    QTextDocument document;
    document.setHtml(html);
    return document.toPlainText();
}

Entity targetEntity(const QString& title) {
    Entity entity;
    entity.id = FirstId;
    entity.title = title;
    return entity;
}
} // namespace

class EditorLinksTest : public QObject {
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
        // Seed the isolated preference before ThemeManager is constructed, so
        // neither its normal read nor its fallback can consult real settings.
        QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                           QStringLiteral("kylinlist"), QStringLiteral("kylinlist"));
        settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("light"));
        settings.sync();
        AppStyle::apply(*qobject_cast<QApplication*>(qApp));
    }
    void init() {
        m_dir.reset(new QTemporaryDir);
        QVERIFY(m_dir->isValid());
        qputenv("KYLINLIST_DATA_DIR", m_dir->path().toUtf8());
        QString error;
        QVERIFY2(Database::instance().openAt(m_dir->path() + QStringLiteral("/editorlinks.db"), &error), qPrintable(error));
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

    void canonicalIdsAndSafeTargets() {
        QCOMPARE(internalLinkId(FirstId), FirstId);
        QVERIFY(isValidLinkTarget(FirstId));
        for (const QString& invalid : {FirstId.toUpper(), QLatin1Char('{') + FirstId + QLatin1Char('}'),
                                      QLatin1Char(' ') + FirstId, FirstId + QLatin1Char('/'),
                                      QStringLiteral("标题相同"), QStringLiteral("12345678-1234-4abc-8def-1234567890ag")}) {
            QVERIFY2(internalLinkId(invalid).isEmpty(), qPrintable(invalid));
            QVERIFY(!isValidLinkTarget(invalid));
            QVERIFY(entityReference(invalid).isEmpty());
        }
        for (const QString& safe : {FirstId, QStringLiteral("https://example.com/路径(二)"),
                                   QStringLiteral("HTTP://example.com"), QStringLiteral("mailto:reader@example.com")})
            QVERIFY2(isSafeLinkTarget(safe), qPrintable(safe));
        for (const QString& unsafe : {QString(), QStringLiteral("javascript:alert(1)"), QStringLiteral("JaVaScRiPt:alert(1)"),
                                     QStringLiteral("data:text/html,test"), QStringLiteral("file:///tmp/test.txt"),
                                     QStringLiteral("ftp://example.com"), QStringLiteral("//example.com"),
                                     QStringLiteral("https://"), QStringLiteral("https://example.com\n/path")}) {
            QVERIFY2(!isSafeLinkTarget(unsafe), qPrintable(unsafe));
            QVERIFY(markdownLink(unsafe, QStringLiteral("文字")).isEmpty());
        }
    }

    void referenceEscapingAndUtf16Ranges() {
        const QString label = QStringLiteral("文档 [终稿] | C:\\笔记\\草稿 🦄 **原样** <u>字</u>");
        const QString reference = entityReference(FirstId, label);
        QCOMPARE(reference, QStringLiteral("[[%1|文档 \\[终稿\\] \\| C:\\\\笔记\\\\草稿 🦄 **原样** <u>字</u>]]").arg(FirstId));
        QCOMPARE(entityReference(SecondId), QStringLiteral("[[%1]]").arg(SecondId));
        QCOMPARE(markdownLink(FirstId, label), reference);
        const QString prefix = QStringLiteral("前🦄 ");
        const QString body = prefix + reference + QStringLiteral(" 与 ") + entityReference(SecondId);
        const auto links = markdownLinks(body);
        QCOMPARE(links.size(), 2);
        QCOMPARE(links.at(0).start, prefix.size());
        QCOMPARE(links.at(0).length, reference.size());
        QCOMPARE(body.mid(links.at(0).start, links.at(0).length), reference);
        QCOMPARE(links.at(0).target, FirstId);
        QCOMPARE(links.at(0).label, label);
        QVERIFY(links.at(0).wiki);
        QCOMPARE(links.at(1).start, prefix.size() + reference.size() + QStringLiteral(" 与 ").size());
        QCOMPARE(links.at(1).target, SecondId);
        QVERIFY(links.at(1).label.isEmpty());
        QCOMPARE(internalTargets(body + reference), QStringList({FirstId, SecondId}));
        const auto literalBackslash = markdownLinks(QStringLiteral("[[%1|反斜杠\\q]]").arg(FirstId));
        QCOMPARE(literalBackslash.size(), 1);
        QCOMPARE(literalBackslash.first().label, QStringLiteral("反斜杠\\q"));
    }

    void aliasesRemainOneSourceLine() {
        const QString first = entityReference(FirstId, QStringLiteral("甲\r\n~~~\n乙"));
        const QString source = first + QLatin1Char(' ') + entityReference(SecondId, QStringLiteral("后续引用"));
        QVERIFY(!source.contains(QLatin1Char('\n'))); QVERIFY(!source.contains(QLatin1Char('\r')));
        const auto links = markdownLinks(source);
        QCOMPARE(links.size(), 2);
        QCOMPARE(links.first().label, QStringLiteral("甲 ~~~ 乙"));
        QCOMPARE(links.last().target, SecondId);
        QVERIFY(markdownLinks(QStringLiteral("[[%1|多行\n别名]]").arg(FirstId)).isEmpty());
    }

    void externalUnicodeAndParentheses_data() {
        QTest::addColumn<QString>("target");
        QTest::newRow("unicode-and-entity-looking-query") << QStringLiteral("https://example.com/路径/🦄(part)?x=1&copy;=2");
        QTest::newRow("nested-parentheses") << QStringLiteral("https://example.com/a(b(c))/d)e(");
        QTest::newRow("unicode-mailto") << QStringLiteral("mailto:reader@example.com?subject=计划(二)&body=内容");
    }
    void externalUnicodeAndParentheses() {
        QFETCH(QString, target);
        const QString label = QStringLiteral("[检查] | \\ **不是加粗** `不是代码` <u>字</u> &copy; 🦄");
        const QString source = markdownLink(target, label);
        QVERIFY(!source.isEmpty());
        const auto links = markdownLinks(QStringLiteral("🦄 ") + source);
        QCOMPARE(links.size(), 1);
        QCOMPARE(links.first().start, QStringLiteral("🦄 ").size());
        QCOMPARE(links.first().length, source.size());
        QCOMPARE(links.first().target, target);
        QCOMPARE(links.first().label, label);
        QVERIFY(!links.first().wiki);
        QTextDocument document;
        document.setHtml(Markdown::toHtml(source));
        QCOMPARE(document.toPlainText(), label);
        const QTextCursor anchor = document.find(label);
        QVERIFY(!anchor.isNull());
        QCOMPARE(QUrl(anchor.charFormat().anchorHref()), QUrl(target));
        QVERIFY(internalTargets(source).isEmpty());
    }

    void excludedContexts_data() {
        QTest::addColumn<QString>("source");
        QTest::addColumn<bool>("visible");
        const QString reference = entityReference(FirstId, QStringLiteral("引用"));
        QTest::newRow("inline-code") << QLatin1Char('`') + reference + QLatin1Char('`') << false;
        QTest::newRow("long-inline-code") << QStringLiteral("`` a ` ") + reference + QStringLiteral(" ``") << false;
        QTest::newRow("multiline-inline-code") << QStringLiteral("`a\n") + reference + QStringLiteral("\nb`") << false;
        QTest::newRow("unmatched-backtick") << QLatin1Char('`') + reference << true;
        QTest::newRow("backtick-fence") << QStringLiteral("```text\n") + reference + QStringLiteral("\n```\n") << false;
        QTest::newRow("tilde-fence") << QStringLiteral("~~~~\n") + reference + QStringLiteral("\n~~~\n") << false;
        QTest::newRow("quoted-marker-does-not-close-top-level-fence")
            << QStringLiteral("~~~\n> ~~~\n") + reference + QStringLiteral("\n~~~\n") << false;
        QTest::newRow("nested-quote-marker-does-not-close-quote-fence")
            << QStringLiteral("> ~~~\n> > ~~~\n> ") + reference + QStringLiteral("\n> ~~~\n") << false;
        QTest::newRow("quote-fence") << QStringLiteral("> ```\n> ") + reference + QStringLiteral("\n> ```\n") << false;
        QTest::newRow("list-fence") << QStringLiteral("- ```\n  ") + reference + QStringLiteral("\n  ```\n") << false;
        QTest::newRow("indented-code") << QStringLiteral("    ") + reference << false;
        QTest::newRow("escaped-marker") << QLatin1Char('\\') + reference << false;
        QTest::newRow("even-backslashes") << QStringLiteral("\\\\") + reference << true;
        QTest::newRow("image-alt") << QStringLiteral("![图片 ") + reference + QStringLiteral("](https://example.com/a.png)") << false;
        QTest::newRow("wiki-embed") << QLatin1Char('!') + reference << false;
        QTest::newRow("plain-reference") << reference << true;
    }
    void excludedContexts() {
        QFETCH(QString, source);
        QFETCH(bool, visible);
        QCOMPARE(markdownLinks(source).size(), visible ? 1 : 0);
        QCOMPARE(internalTargets(source), visible ? QStringList({FirstId}) : QStringList());
        if (!visible) {
            const QString html = Markdown::toHtml(source, Markdown::entityMap({targetEntity(QStringLiteral("引用目标"))}));
            QVERIFY(!html.contains(QStringLiteral("class=\"internal-link")));
        }
    }

    void mixedReferencesAndExternalLinks() {
        const QString first = entityReference(FirstId, QStringLiteral("甲"));
        const QString second = entityReference(SecondId);
        const QString external = markdownLink(QStringLiteral("https://example.com/a(b)"), QStringLiteral("外部 [文档]"));
        const QString source = first + QLatin1Char(' ') + external + QStringLiteral("\n\n```\n")
            + second + QStringLiteral("\n```\n\n") + second + QLatin1Char(' ') + first
            + QStringLiteral(" ![图片](https://example.com/image.png) \\[隐藏](https://example.com)");
        const auto links = markdownLinks(source);
        QCOMPARE(links.size(), 4);
        QCOMPARE(links.at(0).target, FirstId);
        QCOMPARE(links.at(1).target, QStringLiteral("https://example.com/a(b)"));
        QCOMPARE(links.at(2).target, SecondId);
        QCOMPARE(links.at(3).target, FirstId);
        QCOMPARE(internalTargets(source), QStringList({FirstId, SecondId}));
        for (int i = 1; i < links.size(); ++i)
            QVERIFY(links.at(i - 1).start + links.at(i - 1).length <= links.at(i).start);
        QVERIFY(markdownLinks(QStringLiteral("[[同名标题]] [[%1]]").arg(FirstId.toUpper())).isEmpty());
    }

    void literalAliasesCurrentTitlesAndMissingMaps() {
        Entity target = targetEntity(QStringLiteral("当前 <标题> & 文字"));
        const QString alias = QStringLiteral("别名 [A] | \\ **粗体** `代码` <u>字</u> &copy; 🦄");
        Entity source;
        source.body = entityReference(FirstId, alias) + QStringLiteral(" / ") + entityReference(FirstId);
        const QString original = source.body;
        auto targets = Markdown::entityMap({target});
        const QString html = Markdown::displayHtml(source, targets);
        QCOMPARE(textOf(html), alias + QStringLiteral(" / ") + target.title);
        QVERIFY(!html.contains(QStringLiteral("<strong>")));
        QVERIFY(!html.contains(QStringLiteral("<u>")));
        QVERIFY(html.contains(QStringLiteral("href=\"%1\"").arg(FirstId)));
        target.title = QStringLiteral("更名后的标题");
        targets.insert(target.id, target);
        QCOMPARE(Markdown::plainText(source, targets), alias + QStringLiteral(" / ") + target.title);
        const QString missing = Markdown::displayHtml(source);
        QVERIFY(!missing.contains(QStringLiteral("<a ")));
        QVERIFY(missing.contains(QStringLiteral("class=\"broken-link\"")));
        QCOMPARE(Markdown::plainText(source), alias + QStringLiteral(" / 关联内容不可用"));
        QVERIFY(!Markdown::plainText(source).contains(FirstId));
        QCOMPARE(source.body, original);
        QVERIFY(!Markdown::toHtml(QStringLiteral("[[更名后的标题]]"), targets).contains(QStringLiteral("internal-link")));
    }

    void escapedMarkupRemainsLiteralIncludingCode() {
        const QString source = QStringLiteral("\\<u>字\\</u> `\\<u>代码\\</u>`");
        const QString html = Markdown::toHtml(source);
        QCOMPARE(textOf(html), QStringLiteral("<u>字</u> \\<u>代码\\</u>"));
        QVERIFY(!html.contains(QStringLiteral("<u>")));
    }

    void noHtmlUnsafeLinksOrGeneratedTagSpoofing() {
        const QString alias = QStringLiteral("<script>alert(1)</script> <u>下划线</u> <mark data-color=\"yellow\">颜色</mark>");
        const QString source = QStringLiteral("kylin-reference-0- <x-wikilink data-target=\"%1\">伪造</x-wikilink> ").arg(FirstId)
            + entityReference(FirstId, alias)
            + QStringLiteral(" [脚本](javascript:alert(1)) [数据](data:text/html,test) [本地](file:///tmp/test.txt)")
            + QStringLiteral(" [安全](https://example.com) <script>bad()</script>");
        const QString html = Markdown::toHtml(source, Markdown::entityMap({targetEntity(QStringLiteral("目标"))}));
        QVERIFY(!html.contains(QStringLiteral("<script>")));
        QVERIFY(!html.contains(QStringLiteral("<x-wikilink")));
        QVERIFY(!html.contains(QStringLiteral("<u>")));
        QVERIFY(!html.contains(QStringLiteral("background-color:")));
        QVERIFY(!html.contains(QStringLiteral("href=\"javascript:")));
        QVERIFY(!html.contains(QStringLiteral("href=\"data:")));
        QVERIFY(!html.contains(QStringLiteral("href=\"file:")));
        QVERIFY(html.contains(QStringLiteral("href=\"https://example.com\"")));
        QCOMPARE(html.count(QStringLiteral("class=\"internal-link\"")), 1);
        QVERIFY(textOf(html).contains(alias));
        QVERIFY(textOf(html).contains(QStringLiteral("<x-wikilink data-target=\"%1\">伪造</x-wikilink>").arg(FirstId)));
        QCOMPARE(markdownLinks(source).size(), 2);
    }

    void previewNavigationTrashAndDisappearance() {
        const QString id = m_store->createTask(QStringLiteral("导航目标"));
        QVERIFY(!id.isEmpty());
        const QString sourceId = m_store->createNote(QStringLiteral("来源"));
        Entity source = m_store->entity(sourceId);
        source.body = entityReference(id, QStringLiteral("固定显示")) + QStringLiteral(" / ") + entityReference(id);
        QVERIFY(m_store->saveTask(source));
        const QString original = source.body;
        DocumentPreview preview;
        preview.setStore(m_store.get());
        preview.resize(540, 180);
        preview.setHtml(Markdown::toHtml(original, Markdown::entityMap(m_store->entities())));
        preview.show();
        const QTextCursor anchor = preview.document()->find(QStringLiteral("固定显示"));
        QVERIFY(!anchor.isNull());
        QCOMPARE(anchor.charFormat().anchorHref(), id);
        QCOMPARE(preview.internalLinkAt(preview.cursorRect(anchor).center()), id);
        QSignalSpy activated(&preview, &DocumentPreview::internalLinkActivated);
        QVERIFY(QMetaObject::invokeMethod(&preview, "anchorClicked", Qt::DirectConnection, Q_ARG(QUrl, QUrl(id))));
        QCOMPARE(activated.count(), 1);
        QCOMPARE(activated.takeFirst().at(0).toString(), id);
        Entity renamed = m_store->entity(id);
        renamed.title = QStringLiteral("新目标标题");
        QVERIFY(m_store->saveTask(renamed));
        preview.setHtml(Markdown::toHtml(original, Markdown::entityMap(m_store->entities())));
        QCOMPARE(preview.document()->toPlainText(), QStringLiteral("固定显示 / 新目标标题"));
        QVERIFY(m_store->trashTask(id));
        const QString trash = Markdown::toHtml(original, Markdown::entityMap(m_store->entities()));
        QVERIFY(trash.contains(QStringLiteral("trash-link")));
        QCOMPARE(textOf(trash), QStringLiteral("固定显示 回收站 / 新目标标题 回收站"));
        QVERIFY(QMetaObject::invokeMethod(&preview, "anchorClicked", Qt::DirectConnection, Q_ARG(QUrl, QUrl(id))));
        QCOMPARE(activated.count(), 1);
        activated.clear();
        QVERIFY(m_store->deletePermanently(id));
        preview.setHtml(Markdown::toHtml(original, Markdown::entityMap(m_store->entities())));
        QCOMPARE(preview.document()->toPlainText(), QStringLiteral("固定显示 / 关联内容不可用"));
        QVERIFY(!preview.toHtml().contains(QStringLiteral("href=\"%1\"").arg(id)));
        QVERIFY(QMetaObject::invokeMethod(&preview, "anchorClicked", Qt::DirectConnection, Q_ARG(QUrl, QUrl(id))));
        QCOMPARE(activated.count(), 0);
        QCOMPARE(m_store->entity(sourceId).body, original);
    }

    void matchingEntitiesAndPickerRevalidation() {
        const QString title = QStringLiteral("Shared [标题]");
        QStringList tasks = {m_store->createTask(title), m_store->createTask(title)};
        tasks.sort();
        const QString note = m_store->createNote(title);
        const QString deleted = m_store->createTask(title);
        QVERIFY(m_store->trashTask(deleted));
        QVERIFY(!m_store->createTask(QStringLiteral("其他标题")).isEmpty());
        QVERIFY(matchingEntities(nullptr, QString()).isEmpty());
        const auto matches = matchingEntities(m_store.get(), QStringLiteral("  SHARED  "));
        QCOMPARE(matches.size(), 3);
        QCOMPARE(matches.at(0).id, tasks.at(0));
        QCOMPARE(matches.at(1).id, tasks.at(1));
        QCOMPARE(matches.at(2).id, note);
        EntityPickerDialog picker(m_store.get());
        QCOMPARE(picker.windowTitle(), QStringLiteral("关联任务/笔记"));
        auto* search = picker.findChild<QLineEdit*>(QStringLiteral("EntityPickerSearch"));
        auto* list = picker.findChild<QListWidget*>(QStringLiteral("EntityPickerList"));
        auto* buttons = picker.findChild<QDialogButtonBox*>();
        QVERIFY(search && list && buttons);
        QCOMPARE(buttons->button(QDialogButtonBox::Ok)->text(), QStringLiteral("确认关联"));
        search->setText(QStringLiteral(" SHARED "));
        QCOMPARE(list->count(), 3);
        QCOMPARE(picker.selectedEntity().id, tasks.first());
        {
            QSignalBlocker blocker(m_store.get()); // Exercise the acceptance recheck, not just live refresh.
            QVERIFY(m_store->trashTask(tasks.first()));
        }
        QVERIFY(picker.selectedEntity().id.isEmpty());
        picker.accept();
        QCOMPARE(picker.result(), int(QDialog::Rejected));
        QCOMPARE(list->count(), 2);
        search->setText(QStringLiteral("没有匹配项"));
        QVERIFY(picker.selectedEntity().id.isEmpty());
        QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
    }

    void copyLinkWritesCanonicalReference() {
        QClipboard* clipboard = QGuiApplication::clipboard();
        if (!clipboard) QSKIP("The platform has no clipboard.");
        const QString id = m_store->createTask(QStringLiteral("复制 [标题] | \\ 🦄"));
        QVERIFY(!id.isEmpty());
        EntityCommands commands(m_store.get());
        int executed = 0;
        connect(&commands, &EntityCommands::commandExecuted, this, [&](EntityCommand command, const QString& target) {
            if (command == EntityCommand::CopyLink && target == id) ++executed;
        });
        QSignalSpy errors(&commands, &EntityCommands::errorOccurred);
        clipboard->setText(QStringLiteral("测试哨兵"));
        commands.execute(EntityCommand::CopyLink, id);
        QCOMPARE(clipboard->text(), entityReference(id, m_store->entity(id).title));
        QCOMPARE(executed, 1);
        QCOMPARE(errors.count(), 0);
        Entity renamed = m_store->entity(id);
        renamed.title = QStringLiteral("最新标题");
        QVERIFY(m_store->saveTask(renamed));
        commands.execute(EntityCommand::CopyLink, id);
        const QString copied = clipboard->text();
        QCOMPARE(copied, entityReference(id, renamed.title));
        const auto links = markdownLinks(copied);
        QCOMPARE(links.size(), 1);
        QCOMPARE(links.first().target, id);
        QCOMPARE(links.first().label, renamed.title);
        commands.execute(EntityCommand::CopyLink, newUuid());
        QCOMPARE(clipboard->text(), copied);
        QCOMPARE(errors.count(), 1);
    }

private:
    std::unique_ptr<QTemporaryDir> m_settingsDir;
    std::unique_ptr<QTemporaryDir> m_dir;
    std::unique_ptr<TaskStore> m_store;
    bool m_hadDataDir = false;
    QByteArray m_previousDataDir;
    QSettings::Format m_previousSettingsFormat = QSettings::NativeFormat;
};

QTEST_MAIN(EditorLinksTest)
#include "tst_editorlinks.moc"
