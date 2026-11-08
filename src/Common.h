#ifndef KYLINLIST_COMMON_H
#define KYLINLIST_COMMON_H

#include <QDateTime>
#include <QDir>
#include <QRect>
#include <QString>
#include <QStandardPaths>
#include <QTimeZone>
#include <QUuid>

namespace kl {

inline QString productId() { return QStringLiteral("kylinlist"); }
inline QString productName() { return QStringLiteral("KylinList"); }
inline QString productEnvironmentPrefix() { return QStringLiteral("KYLINLIST"); }
inline QString defaultTimeZone() { return QStringLiteral("Asia/Shanghai"); }

enum class NoteColor { Yellow = 0, Blue = 1, Green = 2, Pink = 3, Purple = 4, White = 5 };
enum { ColorCount = 6 };

inline NoteColor colorFromInt(int value)
{
    return value >= 0 && value < ColorCount ? static_cast<NoteColor>(value) : NoteColor::Yellow;
}
inline int colorToInt(NoteColor color) { return static_cast<int>(color); }

enum class NoteLayer { OnDesktop = 0, Normal = 1, OnTop = 2 };
enum { LayerCount = 3 };
inline NoteLayer layerFromInt(int value)
{
    return value >= 0 && value < LayerCount ? static_cast<NoteLayer>(value) : NoteLayer::OnDesktop;
}
inline int layerToInt(NoteLayer layer) { return static_cast<int>(layer); }
inline NoteLayer nextLayer(NoteLayer layer)
{
    return layerFromInt((layerToInt(layer) + 1) % LayerCount);
}
QString layerLabel(NoteLayer layer);

enum {
    SaveDebounceMs = 300,
    SaveRetryMs = 5000,
    MinWidth = 280,
    MinHeight = 240,
    DefaultWidth = 340,
    DefaultHeight = 360,
    MinFontSize = 8,
    MaxFontSize = 72,
    DefaultFontPt = 10,
    TitleBarHeight = 36,
    GripSize = 16,
    WindowReadyRetryMs = 500,
    WindowReadyMaxRetry = 5
};

// 新产品只读取自己的目录和 kylinlist.db。旧 notes.db 不迁移、不读取也不删除。
inline QString dataDirPath()
{
    const QString overridePath = qEnvironmentVariable("KYLINLIST_DATA_DIR");
    if (!overridePath.isEmpty()) return QDir(overridePath).absolutePath();
#ifdef Q_OS_WIN
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
#else
    return QDir::homePath() + QStringLiteral("/.local/share/kylinlist");
#endif
}

inline QString ensureDataDir()
{
    const QString path = dataDirPath();
    return path.isEmpty() || (!QDir(path).exists() && !QDir(path).mkpath(QStringLiteral(".")))
        ? QString() : path;
}
inline QString databasePath() { return dataDirPath() + QStringLiteral("/kylinlist.db"); }
inline QString lockFilePath() { return dataDirPath() + QStringLiteral("/.kylinlist.lock"); }

inline QString newUuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

inline bool isUuid(const QString &id)
{
    if (id.size() != 36) return false;
    for (int i = 0; i < id.size(); ++i) {
        const QChar ch = id.at(i);
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (ch != QLatin1Char('-')) return false;
        } else if (!((ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
                     || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f')))) {
            return false;
        }
    }
    return true;
}

inline qint64 utcMs(const QDateTime &value)
{
    return value.isValid() ? value.toUTC().toMSecsSinceEpoch() : 0;
}
inline QDateTime fromUtcMs(qint64 value)
{
    return QDateTime::fromMSecsSinceEpoch(value, Qt::UTC);
}
inline QDateTime utcNow() { return QDateTime::currentDateTimeUtc(); }
inline QTimeZone appTimeZone() { return QTimeZone(defaultTimeZone().toUtf8()); }
// appNow 是绝对当前时刻（UTC）；显示给 UI 时再经 toAppTime 转换。
inline QDateTime appNow() { return utcNow(); }
inline QDateTime toAppTime(const QDateTime &value)
{
    return value.isValid() ? value.toTimeZone(appTimeZone()) : QDateTime();
}
// 编辑器只提供墙上时间时，忽略其原有时区规格，按 Asia/Shanghai 解释后转 UTC。
inline QDateTime fromAppTime(const QDateTime &value)
{
    if (!value.isValid()) return QDateTime();
    return QDateTime(value.date(), value.time(), appTimeZone()).toUTC();
}
inline QString nowStamp() { return utcNow().toString(Qt::ISODateWithMs); }

inline QString encodeGeometry(const QRect &rect)
{
    return QStringLiteral("%1,%2,%3,%4").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height());
}
inline QRect decodeGeometry(const QString &text)
{
    const QStringList parts = text.split(QLatin1Char(','));
    if (parts.size() != 4) return QRect();
    bool ok[4] = {false, false, false, false};
    const int values[4] = {parts.at(0).trimmed().toInt(&ok[0]), parts.at(1).trimmed().toInt(&ok[1]),
                           parts.at(2).trimmed().toInt(&ok[2]), parts.at(3).trimmed().toInt(&ok[3])};
    if (!(ok[0] && ok[1] && ok[2] && ok[3])) return QRect();
    return QRect(values[0], values[1], values[2], values[3]);
}

} // namespace kl

#endif // KYLINLIST_COMMON_H
