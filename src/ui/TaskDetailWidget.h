#ifndef KYLINLIST_TASKDETAILWIDGET_H
#define KYLINLIST_TASKDETAILWIDGET_H

#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QWidget>
#include <QVector>
#include "../domain/SourceChange.h"

class QCheckBox;
class QFrame;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QToolButton;

namespace kl {
class ActivityTimelineWidget;
class AttachmentWidget;
class EntityCommands;
class EntityTextSession;
class EntityInfoPopup;
class FormatPanel;
class ListSelectPopup;
class MarkdownEditor;
class SelectionBubble;
class TaskDateController;
class TaskStore;

class TaskDetailWidget : public QWidget {
    Q_OBJECT
public:
    explicit TaskDetailWidget(TaskStore* store, QWidget* parent = nullptr);
    ~TaskDetailWidget() override;
    bool setTask(QString id);
    QString entityId() const { return m_id; }
    bool flush();
    bool finishEditing();
    bool canDismissWithEscape() const;
    void focusTitle();
    void openDatePicker();
    void setOverlayMode(bool overlay);
    void setDesktopMode(bool desktop);
    void setDesktopHovered(bool hovered);
    bool hasPendingText() const;
    bool isSavingText() const;
    bool textRejected() const;
    QString titleDraft() const;
    QString bodyDraft() const;
    bool showActivity();
    void importAttachments();
signals:
    void taskRemoved(QString id);
    void internalLinkRequested(QString entityId);
    void relatedDetailRequested(QString entityId);
    void targetUnavailable(QString entityId);
    void focusModeRequested(bool enabled);
    void closeRequested();
    void desktopMoreRequested();
    void draftStateChanged();
private:
    void buildUi();
    bool loadFields(bool resetEditor = false);
    void refreshFromStore();
    void refreshGroups();
    void dirty(const QString& field);
    void bindTextSession();
    void refreshLiveText(QObject* origin = nullptr);
    bool publishBody(bool continuation = false, bool committed = false);
    void refreshTextState();
    void updateEnabled();
    void updateDetailLayout();
    void scheduleDetailLayout();
    void attachSelectedTag(const QString& tag);
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool handleFileDrop(QObject* watched, QEvent* event);
    void setDropHintVisible(bool visible);
    void beginTextSession();
    void scheduleTextSessionEnd();
    bool endTextSession();
    bool cancelTextSession();
    void reloadDraft();
    void copyDraft();
    void showMoreMenu();
    void showFormatPanel();
    void showGroupMenu();
    void showInfoPopup();
    void showPriorityMenu();
    void updateDateChip(bool refreshIcon = true);
    void updateDateChipLayout();
    void updateParentLink();
    void refreshIcons();
    void setStatus(const QString& text, bool error = false, bool recoverable = false);

    QPointer<TaskStore> m_store;
    QPointer<EntityTextSession> m_textSession;
    QString m_id;

    bool m_loading = false, m_saving = false;
    QSet<QString> m_dirty;
    QString m_liveBody, m_lastSessionBody;
    QVector<SourceChange> m_pendingBodyChanges;
    bool m_bodyPublishPending = false, m_bodyConflict = false;
    bool m_sessionEndPending = false;
    bool m_remoteRefreshPending = false;
    QWidget* m_detailHost = nullptr;
    QWidget* m_form = nullptr;
    QScrollArea* m_scroll = nullptr;
    QWidget *m_statusHost = nullptr, *m_activityHost = nullptr, *m_attachmentPane = nullptr;
    QScrollArea* m_attachmentScroll = nullptr;
    QFrame* m_attachmentDivider = nullptr;
    QLabel *m_attachmentTitle = nullptr, *m_attachmentCount = nullptr;
    QToolButton* m_attachmentAdd = nullptr;
    bool m_layoutPending = false;
    QLabel *m_empty = nullptr, *m_status = nullptr, *m_dropHint = nullptr;
    QWidget* m_recoveryActions = nullptr;
    QToolButton *m_reloadDraft = nullptr, *m_copyDraft = nullptr;
    QLineEdit* m_title = nullptr;
    QFrame* m_header = nullptr;
    QHBoxLayout* m_headerRow = nullptr;
    QCheckBox* m_completed = nullptr;
    QPushButton* m_dateChip = nullptr;
    QToolButton *m_groupChip = nullptr, *m_formatButton = nullptr, *m_moreButton = nullptr;
    QToolButton *m_infoButton = nullptr, *m_closeButton = nullptr;
    QFrame *m_headerSeparator = nullptr, *m_dateSeparator = nullptr;
    QWidget* m_parentHost = nullptr;
    QPushButton* m_parentLink = nullptr;
    QToolButton* m_parentArrow = nullptr;
    QString m_parentId;
    EntityInfoPopup* m_infoPopup = nullptr;
    ListSelectPopup* m_groupPopup = nullptr;
    TaskDateController* m_dateController = nullptr;
    EntityCommands* m_commands = nullptr;
    bool m_focusMode = false;
    bool m_desktopMode = false, m_desktopHovered = false;
    SelectionBubble* m_bubble = nullptr;
    FormatPanel* m_formatPanel = nullptr;
    MarkdownEditor* m_description = nullptr;
    AttachmentWidget* m_attachments = nullptr;
    ActivityTimelineWidget* m_activity = nullptr;
};
}  // namespace kl

#endif // KYLINLIST_TASKDETAILWIDGET_H
