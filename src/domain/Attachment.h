#ifndef KYLINNOTE_ATTACHMENT_H
#define KYLINNOTE_ATTACHMENT_H

#include <QDateTime>
#include <QString>

namespace kn {

// 本地附件/图片的元数据。文件内容始终保存在 dataDir/attachments 下，
// relativePath 只是该目录内的 posix 安全文件名（<附件id>_<安全扩展名>），
// 绝不包含用户原始路径，也不以用户文件名直接作为存储名。
// 领域对象与数据库只依赖 QtCore/QtGui，不依赖任何 UI 类型。
struct Attachment {
    QString id;                                // UUID-like：8-4-4-4-12 小写十六进制
    qint64 taskId = 0;                         // 所属任务；任务永久删除时级联删除
    QString originalName = QStringLiteral(""); // 用户可见的原始文件名（不含目录）
    QString relativePath = QStringLiteral(""); // attachments 目录内的安全文件名
    QString mime = QStringLiteral("");         // 图片按真实解码结果，文档按常见扩展名
    qint64 size = 0;                           // 字节数，1..2*1024*1024
    bool isImage = false;                      // 由 QImageReader 真实解码判定，不信任扩展名
    QDateTime createdAt;                       // UTC 创建时间
    int sortOrder = 0;                         // 任务内展示顺序
};

} // namespace kn

#endif // KYLINNOTE_ATTACHMENT_H
