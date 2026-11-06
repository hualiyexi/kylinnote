#ifndef KYLINNOTE_ATTACHMENTCODEC_H
#define KYLINNOTE_ATTACHMENTCODEC_H

#include "Attachment.h"
#include "TaskCodec.h"

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QJsonObject>
#include <QSet>
#include <QUuid>
#include <QVector>
#include <QtEndian>

namespace kn {
namespace attachmentcodec {

const qint64 kMaximumAttachmentBytes = 2 * 1024 * 1024;
const int kMaximumOriginalNameLength = 512;
const int kMaximumRelativePathLength = 128;
const int kMaximumSortOrder = 1000000;

// 备份容器：QDataStream 大端 magic/version + JSON 元数据 + 附件内容帧。
const quint32 kBackupContainerMagic = 0x4B4E424B; // "KNBK"
const quint32 kBackupContainerVersion = 1;
const qint64 kMaximumBackupMetadataBytes = 16 * 1024 * 1024;
const int kMaximumBackupAttachments = 2048;

struct BackupFrame {
    QString id;
    QByteArray content;
};

inline QString newAttachmentId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

inline bool isValidAttachmentId(const QString &id)
{
    if (id.size() != 36) return false;
    for (int i = 0; i < id.size(); ++i) {
        const QChar ch = id.at(i);
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (ch != QLatin1Char('-')) return false;
        } else if (!((ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
                     || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f')))) {
            return false;
        }
    }
    return true;
}

// 仅允许单个 posix 安全文件名：不允许目录分隔符、..、隐藏文件或控制字符。
inline bool isSafeAttachmentRelativePath(const QString &name)
{
    if (name.isEmpty() || name.size() > kMaximumRelativePathLength
        || name.startsWith(QLatin1Char('.')) || name.contains(QStringLiteral(".."))) return false;
    for (const QChar &ch : name) {
        const bool ok = (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
            || (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
            || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
            || ch == QLatin1Char('.') || ch == QLatin1Char('_') || ch == QLatin1Char('-');
        if (!ok) return false;
    }
    return true;
}

// 存储扩展名只保留小写字母和数字；无法识别时回退 bin，不直接信任用户文件名。
inline QString safeAttachmentExtension(const QString &suffix)
{
    QString result;
    for (const QChar &ch : suffix.trimmed().toLower()) {
        if ((ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
            || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))) {
            if (result.size() >= 16) break;
            result.append(ch);
        }
    }
    return result.isEmpty() ? QStringLiteral("bin") : result;
}

inline QString attachmentRelativePath(const QString &id, const QString &extension)
{
    return id + QLatin1Char('_') + safeAttachmentExtension(extension);
}

inline bool isValidMimeType(const QString &mime)
{
    if (mime.size() < 3 || mime.size() > 100) return false;
    const int slash = mime.indexOf(QLatin1Char('/'));
    if (slash <= 0 || slash != mime.lastIndexOf(QLatin1Char('/')) || slash == mime.size() - 1) return false;
    for (const QChar &ch : mime) {
        const bool ok = (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
            || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
            || ch == QLatin1Char('/') || ch == QLatin1Char('.') || ch == QLatin1Char('+')
            || ch == QLatin1Char('-');
        if (!ok) return false;
    }
    return true;
}

inline bool validateAttachment(const Attachment &attachment, QString *error)
{
    if (!isValidAttachmentId(attachment.id) || attachment.taskId <= 0)
        return taskcodec::fail(error, QStringLiteral("附件 id 或任务 id 非法"));
    if (attachment.originalName.trimmed().isEmpty()
        || attachment.originalName.size() > kMaximumOriginalNameLength
        || attachment.originalName.contains(QLatin1Char('\0')))
        return taskcodec::fail(error, QStringLiteral("附件原始文件名不可读"));
    if (!isSafeAttachmentRelativePath(attachment.relativePath)
        || !attachment.relativePath.startsWith(attachment.id + QLatin1Char('_')))
        return taskcodec::fail(error, QStringLiteral("附件存储路径不安全或与 id 不匹配"));
    if (!isValidMimeType(attachment.mime))
        return taskcodec::fail(error, QStringLiteral("附件 MIME 类型非法"));
    if (attachment.size <= 0 || attachment.size > kMaximumAttachmentBytes)
        return taskcodec::fail(error, QStringLiteral("附件大小必须在 1 字节到 2 MiB 之间"));
    if (attachment.isImage && !attachment.mime.startsWith(QStringLiteral("image/")))
        return taskcodec::fail(error, QStringLiteral("图片附件的 MIME 必须是 image/*"));
    if (!attachment.createdAt.isValid() || !taskcodec::validDate(attachment.createdAt))
        return taskcodec::fail(error, QStringLiteral("附件创建时间非法"));
    if (attachment.sortOrder < 0 || attachment.sortOrder > kMaximumSortOrder)
        return taskcodec::fail(error, QStringLiteral("附件排序值非法"));
    if (error) error->clear();
    return true;
}

inline QJsonObject toJson(const Attachment &attachment)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), attachment.id);
    object.insert(QStringLiteral("taskId"), QString::number(attachment.taskId));
    object.insert(QStringLiteral("originalName"), attachment.originalName);
    object.insert(QStringLiteral("relativePath"), attachment.relativePath);
    object.insert(QStringLiteral("mime"), attachment.mime);
    object.insert(QStringLiteral("size"), attachment.size);
    object.insert(QStringLiteral("isImage"), attachment.isImage);
    object.insert(QStringLiteral("createdAt"), taskcodec::dateJson(attachment.createdAt));
    object.insert(QStringLiteral("sortOrder"), attachment.sortOrder);
    return object;
}

inline bool fromJson(const QJsonObject &object, Attachment *out, QString *error)
{
    Attachment attachment;
    const char *const strings[] = {"id", "originalName", "relativePath", "mime"};
    for (const char *key : strings) {
        if (!object.value(QLatin1String(key)).isString())
            return taskcodec::fail(error, QStringLiteral("附件文本字段缺失或类型错误"));
    }
    attachment.id = object.value(QStringLiteral("id")).toString();
    attachment.originalName = object.value(QStringLiteral("originalName")).toString();
    attachment.relativePath = object.value(QStringLiteral("relativePath")).toString();
    attachment.mime = object.value(QStringLiteral("mime")).toString();
    if (!taskcodec::readId(object.value(QStringLiteral("taskId")), false, &attachment.taskId)
        || !taskcodec::readInteger(object.value(QStringLiteral("size")), 1, kMaximumAttachmentBytes,
                                   &attachment.size))
        return taskcodec::fail(error, QStringLiteral("附件 taskId/size 非法"));
    qint64 sortOrder = 0;
    if (!taskcodec::readInteger(object.value(QStringLiteral("sortOrder")), 0, kMaximumSortOrder, &sortOrder))
        return taskcodec::fail(error, QStringLiteral("附件排序值非法"));
    attachment.sortOrder = static_cast<int>(sortOrder);
    const QJsonValue isImage = object.value(QStringLiteral("isImage"));
    if (!isImage.isBool()) return taskcodec::fail(error, QStringLiteral("附件 isImage 必须是布尔值"));
    attachment.isImage = isImage.toBool();
    if (!taskcodec::readDate(object.value(QStringLiteral("createdAt")), &attachment.createdAt, error))
        return false;
    if (!validateAttachment(attachment, error)) return false;
    *out = attachment;
    return true;
}

inline bool isBackupContainer(const QByteArray &bytes)
{
    if (bytes.size() < 4) return false;
    return qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData())) == kBackupContainerMagic;
}

