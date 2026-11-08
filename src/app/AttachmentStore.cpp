#include "AttachmentStore.h"

namespace kl {

AttachmentStore::AttachmentStore(const QString &dataDir)
    : m_root(dataDir.trimmed().isEmpty() ? QString() : QDir(dataDir).filePath(QStringLiteral("attachments"))) {}
QString AttachmentStore::rootPath() const { return m_root; }
QString AttachmentStore::filePath(const Attachment &attachment) const
{
    return attachmentcodec::isSafeAttachmentRelativePath(attachment.relativePath) ? absolutePath(attachment.relativePath) : QString();
}

Attachment AttachmentStore::importFile(const QString &entityId, const QString &sourcePath, int sortOrder, QString *error) const
{
    if (!isUuid(entityId)) return failAttachment(QStringLiteral("附件任务 id 必须为 UUID"), error);
    if (sourcePath.trimmed().isEmpty()) return failAttachment(QStringLiteral("附件路径不能为空"), error);
    const QFileInfo info(sourcePath);
    if (!info.exists() || !info.isFile()) return failAttachment(QStringLiteral("附件必须是常规文件: ") + sourcePath, error);
    if (info.size() <= 0 || info.size() > attachmentcodec::kMaximumAttachmentBytes)
        return failAttachment(QStringLiteral("附件必须大小为 1 字节到 2 MiB"), error);
    QByteArray content;
    {
        QFile source(info.absoluteFilePath());
        if (!source.open(QIODevice::ReadOnly)) return failAttachment(QStringLiteral("附件文件不可读: ") + source.errorString(), error);
        content = source.read(attachmentcodec::kMaximumAttachmentBytes + 1);
        if (source.error() != QFileDevice::NoError || content.size() != info.size()
            || content.size() > attachmentcodec::kMaximumAttachmentBytes)
            return failAttachment(QStringLiteral("读取附件内容失败"), error);
    }
    QString mime, extension;
    const bool isImage = decodeImage(content, &mime, &extension);
    if (!isImage) { extension = attachmentcodec::safeAttachmentExtension(info.suffix()); mime = documentMime(extension); }
    Attachment attachment;
    attachment.entityId = entityId; attachment.originalName = info.fileName(); attachment.mime = mime;
    attachment.size = content.size(); attachment.sha256 = sha256(content); attachment.isImage = isImage;
    attachment.localState = AttachmentFileState::Available;
    attachment.createdAt = utcNow(); attachment.verifiedAt = attachment.createdAt; attachment.sortOrder = sortOrder;
    if (!ensureStorage(error)) return Attachment();
    for (int attempt = 0; attempt < 4; ++attempt) {
        attachment.id = newUuid(); attachment.relativePath = attachmentcodec::attachmentRelativePath(attachment.id, extension);
        if (!QFile::exists(absolutePath(attachment.relativePath))) break;
        attachment.id.clear();
    }
    if (attachment.id.isEmpty()) return failAttachment(QStringLiteral("无法分配唯一附件存储名"), error);
    if (!attachmentcodec::validateAttachment(attachment, error)) return Attachment();
    if (!writeFile(attachment.relativePath, content, error)) return Attachment();
    if (error) error->clear();
    return attachment;
}

bool AttachmentStore::writeFile(const QString &relativePath, const QByteArray &content, QString *error) const
{
    if (!attachmentcodec::isSafeAttachmentRelativePath(relativePath)) return fail(QStringLiteral("附件存储路径不安全"), error);
    if (content.isEmpty() || content.size() > attachmentcodec::kMaximumAttachmentBytes)
        return fail(QStringLiteral("附件内容为空或超过 2 MiB"), error);
    if (!ensureStorage(error)) return false;
    QSaveFile file(absolutePath(relativePath)); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return fail(QStringLiteral("创建附件文件失败: ") + file.errorString(), error);
    if (file.write(content) != content.size()) { file.cancelWriting(); return fail(QStringLiteral("写入附件失败"), error); }
    if (!file.commit()) return fail(QStringLiteral("提交附件文件失败: ") + file.errorString(), error);
    if (error) error->clear();
    return true;
}

bool AttachmentStore::readContent(const Attachment &attachment, QByteArray *out, QString *error) const
{
    if (!out || !attachmentcodec::isSafeAttachmentRelativePath(attachment.relativePath))
        return fail(QStringLiteral("附件存储路径不安全"), error);
    QFile file(absolutePath(attachment.relativePath));
    if (!file.open(QIODevice::ReadOnly)) return fail(QStringLiteral("附件文件缺失或不可读: ") + file.errorString(), error);
    const QByteArray content = file.read(attachmentcodec::kMaximumAttachmentBytes + 1);
    if (file.error() != QFileDevice::NoError) return fail(QStringLiteral("读取附件失败"), error);
    if (content.isEmpty() || content.size() > attachmentcodec::kMaximumAttachmentBytes || content.size() != attachment.size)
        return fail(QStringLiteral("附件文件与元数据大小不一致"), error);
    if (sha256(content) != attachment.sha256) return fail(QStringLiteral("附件 SHA256 校验失败"), error);
    *out = content;
    if (error) error->clear();
    return true;
}

bool AttachmentStore::removeFile(const QString &relativePath, QString *error) const
{
    if (!attachmentcodec::isSafeAttachmentRelativePath(relativePath)) return fail(QStringLiteral("附件存储路径不安全"), error);
    const QString path = absolutePath(relativePath);
    if (!QFile::exists(path)) { if (error) error->clear(); return true; }
    if (!QFile::remove(path)) return fail(QStringLiteral("附件清理失败，保留为孤儿文件: ") + relativePath, error);
    if (error) error->clear(); return true;
}
bool AttachmentStore::openWithDefaultProgram(const Attachment &attachment, QString *error) const
{
    QByteArray content;
    if (!readContent(attachment, &content, error)) return false;
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(absolutePath(attachment.relativePath))))
        return fail(QStringLiteral("无法调用系统默认程序打开附件"), error);
    if (error) error->clear(); return true;
}
bool AttachmentStore::copyTo(const Attachment &attachment, const QString &destinationPath, QString *error) const
{
    if (destinationPath.trimmed().isEmpty()) return fail(QStringLiteral("目标路径不能为空"), error);
    QByteArray content;
    if (!readContent(attachment, &content, error)) return false;
    QSaveFile file(destinationPath); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return fail(QStringLiteral("创建目标文件失败: ") + file.errorString(), error);
    if (file.write(content) != content.size()) { file.cancelWriting(); return fail(QStringLiteral("写入目标文件失败"), error); }
    if (!file.commit()) return fail(QStringLiteral("提交目标文件失败: ") + file.errorString(), error);
    if (error) error->clear(); return true;
}

} // namespace kl
