#include "AttachmentStore.h"

#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace kl {

#if defined(Q_OS_LINUX)
namespace {

// Keep the bundle's loader and plugin paths out of desktop viewers.
void restorePreWrapperEnvironment(QProcessEnvironment *environment)
{
    const char *const variables[] = {
        "LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QT_QPA_PLATFORM"
    };
    for (const char *name : variables) {
        const QString variable = QString::fromLatin1(name);
        const QString marker = QStringLiteral("KYLINLIST_PRE_") + variable;
        const QString original = environment->value(marker);
        if (original == QLatin1String("0")) environment->remove(variable);
        else if (original.startsWith(QLatin1Char('1'))) environment->insert(variable, original.mid(1));
        environment->remove(marker);
    }
}

bool openerFail(const QString &message, QString *error)
{
    if (error) *error = message;
    return false;
}

bool openWithSystemOpener(const QString &path, QString *error)
{
    QString program;
    QStringList arguments;
    const QString xdgOpen = QStandardPaths::findExecutable(QStringLiteral("xdg-open"));
    if (!xdgOpen.isEmpty()) {
        program = xdgOpen;
    } else {
        const QString gio = QStandardPaths::findExecutable(QStringLiteral("gio"));
        if (gio.isEmpty()) return openerFail(QStringLiteral("未找到 xdg-open/gio 系统打开工具"), error);
        program = gio;
        arguments.append(QStringLiteral("open"));
    }
    arguments.append(path);
    // argv 直传（无 shell 拼接）；子进程环境基于当前桌面 session，只精确还原 wrapper
    // 注入的四个变量。实例版 startDetached() 遵循 setProcessEnvironment（Qt 5.12
    // qprocess_unix.cpp：QProcessPrivate::startDetached 由 environment 生成 envp 后
    // execve），并经双 fork + setsid 完全脱离本进程生命周期。
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    restorePreWrapperEnvironment(&environment);
    QProcess opener;
    opener.setProgram(program);
    opener.setArguments(arguments);
    opener.setProcessEnvironment(environment);
    // detached 只能确认请求已交接给系统打开工具，不代表外部程序真的打开了文件。
    if (!opener.startDetached())
        return openerFail(QStringLiteral("无法调用系统默认程序打开附件"), error);
    if (error) error->clear();
    return true;
}

} // namespace
#endif // Q_OS_LINUX

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
    if (!attachmentcodec::validateAttachment(attachment, error) || !readContent(attachment, &content, error))
        return false;
    QString copyPath;
    if (!ensureOpenCopy(attachment, content, &copyPath, error)) return false;
#if defined(Q_OS_LINUX)
    return openWithSystemOpener(copyPath, error);
#else
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(copyPath)))
        return fail(QStringLiteral("无法调用系统默认程序打开附件"), error);
    if (error) error->clear(); return true;
#endif
}
QString AttachmentStore::openCopySuffix(const Attachment &attachment)
{
    // 后缀只能来自已校验的受管存储名（id_<ext>），不信任用户提供的原始文件名。
    return attachmentcodec::safeAttachmentExtension(
        attachment.relativePath.mid(attachment.id.size() + 1));
}
bool AttachmentStore::ensureOpenCopy(const Attachment &attachment, const QByteArray &content,
                                     QString *path, QString *error) const
{
    if (!attachmentcodec::isSafeAttachmentRelativePath(attachment.relativePath))
        return fail(QStringLiteral("附件存储路径不安全"), error);
    if (m_openCopies.isNull()) {
        m_openCopies.reset(new QTemporaryDir(
            QDir::temp().filePath(QStringLiteral("kylinlist-open-XXXXXX"))));
        if (!m_openCopies->isValid()) {
            m_openCopies.reset();
            return fail(QStringLiteral("无法创建附件临时打开目录"), error);
        }
    }
    // 文件名 = 附件 id + 已验证 SHA 前缀 + 真实后缀：同一附件内容不变即命中复用，
    // 且外部程序拿到的是带点分隔后缀的常规文件名；目录与文件名均与原始文件名无关。
    const QString copyName = QStringLiteral("%1_%2.%3")
        .arg(attachment.id, attachment.sha256.left(16), openCopySuffix(attachment));
    const QString target = m_openCopies->filePath(copyName);
    const QFileInfo existing(target);
    bool current = false;
    if (existing.isFile() && !existing.isSymLink() && existing.size() == content.size()) {
        QFile copy(target);
        current = copy.open(QIODevice::ReadOnly) && copy.read(content.size() + 1) == content;
    }
    if (!current) {
        QSaveFile file(target); file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly))
            return fail(QStringLiteral("创建附件打开副本失败: ") + file.errorString(), error);
        if (file.write(content) != content.size()) {
            file.cancelWriting();
            return fail(QStringLiteral("写入附件打开副本失败"), error);
        }
        if (!file.commit())
            return fail(QStringLiteral("提交附件打开副本失败: ") + file.errorString(), error);
    }
    *path = target;
    if (error) error->clear();
    return true;
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
