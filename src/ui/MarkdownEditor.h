#ifndef KYLINLIST_MARKDOWNEDITOR_H
#define KYLINLIST_MARKDOWNEDITOR_H

#include <QHash>
#include <QPair>
#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <functional>
#include "AppIcons.h"
#include "../domain/Entity.h"
class QTextEdit;
class QStackedWidget;
class QMenu;
class QTextBlockFormat;
namespace kl {
class DocumentPreview;
class TaskStore;
class EditorCompletion;
class LinkTools;
class LinkEditorPopup;
class MarkdownEditor : public QWidget {
    Q_OBJECT
public:
    enum class FormatAction { Bold, Italic, Strike, Heading, List, OrderedList, Quote, Code, Link, EntityReference,
                              Underline, Highlight, ClearFormat, HorizontalRule, Count };
    enum class BlockStyle { Paragraph, Heading1, Heading2, Heading3, UnorderedList, OrderedList,
                            TaskList, Quote, HorizontalRule };
    explicit MarkdownEditor(QWidget* parent = nullptr);
    ~MarkdownEditor() override;
    void setStore(TaskStore* store);
    void setEntityId(const QString& entityId);
    void setContent(const QString& content);
    void applyLiveContent(const QString& content);
    void setSharedHistory(std::function<void(bool)> requested, std::function<bool(bool)> available);
    void applyFontPreference(const QString& family);
    QString content() const;
    // Rendered body text with formatting markers removed; whitespace is not trimmed.
    QString plainTextContent() const;
    QTextEdit* textEditor() const { return m_editor; }
    DocumentPreview* preview() const { return m_preview; }
    void setPreview(bool preview);
    void setCompact(bool compact);
    // Flow mode delegates scrolling to the host; contentHeight() always measures natural content.
    void setFlowLayout(bool enabled);
    bool flowLayout() const { return m_flowLayout; }
    void setExpandWhenEmpty(bool enabled);
    bool expandWhenEmpty() const { return m_expandWhenEmpty; }
    void setFillAvailableHeight(bool enabled);
    bool fillAvailableHeight() const { return m_fillAvailableHeight; }
    void setBlockControlsEnabled(bool enabled);
    bool blockControlsEnabled() const { return m_blockControlsEnabled; }
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
    void applyBlockStyle(BlockStyle style);
    QString blockStyleLabel(BlockStyle style) const;
    IconId blockStyleIcon(BlockStyle style) const;
    QVector<BlockStyle> blockStyles() const;
    // Source offsets stay independent of the rich text document's hidden markers.
    QPair<int, int> sourceSelection() const;
    QString selectedSource() const;
    void selectSource(int anchor, int position);
    quint64 contextGeneration() const { return m_contextGeneration; }
    bool ownsPopup(const QWidget* popup) const;
    bool isComposing() const { return m_preeditActive; }
signals:
    void contentChanged();
    void editFinished(bool continuation);
    void internalLinkActivated(QString entityId);
    void selectionChanged(bool hasSelection);
    void contentHeightChanged(int height);
    void highlightColorChanged(QString color);
    void attachmentRequested();
    void expandRequested();
    void tagSelected(QString tag);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
private:
    struct LinkContext {
        int start = -1;
        int length = 0;
        QString source;
        QString target;
        QString label;
        quint64 generation = 0;
    };
    void updateContentHeight();
    void clearFormatting();
    void applyInline(const QString& before, const QString& after, int formatKind);
    void refreshPreview();
    void refreshTargets();
    void insertLink();
    void beginReference();
    void beginTag();
    void renderDocument();
    void updateLineStates(bool force = false);
    void scheduleLineUpdate();
    void sourceEditFinished();
    void continueParagraph(const QTextBlockFormat& previous);
    void synchronizeSource();
    void replaceSource(int start, int length, const QString& replacement, bool preserveSelection = false);
    int sourceOffsetForPosition(int position, bool selectionEnd = false) const;
    int documentPositionForSource(int offset) const;
    bool completionAllowedAt(int offset) const;
    bool blockInsertionAllowed(int block) const;
    void showBlockMenu(int block, const QRect& globalAnchor);
    void scheduleHints();
    void updateHints();
    void closeHints(bool dismissed = false);
    void acceptHint(const QString& value, bool create);
    void updateLinkHover(const QPoint& viewportPosition);
    void openLinkEditor(const LinkContext& link);
    void applyLinkEdit(const QString& label, const QString& target, bool remove);
    void closePopups();
    QRect editorPopupBounds() const;
    QRect sourceAnchorRect(int offset) const;
    LinkContext linkAt(const QPoint& viewportPosition) const;
    bool linkContextValid(const LinkContext& context) const;
    QTextEdit* m_editor = nullptr;
    DocumentPreview* m_preview = nullptr;
    QStackedWidget* m_stack = nullptr;
    QPointer<QMenu> m_blockMenu;
    EditorCompletion* m_completion = nullptr;
    QPointer<LinkTools> m_linkTools;
    QPointer<LinkEditorPopup> m_linkEditor;
    QPointer<TaskStore> m_store;
    QHash<QString, Entity> m_targets;
    QString m_entityId;
    QStringList m_sourceLines;
    QString m_highlightColor = QStringLiteral("yellow");
    LinkContext m_hoverLink;
    LinkContext m_editedLink;
    LinkContext m_pressedLink;
    QPoint m_linkPressPosition;
    quint64 m_contextGeneration = 1;
    int m_hintMode = 0;
    int m_hintStart = -1;
    int m_hintEnd = -1;
    QString m_hintText;
    QString m_dismissedHint;
    int m_dismissedHintStart = -1;
    int m_pressedCheckLine = -1;
    int m_contentHeight = 0;
    int m_scrollMinimumHeight = 0;
    int m_scrollMaximumHeight = QWIDGETSIZE_MAX;
    qreal m_scrollDocumentMargin = 0;
    QMargins m_scrollContentsMargins;
    QSizePolicy m_scrollSizePolicy;
    bool m_flowLayout = false;
    bool m_expandWhenEmpty = false;
    bool m_fillAvailableHeight = false;
    bool m_blockControlsEnabled = false;
    bool m_sizing = false;
    bool m_loading = false;
    bool m_rendering = false;
    bool m_editingSource = false;
    bool m_lineUpdatePending = false;
    bool m_hintUpdatePending = false;
    bool m_preeditActive = false;
    bool m_acceptingHint = false;
};
}  // namespace kl

#endif // KYLINLIST_MARKDOWNEDITOR_H
