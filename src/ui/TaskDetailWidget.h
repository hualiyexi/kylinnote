#pragma once
#include <QSet>
#include <QTimer>
#include <QWidget>
#include "../domain/Task.h"
class QLineEdit;
class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QListWidget;
class QPushButton;
class QLabel;
namespace kn {
class TaskStore;
class MarkdownEditor;
class AttachmentWidget;
class TaskDetailWidget : public QWidget {
    Q_OBJECT
public:
    explicit TaskDetailWidget(TaskStore* store, QWidget* parent = nullptr);
    bool setTask(qint64 id);
    qint64 taskId() const {
        return m_id;
    }
    bool flush();
    void focusTitle();
signals:
    void taskRemoved(qint64 id);

private:
    void buildUi();
    void loadFields();
    void refreshGroups();
    void dirty(const QString& field);
    void updateEnabled();
    TaskStore* m_store;
    qint64 m_id = 0;
    bool m_loading = false, m_saving = false;
    QSet<QString> m_dirty;
    QTimer m_saveTimer;
    QWidget* m_form = nullptr;
    QLabel *m_empty = nullptr, *m_status = nullptr;
    QLineEdit *m_title = nullptr, *m_tags = nullptr, *m_checkInput = nullptr;
    QCheckBox *m_completed = nullptr, *m_pinned = nullptr, *m_favorite = nullptr;
    QComboBox *m_group = nullptr, *m_priority = nullptr, *m_recurrence = nullptr, *m_color = nullptr,
              *m_layer = nullptr;
    QCheckBox *m_hasStart = nullptr, *m_hasDue = nullptr, *m_hasReminder = nullptr;
    QDateTimeEdit *m_start = nullptr, *m_due = nullptr, *m_reminder = nullptr;
    MarkdownEditor* m_description = nullptr;
    AttachmentWidget* m_attachments = nullptr;
    QListWidget* m_checklist = nullptr;
    QPushButton *m_remove = nullptr, *m_restore = nullptr;
};
}  // namespace kn
