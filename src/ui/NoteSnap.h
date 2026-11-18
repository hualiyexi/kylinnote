#pragma once
#include <QRect>
#include <QVector>

namespace kl {
namespace NoteSnap {

// 桌面便签拖动吸附与排布的纯几何助手：不使用任何 widget、screen 或窗口系统
// API，全部矩形由调用者提供，可离屏（offscreen）单测。
//
// 坐标与边缘语义：
// - 所有矩形处于同一个由调用者选定的二维坐标系（通常是 QWidget::geometry() /
//   QScreen::availableGeometry() 所在的全局桌面坐标）。
// - "paper" 指便签可见纸张矩形；availableScreen 指目标显示器的工作区
//   （availableGeometry）。允许负坐标（多显示器左侧屏）。
// - 远侧边缘一律按排他语义计算：右边缘 = x() + width()，下边缘 = y() + height()。
//   绝不使用 QRect::right()/bottom()（差一的包含式语义），因此贴边结果无差一。

// 拖动吸附：把 proposedPaper 吸附到工作区边缘或其它便签的可见纸张边上。
//
// 返回值与 proposedPaper 尺寸始终相同，也不会改动任何 peer 或窗口状态
// （不做系统最大化之类的窗口操作）。x、y 两轴独立求最佳吸附；两轴同时命中
// 即自然的对角吸附。
//
// 候选（每轴分别评估，仅当位移 |delta| <= threshold 时捕获，含等于）：
// 1. 工作区左/右（上/下）边缘贴平；
// 2. 每个 peer（按输入顺序，且仅当它与纸张在另一轴上重叠、或间隙 <= threshold
//    时才算"相关"，远处的便签不产生吸引）：
//    a. 近侧边缘对齐（左边对左边 / 上边对上边）；
//    b. 远侧边缘对齐（右边对右边 / 下边对下边）；
//    c. 贴在 peer 前方并留请求间距 gap；
//    d. 贴在 peer 后方并留请求间距 gap；
//    e. gap > 0 时，另有贴平（零间距）的前方/后方候选。
//
// 每轴取 |delta| 最小者；距离相等时保留先评估者，因此优先级为
// 工作区边缘 > 先输入的 peer > 同一 peer 内的对齐 > 请求间距 > 零间距。
// 拖离后 raw 提议几何超出 threshold 即自然脱吸附。
//
// threshold < 0 或 gap < 0 按 0 处理。proposedPaper 尺寸非法（宽或高 <= 0）
// 时原样返回；尺寸非法的 peer 被忽略；availableScreen 非法时仅做 peer 吸附。
QRect snapped(const QRect &proposedPaper, const QRect &availableScreen,
              const QVector<QRect> &peerPapers, int threshold = 10, int gap = 8);

// 整齐排布：把 papers 按输入顺序、保持各自尺寸排入工作区。
//
// vertical == false：从工作区左上角起向右排成一行，放不下（cursor + 宽 > 工作区
// 右边缘，排他语义）即换行，新行 y = 当前行最高便签 + gap；
// vertical == true：从上到下排成一列，放不下即换列，新列 x = 当前列最宽便签 + gap。
//
// - 输出数量与顺序与输入一致，任何情况下不改变便签尺寸。
// - 每个放置位先夹紧在工作区内：放得下则不越过远边缘；某轴上比工作区还大时
//   贴住该轴起点（左/上），保证左上角始终可达；溢出时可能与前面的便签重叠。
// - 尺寸非法（宽或高 <= 0）的输入原样透传，且不占用排布游标。
// - availableScreen 非法时输入原样返回；gap < 0 按 0 处理。
QVector<QRect> arranged(const QVector<QRect> &papers, const QRect &availableScreen,
                        bool vertical, int gap = 8);

}  // namespace NoteSnap
}  // namespace kl
