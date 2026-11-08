#include "EntityPicker.h"

#include <algorithm>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include "../app/TaskStore.h"

namespace kl {
EntityPickerDialog::EntityPickerDialog(TaskStore* store, QWidget* parent) : QDialog(parent), m_store(store) {
    setObjectName(QStringLiteral("EntityPicker")); setWindowTitle(QStringLiteral("选择实体引用")); resize(460, 420);
    auto* layout = new QVBoxLayout(this);
    m_search = new QLineEdit(this); m_search->setObjectName(QStringLiteral("EntityPickerSearch"));
    m_search->setPlaceholderText(QStringLiteral("搜索任务或笔记标题")); layout->addWidget(m_search);
    m_list = new QListWidget(this); m_list->setObjectName(QStringLiteral("EntityPickerList")); layout->addWidget(m_list, 1);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("插入引用"));
    m_buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消")); layout->addWidget(m_buttons);
    if (m_store) {
        m_entities = m_store->entities();
        std::sort(m_entities.begin(), m_entities.end(), [](const Entity& a, const Entity& b) {
            if (a.deleted != b.deleted) return !a.deleted;
            const int title = QString::localeAwareCompare(a.title, b.title);
            if (title != 0) return title < 0;
            if (a.kind != b.kind) return a.kind == EntityKind::Task;
            return a.id < b.id;
        });
    }
    populate(QString());
    connect(m_search, &QLineEdit::textChanged, this, &EntityPickerDialog::populate);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { accept(); });
    connect(m_buttons, &QDialogButtonBox::accepted, this, &EntityPickerDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &EntityPickerDialog::reject);
}
void EntityPickerDialog::populate(const QString& search) {
    m_list->clear();
    for (const Entity& entity : m_entities) {
        if (entity.deleted || (!search.trimmed().isEmpty() && !entity.title.contains(search.trimmed(), Qt::CaseInsensitive))) continue;
        auto* item = new QListWidgetItem(QStringLiteral("%1 · %2").arg(
            entity.kind == EntityKind::Note ? QStringLiteral("笔记") : QStringLiteral("任务"), entity.title), m_list);
        item->setData(Qt::UserRole, entity.id);
    }
    if (m_list->count() > 0) m_list->setCurrentRow(0);
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(m_list->currentItem() != nullptr);
}
Entity EntityPickerDialog::selectedEntity() const {
    const QListWidgetItem* item = m_list->currentItem();
    return item && m_store ? m_store->entity(item->data(Qt::UserRole).toString()) : Entity();
}
} // namespace kl
