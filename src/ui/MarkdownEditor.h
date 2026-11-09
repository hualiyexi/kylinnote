#ifndef KYLINLIST_MARKDOWNEDITOR_H
#define KYLINLIST_MARKDOWNEDITOR_H

#include <QPointer>
#include <QVector>
#include <QWidget>
#include "AppIcons.h"
#include "../domain/Entity.h"
class QTextEdit;
class QStackedWidget;
class QTabBar;
class QFrame;
class QToolButton;
namespace kl {
class DocumentPreview;
class TaskStore;
class MarkdownEditor : public QWidget {
    Q_OBJECT
public:
    // 单一格式动作枚举：供悬浮格式面板、选中文字气泡条复用；Count 为哨兵值。
    enum class FormatAction { Bold, Italic, Strike, Heading, List, OrderedList, Quote, Code, Checklist, Link, EntityReference, Count };
    explicit MarkdownEditor(QWidget* parent = nullptr);
    void setStore(TaskStore* store);
    void setContent(const QString& content);
    QString content() const;
    QTextEdit* textEditor() const { return m_editor; }
    DocumentPreview* preview() const { return m_preview; }
    void setPreview(bool preview);
    void setCompact(bool compact);
    bool insertEntityReference(const QString& entityId);
    void applyFormat(FormatAction action);
    QString formatLabel(FormatAction action) const;
    IconId formatIcon(FormatAction action) const;
    QString formatShortcut(FormatAction action) const;
    QVector<FormatAction> primaryFormats() const;
    QVector<FormatAction> allFormats() const;
signals:
    void contentChanged();
    void internalLinkActivated(QString entityId);
    void selectionChanged(bool hasSelection);
private:
    void applyInline(const QString& before, const QString& after, int formatKind);
    void applyBlock(const QString& prefix, int kind);
    void refreshPreview();
    void insertLink();
    void chooseEntityReference();
    QTextEdit* m_editor = nullptr;
    DocumentPreview* m_preview = nullptr;
    QStackedWidget* m_stack = nullptr;
    QTabBar* m_tabs = nullptr;
    QFrame* m_toolbar = nullptr;
    QWidget* m_actionHost = nullptr;
    QToolButton* m_referenceButton = nullptr;
    QPointer<TaskStore> m_store;
    bool m_loading = false;
};
}  // namespace kl

#endif // KYLINLIST_MARKDOWNEDITOR_H
