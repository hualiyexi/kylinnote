#include "Settings.h"

namespace kn {

namespace {
const char *kKeyConfirmDelete = "ui/confirmDelete";
const char *kKeyHintShown     = "ui/hintShown";
const char *kKeyDefaultLayer  = "ui/defaultLayer";
}

Settings::Settings()
    : m_store(QSettings::IniFormat, QSettings::UserScope,
              QStringLiteral("kylinnote"), QStringLiteral("kylinnote"))
{
}

Settings &Settings::instance()
{
    static Settings s;
    return s;
}

bool Settings::confirmDelete() const
{
    return m_store.value(QLatin1String(kKeyConfirmDelete), true).toBool();
}

void Settings::setConfirmDelete(bool enabled)
{
    m_store.setValue(QLatin1String(kKeyConfirmDelete), enabled);
}

bool Settings::hintShown() const
{
    return m_store.value(QLatin1String(kKeyHintShown), false).toBool();
}

void Settings::setHintShown(bool shown)
{
    m_store.setValue(QLatin1String(kKeyHintShown), shown);
}

NoteLayer Settings::defaultLayer() const
{
    return layerFromInt(m_store.value(QLatin1String(kKeyDefaultLayer),
                                      layerToInt(NoteLayer::OnDesktop)).toInt());
}

void Settings::setDefaultLayer(NoteLayer layer)
{
    m_store.setValue(QLatin1String(kKeyDefaultLayer), layerToInt(layer));
}

void Settings::sync()
{
    m_store.sync();
}

} // namespace kn
