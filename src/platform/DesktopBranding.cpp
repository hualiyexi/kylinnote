#include "DesktopBranding.h"

#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QIODevice>
#include <QSaveFile>
#include <QStringList>

#include "../app/Branding.h"

namespace kl {
namespace DesktopBranding {
namespace {

// 与 app/Branding.cpp 的默认显示名保持一致：displayName 等于默认名时
// 视为未定制，不改写 desktop 的 Name 键（避免生成无意义的覆盖）。
const QString kDefaultDisplayName = QStringLiteral("麒麟清单");
// 输出根与源 desktop 的默认值；测试用环境变量整体重定向到临时目录。
const QString kDefaultRoot = QStringLiteral("/usr/local");
const QString kDefaultSourceDesktop = QStringLiteral("/usr/share/applications/KylinList.desktop");
const QString kDesktopFileName = QStringLiteral("KylinList.desktop");
const QString kApplicationsSubdir = QStringLiteral("share/applications");
const QString kBrandingSubdir = QStringLiteral("share/kylinlist/branding");
// 自定义图标的固定文件名前缀，扩展名跟随源 logo；本模块只清理
// “active.*” 命名的文件，绝不碰目录里的其他内容。
const QString kActiveIconPrefix = QStringLiteral("active.");
const QString kDesktopGroup = QStringLiteral("[Desktop Entry]");
const QString kManagedKey = QStringLiteral("X-KylinList-Managed-Branding");
const QString kNameKey = QStringLiteral("Name");
const QString kNameZhCnKey = QStringLiteral("Name[zh_CN]");
const QString kIconKey = QStringLiteral("Icon");

struct Paths {
    QString root;           // 输出根（默认 /usr/local）
    QString sourceDesktop;  // 安装包默认入口（默认 /usr/share/applications/...）
    QString targetDesktop;  // <root>/share/applications/KylinList.desktop
    QString brandingDir;    // <root>/share/kylinlist/branding
};

struct LogoMaterial {
    bool valid = false;   // 是否通过 QImageReader 真实解码验证
    QString sourcePath;   // Branding 解析出的 logo 原始路径
    QString activePath;   // <brandingDir>/active.<原扩展名>
};

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    qWarning() << "[kylinlist] DesktopBranding:" << message;
    return false;
}

// 比 QFileInfo::exists 多覆盖“悬空符号链接”等边缘情况。
bool pathPresent(const QString &path)
{
    const QFileInfo info(path);
    return info.isSymLink() || info.exists();
}

bool isGroupHeader(const QString &trimmed)
{
    return trimmed.size() >= 2
        && trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']'));
}

Paths resolvePaths()
{
    Paths paths;
    const QString rootOverride = qEnvironmentVariable("KYLINLIST_DESKTOP_BRANDING_ROOT").trimmed();
    paths.root = rootOverride.isEmpty() ? kDefaultRoot : QDir(rootOverride).absolutePath();
    const QString sourceOverride = qEnvironmentVariable("KYLINLIST_DESKTOP_ENTRY_SOURCE").trimmed();
    paths.sourceDesktop = sourceOverride.isEmpty()
        ? kDefaultSourceDesktop : QDir(sourceOverride).absolutePath();
    paths.targetDesktop = QDir(QDir(paths.root).filePath(kApplicationsSubdir)).filePath(kDesktopFileName);
    paths.brandingDir = QDir(paths.root).filePath(kBrandingSubdir);
    return paths;
}

// desktop 文件规范要求的值转义：反斜杠、换行、回车、制表符。
QString escapeDesktopValue(const QString &value)
{
    QString escaped;
    escaped.reserve(value.size() + 8);
    for (const QChar ch : value) {
        if (ch == QLatin1Char('\\')) escaped += QStringLiteral("\\\\");
        else if (ch == QLatin1Char('\n')) escaped += QStringLiteral("\\n");
        else if (ch == QLatin1Char('\r')) escaped += QStringLiteral("\\r");
        else if (ch == QLatin1Char('\t')) escaped += QStringLiteral("\\t");
        else escaped += ch;
    }
    return escaped;
}

// 判断 desktop 是否带本模块的管理标记（须在 [Desktop Entry] 组内且值为 true）。
bool isManagedDesktop(const QString &desktopPath)
{
    QFile file(desktopPath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    file.close();
    const QString prefix = kManagedKey + QLatin1Char('=');
    bool inEntryGroup = false;
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (isGroupHeader(trimmed)) {
            inEntryGroup = trimmed == kDesktopGroup;
            continue;
        }
        if (inEntryGroup && line.startsWith(prefix)
                && line.mid(prefix.size()).trimmed() == QStringLiteral("true")) {
            return true;
        }
    }
    return false;
}

