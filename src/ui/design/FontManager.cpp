#include "FontManager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QSettings>
#include <QWidget>
#include "TypeSystem.h"

namespace kl {
namespace {

const char* const kPreferenceKey = "appearance/font";

struct FontChoice {
    const char* id;
    const char* regular;
    const char* bold;
};
const FontChoice kChoices[] = {
    {"noto-sans-sc", "NotoSansCJKsc-Regular.otf", "NotoSansCJKsc-Bold.otf"},
    {"lxgw-wenkai", "LXGWWenKai-Regular.ttf", "LXGWWenKai-Medium.ttf"}
};

QStringList fontDirectories() {
    QStringList result;
    const QString override = qEnvironmentVariable("KYLINLIST_FONT_DIR");
    if (!override.isEmpty()) result.append(QFileInfo(override).absoluteFilePath());
    result.append(QCoreApplication::applicationDirPath() + QStringLiteral("/fonts"));
    result.append(QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("../../src/resources/fonts")));
    result.append(QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("../src/resources/fonts")));
#ifdef Q_OS_LINUX
    result.append(QStringLiteral("/usr/share/kylinlist/fonts"));
    result.append(QStringLiteral("/usr/local/share/kylinlist/fonts"));
#endif
    return result;
}

QString resolveSystemFamily() {
    const QStringList available = QFontDatabase().families();
    for (const QString& candidate : ds::fontFamilies())
        if (available.contains(candidate)) return candidate;
    return QApplication::font().family();
}

} // namespace

FontManager& FontManager::instance() {
    static FontManager manager;
    return manager;
}

FontManager::FontManager() {
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("kylinlist"), QStringLiteral("kylinlist"));
    const QString stored = settings.value(QLatin1String(kPreferenceKey)).toString();
    if (stored.isEmpty() || stored == QLatin1String("default")
        || !familyFor(stored).isEmpty()) m_preference = stored.isEmpty() ? QStringLiteral("default") : stored;
}

QString FontManager::familyFor(const QString& id) const {
    if (id == QLatin1String("default") || id.isEmpty()) return resolveSystemFamily();
    const auto cached = m_registeredFamilies.constFind(id);
    if (cached != m_registeredFamilies.constEnd()) return cached.value();
    for (const FontChoice& choice : kChoices) {
        if (id != QLatin1String(choice.id)) continue;
        for (const QString& directory : fontDirectories()) {
            const QString regular = QDir(directory).absoluteFilePath(QLatin1String(choice.regular));
            const QString bold = QDir(directory).absoluteFilePath(QLatin1String(choice.bold));
            if (!QFileInfo(regular).isFile() || !QFileInfo(bold).isFile()) continue;
            const int regularId = QFontDatabase::addApplicationFont(regular);
            if (regularId < 0) return QString();
            const int boldId = QFontDatabase::addApplicationFont(bold);
            if (boldId < 0) return QString();
            const QString family = QFontDatabase::applicationFontFamilies(regularId).value(0);
            if (family.isEmpty()) return QString();
            m_registeredFamilies.insert(id, family);
            return family;
        }
        return QString();
    }
    return QString();
}

QString FontManager::family() const {
    const QString selected = familyFor(m_preference);
    return selected.isEmpty() ? resolveSystemFamily() : selected;
}

QStringList FontManager::availableChoices() const {
    QStringList result{QStringLiteral("default")};
    for (const FontChoice& choice : kChoices)
        if (!familyFor(QLatin1String(choice.id)).isEmpty()) result.append(QLatin1String(choice.id));
    return result;
}

bool FontManager::setPreference(const QString& id) {
    if (id == m_preference) return true;
    if (familyFor(id).isEmpty()) return false;
    m_preference = id;
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("kylinlist"), QStringLiteral("kylinlist"));
    settings.setValue(QLatin1String(kPreferenceKey), id);
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) applyBootstrap(*app);
    emit fontChanged(family());
    return true;
}

void FontManager::applyBootstrap(QApplication& app) {
    app.setFont(ds::qfont(ds::FontScene::Base));
    const QString selected = family();
    for (QWidget* widget : QApplication::allWidgets()) {
        QFont font = widget->font();
        // 只改 UI 字体族，保留各场景字号/字重；代码与 Emoji 有独立入口，后续不被覆盖。
        if (font.styleHint() == QFont::Monospace || widget->property("emojiFont").toBool()
            || font.family() == ds::bundledEmojiFamilyName()) continue;
        font.setFamily(selected);
        widget->setFont(font);
        widget->updateGeometry();
        widget->update();
    }
}

} // namespace kl
