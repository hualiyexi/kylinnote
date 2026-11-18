#pragma once

#include <QIcon>

namespace kl {

// 导航使用原始透明品牌；没有有效配置时与软件图标共用默认品牌标。
QIcon brandLogoIcon();

// 窗口／任务栏／托盘使用固定白底版本，不改写原始素材。仅在 GUI 线程调用。
QIcon applicationBrandIcon();

} // namespace kl
