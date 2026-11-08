#ifndef KYLINLIST_MARKDOWNEDITOR_H
#define KYLINLIST_MARKDOWNEDITOR_H

#include <QPointer>
#include <QWidget>
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
    explicit MarkdownEditor(QWidget* parent = nullptr);
    void setStore(TaskStore* store);
    void setContent(const QString& content);
    QString content() const;
    QTextEdit* textEditor() const { return m_editor; }
    DocumentPreview* preview() const { return m_preview; }
    void setPreview(bool preview);
    void setCompact(bool compact);
    bool insertEntityReference(const QString& entityId);
signals:
    void contentChanged();
    void internalLinkActivated(QString entityId);
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
    QToolButton* m_referenceButton = nullptr;
    QPointer<TaskStore> m_store;
    bool m_loading = false;
};
}  // namespace kl

#endif // KYLINLIST_MARKDOWNEDITOR_H
