#pragma once

#include <QDate>
#include <QString>

namespace kl {

struct LunarDate {
    int year = 0;
    int month = 1;
    int day = 1;
    bool leapMonth = false;
    bool lastDay = false;
};

class LunarCalendar {
public:
    // 明确支持太阳历 1900-01-31 .. 2100-12-31（年表农历 1900..2100 年）；
    // 范围外失败，不使用网络。
    static bool fromSolar(const QDate& solar, LunarDate* lunar, QString* error = nullptr);
    static bool toSolar(const LunarDate& lunar, QDate* solar, QString* error = nullptr);
    // 农历每月/每年均使用调用者保存的原始锚点，不从 current 反推并覆盖锚点。
    // lastDay 表示原日期就是原月月末，之后始终取目标月实际月末；固定日在短月
    // 临时夹到该月月末，但不会改变后续月份的原锚点。
    // 每月规则在 repeatLeapMonth=true 时包含闰月，否则只取正常月。
    // 每年规则保留原闰月锚点；非闰年回到正常月，开启重复后正常月与对应闰月均重复。
    static QDate nextMonthly(const QDate& current, const LunarDate& anchor,
                             bool repeatLeapMonth, bool* found = nullptr);
    static QDate nextYearly(const QDate& current, const LunarDate& anchor,
                            bool repeatLeapMonth, bool* found = nullptr);
    static int minimumSolarYear();
    static int maximumSolarYear();
};

} // namespace kl
