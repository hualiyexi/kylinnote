#include "UsageStatistics.h"

namespace kl {

qint64 UsageStatistics::firstTaskDays(const QDateTime &now) const
{
    if (!hasFirstTask() || !now.isValid()) return -1;
    const QDateTime first = firstTaskAt.toUTC();
    const QDateTime current = now.toUTC();
    const qint64 seconds = first.secsTo(current);
    // 时钟回拨或测试注入的早期时刻：上报口径必须非负，统一取 0。
    if (seconds <= 0) return 0;
    return seconds / 86400;
}

} // namespace kl
