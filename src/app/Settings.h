#pragma once
#include <QSettings>
#include "../Common.h"
namespace kn {
class Settings {
public:
    static Settings& instance();
    NoteLayer defaultLayer() const;
    void setDefaultLayer(NoteLayer layer);
    void sync();

private:
    Settings();
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;
    QSettings m_store;
};
}  // namespace kn