// 基于源 desktop 生成覆盖内容：仅改写 [Desktop Entry] 组内的
// Name / Name[zh_CN]（有自定义显示名时）与 Icon（有可用自定义图标时），
// 并写入管理标记；其余行原样保留。缺失的键插到组头之后。
// 失败返回空串并给出中文错误说明。
QString buildOverrideContent(const QString &sourceContent,
                             bool customName, const QString &displayName,
                             bool customIcon, const QString &iconValue,
                             QString *errorMessage)
{
    QStringList lines = sourceContent.split(QLatin1Char('\n'));
    bool inEntryGroup = false;
    int entryHeaderIndex = -1;
    bool sawName = false;
    bool sawNameZhCn = false;
    bool sawIcon = false;
    bool sawMarker = false;

    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i);
        const QString trimmed = line.trimmed();
        if (isGroupHeader(trimmed)) {
            inEntryGroup = trimmed == kDesktopGroup;
            if (inEntryGroup && entryHeaderIndex < 0) entryHeaderIndex = i;
            continue;
        }
        if (!inEntryGroup || trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) continue;
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals <= 0) continue;  // 无 '=' 或以 '=' 开头的行不是键值对
        const QString key = line.left(equals);
        if (key == kManagedKey) {
            lines[i] = kManagedKey + QStringLiteral("=true");
            sawMarker = true;
        } else if (key == kNameKey || key == kNameZhCnKey) {
            // [Desktop Action] 等其他组的 Name 不在 inEntryGroup 内，不会误改。
            if (customName)
                lines[i] = key + QLatin1Char('=') + escapeDesktopValue(displayName);
            if (key == kNameKey) sawName = true;
            else sawNameZhCn = true;
        } else if (key == kIconKey) {
            if (customIcon)
                lines[i] = kIconKey + QLatin1Char('=') + escapeDesktopValue(iconValue);
            sawIcon = true;
        }
    }

    if (entryHeaderIndex < 0) {
        if (errorMessage)
            *errorMessage = QStringLiteral("源 desktop 缺少 %1 组，无法生成覆盖").arg(kDesktopGroup);
        return QString();
    }

    // 缺失的键插入组头之后（倒序插入保持自然顺序）；键序不影响 desktop 解析。
    QStringList insertions;
    if (customName && !sawName)
        insertions << kNameKey + QLatin1Char('=') + escapeDesktopValue(displayName);
    if (customName && !sawNameZhCn)
        insertions << kNameZhCnKey + QLatin1Char('=') + escapeDesktopValue(displayName);
    if (customIcon && !sawIcon)
        insertions << kIconKey + QLatin1Char('=') + escapeDesktopValue(iconValue);
    if (!sawMarker) insertions << kManagedKey + QStringLiteral("=true");
    for (int i = insertions.size() - 1; i >= 0; --i)
        lines.insert(entryHeaderIndex + 1, insertions.at(i));

    QString content = lines.join(QLatin1Char('\n'));
    if (!content.endsWith(QLatin1Char('\n'))) content += QLatin1Char('\n');
    return content;
}

// 只用 QImageReader 真实解码验证 logo；坏图、文件缺失、缺解码插件都视为
// 无自定义图标（只回退图标，不影响显示名）。可用时解析 active 目标路径。
LogoMaterial resolveLogo(const QString &brandingDir, const QString &sourceLogoPath)
{
    LogoMaterial material;
    material.sourcePath = sourceLogoPath;
    if (sourceLogoPath.isEmpty()) return material;
    const QFileInfo info(sourceLogoPath);
    if (!info.isFile()) return material;
    QImageReader reader(sourceLogoPath);
    if (reader.read().isNull()) return material;  // 必须真实解码成功

    QString extension = info.suffix().toLower();  // 保留原扩展名
    if (extension.isEmpty())
        extension = QString::fromLatin1(QImageReader::imageFormat(sourceLogoPath)).toLower();
    if (extension.isEmpty()) extension = QStringLiteral("png");
    material.activePath = QDir(brandingDir).filePath(kActiveIconPrefix + extension);
    material.valid = true;
    return material;
}

// QSaveFile 原子替换写入。
bool writeBytesAtomically(const QString &path, const QByteArray &bytes,
                          const QString &what, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return fail(error, QStringLiteral("无法创建%1所在目录：%2").arg(what, path));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(error, QStringLiteral("无法写入%1：%2（%3）")
                    .arg(what, path, file.errorString()));
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return fail(error, QStringLiteral("写入%1内容失败：%2").arg(what, path));
    }
    if (!file.commit())
        return fail(error, QStringLiteral("保存%1失败：%2（%3）")
                    .arg(what, path, file.errorString()));
    return true;
}

// 删除本模块写入的 active.* 图标（含旧扩展名），不动目录里其他文件。
bool removeActiveIcons(const QString &brandingDir, QString *error)
{
    QDir dir(brandingDir);
    if (!dir.exists()) return true;
    const QStringList entries =
        dir.entryList(QStringList() << (kActiveIconPrefix + QLatin1Char('*')), QDir::Files);
    for (const QString &name : entries) {
        const QString path = dir.filePath(name);
        if (!QFile::remove(path))
            return fail(error, QStringLiteral("无法删除图标文件：%1").arg(path));
    }
    return true;
}

