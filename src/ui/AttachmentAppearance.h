#pragma once

#include <QString>
#include "../domain/Attachment.h"

namespace kl {
namespace AttachmentAppearance {

enum class FileType { Document, Spreadsheet, Presentation, Pdf, Text, Archive, Audio, Video, Image, Other };

// 仅负责附件呈现；文件校验、受管复制和持久化仍由 AttachmentStore 处理。
FileType fileType(const Attachment& attachment);
QString iconName(FileType type);
QString typeName(FileType type);
QString sizeLabel(qint64 bytes);

}  // namespace AttachmentAppearance
}  // namespace kl
