#include "ReminderPopup.h"
#include <QComboBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>
#include "../domain/ViewSpec.h"
namespace kl {
ReminderPopup::ReminderPopup(const Entity& task, QWidget* parent)
    : ReminderPopup(task, QString(), parent) {
}
ReminderPopup::ReminderPopup(const Entity& task, QString reminderId, QWidget* parent)
    : QDialog(parent, Qt::Tool | Qt::WindowStaysOnTopHint), m_entityId(task.id), m_reminderId(reminderId) {
    if (m_reminderId.isEmpty()) {
        const int primary = primaryReminderIndex(task);
        if (primary >= 0)
            m_reminderId = task.reminders.at(primary).id;
    }
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("待办提醒 · 麒麟清单"));
    setObjectName(QStringLiteral("ReminderPopup"));
    setMinimumWidth(390);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 20, 22, 18);
    layout->setSpacing(14);
    auto* caption = new QLabel(QStringLiteral("现在该看看这件事了"), this);
    caption->setProperty("muted", true);
    layout->addWidget(caption);
    auto* title = new QLabel(task.title, this);
    title->setWordWrap(true);
    title->setTextFormat(Qt::PlainText);
    QFont f = title->font();
    f.setPointSize(15);
    f.setBold(true);
    title->setFont(f);
    layout->addWidget(title);
    auto* time = new QLabel(scheduleLabel(task), this);
    time->setWordWrap(true);
    layout->addWidget(time);
    auto* row = new QHBoxLayout;
    auto* delay = new QComboBox(this);
    delay->addItem(QStringLiteral("10分钟后"), 10);
    delay->addItem(QStringLiteral("30分钟后"), 30);
    delay->addItem(QStringLiteral("1小时后"), 60);
    auto* snooze = new QPushButton(QStringLiteral("稍后提醒"), this);
    row->addWidget(delay);
    row->addWidget(snooze);
    row->addStretch();
    layout->addLayout(row);
    auto* buttons = new QHBoxLayout;
    auto *open = new QPushButton(QStringLiteral("查看任务"), this),
         *done = new QPushButton(QStringLiteral("标记完成"), this),
         *dismiss = new QPushButton(QStringLiteral("知道了"), this);
    done->setProperty("primary", true);
    buttons->addWidget(open);
    buttons->addStretch();
    buttons->addWidget(dismiss);
    buttons->addWidget(done);
    layout->addLayout(buttons);
    connect(snooze, &QPushButton::clicked, this, [this, delay]() {
        emit snoozeRequested(m_entityId, m_reminderId, delay->currentData().toInt());
    });
    connect(done, &QPushButton::clicked, this, [this]() { emit completeRequested(m_entityId); });
    connect(open, &QPushButton::clicked, this, [this]() {
        emit openRequested(m_entityId);
        close();
    });
    connect(dismiss, &QPushButton::clicked, this, &QDialog::close);
    adjustSize();
    if (QGuiApplication::primaryScreen()) {
        const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
        move(area.right() - width() - 24, area.bottom() - height() - 24);
    }
}
}  // namespace kl
