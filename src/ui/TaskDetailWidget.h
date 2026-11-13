#ifndef KYLINLIST_TASKDETAILWIDGET_H
#define KYLINLIST_TASKDETAILWIDGET_H

#include <QSet>
#include <QTimer>
#include <QWidget>
#include "../domain/Entity.h"
class QLineEdit;
class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QListWidget;
class QPushButton;
class QLabel;
class QToolButton;
namespace kl {
class TaskStore;
class MarkdownEditor;
class AttachmentWidget;
class SelectionBubble;
class FormatPanel;
class EntityInfoPopup;
class ListSelectPopup;
class TaskDetailWidget : public QWidget {
    Q_OBJECT
public:
    explicit TaskDetailWidget(TaskStore* store, QWidget* parent = nullptr);
    bool setTask(QString id);
    QString entityId() const { return m_id; }
    bool flush();
    bool finishEditing();
    void focusTitle();
    void setOverlayMode(bool overlay);
    // 检查项“添加新项”输入框中有未回车提交的文字时返回 true；这类草稿不会被 flush() 保存。
    bool hasDraftInput(QString* what = nullptr) const;
signals:
    void taskRemoved(QString id);
    void internalLinkRequested(QString entityId);
    void focusModeRequested(bool enabled);
    void closeRequested();
private:
    void buildUi();
    void loadFields(bool resetEditor = false);
    void refreshGroups();
    void dirty(const QString& field);
    void updateEnabled();
    bool eventFilter(QObject* watched, QEvent* event) override;
    void beginTextSession();
    bool endTextSession();
    void showMoreMenu();
    void showFormatPanel();
    void showGroupMenu();
    void showInfoPopup();
    void addChecklistRow();
    void updateChecklistHeight();
    void updateDateChip();
    void refreshIcons();
    void setStatus(const QString& text, bool error = false);
    void openDatePicker();
    void setSettingsExpanded(bool expanded);
    TaskStore* m_store;
    QString m_id;
    bool m_loading = false, m_saving = false;
    QSet<QString> m_dirty;
    QString m_editSession;
    QTimer m_saveTimer;
    QWidget* m_detailHost = nullptr;   // 有实体时显示的整体（顶栏+主区+底栏）
    QWidget* m_form = nullptr;         // 主区内容（正文/检查清单/折叠设置）
    QWidget* m_settingsHost = nullptr; // 低频字段折叠区，默认隐藏
    QWidget *m_scheduleHost = nullptr, *m_priorityHost = nullptr, *m_appearanceHost = nullptr;
    QLabel *m_empty = nullptr, *m_status = nullptr;
    QLineEdit *m_title = nullptr, *m_tags = nullptr, *m_checkInput = nullptr;
    QCheckBox *m_completed = nullptr, *m_pinned = nullptr, *m_pinnedAt = nullptr;
    QComboBox *m_group = nullptr, *m_priority = nullptr, *m_recurrence = nullptr, *m_color = nullptr,
              *m_layer = nullptr;
    QCheckBox *m_hasStart = nullptr, *m_hasDue = nullptr, *m_hasReminder = nullptr;
    QDateTimeEdit *m_start = nullptr, *m_due = nullptr, *m_reminder = nullptr;
    QPushButton* m_dateChip = nullptr;      // 顶栏「设置日期」chip，打开 DatePickerPopup
    QPushButton* m_settingsToggle = nullptr;
    QToolButton* m_groupChip = nullptr;     // 底栏左侧所属清单 chip
    QToolButton* m_formatButton = nullptr;  // 底栏「A」格式按钮 → 悬浮格式面板
    QToolButton* m_commentButton = nullptr;
    QToolButton* m_moreButton = nullptr;
    QToolButton *m_infoButton = nullptr, *m_reminderButton = nullptr, *m_closeButton = nullptr;
    EntityInfoPopup* m_infoPopup = nullptr;
    ListSelectPopup* m_groupPopup = nullptr;
    bool m_focusMode = false;
    bool m_overlayMode = false;
    SelectionBubble* m_bubble = nullptr;    // 选中文字气泡工具条
    FormatPanel* m_formatPanel = nullptr;   // 悬浮格式面板（卡片岛）
    MarkdownEditor* m_description = nullptr;
    AttachmentWidget* m_attachments = nullptr;
    QListWidget* m_checklist = nullptr;
    QPushButton *m_remove = nullptr, *m_restore = nullptr;
};
}  // namespace kl

#endif // KYLINLIST_TASKDETAILWIDGET_H
