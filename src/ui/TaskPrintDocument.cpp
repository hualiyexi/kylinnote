#include "TaskPrintDocument.h"

#include <QTextDocument>
#include "../app/TaskStore.h"
#include "Markdown.h"
#include "TaskListModel.h"
#include "design/TypeSystem.h"

namespace kl {

void populateTaskPrintDocument(QTextDocument* document, const TaskListModel& model,
                               const TaskStore& store, const QString& title,
                               bool includeContent) {
    if (!document) return;
    QString html = QStringLiteral("<h1>%1</h1>").arg(title.toHtmlEscaped());
    for (int row = 0; row < model.rowCount(); ++row) {
        const QModelIndex index = model.index(row, 0);
        if (index.data(TaskListModel::HeaderRole).toBool()) {
            html += QStringLiteral("<h2>%1</h2>").arg(index.data().toString().toHtmlEscaped());
            continue;
        }
        const Entity entity = store.entity(model.entityId(index));
        if (entity.id.isEmpty()) continue;
        html += QStringLiteral("<p><b>%1</b>").arg(entity.title.toHtmlEscaped());
        if (entity.completed) html += QStringLiteral("（已完成）");
        else if (entity.status == TaskStatus::Abandoned) html += QStringLiteral("（已放弃）");
        QStringList metadata;
        const QString schedule = index.data(TaskListModel::ScheduleTooltipRole).toString();
        const QString list = index.data(TaskListModel::ListNameRole).toString();
        if (!schedule.isEmpty()) metadata.append(schedule);
        if (!list.isEmpty()) metadata.append(list);
        if (!metadata.isEmpty())
            html += QStringLiteral("<br>%1").arg(metadata.join(QStringLiteral(" · ")).toHtmlEscaped());
        if (includeContent && !entity.body.trimmed().isEmpty()) {
            QString body = Markdown::plainText(entity).toHtmlEscaped();
            body.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
            html += QStringLiteral("<br>%1").arg(body);
        }
        if (includeContent) {
            for (const ChecklistItem& item : entity.checklist) {
                html += QStringLiteral("<br>%1 %2")
                    .arg(item.done ? QStringLiteral("[×]") : QStringLiteral("[ ]"),
                         item.text.toHtmlEscaped());
            }
        }
        html += QStringLiteral("</p>");
    }
    document->setDefaultFont(ds::qfont(ds::FontScene::Body));
    document->setHtml(html);
}

}  // namespace kl
