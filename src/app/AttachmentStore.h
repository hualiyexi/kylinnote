#ifndef KYLINLIST_ATTACHMENTSTORE_H
#define KYLINLIST_ATTACHMENTSTORE_H

#include "../domain/AttachmentCodec.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QSaveFile>
#include <QUrl>

namespace kl {

// 仅管理 dataDir/attachments 下的受管文件；SQL 事务由 TaskRepository/TaskStore 负责。
class AttachmentStore
{
public:
    explicit AttachmentStore(const QString &dataDir);
    QString rootPath() const;
    QString filePath(const Attachment &attachment) const;
    Attachment importFile(const QString &entityId, const QString &sourcePath, int sortOrder, QString *error) const;
    bool writeFile(const QString &relativePath, const QByteArray &content, QString *error) const;
    bool readContent(const Attachment &attachment, QByteArray *out, QString *error) const;
    bool removeFile(const QString &relativePath, QString *error) const;
    bool openWithDefaultProgram(const Attachment &attachment, QString *error) const;
    bool copyTo(const Attachment &attachment, const QString &destinationPath, QString *error) const;
    static QString sha256(const QByteArray &content)
    {
        return QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
    }

private:
    static bool fail(const QString &message, QString *error)
    {
        if (error) *error = message;
        return false;
    }
    static Attachment failAttachment(const QString &message, QString *error)
    {
        fail(message, error); return Attachment();
    }
    bool ensureStorage(QString *error) const
    {
        if (m_root.isEmpty()) return fail(QStringLiteral("附件数据目录未配置"), error);
        if (!QDir().mkpath(m_root)) return fail(QStringLiteral("无法创建附件目录: ") + m_root, error);
        return true;
    }
    QString absolutePath(const QString &relativePath) const { return QDir(m_root).filePath(relativePath); }
    static QString imageMime(const QString &format)
    {
        if (format == QStringLiteral("jpg") || format == QStringLiteral("jpeg")) return QStringLiteral("image/jpeg");
        if (format == QStringLiteral("svg") || format == QStringLiteral("svgz")) return QStringLiteral("image/svg+xml");
        if (format == QStringLiteral("tif") || format == QStringLiteral("tiff")) return QStringLiteral("image/tiff");
        if (format == QStringLiteral("ico")) return QStringLiteral("image/x-icon");
        return QStringLiteral("image/") + format;
    }
    static bool decodeImage(const QByteArray &content, QString *mime, QString *extension)
    {
        QBuffer buffer; buffer.setData(content);
        if (!buffer.open(QIODevice::ReadOnly)) return false;
        QImageReader reader(&buffer); reader.setDecideFormatFromContent(true);
        if (!reader.canRead()) return false;
        const QByteArray format = reader.format().toLower();
        const QImage image = reader.read();
        if (image.isNull() || format.isEmpty()) return false;
        *extension = attachmentcodec::safeAttachmentExtension(QString::fromLatin1(format));
        *mime = imageMime(QString::fromLatin1(format)); return true;
    }
    static QString documentMime(const QString &extension)
    {
        static const struct { const char *ext; const char *mime; } known[] = {
            {"txt","text/plain"},{"md","text/markdown"},{"markdown","text/markdown"},{"log","text/plain"},
            {"ini","text/plain"},{"csv","text/csv"},{"json","application/json"},{"xml","application/xml"},
            {"html","text/html"},{"htm","text/html"},{"pdf","application/pdf"},{"rtf","application/rtf"},
            {"zip","application/zip"},{"gz","application/gzip"},{"7z","application/x-7z-compressed"},
            {"rar","application/vnd.rar"},{"mp3","audio/mpeg"},{"mp4","video/mp4"},{"doc","application/msword"},
            {"xls","application/vnd.ms-excel"},{"ppt","application/vnd.ms-powerpoint"},
            {"docx","application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
            {"xlsx","application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
            {"pptx","application/vnd.openxmlformats-officedocument.presentationml.presentation"}
        };
        for (const auto &entry : known) if (extension == QLatin1String(entry.ext)) return QString::fromLatin1(entry.mime);
        return QStringLiteral("application/octet-stream");
    }
    QString m_root;
};

} // namespace kl

#endif // KYLINLIST_ATTACHMENTSTORE_H
