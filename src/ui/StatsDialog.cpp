#include "StatsDialog.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>

#include "../Common.h"
#include "../app/TaskStore.h"

namespace kl {

StatsDialog::StatsDialog(TaskStore *store, QWidget *parent)
    : QDialog(parent), m_store(store)
{
    setObjectName(QStringLiteral("StatsDialog"));
    setWindowTitle(QStringLiteral("使用统计"));
    setMinimumWidth(560);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 20);
    layout->setSpacing(16);

    auto *title = new QLabel(QStringLiteral("使用统计"), this);
    title->setObjectName(QStringLiteral("StatsTitle"));
    QFont titleFont = title->font();
    titleFont.setPointSizeF(20);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *caption = new QLabel(QStringLiteral("这台设备的任务与笔记历史"), this);
    caption->setObjectName(QStringLiteral("StatsCaption"));
    layout->addWidget(caption);

    auto *localGroup = new QGroupBox(QStringLiteral("本机统计"), this);
    localGroup->setObjectName(QStringLiteral("StatsLocalGroup"));
    auto *grid = new QGridLayout(localGroup);
    m_localTasks = addMetric(grid, 0, QStringLiteral("累计创建任务"),
                             QStringLiteral("StatsLocalTasks"));
    m_localNotes = addMetric(grid, 1, QStringLiteral("累计创建笔记"),
                             QStringLiteral("StatsLocalNotes"));
    m_localCompleted = addMetric(grid, 2, QStringLiteral("累计完成任务"),
                                 QStringLiteral("StatsLocalCompleted"));
    m_localDays = addMetric(grid, 3, QStringLiteral("首个任务距今"),
                            QStringLiteral("StatsLocalDays"));
    layout->addWidget(localGroup);

    auto *note = new QLabel(QStringLiteral("按本机历史记录统计；删除任务或笔记不会清除累计计数。"), this);
    note->setObjectName(QStringLiteral("StatsLocalNote"));
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto *close = new QPushButton(QStringLiteral("关闭"), this);
    close->setObjectName(QStringLiteral("StatsClose"));
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    if (m_store) {
        connect(m_store, &TaskStore::taskChanged, this, &StatsDialog::refreshLocal);
        connect(m_store, &TaskStore::tasksReset, this, &StatsDialog::refreshLocal);
    }
}

QLabel *StatsDialog::addMetric(QGridLayout *layout, int column, const QString &name,
                               const QString &objectName)
{
    auto *container = new QVBoxLayout();
    auto *value = new QLabel(QStringLiteral("—"), this);
    value->setObjectName(objectName);
    QFont font = value->font();
    font.setPointSizeF(22);
    font.setBold(true);
    value->setFont(font);
    container->addWidget(value);
    auto *caption = new QLabel(name, this);
    caption->setWordWrap(true);
    container->addWidget(caption);
    container->setContentsMargins(8, 12, 8, 12);
    layout->addLayout(container, 0, column);
    return value;
}

void StatsDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    refreshLocal();
}

void StatsDialog::refreshLocal()
{
    // 隐藏时不因任务变更反复扫描事件日志；下次打开会完整刷新。
    if (!m_store || !isVisible()) return;
    QString error;
    const UsageStatistics stats = m_store->usageStatistics(&error);
    if (!error.isEmpty()) {
        m_localTasks->setText(QStringLiteral("—"));
        m_localNotes->setText(QStringLiteral("—"));
        m_localCompleted->setText(QStringLiteral("—"));
        m_localDays->setText(QStringLiteral("—"));
        return;
    }
    m_localTasks->setText(QString::number(stats.tasksCreated));
    m_localNotes->setText(QString::number(stats.notesCreated));
    m_localCompleted->setText(QString::number(stats.tasksCompleted));
    m_localDays->setText(stats.hasFirstTask()
        ? QStringLiteral("%1 天").arg(stats.firstTaskDays(appNow()))
        : QStringLiteral("尚未创建"));
}

} // namespace kl
