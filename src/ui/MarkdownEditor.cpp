#include "MarkdownEditor.h"
#include <QFrame>
#include <QGridLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QShortcut>
#include <QStackedWidget>
#include <QTabBar>
#include <QTextBlock>
#include <QTextEdit>
#include <QTextList>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>
#include "../app/TaskStore.h"
#include "AppIcons.h"
#include "AppStyle.h"
#include "EntityPicker.h"
#include "Markdown.h"

namespace kl {
MarkdownEditor::MarkdownEditor(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("MarkdownEditor"));
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(5);
    m_toolbar = new QFrame(this); auto* toolbar = m_toolbar; toolbar->setObjectName(QStringLiteral("EditorToolbar"));
    auto* grid = new QGridLayout(toolbar); grid->setContentsMargins(3, 3, 3, 3); grid->setSpacing(0);
    auto* more = new QToolButton(toolbar);
    more->setObjectName(QStringLiteral("moreFormats")); more->setText(QStringLiteral("更多格式"));
    more->setIcon(AppIcons::icon(IconId::More)); more->setIconSize(QSize(16, 16));
    more->setToolButtonStyle(Qt::ToolButtonIconOnly); more->setAutoRaise(true);
    more->setToolTip(QStringLiteral("更多格式")); more->setAccessibleName(QStringLiteral("更多格式"));
    more->setFocusPolicy(Qt::NoFocus); more->setPopupMode(QToolButton::InstantPopup);
    auto* moreMenu = new QMenu(more); moreMenu->setObjectName(QStringLiteral("MoreFormatsMenu"));
    moreMenu->setToolTipsVisible(true); more->setMenu(moreMenu);
    struct Tool { const char* label; const char* before; const char* after; int kind; const char* key; IconId icon; };
    const Tool tools[] = {{"加粗", "**", "**", 0, "Ctrl+B", IconId::Bold},
                          {"斜体", "*", "*", 1, "Ctrl+I", IconId::Italic},
                          {"删除线", "~~", "~~", 2, "Ctrl+Shift+X", IconId::Strike},
                          {"标题", "## ", "", 3, "", IconId::Heading},
                          {"列表", "- ", "", 4, "", IconId::List},
                          {"编号", "1. ", "", 5, "", IconId::OrderedList},
                          {"块引用", "> ", "", 6, "", IconId::Quote},
                          {"代码", "`", "`", 7, "", IconId::Code},
                          {"清单", "- [ ] ", "", 8, "", IconId::Checklist}};
    for (int i = 0; i < 9; ++i) {
        const Tool tool = tools[i];
        const bool primary = tool.kind <= 2;
        auto* button = new QToolButton(primary ? static_cast<QWidget*>(toolbar) : static_cast<QWidget*>(moreMenu));
        const QString label = QString::fromUtf8(tool.label);
        button->setText(label); button->setObjectName(QStringLiteral("format%1").arg(tool.kind));
        button->setIcon(AppIcons::icon(tool.icon)); button->setIconSize(QSize(16, 16));
        button->setToolButtonStyle(primary ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
        button->setAutoRaise(true);
        button->setToolTip(label + (QString::fromLatin1(tool.key).isEmpty()
            ? QString() : QStringLiteral(" (%1)").arg(QString::fromLatin1(tool.key))));
        button->setAccessibleName(AppIcons::accessibleName(tool.icon));
        button->setFocusPolicy(Qt::NoFocus);
        if (primary) {
            grid->addWidget(button, 0, tool.kind);
        } else {
            auto* menuItem = new QWidgetAction(moreMenu); menuItem->setDefaultWidget(button);
            moreMenu->addAction(menuItem); connect(button, &QToolButton::clicked, moreMenu, &QMenu::hide);
        }
        auto action = [this, tool]() {
            m_tabs->setCurrentIndex(0);
            if ((tool.kind >= 3 && tool.kind <= 6) || tool.kind == 8) applyBlock(QString::fromLatin1(tool.before), tool.kind);
            else applyInline(QString::fromLatin1(tool.before), QString::fromLatin1(tool.after), tool.kind);
        };
        connect(button, &QToolButton::clicked, this, action);
        if (*tool.key) {
            auto* key = new QShortcut(QKeySequence(QString::fromLatin1(tool.key)), this);
            key->setContext(Qt::WidgetWithChildrenShortcut); connect(key, &QShortcut::activated, this, action);
        }
    }
    auto* link = new QToolButton(toolbar); link->setText(QStringLiteral("链接")); link->setObjectName(QStringLiteral("insertLink"));
    link->setIcon(AppIcons::icon(IconId::Link)); link->setIconSize(QSize(16, 16));
    link->setToolButtonStyle(Qt::ToolButtonIconOnly); link->setAutoRaise(true);
    link->setToolTip(AppIcons::accessibleName(IconId::Link)); link->setAccessibleName(AppIcons::accessibleName(IconId::Link));
    link->setFocusPolicy(Qt::NoFocus); grid->addWidget(link, 0, 3); grid->addWidget(more, 0, 4);
    connect(link, &QToolButton::clicked, this, &MarkdownEditor::insertLink);
    m_referenceButton = new QToolButton(moreMenu); m_referenceButton->setText(QStringLiteral("实体引用"));
    m_referenceButton->setObjectName(QStringLiteral("insertEntityReference"));
    m_referenceButton->setIcon(AppIcons::icon(IconId::Link)); m_referenceButton->setIconSize(QSize(16, 16));
    m_referenceButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); m_referenceButton->setAutoRaise(true);
    m_referenceButton->setToolTip(QStringLiteral("实体引用")); m_referenceButton->setAccessibleName(QStringLiteral("实体引用"));
    m_referenceButton->setFocusPolicy(Qt::NoFocus); m_referenceButton->setEnabled(false);
    auto* referenceItem = new QWidgetAction(moreMenu); referenceItem->setDefaultWidget(m_referenceButton);
    moreMenu->addAction(referenceItem); connect(m_referenceButton, &QToolButton::clicked, moreMenu, &QMenu::hide);
    connect(m_referenceButton, &QToolButton::clicked, this, &MarkdownEditor::chooseEntityReference);
    layout->addWidget(toolbar);
    m_tabs = new QTabBar(this); m_tabs->addTab(QStringLiteral("编辑")); m_tabs->addTab(QStringLiteral("预览"));
    m_tabs->setExpanding(false); layout->addWidget(m_tabs);
    m_stack = new QStackedWidget(this); m_editor = new QTextEdit(this);
    m_editor->setObjectName(QStringLiteral("DescriptionEditor")); m_editor->setAcceptRichText(false); m_editor->setMinimumHeight(150);
    m_editor->setPlaceholderText(QStringLiteral("记录任务说明，选中文字后点击上方按钮排版。\n也可以粘贴 Markdown，并切换到“预览”。"));
    m_preview = new DocumentPreview(this); m_preview->setMinimumHeight(150);
    m_stack->addWidget(m_editor); m_stack->addWidget(m_preview); layout->addWidget(m_stack, 1);
    connect(m_preview, &DocumentPreview::internalLinkActivated, this, &MarkdownEditor::internalLinkActivated);
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int index) { refreshPreview(); m_stack->setCurrentIndex(index); });
    connect(m_editor, &QTextEdit::textChanged, this, [this]() {
        if (!m_loading) { refreshPreview(); emit contentChanged(); }
    });
}
void MarkdownEditor::setStore(TaskStore* store) {
    if (m_store == store) return;
    if (m_store) disconnect(m_store, nullptr, this, nullptr);
    m_store = store; m_preview->setStore(store); m_referenceButton->setEnabled(m_store != nullptr);
    if (m_store) {
        connect(m_store, &TaskStore::taskChanged, this, [this](QString) { refreshPreview(); });
        connect(m_store, &TaskStore::tasksReset, this, &MarkdownEditor::refreshPreview);
    }
    refreshPreview();
}
void MarkdownEditor::setContent(const QString& content) {
    m_loading = true; m_editor->setPlainText(content); m_editor->document()->setModified(false); refreshPreview(); m_loading = false;
}
QString MarkdownEditor::content() const { return m_editor->toPlainText(); }
void MarkdownEditor::setPreview(bool preview) { m_tabs->setCurrentIndex(preview ? 1 : 0); }
void MarkdownEditor::setCompact(bool compact) {
    m_toolbar->setVisible(!compact); m_tabs->setVisible(!compact); m_editor->setFrameShape(QFrame::NoFrame);
    m_editor->setContentsMargins(0, 0, 0, 0); m_editor->document()->setDocumentMargin(0);
    m_preview->setContentsMargins(0, 0, 0, 0); m_preview->document()->setDocumentMargin(0);
}
void MarkdownEditor::refreshPreview() {
    m_preview->setHtml(Markdown::toHtml(m_editor->toPlainText(), m_store ? Markdown::entityMap(m_store->entities())
                                                                        : Markdown::EntityMap()));
}
void MarkdownEditor::applyInline(const QString& before, const QString& after, int kind) {
    QTextCursor cursor = m_editor->textCursor(); Q_UNUSED(kind);
    const QString selected = cursor.hasSelection() ? cursor.selectedText() : QStringLiteral("文字");
    const int start = cursor.selectionStart(); cursor.beginEditBlock(); cursor.insertText(before + selected + after); cursor.endEditBlock();
    cursor.setPosition(start + before.size()); cursor.setPosition(start + before.size() + selected.size(), QTextCursor::KeepAnchor);
    m_editor->setTextCursor(cursor); m_editor->setFocus();
}
void MarkdownEditor::applyBlock(const QString& prefix, int kind) {
    QTextCursor cursor = m_editor->textCursor(); cursor.beginEditBlock(); Q_UNUSED(kind);
    const int end = cursor.selectionEnd(); int inserted = 0; cursor.setPosition(cursor.selectionStart());
    cursor.movePosition(QTextCursor::StartOfBlock);
    do {
        cursor.insertText(prefix); inserted += prefix.size();
        if (!cursor.movePosition(QTextCursor::NextBlock)) break;
    } while (cursor.position() <= end + inserted);
    cursor.endEditBlock(); m_editor->setTextCursor(cursor); m_editor->setFocus();
}
void MarkdownEditor::insertLink() {
    m_tabs->setCurrentIndex(0); bool ok = false;
    const QString link = QInputDialog::getText(this, QStringLiteral("插入链接"), QStringLiteral("地址（http、https 或 mailto）："),
                                               QLineEdit::Normal, QStringLiteral("https://"), &ok);
    if (!ok || link.trimmed().isEmpty()) return;
    const QUrl url(link.trimmed());
    if (!QStringList({QStringLiteral("https"), QStringLiteral("http"), QStringLiteral("mailto")}).contains(url.scheme().toLower())) return;
    QTextCursor cursor = m_editor->textCursor(); QString label = cursor.selectedText(); if (label.isEmpty()) label = QStringLiteral("链接文字");
    cursor.insertText(QStringLiteral("[%1](%2)").arg(label, url.toString())); m_editor->setTextCursor(cursor); m_editor->setFocus();
}
void MarkdownEditor::chooseEntityReference() {
    if (!m_store) return;
    m_tabs->setCurrentIndex(0); EntityPickerDialog dialog(m_store, this);
    if (dialog.exec() == QDialog::Accepted) insertEntityReference(dialog.selectedEntity().id);
}
bool MarkdownEditor::insertEntityReference(const QString& entityId) {
    if (!m_store) return false;
    const Entity target = m_store->entity(entityId); if (target.id.isEmpty()) return false;
    m_tabs->setCurrentIndex(0); QString label = target.title;
    label.replace(QLatin1Char('\\'), QStringLiteral("\\\\")); label.replace(QLatin1Char('['), QStringLiteral("\\["));
    label.replace(QLatin1Char(']'), QStringLiteral("\\]"));
    QTextCursor cursor = m_editor->textCursor(); cursor.insertText(QStringLiteral("[%1](kl:%2)").arg(label, target.id));
    m_editor->setTextCursor(cursor); m_editor->setFocus(); return true;
}
} // namespace kl
