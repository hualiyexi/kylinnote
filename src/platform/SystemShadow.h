#pragma once

namespace kl {

// Windows 会给无边框弹出窗口补画一层直角系统投影，圆角卡片下会出现
// 方角阴影，与应用的圆角风格冲突。该函数安装应用级过滤器，在窗口取得
// 原生句柄时清除窗口类的 CS_DROPSHADOW，使所有浮层阴影只由应用自绘。
// 幂等；非 Windows 平台为空操作。
void disableNativeWindowShadows();

}  // namespace kl
