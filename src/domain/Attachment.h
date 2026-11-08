#ifndef KYLINLIST_ATTACHMENT_H
#define KYLINLIST_ATTACHMENT_H

#include <QDateTime>
#include <QString>

namespace kl {

enum class AttachmentFileState { Available = 0, Missing = 1, Corrupt = 2 };
inline QString attachmentFileStateName(AttachmentFileState state)
{
    switch (state) {
    case AttachmentFileState::Available: return QStringLiteral("available");
    case AttachmentFileState::Missing: return QStringLiteral("missing");
    case AttachmentFileState::Corrupt: return QStringLiteral("corrupt");
    }
    return QString();
}
inline AttachmentFileState attachmentFileStateFromName(const QString &name)
{
    if (name == QStringLiteral("missing")) return AttachmentFileState::Missing;
    if (name == QStringLiteral("corrupt")) return AttachmentFileState::Corrupt;
    return AttachmentFileState::Available;
}

struct Attachment {
    QString id;
    QString entityId;
    QString originalName;
    QString relativePath;
    QString mime;
    qint64 size = 0;
    QString sha256;
    bool isImage = false;
    AttachmentFileState localState = AttachmentFileState::Available;
    QDateTime createdAt;
    QDateTime verifiedAt;
    int sortOrder = 0;
    qint64 revision = 1;
    qint64 serverRevision = 0;
};

} // namespace kl

#endif // KYLINLIST_ATTACHMENT_H
