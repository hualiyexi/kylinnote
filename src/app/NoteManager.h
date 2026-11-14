#pragma once
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
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
    // finishEditing 落盘会经 taskChanged 重新进入 syncTask 的关窗分支；
    // m_finishing 让同步重入的刷新/关窗等待最外层保存结束；
    // 重入或保存失败返回 false，保留窗口供最外层收尾或后续重试。
    bool finishNoteEditing(NoteWidget* note);
    TaskStore* m_store;
    QMap<QString, NoteWidget*> m_notes;
    QPointer<NoteWidget> m_active;
    QSet<QString> m_finishing;
    bool m_hidden = false;
    bool m_restoreFrozen = false;
};
}  // namespace kl
