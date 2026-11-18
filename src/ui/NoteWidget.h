#pragma once

#include <QImage>
#include <QColor>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <functional>
#include "../domain/Entity.h"

class QLabel;
namespace kl {
class TitleBar;
class AutostartSettings;
class TaskDetailWidget;
class TaskStore;

class NoteWidget : public QWidget {
    Q_OBJECT
public:
    explicit NoteWidget(const Entity& task, QWidget* parent = nullptr);
    ~NoteWidget() override;
    static QSize minimumPaperSize();
    QString id() const { return m_task.id; }
    int zOrder() const { return m_task.zOrder; }
    Entity snapshot() const;
    void applyTask(const Entity& task);
    void applySaved(const Entity& task);
    void setSaveFailed(bool failed);
    bool saveFailed() const;
    bool descriptionDirty() const;
    bool desktopDirty() const { return m_desktopDirty; }
    bool isSaving() const;
    bool textRejected() const;
    void setLayer(NoteLayer layer);
    NoteLayer layer() const { return m_task.layer; }
    NoteLayer cycleLayer();
    void raiseTemporarily();
    void focusEditor();
    bool flush();
    bool finishEditing();
    void setStore(TaskStore* store);
    void setAutostart(AutostartSettings* autostart);
    void refreshAttachments();
    QRect paperGeometry() const;
    void setPaperGeometry(const QRect& geometry);
    void setSnapProvider(std::function<QRect(const QRect&)> provider);
    TaskDetailWidget* detail() const { return m_detail; }
signals:
    void activated(NoteWidget* note);
    void closeRequested(NoteWidget* note);
    void saveRequested(NoteWidget* note);
    void taskOpenRequested(QString id);
    void relatedDetailRequested(QString id);
    void completionRequested(QString id, bool completed);
    void newNoteRequested();
    void taskWidgetRequested();
    void arrangeRequested(bool vertical);
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
    void syncChromeGeometry();
    void setControlsHovered(bool hovered);
    void showMoreMenu();
    void showColorMenu();
    Entity m_task;
    QRect m_observedGeometry;
    QPointer<TaskStore> m_store;
    QPointer<AutostartSettings> m_autostart;
    TitleBar* m_titleBar = nullptr;
    QWidget* m_paper = nullptr;
    QWidget* m_border = nullptr;
    TaskDetailWidget* m_detail = nullptr;
    QLabel* m_status = nullptr;
    QVector<QWidget*> m_resizeAreas;
    std::function<QRect(const QRect&)> m_snapProvider;
    QPointer<QWidget> m_menu;
    QTimer m_saveTimer;
    QImage m_shadow;
    QColor m_shadowColor;
    int m_appliedColor = -1;
    bool m_loading = false, m_saveFailed = false, m_temporary = false;
    bool m_applyingFlags = false, m_saving = false, m_desktopDirty = false;
    bool m_hovered = false;
};
} // namespace kl
