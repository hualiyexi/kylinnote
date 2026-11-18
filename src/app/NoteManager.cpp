#include "NoteManager.h"
#include <algorithm>
#include <QTimer>
#include <QGuiApplication>
#include <QScreen>
#include "../ui/NoteSnap.h"
#include "../ui/PopupShadow.h"
#include "../platform/ScreenFit.h"
#include "../ui/NoteWidget.h"
#include "../app/AutostartSettings.h"
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
    const auto windows = m_notes.values();
    m_notes.clear();
    qDeleteAll(windows);
}
bool NoteManager::restoreAll() {
    if (m_restoreFrozen || !m_store) return true;
    const auto ids = m_notes.keys();
    for (QString id : ids)
        syncTask(id);
    for (const Entity& task : m_store->entities())
        syncTask(task.id);
    return true;
}
void NoteManager::syncTask(QString id) {
    // finishEditing 的同步保存尚未结束时，等最外层取得真实结果后再刷新或关窗。
    if (m_restoreFrozen || !m_store || m_finishing.contains(id)) return;
    const Entity task = presentedTask(m_store, id);
    const DesktopNote desktop = m_store->desktopNote(id);
    NoteWidget* window = m_notes.value(id, nullptr);
    if (window && window->isSaving()) return;
    // 归档清单的任务退出桌面活动投影；desktop_notes 行保留，取消归档后由 restoreAll 恢复。
    if (task.id.isEmpty() || task.kind != EntityKind::Task || !desktop.isOpen()
        || task.deleted || task.status != TaskStatus::Active || m_store->isListArchived(task.listId)) {
        if (window) {
            // 落盘期间 taskChanged 会重入 syncTask 的本分支；重入时让最外层继续，
            // 保存失败保留窗口，成功后再移除投影。
            if (!finishNoteEditing(window)) return;
            if (m_notes.value(id) == window) {
                m_notes.remove(id);
                window->hide();
                window->deleteLater();
            }
        }
        return;
    }
    if (window) {
        window->applyTask(task);
        return;
    }
    Entity presentation = task;
    presentation.geometry = ScreenFit::fitPaper(task.geometry, PopupShadowMargin, NoteWidget::minimumPaperSize());
    window = new NoteWidget(presentation);
    window->setStore(m_store);
    window->setAutostart(m_autostart);
    m_notes.insert(id, window);
    window->setSnapProvider([this, id](const QRect& proposed) {
        QScreen* screen = QGuiApplication::screenAt(proposed.center());
        if (!screen) screen = QGuiApplication::primaryScreen();
        QVector<QRect> peers;
        for (auto it = m_notes.constBegin(); it != m_notes.constEnd(); ++it)
            if (it.key() != id && it.value()->isVisible()) peers.append(it.value()->paperGeometry());
        return NoteSnap::snapped(proposed, screen ? screen->availableGeometry() : QRect(), peers);
    });
    connect(window, &NoteWidget::newNoteRequested, this, [this]() { createNote(); });
    connect(window, &NoteWidget::arrangeRequested, this, [this, window](bool vertical) {
        arrangeNotes(window, vertical);
    });
    connect(window, &QObject::destroyed, this, [this, id, window]() {
        if (m_notes.value(id) == window) m_notes.remove(id);
    });
    connect(window, &NoteWidget::activated, this, [this](NoteWidget* note) { m_active = note; });
    connect(window, &NoteWidget::saveRequested, this, &NoteManager::handleSave);
    connect(window, &NoteWidget::taskOpenRequested, this, &NoteManager::taskOpenRequested);
    connect(window, &NoteWidget::taskWidgetRequested, this, &NoteManager::taskWidgetRequested);
    connect(window, &NoteWidget::relatedDetailRequested, this, &NoteManager::relatedDetailRequested);
    connect(window, &NoteWidget::completionRequested, this, [this](QString id, bool completed) {
        if (NoteWidget* window = m_notes.value(id)) {
            if (finishNoteEditing(window))
                m_store->setCompleted(id, completed);
        }
    });
    connect(window, &NoteWidget::closeRequested, this, [this](NoteWidget* note) {
        closeNote(note);
    });
    if (!m_hidden)
        window->show();
}
void NoteManager::handleSave(NoteWidget* window) {
    if (m_restoreFrozen || !m_store || !window || m_notes.value(window->id()) != window) return;
    const QPointer<NoteManager> guard(this);
    const QPointer<NoteWidget> note(window);
    const QPointer<TaskStore> store(m_store);
    const QString id = window->id();
    const Entity snapshot = window->snapshot();
    const DesktopNote desktop = store->desktopNote(id);
    const Entity current = store->entity(id);
    if (desktop.id.isEmpty() || current.id.isEmpty()) return;
    // 正文由 NoteWidget 走共享 TextDraft 提交；投影管理器只保存桌面字段。
    const bool changed = window->desktopDirty()
        && (desktop.color != snapshot.color || desktop.geometry != snapshot.geometry
            || desktop.zOrder != snapshot.zOrder || desktop.layer != snapshot.layer);
    const bool ok = !changed || store->saveDesktopState(id, snapshot.color, snapshot.geometry,
                                                       snapshot.zOrder, snapshot.layer, desktop.open);
    if (!guard || !note || !store || m_notes.value(id) != window) return;
    if (!ok) {
        note->setSaveFailed(true);
        return;
    }
    Entity saved = presentedTask(store, id);
    saved.color = snapshot.color; saved.geometry = snapshot.geometry;
    saved.zOrder = snapshot.zOrder; saved.layer = snapshot.layer;
    note->applySaved(saved);
    QTimer::singleShot(0, this, [this, id]() { syncTask(id); });
}
NoteWidget* NoteManager::createNote() {
    if (m_restoreFrozen || !m_store) return nullptr;
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
    if (m_restoreFrozen || !m_store || !window || m_notes.value(window->id()) != window) return;
    const QPointer<NoteManager> guard(this);
    const QPointer<NoteWidget> note(window);
    const QString id = window->id();
    if (!finishNoteEditing(window) || !guard || !note || !m_store) return;
    const Entity snapshot = note->snapshot();
    const Entity current = m_store->entity(id);
    if (current.id.isEmpty() || m_store->desktopNote(id).id.isEmpty()) {
        m_notes.remove(id);
        note->hide();
        note->deleteLater();
    } else if (!m_store->saveDesktopState(id, snapshot.color, snapshot.geometry,
                                         snapshot.zOrder, snapshot.layer, false) && note) {
        note->setSaveFailed(true);
    }
}
bool NoteManager::finishNoteEditing(NoteWidget* window) {
    if (!window) return true;
    const QString id = window->id();
    // 内层尚不知道保存结果，不能把“正在保存”当作成功并提前关窗。
    if (m_finishing.contains(id)) return false;
    m_finishing.insert(id);
    const QPointer<NoteManager> guard(this);
    const bool ok = window->finishEditing();
    if (!guard) return false;
    m_finishing.remove(id);
    return ok;
}
void NoteManager::arrangeNotes(NoteWidget* anchor, bool vertical) {
    if (m_restoreFrozen || !anchor) return;
    QScreen* screen = QGuiApplication::screenAt(anchor->paperGeometry().center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return;
    QList<NoteWidget*> windows;
    for (auto* note : m_notes)
        if (note->isVisible() && screen->geometry().contains(note->paperGeometry().center())) windows.append(note);
    std::stable_sort(windows.begin(), windows.end(), [](NoteWidget* a, NoteWidget* b) {
        const QRect first = a->paperGeometry(), second = b->paperGeometry();
        return first.top() == second.top() ? first.left() < second.left() : first.top() < second.top();
    });
    QVector<QRect> papers;
    for (auto* note : windows) papers.append(note->paperGeometry());
    const auto arranged = NoteSnap::arranged(papers, screen->availableGeometry(), vertical);
    for (int index = 0; index < windows.size(); ++index) windows.at(index)->setPaperGeometry(arranged.at(index));
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
    if (m_restoreFrozen || !m_store) return false;
    bool ok = true;
    const auto windows = m_notes.values();
    for (auto* window : windows) {
        if (!finishNoteEditing(window)) {
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
