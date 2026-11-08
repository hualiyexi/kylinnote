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
void Settings::sync() {
    m_store.sync();
}
}  // namespace kl
