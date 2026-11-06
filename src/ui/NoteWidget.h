#pragma once
#include <QTimer>
#include <QWidget>
#include "../domain/Task.h"
class QLabel;
class QCheckBox;
class QPushButton;
class QStackedWidget;
namespace kn {
class TitleBar;
class ResizeGrip;
class MarkdownEditor;
class NoteChecklist;
class AttachmentWidget;
class TaskStore;
class DocumentPreview;
class NoteWidget : public QWidget {
    Q_OBJECT
public:
    explicit NoteWidget(const Task& task, QWidget* parent = nullptr);
    qint64 id() const {
        return m_task.id;
    }
    int zOrder() const {
        return m_task.zOrder;
    }
    Task snapshot() const;
    void applyTask(const Task& task);
    void applySaved(const Task& task);
    void setSaveFailed(bool failed);
    bool saveFailed() const {
        return m_saveFailed;
    }
    bool descriptionDirty() const {
        return m_descriptionDirty;
    }
    void setLayer(NoteLayer layer);
    NoteLayer layer() const {
        return m_task.layer;
    }
    NoteLayer cycleLayer();
    void raiseTemporarily();
    void focusEditor();
    bool flush();
    void setStore(TaskStore* store);
    void refreshAttachments();
signals:
    void activated(NoteWidget* note);
    void closeRequested(NoteWidget* note);
    void saveRequested(NoteWidget* note);
    void taskOpenRequested(qint64 id);
    void completionRequested(qint64 id, bool completed);
    void checklistToggled(qint64 id, int index, bool completed);

protected:
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void buildUi();
    void updateView();
    void applyWindowFlags();
    void scheduleSave();
    Task m_task;
    TitleBar* m_titleBar = nullptr;
    QLabel *m_title = nullptr, *m_schedule = nullptr, *m_status = nullptr;
    QCheckBox* m_complete = nullptr;
    QPushButton* m_editToggle = nullptr;
    NoteChecklist* m_checklist = nullptr;
    AttachmentWidget* m_attachments = nullptr;
    QStackedWidget* m_body = nullptr;
    MarkdownEditor* m_editor = nullptr;
    DocumentPreview* m_preview = nullptr;
    ResizeGrip* m_grip = nullptr;
    QTimer m_saveTimer;
    bool m_loading = false, m_saveFailed = false, m_descriptionDirty = false, m_temporary = false,
         m_applyingFlags = false, m_pending = false;
};
}  // namespace kn
