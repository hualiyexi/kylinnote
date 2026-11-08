#include "ActivityLogDialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include "../app/TaskStore.h"

namespace kl {
namespace {
QString valueText(const QVariant& value) {
    if (!value.isValid()) return QStringLiteral("NULL");
    if (value.toString().isEmpty()) return QStringLiteral("\"\"");
    return value.toString();
}
QString typeText(const QString& type) {
    if (type == QStringLiteral("created")) return QStringLiteral("创建");
    if (type == QStringLiteral("changed")) return QStringLiteral("修改");
    if (type == QStringLiteral("kind_changed")) return QStringLiteral("类型转换");
    if (type == QStringLiteral("trashed")) return QStringLiteral("移入回收站");
    if (type == QStringLiteral("restored")) return QStringLiteral("恢复");
    if (type == QStringLiteral("deleted")) return QStringLiteral("永久删除");
    if (type == QStringLiteral("attachment_added")) return QStringLiteral("添加附件");
    if (type == QStringLiteral("attachment_removed")) return QStringLiteral("移除附件");
    if (type == QStringLiteral("reminder_added")) return QStringLiteral("添加提醒");
    if (type == QStringLiteral("reminder_removed")) return QStringLiteral("移除提醒");
    return type;
}
QString detailsText(const EntityEvent& event) {
    QString text = QStringLiteral("实体：%1\n类型：%2\n字段：%3\n时间：%4\n\n旧值：\n%5\n\n新值：\n%6")
        .arg(event.entityTitle, typeText(event.type), event.field.isEmpty() ? QStringLiteral("—") : event.field,
             toAppTime(event.occurredAt).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
             valueText(event.oldValue), valueText(event.newValue));
    if (event.type == QStringLiteral("kind_changed")) text += QStringLiteral("\n\n字段已保留，未清除任务专属数据。");
    return text;
}
}
ActivityLogDialog::ActivityLogDialog(TaskStore* store, const QString& entityId, QWidget* parent)
    : QDialog(parent), m_store(store), m_entityId(entityId) {
    setObjectName(QStringLiteral("ActivityLogDialog")); setWindowTitle(QStringLiteral("活动日志")); resize(900, 560);
    auto* layout = new QVBoxLayout(this);
    m_tree = new QTreeWidget(this); m_tree->setObjectName(QStringLiteral("ActivityTree"));
    m_tree->setColumnCount(5); m_tree->setHeaderLabels({QStringLiteral("时间"), QStringLiteral("类型"), QStringLiteral("字段"),
                                                       QStringLiteral("旧值"), QStringLiteral("新值")});
    m_tree->setRootIsDecorated(true); m_tree->setAlternatingRowColors(true); m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setStretchLastSection(true); m_tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(m_tree, 1);
    m_details = new QTextEdit(this); m_details->setObjectName(QStringLiteral("ActivityDetail")); m_details->setReadOnly(true);
    m_details->setPlaceholderText(QStringLiteral("选择一条事件查看完整旧值与新值")); m_details->setMaximumHeight(170);
    layout->addWidget(m_details);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this); buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    layout->addWidget(buttons); connect(buttons, &QDialogButtonBox::rejected, this, &ActivityLogDialog::reject);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &ActivityLogDialog::showEventDetails);

    if (!m_store) return;
    const QVector<EntityEvent> events = m_store->events(entityId); // 打开对话框时才惰性查询，不进入 EntitySnapshot
    QHash<QString, QTreeWidgetItem*> batches;
    for (int i = events.size() - 1; i >= 0; --i) {
        const EntityEvent& event = events.at(i);
        QTreeWidgetItem* batch = batches.value(event.batchId, nullptr);
        if (!batch) {
            batch = new QTreeWidgetItem(m_tree);
            batch->setText(0, toAppTime(event.occurredAt).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
            batch->setText(1, QStringLiteral("批次 %1").arg(event.batchId.left(8))); batch->setExpanded(false);
            batches.insert(event.batchId, batch);
        }
        auto* item = new QTreeWidgetItem(batch);
        item->setText(0, toAppTime(event.occurredAt).toString(QStringLiteral("HH:mm:ss.zzz")));
        item->setText(1, typeText(event.type)); item->setText(2, event.field);
        QString oldValue = valueText(event.oldValue), newValue = valueText(event.newValue);
        if (event.type == QStringLiteral("kind_changed")) newValue += QStringLiteral("（字段已保留）");
        item->setText(3, oldValue); item->setText(4, newValue);
        item->setData(0, Qt::UserRole, detailsText(event));
        const QString tooltip = detailsText(event); for (int column = 0; column < 5; ++column) item->setToolTip(column, tooltip);
    }
    for (QTreeWidgetItem* batch : batches) batch->setText(1, batch->text(1) + QStringLiteral(" · %1 条").arg(batch->childCount()));
    if (m_tree->topLevelItemCount() == 0) {
        auto* empty = new QTreeWidgetItem(m_tree); empty->setText(0, QStringLiteral("暂无活动日志")); empty->setFlags(Qt::NoItemFlags);
    }
    m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
}
void ActivityLogDialog::showEventDetails() {
    const QList<QTreeWidgetItem*> selected = m_tree->selectedItems();
    m_details->setPlainText(selected.isEmpty() ? QString() : selected.first()->data(0, Qt::UserRole).toString());
}
} // namespace kl
