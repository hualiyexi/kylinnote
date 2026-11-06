#ifndef KYLINNOTE_NOTEMANAGER_H
#define KYLINNOTE_NOTEMANAGER_H

#include "../Common.h"
#include "../db/NoteDao.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace kn {

class NoteWidget;

// 便签集合的调度中心：新建、软删、显示隐藏、防抖落库与失败重试
class NoteManager : public QObject
{
    Q_OBJECT
public:
    explicit NoteManager(QObject *parent = nullptr);

    int count() const { return m_notes.size(); }
    bool isEmpty() const { return m_notes.isEmpty(); }

    // 启动时从数据库还原全部便签（含位置、尺寸、颜色、叠放顺序）
    bool restoreAll();

    NoteWidget *createNote(const QRect &geometry = QRect(),
                           NoteColor color = NoteColor::Yellow);

    void showAll();
    void hideAll();
    void deleteActive();

    // 当前便签在「贴在桌面 / 普通 / 置顶」间循环，返回切换后的层级
    NoteLayer cycleLayerForActive();
    // 把层级套用到全部便签
    void applyLayerToAll(NoteLayer layer);

    void setActive(NoteWidget *note);
    void requestClose(NoteWidget *note);

    // 退出前把内存中未落库的改动写回
    bool flushAll();

public slots:
    void handleSave(NoteWidget *note);

private slots:
    void onRetryTimeout();

private:
    void registerNote(NoteWidget *note);
    void destroyNote(NoteWidget *note);
    void saveNote(NoteWidget *note);
    void enqueueRetry(NoteWidget *note);
    void raiseByZOrder();

    NoteDao m_dao;
    QList<NoteWidget *> m_notes;
    QPointer<NoteWidget> m_active;
    QList<QPointer<NoteWidget>> m_pending;
    QTimer m_retryTimer;
    int m_nextZ = 1;
};

} // namespace kn

#endif // KYLINNOTE_NOTEMANAGER_H
