#include "AttachmentAppearance.h"

#include <QFileInfo>
#include <QStringList>

namespace kl {
namespace AttachmentAppearance {

FileType fileType(const Attachment& attachment) {
    if (attachment.isImage) return FileType::Image;
    const QString suffix = QFileInfo(attachment.originalName).suffix().toLower();
    if (QStringList{QStringLiteral("doc"), QStringLiteral("docx"), QStringLiteral("odt"),
                    QStringLiteral("rtf")}.contains(suffix)) return FileType::Document;
    if (QStringList{QStringLiteral("xls"), QStringLiteral("xlsx"), QStringLiteral("ods"),
                    QStringLiteral("csv")}.contains(suffix)) return FileType::Spreadsheet;
    if (QStringList{QStringLiteral("ppt"), QStringLiteral("pptx"),
                    QStringLiteral("odp")}.contains(suffix)) return FileType::Presentation;
    if (suffix == QLatin1String("pdf")) return FileType::Pdf;
    if (QStringList{QStringLiteral("txt"), QStringLiteral("md"), QStringLiteral("markdown"),
                    QStringLiteral("json"), QStringLiteral("xml"), QStringLiteral("log"),
                    QStringLiteral("yaml"), QStringLiteral("yml")}.contains(suffix)) return FileType::Text;
    if (QStringList{QStringLiteral("zip"), QStringLiteral("7z"), QStringLiteral("rar"),
                    QStringLiteral("tar"), QStringLiteral("gz"), QStringLiteral("bz2"),
                    QStringLiteral("xz")}.contains(suffix)) return FileType::Archive;
    if (QStringList{QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("flac"),
                    QStringLiteral("ogg"), QStringLiteral("m4a"),
                    QStringLiteral("aac")}.contains(suffix)) return FileType::Audio;
    if (QStringList{QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("avi"),
                    QStringLiteral("mov"), QStringLiteral("webm")}.contains(suffix)) return FileType::Video;

    const QString mime = attachment.mime.toLower();
    if (mime == QLatin1String("application/pdf")) return FileType::Pdf;
    if (mime == QLatin1String("application/msword") || mime.contains(QLatin1String("wordprocessingml"))
        || mime == QLatin1String("application/vnd.oasis.opendocument.text")) return FileType::Document;
    if (mime.contains(QLatin1String("spreadsheet")) || mime.contains(QLatin1String("ms-excel")))
        return FileType::Spreadsheet;
    if (mime.contains(QLatin1String("presentation")) || mime.contains(QLatin1String("ms-powerpoint")))
        return FileType::Presentation;
    if (mime.startsWith(QLatin1String("audio/"))) return FileType::Audio;
    if (mime.startsWith(QLatin1String("video/"))) return FileType::Video;
    // 未识别的扩展名不能仅因内容是纯文本就伪装成 txt；如歌词 .lrc 保持未知文件。
    if (suffix.isEmpty() && mime.startsWith(QLatin1String("text/"))) return FileType::Text;
    return FileType::Other;
}

QString iconName(FileType type) {
    switch (type) {
        case FileType::Document: return QStringLiteral("doc");
        case FileType::Spreadsheet: return QStringLiteral("xls");
        case FileType::Presentation: return QStringLiteral("ppt");
        case FileType::Pdf: return QStringLiteral("pdf");
        case FileType::Text: return QStringLiteral("text");
        case FileType::Archive: return QStringLiteral("zip");
        case FileType::Audio: return QStringLiteral("audio");
        case FileType::Video: return QStringLiteral("video");
        case FileType::Image: return QStringLiteral("photo");
        case FileType::Other: return QStringLiteral("other");
    }
    return QStringLiteral("other");
}

QString typeName(FileType type) {
    switch (type) {
        case FileType::Document: return QStringLiteral("document");
        case FileType::Spreadsheet: return QStringLiteral("spreadsheet");
        case FileType::Presentation: return QStringLiteral("presentation");
        case FileType::Pdf: return QStringLiteral("pdf");
        case FileType::Text: return QStringLiteral("text");
        case FileType::Archive: return QStringLiteral("archive");
        case FileType::Audio: return QStringLiteral("audio");
        case FileType::Video: return QStringLiteral("video");
        case FileType::Image: return QStringLiteral("image");
        case FileType::Other: return QStringLiteral("other");
    }
    return QStringLiteral("other");
}

QString sizeLabel(qint64 bytes) {
    if (bytes < 1024) return QStringLiteral("%1 B").arg(qMax<qint64>(0, bytes));
    const bool megabytes = bytes >= 1024 * 1024;
    QString number = QString::number(bytes / (megabytes ? 1048576.0 : 1024.0), 'f', 2);
    while (number.endsWith(QLatin1Char('0'))) number.chop(1);
    if (number.endsWith(QLatin1Char('.'))) number.chop(1);
    return number + (megabytes ? QStringLiteral(" MB") : QStringLiteral(" KB"));
}

}  // namespace AttachmentAppearance
}  // namespace kl