// 复制 logo 到 active.<ext>，随后清理其他扩展名的旧 active 图标。
bool installActiveIcon(const Paths &paths, const LogoMaterial &logo, QString *error)
{
    QFile source(logo.sourcePath);
    if (!source.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("无法读取自定义图标：%1（%2）")
                    .arg(logo.sourcePath, source.errorString()));
    const QByteArray bytes = source.readAll();
    source.close();
    if (!writeBytesAtomically(logo.activePath, bytes, QStringLiteral("图标文件"), error))
        return false;

    QDir dir(paths.brandingDir);
    const QString keep = QFileInfo(logo.activePath).fileName();
    const QStringList entries =
        dir.entryList(QStringList() << (kActiveIconPrefix + QLatin1Char('*')), QDir::Files);
    for (const QString &name : entries) {
        if (name == keep) continue;
        if (!QFile::remove(dir.filePath(name)))
            return fail(error, QStringLiteral("无法清理旧图标：%1").arg(dir.filePath(name)));
    }
    return true;
}

// 尽力移除本模块自建的空目录（<root>/share/kylinlist 与其下的 branding）；
// 目录非空或移除失败都静默忽略，绝不递归删除。
void removeEmptyBrandingDirs(const Paths &paths)
{
    const QString kylinlistDir = QFileInfo(paths.brandingDir).absolutePath();  // share/kylinlist
    const QString shareDir = QFileInfo(kylinlistDir).absolutePath();          // share
    QDir(kylinlistDir).rmdir(QStringLiteral("branding"));
    QDir(shareDir).rmdir(QFileInfo(kylinlistDir).fileName());
}

// 无有效自定义时的回退：删除自己管理的 desktop 与 active 图标。
// 目标存在但无管理标记（管理员手工文件）时保留不动，也不算错误。
bool removeManagedOverride(const Paths &paths, QString *error)
{
    if (pathPresent(paths.targetDesktop) && isManagedDesktop(paths.targetDesktop)) {
        if (!QFile::remove(paths.targetDesktop))
            return fail(error, QStringLiteral("无法删除本程序管理的 desktop：%1")
                        .arg(paths.targetDesktop));
    }
    if (!removeActiveIcons(paths.brandingDir, error)) return false;
    removeEmptyBrandingDirs(paths);
    return true;
}

} // namespace

bool refreshSystem(QString *error)
{
    if (error) error->clear();
    Branding::load();
    const Paths paths = resolvePaths();

    const QString displayName = Branding::displayName();
    const bool customName = displayName != kDefaultDisplayName;  // 等于默认名视为未定制
    const LogoMaterial logo = resolveLogo(paths.brandingDir, Branding::logoPath());

    // 仅在“自定义显示名”或“可解码自定义图标”任一存在时才生成覆盖；
    // 否则删除自建 override，回退安装包默认入口（幂等）。
    if (!customName && !logo.valid)
        return removeManagedOverride(paths, error);

    // 目标已存在但不是本程序管理的文件：拒绝覆盖，交由管理员处理。
    if (pathPresent(paths.targetDesktop) && !isManagedDesktop(paths.targetDesktop))
        return fail(error, QStringLiteral("目标 desktop 已存在且不是本程序管理，拒绝覆盖：%1")
                    .arg(paths.targetDesktop));

    QFile sourceFile(paths.sourceDesktop);
    if (!sourceFile.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("无法读取源 desktop：%1（%2）")
                    .arg(paths.sourceDesktop, sourceFile.errorString()));
    const QString sourceContent = QString::fromUtf8(sourceFile.readAll());
    sourceFile.close();

    QString errorMessage;
    const QString content = buildOverrideContent(sourceContent, customName, displayName,
                                                 logo.valid, logo.activePath, &errorMessage);
    if (content.isNull()) return fail(error, errorMessage);

    // 先落图标再写 desktop：保证 desktop 引用的图标文件总是存在。
    if (logo.valid) {
        if (!installActiveIcon(paths, logo, error)) return false;
    } else if (!removeActiveIcons(paths.brandingDir, error)) {
        // 名称有效但图标无效：清掉不再被引用的旧 active 图标。
        return false;
    }

    return writeBytesAtomically(paths.targetDesktop, content.toUtf8(),
                                QStringLiteral("desktop 文件"), error);
}

bool clearSystem(QString *error)
{
    if (error) error->clear();
    const Paths paths = resolvePaths();

    // 只删除带管理标记的 desktop；管理员手工文件原样保留。
    if (pathPresent(paths.targetDesktop) && isManagedDesktop(paths.targetDesktop)) {
        if (!QFile::remove(paths.targetDesktop))
            return fail(error, QStringLiteral("无法删除本程序管理的 desktop：%1")
                        .arg(paths.targetDesktop));
    }
    if (!removeActiveIcons(paths.brandingDir, error)) return false;
    removeEmptyBrandingDirs(paths);
    return true;
}

} // namespace DesktopBranding
} // namespace kl