// 写出容器。调用者负责内容来源可信；读取端仍会独立执行全部上限与安全校验。
inline bool writeBackupContainer(QIODevice *device, const QByteArray &metadataJson,
                                 const QVector<BackupFrame> &frames, QString *error)
{
    if (!device || !device->isOpen() || !device->isWritable())
        return taskcodec::fail(error, QStringLiteral("备份输出设备未打开"));
    QDataStream stream(device);
    stream.setVersion(QDataStream::Qt_5_12);
    stream << kBackupContainerMagic << kBackupContainerVersion;
    stream << static_cast<quint32>(metadataJson.size());
    if (!metadataJson.isEmpty()
        && stream.writeRawData(metadataJson.constData(), metadataJson.size()) != metadataJson.size())
        return taskcodec::fail(error, QStringLiteral("写入备份元数据失败"));
    stream << static_cast<quint32>(frames.size());
    for (const BackupFrame &frame : frames) {
        const QByteArray id = frame.id.toUtf8();
        stream << static_cast<quint32>(id.size());
        if (!id.isEmpty() && stream.writeRawData(id.constData(), id.size()) != id.size())
            return taskcodec::fail(error, QStringLiteral("写入附件帧头失败"));
        stream << static_cast<quint32>(frame.content.size());
        if (!frame.content.isEmpty()
            && stream.writeRawData(frame.content.constData(), frame.content.size()) != frame.content.size())
            return taskcodec::fail(error, QStringLiteral("写入附件内容失败"));
    }
    if (stream.status() != QDataStream::Ok)
        return taskcodec::fail(error, QStringLiteral("写入备份容器失败: ") + device->errorString());
    if (error) error->clear();
    return true;
}

