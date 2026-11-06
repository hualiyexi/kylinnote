#include "AttachmentStore.h"

namespace kn {

AttachmentStore::AttachmentStore(const QString& dataDir)
    : m_root(dataDir.trimmed().isEmpty() ? QString()
                                         : QDir(dataDir).filePath(QStringLiteral("attachments"))) {
}

QString AttachmentStore::rootPath() const {
    return m_root;
}
QString AttachmentStore::filePath(const Attachment& attachment) const {
    return attachmentcodec::isSafeAttachmentRelativePath(attachment.relativePath)
               ? absolutePath(attachment.relativePath)
               : QString();
}

// 复制（不移动）外部文件到 <root>/<附件id>_<安全扩展名>，返回完整元数据。
// 失败时返回 id 为空的 Attachment 并填写 error；原始文件始终保留。
Attachment AttachmentStore::importFile(qint64 taskId,
                                       const QString& sourcePath,
                                       int sortOrder,
                                       QString* error) const {
    if (taskId <= 0)
        return failAttachment(QStringLiteral("附件任务 id 非法"), error);
    if (sourcePath.trimmed().isEmpty())
        return failAttachment(QStringLiteral("附件路径不能为空"), error);
    const QFileInfo info(sourcePath);
    if (!info.exists() || !info.isFile())
        return failAttachment(QStringLiteral("附件必须是非空的常规文件: ") + sourcePath, error);
    if (info.size() <= 0)
        return failAttachment(QStringLiteral("附件不能为空文件"), error);
    if (info.size() > attachmentcodec::kMaximumAttachmentBytes)
        return failAttachment(QStringLiteral("附件超过 2 MiB 大小限制"), error);

    QByteArray content;
    {
        QFile source(info.absoluteFilePath());
        if (!source.open(QIODevice::ReadOnly))
            return failAttachment(QStringLiteral("附件文件不可读: ") + source.errorString(), error);
        content = source.read(attachmentcodec::kMaximumAttachmentBytes + 1);
        if (source.error() != QFileDevice::NoError || content.size() != info.size())
            return failAttachment(QStringLiteral("读取附件内容失败"), error);
    }
    if (content.size() > attachmentcodec::kMaximumAttachmentBytes)
        return failAttachment(QStringLiteral("附件超过 2 MiB 大小限制"), error);

    // 图片必须由 QImageReader 按内容真实解码识别；仅凭扩展名不算图片。
    QString mime;
    QString extension;
    const bool isImage = decodeImage(content, &mime, &extension);
    if (!isImage) {
        extension = attachmentcodec::safeAttachmentExtension(info.suffix());
        mime = documentMime(extension);
    }

    Attachment attachment;
    attachment.taskId = taskId;
    attachment.originalName = info.fileName();
    attachment.mime = mime;
    attachment.size = content.size();
    attachment.isImage = isImage;
    attachment.createdAt = QDateTime::currentDateTimeUtc();
    attachment.sortOrder = sortOrder;
    if (!ensureStorage(error))
        return Attachment();
    for (int attempt = 0; attempt < 4; ++attempt) {
        attachment.id = attachmentcodec::newAttachmentId();
        attachment.relativePath = attachmentcodec::attachmentRelativePath(attachment.id, extension);
        if (!QFile::exists(absolutePath(attachment.relativePath)))
            break;
        attachment.id.clear();
    }
    if (attachment.id.isEmpty())
        return failAttachment(QStringLiteral("无法分配唯一的附件存储名"), error);
    if (!attachmentcodec::validateAttachment(attachment, error))
        return Attachment();
    if (!writeFile(attachment.relativePath, content, error))
        return Attachment();
    if (error)
        error->clear();
    return attachment;
}

// 以原子方式写入附件内容（备份导入的分阶段写入也走这里）。
bool AttachmentStore::writeFile(const QString& relativePath,
                                const QByteArray& content,
                                QString* error) const {
    if (!attachmentcodec::isSafeAttachmentRelativePath(relativePath))
        return fail(QStringLiteral("附件存储路径不安全"), error);
    if (content.isEmpty() || content.size() > attachmentcodec::kMaximumAttachmentBytes)
        return fail(QStringLiteral("附件内容为空或超过 2 MiB 限制"), error);
    if (!ensureStorage(error))
        return false;
    QSaveFile file(absolutePath(relativePath));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("创建附件文件失败: ") + file.errorString(), error);
    if (file.write(content) != content.size()) {
        file.cancelWriting();
        return fail(QStringLiteral("写入附件内容失败: ") + file.errorString(), error);
    }
    if (!file.commit())
        return fail(QStringLiteral("提交附件文件失败: ") + file.errorString(), error);
    if (error)
        error->clear();
    return true;
}

