#include "NoteManager.h"
#include <algorithm>
#include "../platform/ScreenFit.h"
#include "../ui/NoteWidget.h"
#include "Settings.h"
#include "TaskStore.h"
namespace kl {
namespace {
Entity presentedTask(TaskStore* store, const QString& id) {
    Entity task = store->entity(id);
    const DesktopNote desktop = store->desktopNote(id);
    if (!desktop.id.isEmpty()) {
        task.color = desktop.color;
        task.geometry = desktop.geometry;
        task.zOrder = desktop.zOrder;
        task.layer = desktop.layer;
    }
    return task;
}
}  // namespace
NoteManager::NoteManager(TaskStore* store, QObject* parent) : QObject(parent), m_store(store) {
    connect(store, &TaskStore::taskChanged, this, &NoteManager::syncTask);
    connect(store, &TaskStore::desktopNoteChanged, this, &NoteManager::syncTask);
    connect(store, &TaskStore::tasksReset, this, &NoteManager::restoreAll);
    connect(store, &TaskStore::attachmentsChanged, this, [this](QString id) {
        if (NoteWidget* note = m_notes.value(id))
            note->refreshAttachments();
    });
}
NoteManager::~NoteManager() {
    qDeleteAll(m_notes);
}
bool NoteManager::restoreAll() {
    if (m_restoreFrozen) return true;
    const auto ids = m_notes.keys();
    for (QString id : ids)
        syncTask(id);
    for (const Entity& task : m_store->entities())
        syncTask(task.id);
    return true;
}
void NoteManager::syncTask(QString id) {
    if (m_restoreFrozen) return;
    const Entity task = presentedTask(m_store, id);
    const DesktopNote desktop = m_store->desktopNote(id);
    NoteWidget* window = m_notes.value(id, nullptr);
    if (task.id.isEmpty() || task.kind != EntityKind::Task || desktop.id.isEmpty() || !desktop.open
        || task.deleted || task.status != TaskStatus::Active) {
        if (window) {
            if (!window->finishEditing()) return;
            m_notes.remove(id);
            window->hide();
            window->deleteLater();
        }
        return;
    }
    if (window) {
        window->applyTask(task);
        return;
    }
    Entity presentation = task;
    presentation.geometry = ScreenFit::fit(task.geometry);
    window = new NoteWidget(presentation);
    window->setStore(m_store);
    m_notes.insert(id, window);
    connect(window, &NoteWidget::activated, this, [this](NoteWidget* note) { m_active = note; });
    connect(window, &NoteWidget::saveRequested, this, &NoteManager::handleSave);
    connect(window, &NoteWidget::taskOpenRequested, this, &NoteManager::taskOpenRequested);
    connect(window, &NoteWidget::completionRequested, this, [this](QString id, bool completed) {
        if (NoteWidget* window = m_notes.value(id)) {
            if (window->finishEditing())
                m_store->setCompleted(id, completed);
        }
    });
    connect(window, &NoteWidget::closeRequested, this, [this](NoteWidget* note) {
        closeNote(note);
    });
    connect(window, &NoteWidget::checklistToggled, this, [this](QString id, int index, bool completed) {
        Entity task = m_store->entity(id);
        if (index >= 0 && index < task.checklist.size()) {
            task.checklist[index].done = completed;
            m_store->saveTask(task);
        }
    });
    if (!m_hidden)
        window->show();
}
void NoteManager::handleSave(NoteWidget* window) {
    if (m_restoreFrozen) return;
    Entity current = m_store->entity(window->id());
    if (current.id.isEmpty())
        return;
    const Entity snapshot = window->snapshot();
    bool ok = true;
    if (window->descriptionDirty()) {
        current.body = snapshot.body;
        ok = m_store->saveTask(current);
    }
    if (ok) {
        Entity saved = presentedTask(m_store, current.id);
        saved.color = snapshot.color; saved.geometry = snapshot.geometry;
        saved.zOrder = snapshot.zOrder; saved.layer = snapshot.layer;
        window->applySaved(saved);
    } else window->setSaveFailed(true);
}
NoteWidget* NoteManager::createNote() {
    if (m_restoreFrozen) return nullptr;
    const QString id = m_store->createTask(QStringLiteral("新建桌面便签"));
    if (id.isEmpty())
        return nullptr;
    if (!m_store->saveDesktopState(id, NoteColor::Yellow, ScreenFit::defaultGeometry(m_notes.size()),
                                   0, Settings::instance().defaultLayer(), true))
        return nullptr;
    NoteWidget* window = m_notes.value(id);
    if (window) {
        window->raiseTemporarily();
        window->focusEditor();
    }
    return window;
}
void NoteManager::showAll() {
    if (m_restoreFrozen) return;
    m_hidden = false;
    QList<NoteWidget*> ordered = m_notes.values();
    std::stable_sort(ordered.begin(), ordered.end(), [](NoteWidget* a, NoteWidget* b) {
        return a->zOrder() < b->zOrder();
    });
    for (auto* window : ordered) {
        window->show();
        window->raise();
    }
}
void NoteManager::hideAll() {
    m_hidden = true;
    for (auto* window : m_notes)
        window->hide();
}
void NoteManager::closeNote(NoteWidget* window) {
    if (m_restoreFrozen) return;
    if (!window || !window->finishEditing())
        return;
    const Entity snapshot = window->snapshot();
    m_store->saveDesktopState(window->id(), snapshot.color, snapshot.geometry,
                              snapshot.zOrder, snapshot.layer, false);
}
void NoteManager::deleteActive() {
    closeNote(m_active);
}
NoteLayer NoteManager::cycleLayerForActive() {
    if (m_restoreFrozen) return m_active ? m_active->layer() : Settings::instance().defaultLayer();
    return m_active ? m_active->cycleLayer() : Settings::instance().defaultLayer();
}
void NoteManager::applyLayerToAll(NoteLayer layer) {
    if (m_restoreFrozen) return;
    for (auto* window : m_notes)
        window->setLayer(layer);
}
bool NoteManager::flushAll() {
    if (m_restoreFrozen) return false;
    bool ok = true;
    const auto windows = m_notes.values();
    for (auto* window : windows) {
        if (!window->finishEditing()) {
            ok = false;
            continue;
        }
        const Entity snapshot = window->snapshot();
        const DesktopNote desktop = m_store->desktopNote(window->id());
        const bool changed = desktop.color != snapshot.color || desktop.geometry != snapshot.geometry
            || desktop.zOrder != snapshot.zOrder || desktop.layer != snapshot.layer;
        if (changed && !m_store->saveDesktopState(window->id(), snapshot.color, snapshot.geometry,
                                                  snapshot.zOrder, snapshot.layer, desktop.open))
            ok = false;
    }
    return ok;
}
void NoteManager::setRestoreFrozen(bool frozen) {
    m_restoreFrozen = frozen;
    for (auto* window : m_notes) window->setEnabled(!frozen);
}
void NoteManager::closeAllForRestore() {
    const auto windows = m_notes.values(); m_notes.clear(); m_active.clear();
    for (auto* window : windows) { window->hide(); delete window; }
}
}  // namespace kl
