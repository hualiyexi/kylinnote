#ifndef KYLINLIST_MARKDOWNEDITOR_H
#define KYLINLIST_MARKDOWNEDITOR_H

#include <QPointer>
#include <QStringList>
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
    enum class FormatAction { Bold, Italic, Strike, Heading, List, OrderedList, Quote, Code, Checklist, Link, EntityReference,
                              Underline, Highlight, ClearFormat, Count };
    explicit MarkdownEditor(QWidget* parent = nullptr);
    void setStore(TaskStore* store);
    void setContent(const QString& content);
    QString content() const;
    // Rendered body text with formatting markers removed; whitespace is not trimmed.
    QString plainTextContent() const;
    QTextEdit* textEditor() const { return m_editor; }
    DocumentPreview* preview() const { return m_preview; }
    void setPreview(bool preview);
    void setCompact(bool compact);
    // Opt-in for an outer QScrollArea: grow to document height and use no inner scrollbars.
    // Listen to contentHeightChanged(int) when the surrounding layout needs notification.
    // false restores the desktop NoteWidget's ordinary expanding, scrollable editor.
    void setFlowLayout(bool enabled);
    bool flowLayout() const { return m_flowLayout; }
    int contentHeight() const { return m_contentHeight; }
    void setHighlightColor(const QString& color);
    QString highlightColor() const { return m_highlightColor; }
    void applyHighlight(const QString& color);
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
    void contentHeightChanged(int height);
    void highlightColorChanged(QString color);
    void checklistRequested();
    void attachmentRequested();
    void expandRequested();
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
private:
    void updateContentHeight();
    void clearFormatting();
    void applyInline(const QString& before, const QString& after, int formatKind);
    void applyBlock(const QString& prefix, int kind);
    void refreshPreview();
    void insertLink();
    void chooseEntityReference();
    void renderDocument();
    void renderLine(int line, bool source);
    void updateLineStates();
    void revealLines(int first, int last);
    void revealSelection();
    void synchronizeSource();
    QTextEdit* m_editor = nullptr;
    DocumentPreview* m_preview = nullptr;
    QStackedWidget* m_stack = nullptr;
    QTabBar* m_tabs = nullptr;
    QFrame* m_toolbar = nullptr;
    QWidget* m_actionHost = nullptr;
    QToolButton* m_referenceButton = nullptr;
    QPointer<TaskStore> m_store;
    QStringList m_sourceLines;
    QString m_highlightColor = QStringLiteral("yellow");
    int m_editingLine = -1;
    int m_contentHeight = 0;
    int m_scrollMinimumHeight = 0;
    int m_scrollMaximumHeight = QWIDGETSIZE_MAX;
    qreal m_scrollDocumentMargin = 0;
    QMargins m_scrollContentsMargins;
    QSizePolicy m_scrollSizePolicy;
    bool m_flowLayout = false;
    bool m_sizing = false;
    bool m_loading = false;
    bool m_rendering = false;
    bool m_lineUpdatePending = false;
};
}  // namespace kl

#endif // KYLINLIST_MARKDOWNEDITOR_H