namespace detail {
struct Cursor {
    const char *data;
    qint64 size;
    qint64 pos = 0;
};

inline bool takeU32(Cursor *cursor, quint32 *out)
{
    if (cursor->size - cursor->pos < 4) return false;
    *out = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(cursor->data + cursor->pos));
    cursor->pos += 4;
    return true;
}

inline bool takeBytes(Cursor *cursor, quint32 length, QByteArray *out)
{
    if (static_cast<qint64>(length) > cursor->size - cursor->pos) return false;
    *out = QByteArray(cursor->data + cursor->pos, static_cast<int>(length));
    cursor->pos += length;
    return true;
}
} // namespace detail

// 读取不可信容器：所有长度在分配前都与实际上限和剩余字节核对，拒绝尾随数据。
inline bool readBackupContainer(const QByteArray &bytes, QByteArray *metadataJson,
                                QVector<BackupFrame> *frames, QString *error)
{
    if (!isBackupContainer(bytes)) return taskcodec::fail(error, QStringLiteral("备份容器 magic 不匹配"));
    detail::Cursor cursor{bytes.constData(), bytes.size(), 0};
    quint32 magic = 0, version = 0, metadataSize = 0, frameCount = 0;
    if (!detail::takeU32(&cursor, &magic) || !detail::takeU32(&cursor, &version)
        || magic != kBackupContainerMagic)
        return taskcodec::fail(error, QStringLiteral("备份容器头损坏"));
    if (version != kBackupContainerVersion)
        return taskcodec::fail(error, QStringLiteral("不支持的备份容器版本 %1").arg(version));
    if (!detail::takeU32(&cursor, &metadataSize) || metadataSize == 0
        || metadataSize > kMaximumBackupMetadataBytes)
        return taskcodec::fail(error, QStringLiteral("备份元数据大小非法"));
    if (!detail::takeBytes(&cursor, metadataSize, metadataJson))
        return taskcodec::fail(error, QStringLiteral("备份元数据被截断"));
    if (!detail::takeU32(&cursor, &frameCount) || frameCount > kMaximumBackupAttachments)
        return taskcodec::fail(error, QStringLiteral("备份附件帧数量非法"));
    QVector<BackupFrame> parsed;
    QSet<QString> ids;
    for (quint32 i = 0; i < frameCount; ++i) {
        quint32 idSize = 0, contentSize = 0;
        QByteArray id;
        BackupFrame frame;
        if (!detail::takeU32(&cursor, &idSize) || idSize == 0 || idSize > 64
            || !detail::takeBytes(&cursor, idSize, &id))
            return taskcodec::fail(error, QStringLiteral("附件帧头非法或被截断"));
        frame.id = QString::fromUtf8(id);
        if (ids.contains(frame.id)) return taskcodec::fail(error, QStringLiteral("备份包含重复附件内容帧"));
        ids.insert(frame.id);
        if (!detail::takeU32(&cursor, &contentSize) || contentSize == 0
            || contentSize > kMaximumAttachmentBytes
            || !detail::takeBytes(&cursor, contentSize, &frame.content))
            return taskcodec::fail(error, QStringLiteral("附件内容大小超过限制或被截断"));
        parsed.append(frame);
    }
    if (cursor.pos != cursor.size) return taskcodec::fail(error, QStringLiteral("备份容器包含多余尾随数据"));
    *frames = parsed;
    if (error) error->clear();
    return true;
}

} // namespace attachmentcodec
} // namespace kn

#endif // KYLINNOTE_ATTACHMENTCODEC_H
