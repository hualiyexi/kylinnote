#pragma once

#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "../domain/Attachment.h"

class QVBoxLayout;
class QLabel;
class QPushButton;

namespace kl {

class TaskStore;

// 任务附件的轻量列表；inline 模式可直接放入正文的滚动布局。
// 文件识别、复制、限制与持久化全部由 TaskStore/AttachmentStore 负责。
class AttachmentWidget final : public QWidget {
    Q_OBJECT
public:
    explicit AttachmentWidget(TaskStore* store, QWidget* parent = nullptr);
    void setStore(TaskStore* store);
    void setTask(QString entityId);
    void setReadOnly(bool readOnly);
    void setCompact(bool compact);
    void setInlineMode(bool inlineMode);
    void refresh();
    QString filePath(const Attachment& attachment) const;
    // 与文件选择入口共用逐文件导入；返回已成功添加的数量。
    int importPaths(const QStringList& paths);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

public slots:
    void importFiles();

signals:
    void contentChanged();
    void heightChanged();

private:
    void clearRows();
    void addRow(const Attachment& attachment);
    bool currentAttachment(const QPointer<TaskStore>& store, const QString& entityId,
                           const QString& attachmentId, Attachment* result = nullptr) const;

    QPointer<TaskStore> m_store;
    QString m_entityId;
    QVector<Attachment> m_items;
    bool m_readOnly = false;
    bool m_compact = false;
    bool m_inlineMode = false;
    bool m_rowsDirty = true;
    int m_naturalHeight = -1;
    QVBoxLayout* m_rows = nullptr;
    QWidget* m_rowsHost = nullptr;
    QWidget* m_header = nullptr;
    QPushButton* m_add = nullptr;
    QLabel* m_empty = nullptr;
};

}  // namespace kl
