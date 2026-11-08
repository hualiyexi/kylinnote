#pragma once
#include <QMap>
#include <QObject>
#include <QPointer>
#include "../domain/Entity.h"
namespace kl {
class TaskStore;
class NoteWidget;
// 仅管理桌面投影；任务生命周期与持久化由 TaskStore 统一负责。
class NoteManager : public QObject {
    Q_OBJECT
public:
    explicit NoteManager(TaskStore* store, QObject* parent = nullptr);
    ~NoteManager() override;
    bool restoreAll();
    NoteWidget* createNote();
    void showAll();
    void hideAll();
    void deleteActive();
    NoteLayer cycleLayerForActive();
    void applyLayerToAll(NoteLayer layer);
    bool flushAll();
    void setRestoreFrozen(bool frozen);
    void closeAllForRestore();
    bool isEmpty() const {
        return m_notes.isEmpty();
    }
    int count() const {
        return m_notes.size();
    }
    NoteWidget* note(QString id) const {
        return m_notes.value(id, nullptr);
    }
signals:
    void taskOpenRequested(QString id);
public slots:
    void handleSave(NoteWidget* note);

private:
    void syncTask(QString id);
    void closeNote(NoteWidget* note);
    TaskStore* m_store;
    QMap<QString, NoteWidget*> m_notes;
    QPointer<NoteWidget> m_active;
    bool m_hidden = false;
    bool m_restoreFrozen = false;
};
}  // namespace kl
