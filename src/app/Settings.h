#ifndef KYLINNOTE_SETTINGS_H
#define KYLINNOTE_SETTINGS_H

#include "../Common.h"

#include <QSettings>

namespace kn {

// 用户偏好（~/.config/kylinnote/kylinnote.conf），与便签业务数据分离
class Settings
{
public:
    static Settings &instance();

    // 删除前是否弹出确认框，勾选"不再提示"后置为 false
    bool confirmDelete() const;
    void setConfirmDelete(bool enabled);

    // 首次运行提示是否已展示过
    bool hintShown() const;
    void setHintShown(bool shown);

    // 新建便签默认采用的层级（贴在桌面 / 普通 / 置顶）
    NoteLayer defaultLayer() const;
    void setDefaultLayer(NoteLayer layer);

    void sync();

private:
    Settings();
    Settings(const Settings &) = delete;
    Settings &operator=(const Settings &) = delete;

    QSettings m_store;
};

} // namespace kn

#endif // KYLINNOTE_SETTINGS_H
