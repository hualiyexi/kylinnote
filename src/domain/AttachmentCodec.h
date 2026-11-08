#ifndef KYLINLIST_ATTACHMENTCODEC_H
#define KYLINLIST_ATTACHMENTCODEC_H

#include "Attachment.h"
#include "EntityCodec.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QIODevice>
#include <QJsonObject>
#include <QSet>
#include <QVector>
#include <QtEndian>

namespace kl {
namespace attachmentcodec {

const qint64 kMaximumAttachmentBytes = 2 * 1024 * 1024;
const int kMaximumOriginalNameLength = 512;
const int kMaximumRelativePathLength = 128;
const int kMaximumSortOrder = 1000000;
// 新格式与旧 KNBK 容器、纯 JSON 备份均不兼容。
const quint32 kBackupContainerMagic = 0x4B4C4231; // "KLB1"
const quint32 kBackupContainerVersion = 1;
const qint64 kMaximumBackupMetadataBytes = 256LL * 1024 * 1024;
const qint64 kMaximumBackupContainerBytes = 2LL * 1024 * 1024 * 1024;
const qint64 kBackupFrameHeaderBytes = 44; // idSize + UUID + contentSize

struct BackupFrame { QString id; QByteArray content; }; // 小型测试夹具；生产路径使用 BackupFrameInfo。
struct BackupFrameInfo { QString id; qint64 offset = 0; qint64 size = 0; };

inline QString newAttachmentId() { return newUuid(); }
inline bool isValidAttachmentId(const QString &id) { return isUuid(id); }
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
            || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) || ch == QLatin1Char('/')
            || ch == QLatin1Char('.') || ch == QLatin1Char('+') || ch == QLatin1Char('-');
        if (!ok) return false;
    }
    return true;
}
inline bool isSha256(const QString &hex)
{
    if (hex.size() != 64) return false;
    for (const QChar &ch : hex)
        if (!((ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
              || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f')))) return false;
    return true;
}
inline bool validateAttachment(const Attachment &attachment, QString *error)
{
    if (!isUuid(attachment.id) || !isUuid(attachment.entityId))
        return entitycodec::fail(error, QStringLiteral("附件或任务 id 必须为 UUID"));
    if (attachment.originalName.trimmed().isEmpty() || attachment.originalName.size() > kMaximumOriginalNameLength
        || attachment.originalName.contains(QLatin1Char('\0')))
        return entitycodec::fail(error, QStringLiteral("附件原始文件名不可读"));
    if (!isSafeAttachmentRelativePath(attachment.relativePath)
        || !attachment.relativePath.startsWith(attachment.id + QLatin1Char('_')))
        return entitycodec::fail(error, QStringLiteral("附件路径不安全或与 id 不匹配"));
    if (!isValidMimeType(attachment.mime) || !isSha256(attachment.sha256))
        return entitycodec::fail(error, QStringLiteral("附件 MIME 或 SHA256 非法"));
    if (attachment.size <= 0 || attachment.size > kMaximumAttachmentBytes)
        return entitycodec::fail(error, QStringLiteral("附件大小必须在 1 字节到 2 MiB 之间"));
    if (attachment.isImage && !attachment.mime.startsWith(QStringLiteral("image/")))
        return entitycodec::fail(error, QStringLiteral("图片附件 MIME 必须为 image/*"));
    if (!attachment.createdAt.isValid() || !entitycodec::validDate(attachment.createdAt)
        || !entitycodec::validDate(attachment.verifiedAt)
        || attachmentFileStateName(attachment.localState).isEmpty())
        return entitycodec::fail(error, QStringLiteral("附件时间或本地状态非法"));
    if (attachment.sortOrder < 0 || attachment.sortOrder > kMaximumSortOrder)
        return entitycodec::fail(error, QStringLiteral("附件排序非法"));
    if (error) error->clear();
    return true;
}
inline QJsonObject toJson(const Attachment &attachment)
{
    QJsonObject object;
    object.insert(QStringLiteral("id"), attachment.id); object.insert(QStringLiteral("entityId"), attachment.entityId);
    object.insert(QStringLiteral("originalName"), attachment.originalName); object.insert(QStringLiteral("relativePath"), attachment.relativePath);
    object.insert(QStringLiteral("mime"), attachment.mime); object.insert(QStringLiteral("size"), static_cast<double>(attachment.size));
    object.insert(QStringLiteral("sha256"), attachment.sha256); object.insert(QStringLiteral("isImage"), attachment.isImage);
    object.insert(QStringLiteral("localState"), attachmentFileStateName(attachment.localState));
    object.insert(QStringLiteral("createdAtUtcMs"), entitycodec::dateJson(attachment.createdAt));
    object.insert(QStringLiteral("verifiedAtUtcMs"), entitycodec::dateJson(attachment.verifiedAt));
    object.insert(QStringLiteral("sortOrder"), attachment.sortOrder);
    return object;
}
inline bool fromJson(const QJsonObject &object, Attachment *out, QString *error)
{
    Attachment attachment;
    const char *const strings[] = {"id", "entityId", "originalName", "relativePath", "mime", "sha256", "localState"};
    for (const char *key : strings)
        if (!object.value(QLatin1String(key)).isString())
            return entitycodec::fail(error, QStringLiteral("附件文本字段缺失或类型非法"));
    attachment.id = object.value(QStringLiteral("id")).toString(); attachment.entityId = object.value(QStringLiteral("entityId")).toString();
    attachment.originalName = object.value(QStringLiteral("originalName")).toString();
    attachment.relativePath = object.value(QStringLiteral("relativePath")).toString(); attachment.mime = object.value(QStringLiteral("mime")).toString();
    attachment.sha256 = object.value(QStringLiteral("sha256")).toString();
    attachment.localState = attachmentFileStateFromName(object.value(QStringLiteral("localState")).toString());
    if (attachmentFileStateName(attachment.localState) != object.value(QStringLiteral("localState")).toString())
        return entitycodec::fail(error, QStringLiteral("附件本地状态非法"));
    qint64 number = 0;
    if (!entitycodec::readInteger(object.value(QStringLiteral("size")), 1, kMaximumAttachmentBytes, &attachment.size)
        || !entitycodec::readInteger(object.value(QStringLiteral("sortOrder")), 0, kMaximumSortOrder, &number))
        return entitycodec::fail(error, QStringLiteral("附件大小或排序非法"));
    attachment.sortOrder = static_cast<int>(number);
    const QJsonValue image = object.value(QStringLiteral("isImage"));
    if (!image.isBool()) return entitycodec::fail(error, QStringLiteral("附件 isImage 必须为布尔值"));
    attachment.isImage = image.toBool();
    if (!entitycodec::readDate(object.value(QStringLiteral("createdAtUtcMs")), &attachment.createdAt, error)
        || !entitycodec::readDate(object.value(QStringLiteral("verifiedAtUtcMs")), &attachment.verifiedAt, error)
        || !validateAttachment(attachment, error)) return false;
    *out = attachment;
    return true;
}

inline bool validateBackupSizes(qint64 metadataBytes, qint64 attachmentBytes, qint64 frameCount,
                                qint64 *containerBytes, QString *error)
{
    if (metadataBytes <= 0 || metadataBytes > kMaximumBackupMetadataBytes)
        return entitycodec::fail(error, QStringLiteral("备份 metadata 必须大小为 1 字节到 256 MiB"));
    if (attachmentBytes < 0 || frameCount < 0
        || attachmentBytes > kMaximumBackupContainerBytes || frameCount > kMaximumBackupContainerBytes / kBackupFrameHeaderBytes)
        return entitycodec::fail(error, QStringLiteral("备份附件大小或数量非法"));
    const qint64 total = 16 + metadataBytes + frameCount * kBackupFrameHeaderBytes + attachmentBytes;
    if (total > kMaximumBackupContainerBytes) return entitycodec::fail(error, QStringLiteral("备份容器超过 2 GiB"));
    if (containerBytes) *containerBytes = total;
    return true;
}
inline bool isBackupContainer(const QByteArray &bytes)
{
    return bytes.size() >= 4
        && qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData())) == kBackupContainerMagic;
}
inline bool writeRaw(QIODevice *device, const char *data, qint64 size, QString *error)
{
    qint64 offset = 0;
    while (offset < size) {
        const qint64 written = device->write(data + offset, size - offset);
        if (written <= 0) return entitycodec::fail(error, QStringLiteral("写入备份失败: ") + device->errorString());
        offset += written;
    }
    return true;
}
inline bool writeU32(QIODevice *device, quint32 value, QString *error)
{
    uchar bytes[4]; qToBigEndian(value, bytes);
    return writeRaw(device, reinterpret_cast<const char *>(bytes), 4, error);
}
inline bool readU32(QIODevice *device, quint32 *value)
{
    uchar bytes[4];
    if (device->read(reinterpret_cast<char *>(bytes), 4) != 4) return false;
    *value = qFromBigEndian<quint32>(bytes); return true;
}
inline bool writeBackupHeader(QIODevice *device, const QByteArray &metadataJson, quint32 frameCount, QString *error)
{
    if (!device || !device->isOpen() || !device->isWritable())
        return entitycodec::fail(error, QStringLiteral("备份输出设备未打开"));
    if (metadataJson.isEmpty() || metadataJson.size() > kMaximumBackupMetadataBytes)
        return entitycodec::fail(error, QStringLiteral("备份 metadata 超过 256 MiB"));
    return writeU32(device, kBackupContainerMagic, error) && writeU32(device, kBackupContainerVersion, error)
        && writeU32(device, static_cast<quint32>(metadataJson.size()), error)
        && writeRaw(device, metadataJson.constData(), metadataJson.size(), error)
        && writeU32(device, frameCount, error);
}
inline bool writeBackupFrameHeader(QIODevice *device, const QString &id, qint64 contentSize, QString *error)
{
    const QByteArray uuid = id.toUtf8();
    if (!isUuid(id) || uuid.size() != 36 || contentSize <= 0 || contentSize > kMaximumAttachmentBytes)
        return entitycodec::fail(error, QStringLiteral("附件帧 id 或大小非法"));
    return writeU32(device, static_cast<quint32>(uuid.size()), error)
        && writeRaw(device, uuid.constData(), uuid.size(), error)
        && writeU32(device, static_cast<quint32>(contentSize), error);
}
inline bool copyBytes(QIODevice *input, QIODevice *output, qint64 size, QCryptographicHash *hash, QString *error)
{
    if (!input || !output || size < 0) return entitycodec::fail(error, QStringLiteral("流式复制参数非法"));
    qint64 remaining = size;
    while (remaining > 0) {
        const QByteArray chunk = input->read(qMin<qint64>(remaining, 64 * 1024));
        if (chunk.isEmpty()) return entitycodec::fail(error, QStringLiteral("读取附件帧失败或被截断: ") + input->errorString());
        if (!writeRaw(output, chunk.constData(), chunk.size(), error)) return false;
        if (hash) hash->addData(chunk);
        remaining -= chunk.size();
    }
    return true;
}
inline bool writeBackupContainer(QIODevice *device, const QByteArray &metadataJson,
                                 const QVector<BackupFrame> &frames, QString *error)
{
    qint64 attachmentBytes = 0;
    for (const BackupFrame &frame : frames) {
        if (!isUuid(frame.id) || frame.content.isEmpty() || frame.content.size() > kMaximumAttachmentBytes)
            return entitycodec::fail(error, QStringLiteral("附件帧内容非法"));
        attachmentBytes += frame.content.size();
    }
    if (!validateBackupSizes(metadataJson.size(), attachmentBytes, frames.size(), nullptr, error)
        || !writeBackupHeader(device, metadataJson, static_cast<quint32>(frames.size()), error)) return false;
    for (const BackupFrame &frame : frames)
        if (!writeBackupFrameHeader(device, frame.id, frame.content.size(), error)
            || !writeRaw(device, frame.content.constData(), frame.content.size(), error)) return false;
    if (error) error->clear();
    return true;
}
inline bool readBackupContainer(QIODevice *device, QByteArray *metadataJson,
                                QVector<BackupFrameInfo> *frames, QString *error)
{
    if (!device || !metadataJson || !frames || !device->isOpen() || !device->isReadable() || device->isSequential())
        return entitycodec::fail(error, QStringLiteral("备份输入必须是可定位的只读设备"));
    const qint64 totalSize = device->size();
    if (totalSize < 16 || totalSize > kMaximumBackupContainerBytes)
        return entitycodec::fail(error, QStringLiteral("备份容器为空、截断或超过 2 GiB"));
    if (!device->seek(0)) return entitycodec::fail(error, QStringLiteral("无法定位备份头"));
    quint32 magic = 0, version = 0, metadataSize = 0, frameCount = 0;
    if (!readU32(device, &magic) || !readU32(device, &version) || magic != kBackupContainerMagic)
        return entitycodec::fail(error, QStringLiteral("不是 KylinList KLB1 备份"));
    if (version != kBackupContainerVersion) return entitycodec::fail(error, QStringLiteral("不支持的备份容器版本 %1").arg(version));
    if (!readU32(device, &metadataSize) || metadataSize == 0 || metadataSize > kMaximumBackupMetadataBytes
        || metadataSize > totalSize - device->pos() - 4)
        return entitycodec::fail(error, QStringLiteral("备份 metadata 大小非法或被截断"));
    const QByteArray metadata = device->read(metadataSize);
    if (metadata.size() != static_cast<qint64>(metadataSize) || !readU32(device, &frameCount))
        return entitycodec::fail(error, QStringLiteral("备份 metadata 或附件数量被截断"));
    if (frameCount > static_cast<quint64>(totalSize - device->pos()) / kBackupFrameHeaderBytes)
        return entitycodec::fail(error, QStringLiteral("附件帧数量超出容器边界"));
    QVector<BackupFrameInfo> parsed; QSet<QString> ids;
    for (quint32 i = 0; i < frameCount; ++i) {
        quint32 idSize = 0, contentSize = 0;
        if (!readU32(device, &idSize) || idSize != 36) return entitycodec::fail(error, QStringLiteral("附件帧头非法或被截断"));
        const QByteArray uuid = device->read(idSize);
        if (uuid.size() != static_cast<int>(idSize)) return entitycodec::fail(error, QStringLiteral("附件帧 UUID 被截断"));
        BackupFrameInfo frame; frame.id = QString::fromUtf8(uuid);
        if (!isUuid(frame.id) || ids.contains(frame.id)) return entitycodec::fail(error, QStringLiteral("附件帧 id 非法或重复"));
        if (!readU32(device, &contentSize) || contentSize == 0 || contentSize > kMaximumAttachmentBytes)
            return entitycodec::fail(error, QStringLiteral("附件内容大小非法"));
        frame.offset = device->pos(); frame.size = contentSize;
        if (frame.size > totalSize - frame.offset) return entitycodec::fail(error, QStringLiteral("附件内容被截断"));
        ids.insert(frame.id); parsed.append(frame);
        if (!device->seek(frame.offset + frame.size)) return entitycodec::fail(error, QStringLiteral("无法跳过附件内容帧"));
    }
    if (device->pos() != totalSize) return entitycodec::fail(error, QStringLiteral("备份包含尾随数据"));
    *metadataJson = metadata; *frames = parsed;
    if (error) error->clear(); return true;
}

} // namespace attachmentcodec
} // namespace kl

#endif // KYLINLIST_ATTACHMENTCODEC_H
