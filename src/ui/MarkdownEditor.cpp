#include "MarkdownEditor.h"
#include <QFrame>
#include <QGridLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QShortcut>
#include <QStackedWidget>
#include <QTabBar>
#include <QTextEdit>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include "../app/TaskStore.h"
#include "AppIcons.h"
#include "AppStyle.h"
#include "EntityPicker.h"
#include "Markdown.h"

namespace kl {
namespace {
struct FormatSpec {
    const char* label;
    const char* before;
    const char* after;
    int kind;
    const char* key;
    IconId icon;
};
// 顺序必须与 MarkdownEditor::FormatAction 一致。
const FormatSpec& formatSpec(MarkdownEditor::FormatAction action) {
    static const FormatSpec specs[] = {
        {"加粗", "**", "**", 0, "Ctrl+B", IconId::Bold},
        {"斜体", "*", "*", 1, "Ctrl+I", IconId::Italic},
        {"删除线", "~~", "~~", 2, "Ctrl+Shift+X", IconId::Strike},
        {"标题", "## ", "", 3, "", IconId::Heading},
        {"列表", "- ", "", 4, "", IconId::List},
        {"编号", "1. ", "", 5, "", IconId::OrderedList},
        {"块引用", "> ", "", 6, "", IconId::Quote},
        {"代码", "`", "`", 7, "", IconId::Code},
        {"清单", "- [ ] ", "", 8, "", IconId::Checklist},
        {"链接", "", "", 9, "", IconId::Link},
        {"实体引用", "", "", 10, "", IconId::Link},
    };
    const int index = static_cast<int>(action);
    if (index < 0 || index >= static_cast<int>(MarkdownEditor::FormatAction::Count)) return specs[0];
    return specs[index];
}
}  // namespace

MarkdownEditor::MarkdownEditor(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("MarkdownEditor"));
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(0);

    // 正文区保持干净：常驻工具条与编辑/预览 tab 不再占位。
    // 格式按钮寄存在屏幕外的宿主里，仍保留 objectName 与真实几何，
    // 因此兼容测试里的 QTest::mouseClick 依旧有效，外部面板也可复用动作。
    m_actionHost = new QWidget(this);
    m_actionHost->setObjectName(QStringLiteral("FormatActionHost"));
    m_actionHost->move(-10000, -10000);
    m_actionHost->resize(11 * 24, 24);

    // 保留 QFrame#EditorToolbar 与 moreFormats 结构（QSS 选择器与对象名兼容），但隐藏、不入布局。
    m_toolbar = new QFrame(this);
    m_toolbar->setObjectName(QStringLiteral("EditorToolbar"));
    auto* grid = new QGridLayout(m_toolbar); grid->setContentsMargins(0, 0, 0, 0); grid->setSpacing(0);
    auto* more = new QToolButton(m_toolbar);
    more->setObjectName(QStringLiteral("moreFormats")); more->setText(QStringLiteral("更多格式"));
    more->setIcon(AppIcons::icon(IconId::More)); more->setIconSize(QSize(16, 16));
    more->setToolButtonStyle(Qt::ToolButtonIconOnly); more->setAutoRaise(true);
    more->setToolTip(QStringLiteral("更多格式")); more->setAccessibleName(QStringLiteral("更多格式"));
    more->setFocusPolicy(Qt::NoFocus); more->setPopupMode(QToolButton::InstantPopup);
    auto* moreMenu = new QMenu(more); moreMenu->setObjectName(QStringLiteral("MoreFormatsMenu"));
    moreMenu->setToolTipsVisible(true); more->setMenu(moreMenu);
    grid->addWidget(more, 0, 0);

