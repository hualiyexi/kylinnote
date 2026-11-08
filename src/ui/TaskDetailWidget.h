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
class TaskDetailWidget : public QWidget {
    Q_OBJECT
public:
    explicit TaskDetailWidget(TaskStore* store, QWidget* parent = nullptr);
    bool setTask(QString id);
    QString entityId() const { return m_id; }
    bool flush();
    bool finishEditing();
    void focusTitle();
signals:
    void taskRemoved(QString id);
    void internalLinkRequested(QString entityId);
private:
    void buildUi();
    void loadFields();
    void refreshGroups();
    void dirty(const QString& field);
    void updateEnabled();
    bool eventFilter(QObject* watched, QEvent* event) override;
    void beginTextSession();
    bool endTextSession();
    void showMoreMenu();
    TaskStore* m_store;
    QString m_id;
    bool m_loading = false, m_saving = false;
    QSet<QString> m_dirty;
    QString m_editSession;
    QTimer m_saveTimer;
    QWidget* m_form = nullptr;
    QWidget *m_scheduleHost = nullptr, *m_priorityHost = nullptr, *m_appearanceHost = nullptr;
    QLabel *m_empty = nullptr, *m_status = nullptr;
    QLineEdit *m_title = nullptr, *m_tags = nullptr, *m_checkInput = nullptr;
    QCheckBox *m_completed = nullptr, *m_pinned = nullptr, *m_pinnedAt = nullptr;
    QComboBox *m_group = nullptr, *m_priority = nullptr, *m_recurrence = nullptr, *m_color = nullptr,
              *m_layer = nullptr;
    QCheckBox *m_hasStart = nullptr, *m_hasDue = nullptr, *m_hasReminder = nullptr;
    QDateTimeEdit *m_start = nullptr, *m_due = nullptr, *m_reminder = nullptr;
    MarkdownEditor* m_description = nullptr;
    AttachmentWidget* m_attachments = nullptr;
    QListWidget* m_checklist = nullptr;
    QPushButton *m_remove = nullptr, *m_restore = nullptr;
    QToolButton *m_moreButton = nullptr;
    // R8: 底部固定工具栏中的低频动作收纳到更多菜单
    // m_convert 和 m_activity 移入更多菜单，不再平铺
};
}  // namespace kl

#endif // KYLINLIST_TASKDETAILWIDGET_H
