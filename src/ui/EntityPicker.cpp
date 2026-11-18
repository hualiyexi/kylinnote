#include "EntityPicker.h"

#include <algorithm>
#include <QAction>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include "../app/TaskStore.h"
#include "AppIcons.h"

namespace kl {
QVector<Entity> matchingEntities(TaskStore* store, const QString& query) {
    QVector<Entity> result;
    if (!store) return result;
    const QString needle = query.trimmed();
    for (const Entity& entity : store->entities()) {
        if (!entity.deleted && (needle.isEmpty() || entity.title.contains(needle, Qt::CaseInsensitive)))
            result.append(entity);
    }
    std::sort(result.begin(), result.end(), [](const Entity& a, const Entity& b) {
        const int title = QString::localeAwareCompare(a.title, b.title);
        if (title != 0) return title < 0;
        if (a.kind != b.kind) return a.kind == EntityKind::Task;
        return a.id < b.id;
    });
    return result;
}

EntityPickerDialog::EntityPickerDialog(TaskStore* store, QWidget* parent) : QDialog(parent), m_store(store) {
    setObjectName(QStringLiteral("EntityPicker")); setWindowTitle(QStringLiteral("关联任务/笔记")); resize(460, 420);
    auto* layout = new QVBoxLayout(this);
    m_search = new QLineEdit(this); m_search->setObjectName(QStringLiteral("EntityPickerSearch"));
    QAction* searchAction = m_search->addAction(AppIcons::icon(IconId::Search), QLineEdit::LeadingPosition);
    searchAction->setToolTip(QStringLiteral("搜索任务或笔记标题"));
    m_search->setAccessibleName(QStringLiteral("搜索任务或笔记标题"));
    m_search->setPlaceholderText(QStringLiteral("搜索任务或笔记标题")); layout->addWidget(m_search);
    m_list = new QListWidget(this); m_list->setObjectName(QStringLiteral("EntityPickerList")); layout->addWidget(m_list, 1);
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确认关联"));
    m_buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消")); layout->addWidget(m_buttons);
    connect(m_search, &QLineEdit::textChanged, this, &EntityPickerDialog::populate);
    connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem*, QListWidgetItem*) {
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(!selectedEntity().id.isEmpty());
    });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { accept(); });
    connect(m_buttons, &QDialogButtonBox::accepted, this, &EntityPickerDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &EntityPickerDialog::reject);
    if (m_store) {
        connect(m_store.data(), &TaskStore::taskChanged, this, [this](QString) { populate(m_search->text()); });
        connect(m_store.data(), &TaskStore::tasksReset, this, [this]() { populate(m_search->text()); });
        connect(m_store.data(), &QObject::destroyed, this, [this]() { populate(m_search->text()); });
    }
    populate(QString());
}
void EntityPickerDialog::populate(const QString& search) {
    const QListWidgetItem* selected = m_list->currentItem();
    const QString selectedId = selected ? selected->data(Qt::UserRole).toString() : QString();
    m_list->clear();
    int selectedRow = -1;
    for (const Entity& entity : matchingEntities(m_store.data(), search)) {
        auto* item = new QListWidgetItem(QStringLiteral("%1 · %2").arg(
            entity.kind == EntityKind::Note ? QStringLiteral("笔记") : QStringLiteral("任务"), entity.title), m_list);
        item->setData(Qt::UserRole, entity.id);
        if (entity.id == selectedId) selectedRow = m_list->count() - 1;
    }
    if (m_list->count() > 0) m_list->setCurrentRow(selectedRow >= 0 ? selectedRow : 0);
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(!selectedEntity().id.isEmpty());
}
Entity EntityPickerDialog::selectedEntity() const {
    const QListWidgetItem* item = m_list->currentItem();
    const Entity selected = item && m_store ? m_store->entity(item->data(Qt::UserRole).toString()) : Entity();
    return selected.deleted ? Entity() : selected;
}
void EntityPickerDialog::accept() {
    // Re-read the store at the acceptance boundary, including a target that was
    // deleted or disappeared while the dialog was open.
    if (selectedEntity().id.isEmpty()) { populate(m_search->text()); return; }
    QDialog::accept();
}
} // namespace kl
