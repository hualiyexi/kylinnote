#pragma once
#include <QMap>
#include <QObject>
#include <QPointer>
#include "../domain/Task.h"
namespace kn {
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
    bool isEmpty() const {
        return m_notes.isEmpty();
    }
    int count() const {
        return m_notes.size();
    }
    NoteWidget* note(qint64 id) const {
        return m_notes.value(id, nullptr);
    }
signals:
    void taskOpenRequested(qint64 id);
public slots:
    void handleSave(NoteWidget* note);

private:
    void syncTask(qint64 id);
    TaskStore* m_store;
    QMap<qint64, NoteWidget*> m_notes;
    QPointer<NoteWidget> m_active;
    bool m_hidden = false;
};
}  // namespace kn
