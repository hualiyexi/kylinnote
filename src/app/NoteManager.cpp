#include "NoteManager.h"

#include "../db/Database.h"
#include "../platform/ScreenFit.h"
#include "../ui/ConfirmDeleteDialog.h"
#include "../ui/NoteWidget.h"
#include "Settings.h"

#include <QApplication>
#include <QDebug>
#include <algorithm>

namespace kn {

NoteManager::NoteManager(QObject *parent)
    : QObject(parent)
{
    m_retryTimer.setInterval(SaveRetryMs);
    m_retryTimer.setSingleShot(false);
    connect(&m_retryTimer, &QTimer::timeout, this, &NoteManager::onRetryTimeout);
}

void NoteManager::registerNote(NoteWidget *note)
{
    m_notes.append(note);

    connect(note, &NoteWidget::saveRequested, this, &NoteManager::handleSave);
    connect(note, &NoteWidget::activated, this, [this](NoteWidget *w) {
        setActive(w);
    });
    connect(note, &NoteWidget::closeRequested, this, [this](NoteWidget *w) {
        requestClose(w);
    });
}

void NoteManager::destroyNote(NoteWidget *note)
{
    if (!note) {
        return;
    }
    m_notes.removeAll(note);
    m_pending.removeAll(note);
    if (m_active == note) {
        m_active.clear();
    }
    note->hide();
    note->disconnect(this);
    note->deleteLater();

    if (m_pending.isEmpty()) {
        m_retryTimer.stop();
    }
}

bool NoteManager::restoreAll()
{
    m_nextZ = m_dao.maxZOrder() + 1;

    const QVector<NoteRecord> records = m_dao.fetchAll();
    for (const NoteRecord &record : records) {
        NoteRecord rec = record;
        // 多屏坐标可能已越界（拔掉外接屏等），先修正再建窗
        rec.geometry = ScreenFit::fit(record.geometry);
        NoteWidget *note = new NoteWidget(this, rec);
        registerNote(note);
        note->setLayer(Settings::instance().defaultLayer());
    }

    raiseByZOrder();
    return true;
}

void NoteManager::raiseByZOrder()
{
    QList<NoteWidget *> ordered = m_notes;
    std::stable_sort(ordered.begin(), ordered.end(), [](NoteWidget *a, NoteWidget *b) {
        return a->zOrder() < b->zOrder();
    });
    for (NoteWidget *note : ordered) {
        note->show();
        note->raise();
        // 窗管可能晚于进程就绪，延迟补一次置顶
        note->delayedRaise(800);
    }
}

NoteWidget *NoteManager::createNote(const QRect &geometry, NoteColor color)
{
    NoteRecord rec;
    rec.geometry = geometry.isValid() ? geometry : ScreenFit::defaultGeometry(m_notes.size());
    rec.color    = color;
    rec.zOrder   = m_nextZ++;
    rec.createdAt = QDateTime::currentDateTimeUtc();

    NoteWidget *note = new NoteWidget(this, rec);
    registerNote(note);
    note->setLayer(Settings::instance().defaultLayer());

    // 立即尝试入库；失败则内存保留并进入重试队列
    NoteRecord toInsert = note->snapshot();
    if (m_dao.insert(toInsert)) {
        note->applySaved(toInsert);
        note->setSaveFailed(false);
    } else {
        note->setSaveFailed(true);
        enqueueRetry(note);
    }

    note->raiseTemporarily();
    m_active = note;
    return note;
}

void NoteManager::showAll()
{
    raiseByZOrder();
    if (!m_notes.isEmpty()) {
        NoteWidget *last = m_notes.last();
        last->focusEditor();
    }
}

void NoteManager::hideAll()
{
    for (NoteWidget *note : m_notes) {
        note->hide();
    }
}

void NoteManager::setActive(NoteWidget *note)
{
    m_active = note;
}

void NoteManager::deleteActive()
{
    NoteWidget *target = m_active.data();
    if (!target && !m_notes.isEmpty()) {
        target = m_notes.last();
    }
    requestClose(target);
}

NoteLayer NoteManager::cycleLayerForActive()
{
    NoteWidget *target = m_active.data();
    if (!target && !m_notes.isEmpty()) {
        target = m_notes.last();
    }
    if (target) {
        return target->cycleLayer();
    }
    return Settings::instance().defaultLayer();
}

void NoteManager::applyLayerToAll(NoteLayer layer)
{
    for (NoteWidget *note : m_notes) {
        note->setLayer(layer);
    }
}

void NoteManager::requestClose(NoteWidget *note)
{
    if (!note) {
        return;
    }

    Settings &settings = Settings::instance();
    if (settings.confirmDelete()) {
        ConfirmDeleteDialog dialog;
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        if (dialog.dontAskAgain()) {
            settings.setConfirmDelete(false);
            settings.sync();
        }
    }

    note->markDeleted(true);

    if (note->id() <= 0) {
        // 从未成功入库的便签，库中无记录可直接丢弃
        destroyNote(note);
        return;
    }

    if (m_dao.softDelete(note->id())) {
        destroyNote(note);
        return;
    }

    // 软删失败：先隐藏但保留内存对象，等待重试
    note->setSaveFailed(true);
    note->hide();
    enqueueRetry(note);
}

void NoteManager::enqueueRetry(NoteWidget *note)
{
    if (!m_pending.contains(note)) {
        m_pending.append(note);
    }
    if (!m_retryTimer.isActive()) {
        m_retryTimer.start();
    }
}

void NoteManager::handleSave(NoteWidget *note)
{
    saveNote(note);
}

void NoteManager::saveNote(NoteWidget *note)
{
    if (!note) {
        return;
    }

    NoteRecord rec = note->snapshot();

    if (note->isMarkedDeleted()) {
        if (rec.id > 0 && m_dao.softDelete(rec.id)) {
            note->setSaveFailed(false);
            m_pending.removeAll(note);
            destroyNote(note);
        } else {
            enqueueRetry(note);
        }
        return;
    }

    const bool ok = (rec.id > 0) ? m_dao.update(rec) : m_dao.insert(rec);
    if (ok) {
        note->applySaved(rec);
        note->setSaveFailed(false);
        m_pending.removeAll(note);
        if (m_pending.isEmpty()) {
            m_retryTimer.stop();
        }
    } else {
        // 写库失败：内存状态完整保留，界面给出提示并安排重试
        note->setSaveFailed(true);
        enqueueRetry(note);
    }
}

void NoteManager::onRetryTimeout()
{
    if (m_pending.isEmpty()) {
        m_retryTimer.stop();
        return;
    }
    const QList<QPointer<NoteWidget>> snapshot = m_pending;
    for (const QPointer<NoteWidget> &ptr : snapshot) {
        if (!ptr.isNull()) {
            saveNote(ptr.data());
        } else {
            m_pending.removeAll(ptr);
        }
    }
    if (m_pending.isEmpty()) {
        m_retryTimer.stop();
    }
}

bool NoteManager::flushAll()
{
    bool allOk = true;
    const QList<NoteWidget *> snapshot = m_notes;
    for (NoteWidget *note : snapshot) {
        if (!note) {
            continue;
        }
        if (note->isMarkedDeleted()) {
            if (note->id() > 0 && !m_dao.softDelete(note->id())) {
                allOk = false;
            }
            continue;
        }
        NoteRecord rec = note->snapshot();
        const bool ok = (rec.id > 0) ? m_dao.update(rec) : m_dao.insert(rec);
        if (ok) {
            note->applySaved(rec);
        } else {
            allOk = false;
        }
    }
    return allOk;
}

} // namespace kn
