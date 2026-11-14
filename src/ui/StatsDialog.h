#ifndef KYLINLIST_STATS_DIALOG_H
#define KYLINLIST_STATS_DIALOG_H

#include <QDialog>
#include <QPointer>

class QGridLayout;
class QLabel;

namespace kl {

class TaskStore;

// 本机使用统计：只展示当前数据目录的累计任务、笔记与完成情况。
class StatsDialog : public QDialog {
    Q_OBJECT
public:
    explicit StatsDialog(TaskStore *store, QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void refreshLocal();

private:
    QLabel *addMetric(QGridLayout *layout, int column, const QString &name,
                      const QString &objectName);

    QPointer<TaskStore> m_store;
    QLabel *m_localTasks = nullptr, *m_localNotes = nullptr, *m_localCompleted = nullptr,
           *m_localDays = nullptr;
};

} // namespace kl

#endif // KYLINLIST_STATS_DIALOG_H
