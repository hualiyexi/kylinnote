#pragma once

#include <QObject>
#include <QPointer>
#include <QRect>

class QWidget;
namespace kl {
class DatePickerPopup;
class TaskStore;

// 列表行与详情共用日期表单的读写；DatePickerPopup 本身只管理临时表单值。
class TaskDateController : public QObject {
    Q_OBJECT
public:
    TaskDateController(TaskStore* store, QWidget* owner);
    void open(const QString& entityId, const QRect& globalAnchor);
    void close();

signals:
    void saved(QString entityId);
    void saveFailed(QString message);

private:
    TaskStore* m_store;
    QWidget* m_owner;
    QPointer<DatePickerPopup> m_popup;
};
}  // namespace kl
