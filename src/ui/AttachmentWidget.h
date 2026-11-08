#pragma once

#include <QPointer>
#include <QVector>
#include <QWidget>

#include "../domain/Attachment.h"

class QVBoxLayout;
class QLabel;
class QPushButton;

namespace kl {

class TaskStore;

// 任务附件的轻量列表：图片显示本地缩略图，文件可直接打开。
// 文件识别、复制、限制与持久化全部由 TaskStore/AttachmentStore 负责。
class AttachmentWidget final : public QWidget {
    Q_OBJECT
public:
    explicit AttachmentWidget(TaskStore* store, QWidget* parent = nullptr);
    void setStore(TaskStore* store);
    void setTask(QString entityId);
    void setReadOnly(bool readOnly);
    void setCompact(bool compact);
    void refresh();
    QString filePath(const Attachment& attachment) const;

private:
    void clearRows();
    void importFiles();
    void addRow(const Attachment& attachment);

    TaskStore* m_store;
    QString m_entityId = QString();
    bool m_readOnly = false;
    bool m_compact = false;
    QVBoxLayout* m_rows = nullptr;
    QWidget* m_header = nullptr;
    QPushButton* m_add = nullptr;
    QLabel* m_empty = nullptr;
};

}  // namespace kl
