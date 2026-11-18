#include "NoteSnap.h"

#include <QtGlobal>

namespace kl {
namespace NoteSnap {
namespace {

bool hasExtent(const QRect &rect)
{
    return rect.width() > 0 && rect.height() > 0;
}

// 两个 [begin, end) 区间：重叠返回 0，否则返回它们之间的空隙距离。
int spanSeparation(int aBegin, int aEnd, int bBegin, int bEnd)
{
    return qMax(0, qMax(bBegin - aEnd, aBegin - bEnd));
}

// 单轴上的最佳吸附位移。相等距离保留先评估者（用严格小于更新），
// 因此评估顺序即优先级：工作区边缘 > 先输入的 peer > 对齐 > 请求间距 > 零间距。
struct AxisSnap {
    int delta = 0;
    bool active = false;

    void consider(int candidate, int threshold)
    {
        if (qAbs(candidate) > threshold)
            return;
        if (!active || qAbs(candidate) < qAbs(delta)) {
            delta = candidate;
            active = true;
        }
    }
};

// peer 在吸附轴上的投影，cross* 是它在另一轴上的区间，用于过滤"不相关"的远便签。
struct PeerSpan {
    int begin = 0;       // peer 在吸附轴上的起点（含）
    int end = 0;         // peer 在吸附轴上的终点（排他）
    int crossBegin = 0;  // peer 在另一轴上的起点（含）
    int crossEnd = 0;    // peer 在另一轴上的终点（排他）
};

// 对一条轴做吸附：paperBegin/paperSize 为纸张在该轴上的位置与长度，
// paperCross* 为纸张在另一轴上的区间；areaBegin/areaSize 为工作区在该轴上的
// 投影。返回该轴吸附后的坐标（未命中时原样返回）。
int snapAxis(int paperBegin, int paperSize, int paperCrossBegin, int paperCrossEnd,
             int areaBegin, int areaSize, bool areaValid,
             const QVector<PeerSpan> &peers, int threshold, int gap)
{
    AxisSnap best;
    const int paperEnd = paperBegin + paperSize;  // 排他远边缘

    // 工作区边缘先评估：距离相等时压过任何 peer；近侧边缘先于远侧边缘。
    if (areaValid) {
        best.consider(areaBegin - paperBegin, threshold);
        best.consider(areaBegin + areaSize - paperEnd, threshold);
    }

    for (const PeerSpan &peer : peers) {
        // 与纸张在另一轴上既不重叠也不邻近（间隙 > threshold）的便签不吸引。
        if (spanSeparation(paperCrossBegin, paperCrossEnd, peer.crossBegin, peer.crossEnd) > threshold)
            continue;
        best.consider(peer.begin - paperBegin, threshold);      // 近侧边缘对齐
        best.consider(peer.end - paperEnd, threshold);          // 远侧边缘对齐
        best.consider(peer.begin - gap - paperEnd, threshold);  // 贴 peer 前方，请求间距
        best.consider(peer.end + gap - paperBegin, threshold);  // 贴 peer 后方，请求间距
        if (gap > 0) {
            best.consider(peer.begin - paperEnd, threshold);    // 贴 peer 前方，零间距
            best.consider(peer.end - paperBegin, threshold);    // 贴 peer 后方，零间距
        }
    }
    return best.active ? paperBegin + best.delta : paperBegin;
}

}  // namespace

QRect snapped(const QRect &proposedPaper, const QRect &availableScreen,
              const QVector<QRect> &peerPapers, int threshold, int gap)
{
    if (!hasExtent(proposedPaper))
        return proposedPaper;
    if (threshold < 0)
        threshold = 0;
    if (gap < 0)
        gap = 0;

    const bool areaValid = hasExtent(availableScreen);

    QVector<PeerSpan> alongX;
    QVector<PeerSpan> alongY;
    alongX.reserve(peerPapers.size());
    alongY.reserve(peerPapers.size());
    for (const QRect &peer : peerPapers) {
        if (!hasExtent(peer))
            continue;
        PeerSpan x;
        x.begin = peer.x();
        x.end = peer.x() + peer.width();
        x.crossBegin = peer.y();
        x.crossEnd = peer.y() + peer.height();
        alongX.append(x);

        PeerSpan y;
        y.begin = peer.y();
        y.end = peer.y() + peer.height();
        y.crossBegin = peer.x();
        y.crossEnd = peer.x() + peer.width();
        alongY.append(y);
    }

    const int x = snapAxis(proposedPaper.x(), proposedPaper.width(),
                           proposedPaper.y(), proposedPaper.y() + proposedPaper.height(),
                           availableScreen.x(), availableScreen.width(), areaValid,
                           alongX, threshold, gap);
    const int y = snapAxis(proposedPaper.y(), proposedPaper.height(),
                           proposedPaper.x(), proposedPaper.x() + proposedPaper.width(),
                           availableScreen.y(), availableScreen.height(), areaValid,
                           alongY, threshold, gap);
    return QRect(x, y, proposedPaper.width(), proposedPaper.height());
}

QVector<QRect> arranged(const QVector<QRect> &papers, const QRect &availableScreen,
                        bool vertical, int gap)
{
    if (gap < 0)
        gap = 0;
    QVector<QRect> result;
    if (!hasExtent(availableScreen))
        return papers;
    result.reserve(papers.size());

    // 主轴 = 排布前进方向（行为 x、列为 y）；wrap 轴 = 换行/换列方向。
    // far* 一律是排他边缘（x()+width() / y()+height()），避免 QRect::right() 差一。
    const int nearMain = vertical ? availableScreen.y() : availableScreen.x();
    const int farMain = vertical ? availableScreen.y() + availableScreen.height()
                                 : availableScreen.x() + availableScreen.width();
    const int nearWrap = vertical ? availableScreen.x() : availableScreen.y();
    const int farWrap = vertical ? availableScreen.x() + availableScreen.width()
                                 : availableScreen.y() + availableScreen.height();

    int cursorMain = nearMain;
    int cursorWrap = nearWrap;
    int lineSpan = 0;  // 当前行/列中主轴方向最长的便签尺寸（行高/列宽）

    for (const QRect &paper : papers) {
        if (!hasExtent(paper)) {
            result.append(paper);  // 非法尺寸原样透传，不占用排布游标
            continue;
        }
        const int sizeMain = vertical ? paper.height() : paper.width();
        const int sizeWrap = vertical ? paper.width() : paper.height();

        // 当前线放不下且不是线首时换行/换列；每线第一个便签永远落在起点。
        if (cursorMain > nearMain && cursorMain + sizeMain > farMain) {
            cursorWrap += lineSpan + gap;
            cursorMain = nearMain;
            lineSpan = 0;
        }

        // 夹紧在工作区内：放得下则不越过远边缘；比工作区还大时贴住近侧（左/上）。
        const int mainPos = qBound(nearMain, cursorMain, qMax(nearMain, farMain - sizeMain));
        const int wrapPos = qBound(nearWrap, cursorWrap, qMax(nearWrap, farWrap - sizeWrap));

        result.append(vertical
                          ? QRect(wrapPos, mainPos, paper.width(), paper.height())
                          : QRect(mainPos, wrapPos, paper.width(), paper.height()));

        // 用夹紧后的位置前进：越界放置后下一个便签自然换线。
        cursorMain = mainPos + sizeMain + gap;
        lineSpan = qMax(lineSpan, sizeWrap);
    }
    return result;
}

}  // namespace NoteSnap
}  // namespace kl
