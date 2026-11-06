#ifndef KYLINNOTE_COMMON_H
#define KYLINNOTE_COMMON_H

#include <QDateTime>
#include <QDir>
#include <QRect>
#include <QString>
#include <QStandardPaths>

namespace kn {

// 便签颜色，数值与数据库 color 列一致，默认黄色
enum class NoteColor {
    Yellow = 0,
    Blue   = 1,
    Green  = 2,
    Pink   = 3,
    Purple = 4,
    White  = 5
};

enum { ColorCount = 6 };

// 颜色枚举与数据库整列之间的安全转换
inline NoteColor colorFromInt(int v)
{
    if (v < 0 || v >= ColorCount) {
        return NoteColor::Yellow;
    }
    return static_cast<NoteColor>(v);
}

inline int colorToInt(NoteColor c)
{
    return static_cast<int>(c);
}

// 便签在窗口栈中的层级
//   OnDesktop：贴在桌面之上、所有应用窗口之下（默认，不遮挡其它软件）
//   Normal   ：普通窗口，与其它应用按焦点正常竞争
//   OnTop    ：始终置顶
enum class NoteLayer { OnDesktop = 0, Normal = 1, OnTop = 2 };

enum { LayerCount = 3 };

inline NoteLayer layerFromInt(int v)
{
    if (v < 0 || v >= LayerCount) {
        return NoteLayer::OnDesktop;
    }
    return static_cast<NoteLayer>(v);
}

inline int layerToInt(NoteLayer l)
{
    return static_cast<int>(l);
}

inline NoteLayer nextLayer(NoteLayer l)
{
    return layerFromInt((layerToInt(l) + 1) % LayerCount);
}

// 层级的人类可读名称，用于托盘提示
QString layerLabel(NoteLayer layer);

// ---- 常量 ----
enum {
    SaveDebounceMs = 300,       // 内容/几何变动后的防抖落库延迟
    SaveRetryMs    = 5000,      // 写库失败后的重试间隔
    MinWidth       = 280,
    MinHeight      = 240,
    DefaultWidth   = 340,
    DefaultHeight  = 360,
    MinFontSize    = 8,
    MaxFontSize    = 72,
    DefaultFontPt  = 10,
    TitleBarHeight = 36,
    GripSize       = 16,
    WindowReadyRetryMs = 500,   // 窗管未就绪时的重试间隔
    WindowReadyMaxRetry = 5
};

// Linux 保留 1.0 的数据位置；Windows 遵循本地应用数据目录。
// 测试/便携调试可显式指定独立目录，不污染真实任务。
inline QString dataDirPath()
{
    const QString overridePath = qEnvironmentVariable("KYLINNOTE_DATA_DIR");
    if (!overridePath.isEmpty()) {
        return QDir(overridePath).absolutePath();
    }
#ifdef Q_OS_WIN
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
#else
    return QDir::homePath() + QStringLiteral("/.local/share/kylinnote");
#endif
}

inline QString ensureDataDir()
{
    const QString path = dataDirPath();
    QDir dir(path);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return QString();
    }
    return path;
}

inline QString databasePath()
{
    return dataDirPath() + QStringLiteral("/notes.db");
}

inline QString lockFilePath()
{
    return dataDirPath() + QStringLiteral("/.kylinnote.lock");
}

inline QString nowStamp()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

inline QString encodeGeometry(const QRect &r)
{
    return QStringLiteral("%1,%2,%3,%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

inline QRect decodeGeometry(const QString &s)
{
    const QStringList parts = s.split(QLatin1Char(','));
    if (parts.size() != 4) {
        return QRect();
    }
    bool okX = false, okY = false, okW = false, okH = false;
    const int x = parts.at(0).trimmed().toInt(&okX);
    const int y = parts.at(1).trimmed().toInt(&okY);
    const int w = parts.at(2).trimmed().toInt(&okW);
    const int h = parts.at(3).trimmed().toInt(&okH);
    if (!(okX && okY && okW && okH)) {
        return QRect();
    }
    return QRect(x, y, w, h);
}

} // namespace kn

#endif // KYLINNOTE_COMMON_H
