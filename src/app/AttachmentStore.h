#ifndef KYLINNOTE_ATTACHMENTSTORE_H
#define KYLINNOTE_ATTACHMENTSTORE_H

#include "../domain/AttachmentCodec.h"

#include <QBuffer>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSaveFile>
#include <QUrl>

namespace kn {

// 独立的本地附件文件存储。只负责 dataDir/attachments 下的文件读写与安全命名，
// 不接触 SQL、不依赖 UI；元数据事务由 TaskRepository/TaskStore 负责。
// 实现位于 AttachmentStore.cpp；仓储与业务服务只通过该接口管理受管文件。
class AttachmentStore {
public:
    explicit AttachmentStore(const QString& dataDir);
    QString rootPath() const;

    QString filePath(const Attachment& attachment) const;

    // 复制（不移动）外部文件到 <root>/<附件id>_<安全扩展名>，返回完整元数据。

    // 失败时返回 id 为空的 Attachment 并填写 error；原始文件始终保留。

    Attachment importFile(qint64 taskId, const QString& sourcePath, int sortOrder, QString* error) const;

    // 以原子方式写入附件内容（备份导入的分阶段写入也走这里）。

    bool writeFile(const QString& relativePath, const QByteArray& content, QString* error) const;

    bool readContent(const Attachment& attachment, QByteArray* out, QString* error) const;

    // 物理清理由调用者在 DB 成功后执行；失败只保留孤儿文件，绝不回滚任务删除。

    bool removeFile(const QString& relativePath, QString* error) const;

    bool openWithDefaultProgram(const Attachment& attachment, QString* error) const;

    // 导出到用户选择的目标路径，原子覆盖；不修改存储中的原文件。

    bool copyTo(const Attachment& attachment, const QString& destinationPath, QString* error) const;

private:
    static bool fail(const QString& message, QString* error) {
        if (error)
            *error = message;
        return false;
    }

    static Attachment failAttachment(const QString& message, QString* error) {
        fail(message, error);
        return Attachment();
    }

    bool ensureStorage(QString* error) const {
        if (m_root.isEmpty())
            return fail(QStringLiteral("附件数据目录未配置"), error);
        if (!QDir().mkpath(m_root))
            return fail(QStringLiteral("无法创建附件目录: ") + m_root, error);
        return true;
    }

    QString absolutePath(const QString& relativePath) const {
        return QDir(m_root).filePath(relativePath);
    }

    // 真实解码验证：canRead 之后还必须成功 read，损坏文件不算图片。
    static bool decodeImage(const QByteArray& content, QString* mime, QString* extension) {
        QBuffer buffer;
        buffer.setData(content);
        if (!buffer.open(QIODevice::ReadOnly))
            return false;
        QImageReader reader(&buffer);
        reader.setDecideFormatFromContent(true);
        if (!reader.canRead())
            return false;
        const QByteArray format = reader.format().toLower();
        const QImage image = reader.read();
        if (image.isNull() || format.isEmpty())
            return false;
        *extension = attachmentcodec::safeAttachmentExtension(QString::fromLatin1(format));
        *mime = imageMime(QString::fromLatin1(format));
        return true;
    }

    static QString imageMime(const QString& format) {
        if (format == QStringLiteral("jpg") || format == QStringLiteral("jpeg"))
            return QStringLiteral("image/jpeg");
        if (format == QStringLiteral("png"))
            return QStringLiteral("image/png");
        if (format == QStringLiteral("gif"))
            return QStringLiteral("image/gif");
        if (format == QStringLiteral("bmp"))
            return QStringLiteral("image/bmp");
        if (format == QStringLiteral("webp"))
            return QStringLiteral("image/webp");
        if (format == QStringLiteral("svg") || format == QStringLiteral("svgz"))
            return QStringLiteral("image/svg+xml");
        if (format == QStringLiteral("tif") || format == QStringLiteral("tiff"))
            return QStringLiteral("image/tiff");
        if (format == QStringLiteral("ico"))
            return QStringLiteral("image/x-icon");
        return QStringLiteral("image/") + format;
    }

    // 非图片附件不限制扩展名；mime 仅作展示元数据，未知类型回退 octet-stream。
    static QString documentMime(const QString& extension) {
        static const struct {
            const char* ext;
            const char* mime;
        } kKnown[] = {
            {"txt", "text/plain"},
            {"md", "text/markdown"},
            {"markdown", "text/markdown"},
            {"log", "text/plain"},
            {"ini", "text/plain"},
            {"csv", "text/csv"},
            {"json", "application/json"},
            {"xml", "application/xml"},
            {"html", "text/html"},
            {"htm", "text/html"},
            {"pdf", "application/pdf"},
            {"rtf", "application/rtf"},
            {"zip", "application/zip"},
            {"gz", "application/gzip"},
            {"7z", "application/x-7z-compressed"},
            {"rar", "application/vnd.rar"},
            {"mp3", "audio/mpeg"},
            {"mp4", "video/mp4"},
            {"doc", "application/msword"},
            {"xls", "application/vnd.ms-excel"},
            {"ppt", "application/vnd.ms-powerpoint"},
            {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
            {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
            {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        };
        for (const auto& entry : kKnown) {
            if (extension == QLatin1String(entry.ext))
                return QString::fromLatin1(entry.mime);
        }
        return QStringLiteral("application/octet-stream");
    }

    QString m_root;
};

}  // namespace kn

#endif  // KYLINNOTE_ATTACHMENTSTORE_H
