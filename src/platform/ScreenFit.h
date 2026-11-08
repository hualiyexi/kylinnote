#ifndef KYLINLIST_SCREENFIT_H
#define KYLINLIST_SCREENFIT_H

#include <QRect>

namespace kl {

// 多屏坐标修正与窗管就绪判定
class ScreenFit
{
public:
    // 屏幕列表是否已可用（窗管可能晚于进程启动）
    static bool screensReady();

    // 将历史几何修正到当前屏幕布局内，返回值可直接用于 setGeometry
    static QRect fit(const QRect &geometry);

    // 新便签的默认几何：主屏可用区左上偏移处，带层叠位移
    static QRect defaultGeometry(int cascadeIndex = 0);
};

} // namespace kl

#endif // KYLINLIST_SCREENFIT_H