    // 9 个格式动作：objectName 仍为 format0..8，行为统一走 applyFormat()。
    for (int i = 0; i <= static_cast<int>(FormatAction::Checklist); ++i) {
        const FormatAction action = static_cast<FormatAction>(i);
        const FormatSpec& spec = formatSpec(action);
        const QString label = QString::fromUtf8(spec.label);
        const QString key = QString::fromLatin1(spec.key);
        auto* button = new QToolButton(m_actionHost);
        button->setObjectName(QStringLiteral("format%1").arg(i));
        button->setGeometry(i * 24, 0, 24, 24);
        button->setText(label);
        button->setIcon(AppIcons::icon(spec.icon)); button->setIconSize(QSize(16, 16));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly); button->setAutoRaise(true);
        button->setToolTip(key.isEmpty() ? label : QStringLiteral("%1 (%2)").arg(label, key));
        button->setAccessibleName(AppIcons::accessibleName(spec.icon));
        button->setFocusPolicy(Qt::NoFocus);
        connect(button, &QToolButton::clicked, this, [this, action]() { applyFormat(action); });
        if (!key.isEmpty()) {
            auto* shortcut = new QShortcut(QKeySequence(key), this);
            shortcut->setContext(Qt::WidgetWithChildrenShortcut);
            connect(shortcut, &QShortcut::activated, this, [this, action]() { applyFormat(action); });
        }
    }
    auto* link = new QToolButton(m_actionHost);
    link->setObjectName(QStringLiteral("insertLink")); link->setText(QStringLiteral("链接"));
    link->setGeometry(9 * 24, 0, 24, 24);
    link->setIcon(AppIcons::icon(IconId::Link)); link->setIconSize(QSize(16, 16));
    link->setToolButtonStyle(Qt::ToolButtonIconOnly); link->setAutoRaise(true);
    link->setToolTip(AppIcons::accessibleName(IconId::Link)); link->setAccessibleName(AppIcons::accessibleName(IconId::Link));
    link->setFocusPolicy(Qt::NoFocus);
    connect(link, &QToolButton::clicked, this, [this]() { applyFormat(FormatAction::Link); });

    m_referenceButton = new QToolButton(m_actionHost);
    m_referenceButton->setObjectName(QStringLiteral("insertEntityReference"));
    m_referenceButton->setText(QStringLiteral("实体引用"));
    m_referenceButton->setGeometry(10 * 24, 0, 24, 24);
    m_referenceButton->setIcon(AppIcons::icon(IconId::Link)); m_referenceButton->setIconSize(QSize(16, 16));
    m_referenceButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); m_referenceButton->setAutoRaise(true);
    m_referenceButton->setToolTip(QStringLiteral("实体引用")); m_referenceButton->setAccessibleName(QStringLiteral("实体引用"));
    m_referenceButton->setFocusPolicy(Qt::NoFocus); m_referenceButton->setEnabled(false);
    connect(m_referenceButton, &QToolButton::clicked, this, [this]() { applyFormat(FormatAction::EntityReference); });

    m_actionHost->show();
    m_toolbar->hide();

    // 编辑/预览切换能力保留：tab 隐藏但 currentChanged 仍驱动 stack。
    m_tabs = new QTabBar(this); m_tabs->addTab(QStringLiteral("编辑")); m_tabs->addTab(QStringLiteral("预览"));
    m_tabs->setExpanding(false); m_tabs->hide();
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
    connect(m_editor, &QTextEdit::selectionChanged, this, [this]() {
        emit selectionChanged(m_editor->textCursor().hasSelection());
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
    Q_UNUSED(compact);
    m_toolbar->hide(); m_tabs->hide();
    m_editor->setFrameShape(QFrame::NoFrame);
    m_editor->setContentsMargins(0, 0, 0, 0); m_editor->document()->setDocumentMargin(0);
    m_preview->setContentsMargins(0, 0, 0, 0); m_preview->document()->setDocumentMargin(0);
}
void MarkdownEditor::refreshPreview() {
    m_preview->setHtml(Markdown::toHtml(m_editor->toPlainText(), m_store ? Markdown::entityMap(m_store->entities())
                                                                        : Markdown::EntityMap()));
}

void MarkdownEditor::applyFormat(FormatAction action) {
    m_tabs->setCurrentIndex(0);
    if (action == FormatAction::Link) { insertLink(); return; }
    if (action == FormatAction::EntityReference) { chooseEntityReference(); return; }
    const FormatSpec& spec = formatSpec(action);
    if ((spec.kind >= 3 && spec.kind <= 6) || spec.kind == 8)
        applyBlock(QString::fromLatin1(spec.before), spec.kind);
    else
        applyInline(QString::fromLatin1(spec.before), QString::fromLatin1(spec.after), spec.kind);
}
QString MarkdownEditor::formatLabel(FormatAction action) const {
    return QString::fromUtf8(formatSpec(action).label);
}
IconId MarkdownEditor::formatIcon(FormatAction action) const {
    return formatSpec(action).icon;
}
QString MarkdownEditor::formatShortcut(FormatAction action) const {
    return QString::fromLatin1(formatSpec(action).key);
}
QVector<MarkdownEditor::FormatAction> MarkdownEditor::primaryFormats() const {
    return {FormatAction::Bold, FormatAction::Italic, FormatAction::Strike, FormatAction::Link};
}
QVector<MarkdownEditor::FormatAction> MarkdownEditor::allFormats() const {
    QVector<FormatAction> actions;
    for (int i = 0; i < static_cast<int>(FormatAction::Count); ++i)
        actions.append(static_cast<FormatAction>(i));
    return actions;
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
