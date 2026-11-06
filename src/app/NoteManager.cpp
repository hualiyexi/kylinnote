#include "NoteManager.h"
#include <algorithm>
#include "../platform/ScreenFit.h"
#include "../ui/NoteWidget.h"
#include "Settings.h"
#include "TaskStore.h"
namespace kn {
NoteManager::NoteManager(TaskStore* store, QObject* parent) : QObject(parent), m_store(store) {
    connect(store, &TaskStore::taskChanged, this, &NoteManager::syncTask);
    connect(store, &TaskStore::tasksReset, this, &NoteManager::restoreAll);
    connect(store, &TaskStore::attachmentsChanged, this, [this](qint64 id) {
        if (NoteWidget* note = m_notes.value(id))
            note->refreshAttachments();
    });
}
NoteManager::~NoteManager() {
    qDeleteAll(m_notes);
}
bool NoteManager::restoreAll() {
    const auto ids = m_notes.keys();
    for (qint64 id : ids)
        syncTask(id);
    for (const Task& task : m_store->tasks())
        syncTask(task.id);
    return true;
}
void NoteManager::syncTask(qint64 id) {
    const Task task = m_store->task(id);
    NoteWidget* window = m_notes.value(id, nullptr);
    if (!task.id || !task.pinned || task.deleted || task.completed) {
        if (window) {
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
    Task presentation = task;
    presentation.geometry = ScreenFit::fit(task.geometry);
    window = new NoteWidget(presentation);
    window->setStore(m_store);
    m_notes.insert(id, window);
    connect(window, &NoteWidget::activated, this, [this](NoteWidget* note) { m_active = note; });
    connect(window, &NoteWidget::saveRequested, this, &NoteManager::handleSave);
    connect(window, &NoteWidget::taskOpenRequested, this, &NoteManager::taskOpenRequested);
    connect(window, &NoteWidget::completionRequested, this, [this](qint64 id, bool completed) {
        if (NoteWidget* window = m_notes.value(id)) {
            if (window->flush())
                m_store->setCompleted(id, completed);
        }
    });
    connect(window, &NoteWidget::closeRequested, this, [this](NoteWidget* note) {
        if (note->flush())
            m_store->setPinned(note->id(), false);
    });
    connect(window, &NoteWidget::checklistToggled, this, [this](qint64 id, int index, bool completed) {
        Task task = m_store->task(id);
        if (index >= 0 && index < task.checklist.size()) {
            task.checklist[index].done = completed;
            m_store->saveTask(task);
        }
    });
    if (!m_hidden)
        window->show();
}
void NoteManager::handleSave(NoteWidget* window) {
    Task current = m_store->task(window->id());
    if (!current.id)
        return;
    const Task snapshot = window->snapshot();
    current.geometry = snapshot.geometry;
    current.color = snapshot.color;
    current.layer = snapshot.layer;
    if (window->descriptionDirty()) {
        current.description = snapshot.description;
        current.contentFormat = ContentFormat::Markdown;
    }
    if (m_store->saveTask(current))
        window->applySaved(m_store->task(current.id));
    else
        window->setSaveFailed(true);
}
NoteWidget* NoteManager::createNote() {
    const qint64 id = m_store->createTask(QStringLiteral("新建桌面便签"));
    if (!id)
        return nullptr;
    Task task = m_store->task(id);
    task.geometry = ScreenFit::defaultGeometry(m_notes.size());
    task.layer = Settings::instance().defaultLayer();
    task.pinned = true;
    if (!m_store->saveTask(task))
        return nullptr;
    NoteWidget* window = m_notes.value(id);
    if (window) {
        window->raiseTemporarily();
        window->focusEditor();
    }
    return window;
}
void NoteManager::showAll() {
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
void NoteManager::deleteActive() {
    if (m_active && m_active->flush())
        m_store->setPinned(m_active->id(), false);
}
NoteLayer NoteManager::cycleLayerForActive() {
    return m_active ? m_active->cycleLayer() : Settings::instance().defaultLayer();
}
void NoteManager::applyLayerToAll(NoteLayer layer) {
    for (auto* window : m_notes)
        window->setLayer(layer);
}
bool NoteManager::flushAll() {
    bool ok = true;
    const auto windows = m_notes.values();
    for (auto* window : windows)
        if (!window->flush())
            ok = false;
    return ok;
}
}  // namespace kn