bool AttachmentStore::readContent(const Attachment& attachment, QByteArray* out, QString* error) const {
    if (!out || !attachmentcodec::isSafeAttachmentRelativePath(attachment.relativePath))
        return fail(QStringLiteral("附件存储路径不安全"), error);
    QFile file(absolutePath(attachment.relativePath));
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("附件文件缺失或不可读: ") + file.errorString(), error);
    const QByteArray content = file.read(attachmentcodec::kMaximumAttachmentBytes + 1);
    if (file.error() != QFileDevice::NoError)
        return fail(QStringLiteral("读取附件失败: ") + file.errorString(), error);
    if (content.isEmpty() || content.size() > attachmentcodec::kMaximumAttachmentBytes ||
        content.size() != attachment.size)
        return fail(QStringLiteral("附件文件与元数据大小不一致"), error);
    *out = content;
    if (error)
        error->clear();
    return true;
}

// 物理清理由调用者在 DB 成功后执行；失败只保留孤儿文件，绝不回滚任务删除。
bool AttachmentStore::removeFile(const QString& relativePath, QString* error) const {
    if (!attachmentcodec::isSafeAttachmentRelativePath(relativePath))
        return fail(QStringLiteral("附件存储路径不安全"), error);
    const QString path = absolutePath(relativePath);
    if (!QFile::exists(path)) {
        if (error)
            error->clear();
        return true;  // 已不存在视为清理完成
    }
    if (!QFile::remove(path))
        return fail(QStringLiteral("附件文件清理失败，保留为孤儿文件: ") + relativePath, error);
    if (error)
        error->clear();
    return true;
}

bool AttachmentStore::openWithDefaultProgram(const Attachment& attachment, QString* error) const {
    if (!attachmentcodec::isSafeAttachmentRelativePath(attachment.relativePath))
        return fail(QStringLiteral("附件存储路径不安全"), error);
    const QString path = absolutePath(attachment.relativePath);
    if (!QFile::exists(path))
        return fail(QStringLiteral("附件文件不存在"), error);
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        return fail(QStringLiteral("无法调用系统默认程序打开附件"), error);
    if (error)
        error->clear();
    return true;
}

// 导出到用户选择的目标路径，原子覆盖；不修改存储中的原文件。
bool AttachmentStore::copyTo(const Attachment& attachment,
                             const QString& destinationPath,
                             QString* error) const {
    if (destinationPath.trimmed().isEmpty())
        return fail(QStringLiteral("目标路径不能为空"), error);
    QByteArray content;
    if (!readContent(attachment, &content, error))
        return false;
    QSaveFile file(destinationPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("创建目标文件失败: ") + file.errorString(), error);
    if (file.write(content) != content.size()) {
        file.cancelWriting();
        return fail(QStringLiteral("写入目标文件失败: ") + file.errorString(), error);
    }
    if (!file.commit())
        return fail(QStringLiteral("提交目标文件失败: ") + file.errorString(), error);
    if (error)
        error->clear();
    return true;
}

}  // namespace kn
