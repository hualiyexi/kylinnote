#include "MarkdownEditor.h"
#include <QFrame>
#include <QGridLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QShortcut>
#include <QStackedWidget>
#include <QTabBar>
#include <QTextBlock>
#include <QTextEdit>
#include <QTextList>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include "AppStyle.h"
#include "Markdown.h"

namespace kn {
MarkdownEditor::MarkdownEditor(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("MarkdownEditor"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    m_toolbar = new QFrame(this);
    auto* toolbar = m_toolbar;
    toolbar->setObjectName(QStringLiteral("EditorToolbar"));
    auto* grid = new QGridLayout(toolbar);
    grid->setContentsMargins(3, 3, 3, 3);
    grid->setSpacing(0);
    struct Tool {
        const char* label;
        const char* before;
        const char* after;
        int kind;
        const char* key;
    };
    const Tool tools[] = {{"加粗", "**", "**", 0, "Ctrl+B"},
                          {"斜体", "*", "*", 1, "Ctrl+I"},
                          {"删除线", "~~", "~~", 2, "Ctrl+Shift+X"},
                          {"标题", "## ", "", 3, ""},
                          {"列表", "- ", "", 4, ""},
                          {"编号", "1. ", "", 5, ""},
                          {"引用", "> ", "", 6, ""},
                          {"代码", "`", "`", 7, ""},
                          {"清单", "- [ ] ", "", 8, ""}};
    for (int i = 0; i < 9; ++i) {
        const Tool tool = tools[i];
        auto* button = new QToolButton(toolbar);
        button->setText(QString::fromUtf8(tool.label));
        button->setObjectName(QStringLiteral("format%1").arg(tool.kind));
        button->setToolTip(QString::fromUtf8(tool.label) +
                           (QString::fromLatin1(tool.key).isEmpty()
                                ? QString()
                                : QStringLiteral(" (%1)").arg(QString::fromLatin1(tool.key))));
        button->setFocusPolicy(Qt::NoFocus);
        grid->addWidget(button, i / 5, i % 5);
        auto action = [this, tool]() {
            m_tabs->setCurrentIndex(0);
            if (tool.kind >= 3 && tool.kind <= 6 || tool.kind == 8)
                applyBlock(QString::fromLatin1(tool.before), tool.kind);
            else
                applyInline(QString::fromLatin1(tool.before), QString::fromLatin1(tool.after), tool.kind);
        };
        connect(button, &QToolButton::clicked, this, action);
        if (*tool.key) {
            auto* key = new QShortcut(QKeySequence(QString::fromLatin1(tool.key)), this);
            key->setContext(Qt::WidgetWithChildrenShortcut);
            connect(key, &QShortcut::activated, this, action);
        }
    }
    auto* link = new QToolButton(toolbar);
    link->setText(QStringLiteral("链接"));
    link->setFocusPolicy(Qt::NoFocus);
    grid->addWidget(link, 1, 4);
    connect(link, &QToolButton::clicked, this, &MarkdownEditor::insertLink);
    layout->addWidget(toolbar);
    m_tabs = new QTabBar(this);
    m_tabs->addTab(QStringLiteral("编辑"));
    m_tabs->addTab(QStringLiteral("预览"));
    m_tabs->setExpanding(false);
    layout->addWidget(m_tabs);
    m_stack = new QStackedWidget(this);
    m_editor = new QTextEdit(this);
    m_editor->setObjectName(QStringLiteral("DescriptionEditor"));
    m_editor->setAcceptRichText(false);
    m_editor->setMinimumHeight(150);
    m_editor->setPlaceholderText(
        QStringLiteral("记录任务说明，选中文字后点击上方按钮排版。\n也可以粘贴 Markdown，并切换到“预览”。"));
    m_preview = new DocumentPreview(this);
    m_preview->setMinimumHeight(150);
    m_stack->addWidget(m_editor);
    m_stack->addWidget(m_preview);
    layout->addWidget(m_stack, 1);
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) {
        refreshPreview();
        m_stack->setCurrentIndex(index);
    });
    connect(m_editor, &QTextEdit::textChanged, this, [this]() {
        if (!m_loading) {
            refreshPreview();
            emit contentChanged();
        }
    });
}
void MarkdownEditor::setContent(const QString& content, ContentFormat format) {
    m_loading = true;
    m_format = ContentFormat::Markdown;
    QString source = content;
    if (format == ContentFormat::Html) {
        QTextDocument legacy;
        legacy.setHtml(content);
        source = legacy.toPlainText();
    }
    m_editor->setPlainText(source);
    m_editor->document()->setModified(false);
    refreshPreview();
    m_loading = false;
}
QString MarkdownEditor::content() const {
    return m_editor->toPlainText();
}
void MarkdownEditor::setPreview(bool preview) {
    m_tabs->setCurrentIndex(preview ? 1 : 0);
}
void MarkdownEditor::setCompact(bool compact) {
    m_toolbar->setVisible(!compact);
    m_tabs->setVisible(!compact);
    m_editor->setFrameShape(QFrame::NoFrame);
    m_editor->setContentsMargins(0, 0, 0, 0);
    m_editor->document()->setDocumentMargin(0);
    m_preview->setContentsMargins(0, 0, 0, 0);
    m_preview->document()->setDocumentMargin(0);
}
void MarkdownEditor::refreshPreview() {
    m_preview->setHtml(Markdown::toHtml(m_editor->toPlainText()));
}
void MarkdownEditor::applyInline(const QString& before, const QString& after, int kind) {
    QTextCursor cursor = m_editor->textCursor();
    Q_UNUSED(kind);
    const QString selected = cursor.hasSelection() ? cursor.selectedText() : QStringLiteral("文字");
    const int start = cursor.selectionStart();
    cursor.beginEditBlock();
    cursor.insertText(before + selected + after);
    cursor.endEditBlock();
    cursor.setPosition(start + before.size());
    cursor.setPosition(start + before.size() + selected.size(), QTextCursor::KeepAnchor);
    m_editor->setTextCursor(cursor);
    m_editor->setFocus();
}
void MarkdownEditor::applyBlock(const QString& prefix, int kind) {
    QTextCursor cursor = m_editor->textCursor();
    cursor.beginEditBlock();
    Q_UNUSED(kind);
    const int end = cursor.selectionEnd();
    int inserted = 0;
    cursor.setPosition(cursor.selectionStart());
    cursor.movePosition(QTextCursor::StartOfBlock);
    do {
        cursor.insertText(prefix);
        inserted += prefix.size();
        if (!cursor.movePosition(QTextCursor::NextBlock))
            break;
    } while (cursor.position() <= end + inserted);
    cursor.endEditBlock();
    m_editor->setTextCursor(cursor);
    m_editor->setFocus();
}
void MarkdownEditor::insertLink() {
    m_tabs->setCurrentIndex(0);
    bool ok = false;
    const QString link = QInputDialog::getText(this,
                                               QStringLiteral("插入链接"),
                                               QStringLiteral("地址（http、https 或 mailto）："),
                                               QLineEdit::Normal,
                                               QStringLiteral("https://"),
                                               &ok);
    if (!ok || link.trimmed().isEmpty())
        return;
    const QUrl url(link.trimmed());
    if (!QStringList({QStringLiteral("https"), QStringLiteral("http"), QStringLiteral("mailto")})
             .contains(url.scheme().toLower()))
        return;
    QTextCursor cursor = m_editor->textCursor();
    QString label = cursor.selectedText();
    if (label.isEmpty())
        label = QStringLiteral("链接文字");
    cursor.insertText(QStringLiteral("[%1](%2)").arg(label, url.toString()));
    m_editor->setTextCursor(cursor);
    m_editor->setFocus();
}
}  // namespace kn
