#pragma once

#include <QDate>
#include <QDateTime>
#include "RecurrenceRule.h"

namespace kl {

enum class RecurrenceStatus { Next, Exhausted, Invalid };
struct RecurrenceAdvance {
    RecurrenceStatus status = RecurrenceStatus::Invalid;
    QDate date;
    int occurrenceOrdinal = 1;
    int completionCount = 0;
    QString error;
};

// current 为当前一次出现的本地日期；completionDate 仅在按完成日期时提供。
// seriesAnchor 是原 series DTSTART 的本地日期，持久化调用者应始终传递，以保留
// 周/月/年间隔相位及农历原始月日、月末和闰月语义；无效时仅回退到本次计算基准。
// 固定区间/农历/自选日期均保留完整规则；不含法定节假日或记忆法。
RecurrenceAdvance nextRecurrence(const RecurrenceSpec& spec, const QDate& current,
                                 const QDate& completionDate = QDate(),
                                 int occurrenceOrdinal = 1, int completionCount = 0,
                                 const QDate& seriesAnchor = QDate());

// 保留本地开始/结束时刻和时间段关系；由上层转换成绝对 UTC 时间。
// seriesAnchor 放在末尾以保持既有 error 参数的调用形式。
bool advanceLocalRange(const RecurrenceSpec& spec, const QDate& currentStart,
                       const QDate& currentEnd, const QDate& completionDate,
                       int occurrenceOrdinal, int completionCount,
                       QDate* nextStart, QDate* nextEnd,
                       RecurrenceAdvance* advance, QString* error = nullptr,
                       const QDate& seriesAnchor = QDate());

} // namespace kl
