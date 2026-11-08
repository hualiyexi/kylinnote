#pragma once
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include "../domain/Entity.h"
class QLabel;
class QCheckBox;
class QPushButton;
class QStackedWidget;
namespace kl {
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
    explicit NoteWidget(const Entity& task, QWidget* parent = nullptr);
    QString id() const {
        return m_task.id;
    }
    int zOrder() const {
        return m_task.zOrder;
    }
    Entity snapshot() const;
    void applyTask(const Entity& task);
    void applySaved(const Entity& task);
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
    bool finishEditing();
    void setStore(TaskStore* store);
    void refreshAttachments();
signals:
    void activated(NoteWidget* note);
    void closeRequested(NoteWidget* note);
    void saveRequested(NoteWidget* note);
    void taskOpenRequested(QString id);
    void completionRequested(QString id, bool completed);
    void checklistToggled(QString id, int index, bool completed);

protected:
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    void updateView();
    void applyWindowFlags();
    void scheduleSave();
    void beginTextSession();
    bool endTextSession();
    Entity m_task;
    QPointer<TaskStore> m_store;
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
    QString m_editSession;
    bool m_loading = false, m_saveFailed = false, m_descriptionDirty = false, m_temporary = false,
         m_applyingFlags = false, m_pending = false;
};
}  // namespace kl
