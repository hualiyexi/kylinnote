#include "Settings.h"
namespace kl {
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
QString Settings::updateBaseUrl() const {
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
