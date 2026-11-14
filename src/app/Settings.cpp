#include "Settings.h"
namespace kl {
namespace {

bool isSmartListKey(const QString& key) {
    static const QStringList allowed = {
        QStringLiteral("all"), QStringLiteral("today"), QStringLiteral("tomorrow"),
        QStringLiteral("next7days"), QStringLiteral("overdue"), QStringLiteral("inbox"),
        QStringLiteral("completed"), QStringLiteral("abandoned"), QStringLiteral("trash")};
    return allowed.contains(key);
}

}  // namespace
Settings::Settings()
    : m_store(QSettings::IniFormat,
              QSettings::UserScope,
              QStringLiteral("kylinlist"),
              QStringLiteral("kylinlist")) {
}
Settings& Settings::instance() {
    static Settings settings;
    return settings;
}

Settings::SmartListVisibility Settings::smartListVisibility(const QString& key) const {
    if (!isSmartListKey(key)) return SmartListVisibility::Show;
    const QString value = m_store.value(QStringLiteral("navigation/smartLists/") + key,
                                        QStringLiteral("show")).toString();
    if (value == QLatin1String("hide")) return SmartListVisibility::Hide;
    if (value == QLatin1String("nonempty")) return SmartListVisibility::WhenNonEmpty;
    return SmartListVisibility::Show;
}

void Settings::setSmartListVisibility(const QString& key, SmartListVisibility visibility) {
    if (!isSmartListKey(key)) return;
    QString value = QStringLiteral("show");
    if (visibility == SmartListVisibility::Hide) value = QStringLiteral("hide");
    else if (visibility == SmartListVisibility::WhenNonEmpty) value = QStringLiteral("nonempty");
    m_store.setValue(QStringLiteral("navigation/smartLists/") + key, value);
}

NoteLayer Settings::defaultLayer() const {
#ifdef Q_OS_WIN
    const NoteLayer fallback = NoteLayer::Normal;
#else
    const NoteLayer fallback = NoteLayer::OnDesktop;
#endif
    return layerFromInt(m_store.value(QStringLiteral("ui/defaultLayer"), layerToInt(fallback)).toInt());
}
void Settings::setDefaultLayer(NoteLayer layer) {
    m_store.setValue(QStringLiteral("ui/defaultLayer"), layerToInt(layer));
}
QString Settings::defaultUpdateBaseUrl() {
    return QStringLiteral("http://143.147.10.31:11924");
}
QString Settings::updateBaseUrl() const {
    // 只有键不存在时才回退默认服务器；显式保存的空串仍表示禁用。
    if (!m_store.contains(QStringLiteral("updates/baseUrl")))
        return defaultUpdateBaseUrl();
    return m_store.value(QStringLiteral("updates/baseUrl")).toString().trimmed();
}
void Settings::setUpdateBaseUrl(const QString& url) {
    m_store.setValue(QStringLiteral("updates/baseUrl"), QString(url).trimmed());
}
bool Settings::updateAutoCheckEnabled() const {
    return m_store.value(QStringLiteral("updates/autoCheck"), true).toBool();
}
void Settings::setUpdateAutoCheckEnabled(bool enabled) {
    m_store.setValue(QStringLiteral("updates/autoCheck"), enabled);
}
QDateTime Settings::lastUpdateCheckAt() const {
    return m_store.value(QStringLiteral("updates/lastCheckAt")).toDateTime();
}
void Settings::setLastUpdateCheckAt(const QDateTime& at) {
    m_store.setValue(QStringLiteral("updates/lastCheckAt"), at);
}
QDateTime Settings::updatePostponeUntil() const {
    return m_store.value(QStringLiteral("updates/postponeUntil")).toDateTime();
}
void Settings::setUpdatePostponeUntil(const QDateTime& at) {
    m_store.setValue(QStringLiteral("updates/postponeUntil"), at);
}
bool Settings::autostartInitialized() const {
    return m_store.value(QStringLiteral("autostart/initialized"), false).toBool();
}
void Settings::setAutostartInitialized(bool done) {
    m_store.setValue(QStringLiteral("autostart/initialized"), done);
}
QString Settings::lastUpdateNoticeVersion() const {
    return m_store.value(QStringLiteral("updates/lastNoticeVersion")).toString();
}
void Settings::setLastUpdateNoticeVersion(const QString& version) {
    m_store.setValue(QStringLiteral("updates/lastNoticeVersion"), version);
}
QDate Settings::lastUpdateNoticeDate() const {
    return m_store.value(QStringLiteral("updates/lastNoticeDate")).toDate();
}
void Settings::setLastUpdateNoticeDate(const QDate& date) {
    m_store.setValue(QStringLiteral("updates/lastNoticeDate"), date);
}
Settings::PendingInstall Settings::pendingInstall() const {
    PendingInstall pending;
    pending.version = m_store.value(QStringLiteral("updates/pending/version")).toString().trimmed();
    if (pending.version.isEmpty()) return PendingInstall();
    pending.sha256 = m_store.value(QStringLiteral("updates/pending/sha256")).toString();
    pending.platform = m_store.value(QStringLiteral("updates/pending/platform")).toString();
    pending.arch = m_store.value(QStringLiteral("updates/pending/arch")).toString();
    pending.variant = m_store.value(QStringLiteral("updates/pending/variant")).toString();
    pending.fileName = m_store.value(QStringLiteral("updates/pending/fileName")).toString();
    pending.valid = true;
    return pending;
}
void Settings::setPendingInstall(const PendingInstall& pending) {
    m_store.setValue(QStringLiteral("updates/pending/version"), pending.version);
    m_store.setValue(QStringLiteral("updates/pending/sha256"), pending.sha256);
    m_store.setValue(QStringLiteral("updates/pending/platform"), pending.platform);
    m_store.setValue(QStringLiteral("updates/pending/arch"), pending.arch);
    m_store.setValue(QStringLiteral("updates/pending/variant"), pending.variant);
    m_store.setValue(QStringLiteral("updates/pending/fileName"), pending.fileName);
}
void Settings::clearPendingInstall() {
    m_store.remove(QStringLiteral("updates/pending"));
}
void Settings::sync() {
    m_store.sync();
}
}  // namespace kl
