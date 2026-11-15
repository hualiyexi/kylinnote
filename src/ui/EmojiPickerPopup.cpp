#include "EmojiPickerPopup.h"

#include <QAction>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPainter>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <QVBoxLayout>
#include "AppIcons.h"
#include "design/ThemeManager.h"
#include "design/TypeSystem.h"

namespace kl {
namespace {

constexpr int kCardWidth = 328;
constexpr int kCardHeight = 420;
constexpr int kShadowMargin = 12;
constexpr int kGridColumns = 9;
constexpr int kCellSize = 30;

QColor token(ds::ColorToken t) { return ThemeManager::instance().color(t); }

// 标准 QToolButton 的文本布局会把部分彩色 emoji 当作普通西文裁剪。
// 直接绘制文本并显式选择 emoji 字体，保证跨平台的字形命中区域一致。
class EmojiCellButton : public QToolButton {
public:
    explicit EmojiCellButton(const QString& emoji, QWidget* parent) : QToolButton(parent) {
        setText(emoji);
        QFont font = ds::emojiFont();
        font.setPixelSize(19);
        setFont(font);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF frame = QRectF(rect()).adjusted(1, 1, -1, -1);
        if (isDown()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(token(ds::ColorToken::FieldHover));
            painter.drawRoundedRect(frame, 6, 6);
        } else if (underMouse()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(token(ds::ColorToken::Hover));
            painter.drawRoundedRect(frame, 6, 6);
        }
        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(token(ds::ColorToken::FocusRing), 1.0));
            painter.drawRoundedRect(frame, 6, 6);
        }
        painter.setFont(font());
        painter.drawText(rect(), Qt::AlignCenter, text());
    }
};

// ---------------------------------------------------------------------------
// 内置 emoji 数据：覆盖常见清单图标场景即可（每类 20~40 个），不求全集。
// aliases 为常见中文别名，空格分隔，供搜索匹配；emoji 与中文均为 UTF-8 源码直书。
// ---------------------------------------------------------------------------
struct EmojiDef {
    const char* emoji;
    const char* aliases;
};

const EmojiDef kPeopleEmoji[] = {
    { "😀", "开心 大笑 笑" },
    { "😄", "微笑 开心" },
    { "😁", "咧嘴 笑" },
    { "😆", "大笑 哈哈" },
    { "😅", "尴尬 笑 汗" },
    { "🤣", "笑趴 捧腹" },
    { "😂", "笑哭 高兴" },
    { "🙂", "微笑" },
    { "😊", "害羞 微笑" },
    { "😇", "天使 善良" },
    { "😉", "眨眼" },
    { "😍", "喜欢 爱慕 花痴" },
    { "😘", "亲亲 飞吻" },
    { "😜", "吐舌 顽皮" },
    { "🤔", "思考 想 想想" },
    { "🤨", "怀疑 挑眉" },
    { "😐", "无语 平静" },
    { "🙄", "翻白眼 白眼" },
    { "😏", "坏笑 得意" },
    { "😳", "脸红 尴尬" },
    { "😡", "生气 愤怒" },
    { "😠", "生气" },
    { "😭", "哭 大哭" },
    { "😢", "哭 难过" },
    { "😱", "尖叫 吓" },
    { "😷", "口罩 生病" },
    { "🤒", "发烧 生病" },
    { "🤕", "受伤 生病" },
    { "😈", "恶魔 坏" },
    { "👻", "鬼 幽灵" },
    { "💀", "骷髅 头骨" },
    { "👽", "外星人" },
    { "🤖", "机器人" },
    { "👍", "赞 点赞 好" },
    { "👎", "踩 不赞成" },
    { "👌", "好的 OK" },
    { "✌️", "耶 胜利" },
    { "🤞", "加油 祈愿" },
    { "🙏", "拜托 感谢 祈祷" },
    { "👏", "鼓掌" },
    { "💪", "加油 强壮 肌肉" },
};

const EmojiDef kNatureEmoji[] = {
    { "🐶", "狗 旺财" },
    { "🐱", "猫 喵" },
    { "🐭", "老鼠" },
    { "🐹", "仓鼠" },
    { "🐰", "兔子" },
    { "🦊", "狐狸" },
    { "🐻", "熊" },
    { "🐼", "熊猫" },
    { "🐨", "考拉 树袋熊" },
    { "🐯", "老虎 虎" },
    { "🦁", "狮子" },
    { "🐮", "牛" },
    { "🐷", "猪" },
    { "🐸", "青蛙" },
    { "🐵", "猴子" },
    { "🐔", "鸡" },
    { "🐧", "企鹅" },
    { "🐦", "鸟" },
    { "🐤", "小鸡" },
    { "🦆", "鸭子" },
    { "🦉", "猫头鹰" },
    { "🐢", "乌龟 龟" },
    { "🐍", "蛇" },
    { "🐙", "章鱼" },
    { "🦀", "螃蟹" },
    { "🐬", "海豚" },
    { "🐳", "鲸鱼" },
    { "🐝", "蜜蜂" },
    { "🐞", "瓢虫" },
    { "🦋", "蝴蝶" },
    { "🐌", "蜗牛" },
    { "🐜", "蚂蚁" },
    { "🌸", "樱花 花" },
    { "🌹", "玫瑰 花" },
    { "🌻", "向日葵 花" },
    { "🌷", "郁金香 花" },
    { "🍀", "四叶草 幸运" },
    { "🍁", "枫叶 秋天" },
    { "🍂", "落叶 秋天" },
    { "🍃", "叶子" },
    { "🌵", "仙人掌" },
    { "🌏", "地球 世界 亚洲" },
    { "🌙", "月亮" },
    { "⭐", "星星" },
    { "✨", "星星 闪光" },
    { "☀️", "太阳 晴天" },
    { "🌈", "彩虹" },
    { "❄️", "雪花 冬天" },
    { "☔", "雨伞 下雨" },
    { "🔥", "火 热门" },
};

const EmojiDef kFoodEmoji[] = {
    { "🍎", "苹果" },
    { "🍊", "橘子 橙子" },
    { "🍋", "柠檬" },
    { "🍌", "香蕉" },
    { "🍉", "西瓜" },
    { "🍇", "葡萄" },
    { "🍓", "草莓" },
    { "🍒", "樱桃" },
    { "🍑", "桃子" },
    { "🍍", "菠萝" },
    { "🥝", "猕猴桃 奇异果" },
    { "🍅", "西红柿 番茄" },
    { "🥕", "胡萝卜" },
    { "🌽", "玉米" },
    { "🍞", "面包" },
    { "🥐", "牛角面包" },
    { "🧀", "奶酪" },
    { "🍳", "煎蛋" },
    { "🍔", "汉堡" },
    { "🍟", "薯条" },
    { "🍕", "披萨" },
    { "🌭", "热狗" },
    { "🌮", "墨西哥卷" },
    { "🍜", "面条" },
    { "🍲", "火锅 汤" },
    { "🍣", "寿司" },
    { "🍱", "便当" },
    { "🍤", "虾" },
    { "🍚", "米饭" },
    { "🍥", "鱼糕" },
    { "🍦", "冰淇淋" },
    { "🍰", "蛋糕" },
    { "🎂", "生日 蛋糕" },
    { "🍮", "布丁" },
    { "🍬", "糖" },
    { "🍭", "棒棒糖" },
    { "🍫", "巧克力" },
    { "🍿", "爆米花" },
    { "🍩", "甜甜圈" },
    { "🍪", "饼干" },
    { "☕", "咖啡" },
    { "🍵", "茶" },
    { "🍺", "啤酒 干杯" },
    { "🍷", "红酒" },
    { "🥂", "干杯 庆祝" },
    { "🍹", "鸡尾酒" },
};

const EmojiDef kActivityEmoji[] = {
    { "⚽", "足球" },
    { "🏀", "篮球" },
    { "🏈", "橄榄球" },
    { "⚾", "棒球" },
    { "🎾", "网球" },
    { "🏐", "排球" },
    { "🎱", "台球" },
    { "🏓", "乒乓球" },
    { "🏸", "羽毛球" },
    { "🎯", "飞镖 目标" },
    { "🎮", "游戏 手柄" },
    { "🎲", "骰子" },
    { "🎨", "调色板 画画" },
    { "🎬", "电影" },
    { "🎤", "麦克风 唱歌" },
    { "🎧", "耳机 音乐" },
    { "🎹", "钢琴 音乐" },
    { "🎸", "吉他" },
    { "🎺", "小号" },
    { "🎻", "小提琴" },
    { "🎫", "门票" },
    { "🏆", "奖杯 冠军" },
    { "🎉", "庆祝 撒花 派对" },
    { "🎊", "庆祝" },
    { "🎈", "气球 生日" },
    { "🎁", "礼物 生日" },
    { "🚩", "旗帜 红旗" },
    { "🏁", "终点" },
    { "🥇", "金牌 第一名" },
    { "🥊", "拳击" },
    { "🥋", "武术 跆拳道" },
    { "🏂", "滑雪" },
    { "🏊", "游泳" },
    { "🚴", "骑车 自行车" },
};

const EmojiDef kTravelEmoji[] = {
    { "🚗", "汽车 车" },
    { "🚕", "出租车" },
    { "🚙", "越野车" },
    { "🚌", "公交车" },
    { "🚎", "无轨电车" },
    { "🏎", "赛车" },
    { "🚓", "警车" },
    { "🚑", "救护车" },
    { "🚒", "消防车" },
    { "🚐", "面包车" },
    { "🚚", "卡车" },
    { "🚛", "大货车" },
    { "🚜", "拖拉机" },
    { "🚲", "自行车" },
    { "🛴", "滑板车" },
    { "🚂", "火车" },
    { "✈️", "飞机 旅行" },
    { "🚀", "火箭 发射" },
    { "🛸", "飞碟" },
    { "⛵", "帆船" },
    { "🚤", "快艇" },
    { "🛳", "轮船" },
    { "🗺", "地图" },
    { "🗽", "自由女神" },
    { "🗼", "东京塔 塔" },
    { "🏰", "城堡" },
    { "🏯", "城楼" },
    { "🏟", "体育场" },
    { "⛺", "帐篷 露营" },
    { "🏕", "露营" },
    { "🏖", "海滩" },
    { "🏔", "雪山" },
    { "🌋", "火山" },
    { "🗻", "富士山 山" },
    { "🏠", "家 房子" },
    { "🏡", "家 房子 花园" },
    { "🏢", "办公楼 写字楼" },
    { "🏥", "医院" },
    { "🏦", "银行" },
    { "🏨", "酒店 宾馆" },
    { "🏪", "便利店" },
    { "🏫", "学校" },
    { "🏭", "工厂" },
    { "🌉", "桥 夜景" },
    { "🌃", "夜景 星空" },
    { "🌇", "日落" },
    { "🌆", "傍晚" },
};

const EmojiDef kObjectEmoji[] = {
    { "⌚", "手表" },
    { "📱", "手机" },
    { "💻", "电脑 笔记本" },
    { "⌨️", "键盘" },
    { "🖥", "电脑 显示器" },
    { "🖨", "打印机" },
    { "🖱", "鼠标" },
    { "💾", "保存 软盘" },
    { "💿", "光盘" },
    { "📀", "光盘" },
    { "📷", "相机 拍照" },
    { "📹", "摄像机" },
    { "🎥", "摄影机" },
    { "📞", "电话" },
    { "☎️", "电话" },
    { "📺", "电视" },
    { "📻", "收音机" },
    { "🎙", "麦克风" },
    { "⏰", "闹钟 时间" },
    { "⏳", "沙漏 时间" },
    { "📡", "天线 信号" },
    { "🔋", "电池 充电" },
    { "🔌", "插头 充电" },
    { "💡", "灯泡 主意" },
    { "🔦", "手电筒" },
    { "💎", "钻石 珠宝" },
    { "🔧", "扳手 修理" },
    { "🔨", "锤子" },
    { "🔒", "锁 加密" },
    { "🔓", "解锁" },
    { "🔑", "钥匙" },
    { "🚪", "门" },
    { "🛏", "床" },
    { "🛋", "沙发" },
    { "🚿", "淋浴" },
    { "📦", "包裹 快递" },
    { "✉️", "信 邮件" },
    { "📮", "邮筒 信箱" },
    { "📌", "图钉 标记" },
    { "📎", "回形针 附件" },
    { "✂️", "剪刀" },
    { "📚", "书 学习" },
    { "📖", "书 阅读" },
};

const EmojiDef kSymbolEmoji[] = {
    { "❤️", "爱心 喜欢" },
    { "💛", "黄心 爱心" },
    { "💚", "绿心 爱心" },
    { "💙", "蓝心 爱心" },
    { "💜", "紫心 爱心" },
    { "🖤", "黑心 爱心" },
    { "💔", "心碎 分手" },
    { "❣️", "心 强调" },
    { "💕", "爱心 恋爱" },
    { "💞", "爱心" },
    { "💓", "心动" },
    { "💗", "爱心" },
    { "💖", "爱心 闪" },
    { "💘", "爱神 丘比特" },
    { "💝", "礼物 爱心" },
    { "💯", "满分 一百分" },
    { "💢", "生气" },
    { "💥", "爆炸" },
    { "💦", "汗 水滴" },
    { "💨", "风 跑" },
    { "💣", "炸弹" },
    { "💬", "说话 对话" },
    { "💭", "想法 思考" },
    { "💤", "睡觉" },
    { "♻️", "回收 循环" },
    { "✅", "完成 对" },
    { "❌", "错 取消" },
    { "❓", "问号 疑问" },
    { "❗", "感叹 注意" },
    { "⭕", "圈 正确" },
    { "🚫", "禁止" },
    { "⚠️", "警告 注意" },
    { "🔴", "红点" },
    { "🔵", "蓝点" },
    { "⚫", "黑点" },
    { "⚪", "白点" },
    { "🎵", "音乐" },
    { "🎶", "音乐 歌曲" },
    { "➕", "加" },
    { "➖", "减" },
    { "💲", "美元 钱" },
};

struct EmojiGroupDef {
    const char* title;
    const char* keywords;
    const EmojiDef* defs;
    int count;
};

const EmojiGroupDef kGroups[] = {
    { "人物", "表情 人 脸 手", kPeopleEmoji, sizeof(kPeopleEmoji) / sizeof(kPeopleEmoji[0]) },
    { "动物与自然", "动物 自然 植物 天气", kNatureEmoji, sizeof(kNatureEmoji) / sizeof(kNatureEmoji[0]) },
    { "食物", "食物 吃 水果 饮料", kFoodEmoji, sizeof(kFoodEmoji) / sizeof(kFoodEmoji[0]) },
    { "活动", "活动 运动 庆祝 音乐", kActivityEmoji, sizeof(kActivityEmoji) / sizeof(kActivityEmoji[0]) },
    { "旅行与地点", "旅行 地点 交通 建筑", kTravelEmoji, sizeof(kTravelEmoji) / sizeof(kTravelEmoji[0]) },
    { "物体", "物品 物体 工具 电子产品", kObjectEmoji, sizeof(kObjectEmoji) / sizeof(kObjectEmoji[0]) },
    { "符号", "符号 标志 心 形状", kSymbolEmoji, sizeof(kSymbolEmoji) / sizeof(kSymbolEmoji[0]) },
};

// 判断码点是否属于常见 emoji 区段（够用即可，不追求完整 emoji 属性表）。
bool isEmojiCodepoint(uint codepoint)
{
    return (codepoint >= 0x1F000 && codepoint <= 0x1FAFF)
        || (codepoint >= 0x2600 && codepoint <= 0x27BF)
        || (codepoint >= 0x2B00 && codepoint <= 0x2BFF)
        || codepoint == 0x3030 || codepoint == 0x303D
        || codepoint == 0x3297 || codepoint == 0x3299;
}

uint codepointAt(const QString& text, int index, int* length)
{
    const QChar ch = text.at(index);
    if (ch.isHighSurrogate() && index + 1 < text.size() && text.at(index + 1).isLowSurrogate()) {
        if (length) *length = 2;
        return QChar::surrogateToUcs4(ch, text.at(index + 1));
    }
    if (length) *length = 1;
    return ch.unicode();
}

// 取文本中第一个 emoji（连同其后的变体选择符 / ZWJ 序列），没有则返回空串。
QString firstEmojiIn(const QString& text)
{
    int index = 0;
    while (index < text.size()) {
        int length = 0;
        const uint cp = codepointAt(text, index, &length);
        if (cp == 0xFE0F || !isEmojiCodepoint(cp)) { // 裸变体选择符与普通字符都跳过
            index += length;
            continue;
        }
        QString emoji = text.mid(index, length);
        index += length;
        // 吸收紧随其后的变体选择符与 ZWJ 序列（如 ❤️、家庭组合）。
        while (index < text.size()) {
            int nextLength = 0;
            const uint next = codepointAt(text, index, &nextLength);
            if (next == 0xFE0F) {
                emoji += text.mid(index, nextLength);
                index += nextLength;
            } else if (next == 0x200D) {
                int afterLength = 0;
                const uint after = index + nextLength < text.size()
                    ? codepointAt(text, index + nextLength, &afterLength) : 0;
                if (isEmojiCodepoint(after)) {
                    emoji += text.mid(index, nextLength + afterLength);
                    index += nextLength + afterLength;
                } else {
                    break;
                }
            } else {
                break;
            }
        }
        return emoji;
    }
    return QString();
}

bool containsQuery(const QString& haystack, const QString& query)
{
    return !query.isEmpty() && haystack.contains(query, Qt::CaseInsensitive);
}

} // namespace

// ---------------------------------------------------------------------------
bool EmojiPickerPopup::Cell::matches(const QString& query) const
{
    if (query.isEmpty()) return true;
    return containsQuery(categoryTitle, query)
        || containsQuery(categoryKeywords, query)
        || containsQuery(aliases, query)
        || emoji.contains(query);
}

EmojiPickerPopup::EmojiPickerPopup(QWidget* parent)
    : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setObjectName(QStringLiteral("EmojiPickerPopup"));
    setWindowTitle(QStringLiteral("选择图标"));
    setAttribute(Qt::WA_TranslucentBackground);
    setFont(ds::qfont(ds::FontScene::Base));
    buildUi();
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](ds::Theme) { applyTheme(); });
}

void EmojiPickerPopup::buildUi()
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kShadowMargin, kShadowMargin, kShadowMargin, kShadowMargin);
    outer->setSpacing(0);

    m_card = new QWidget(this);
    m_card->setObjectName(QStringLiteral("EmojiPickerCard"));
    m_card->setAttribute(Qt::WA_StyledBackground);
    m_card->setFixedSize(kCardWidth, kCardHeight);
    auto* shadow = new QGraphicsDropShadowEffect(m_card);
    shadow->setBlurRadius(24);
    shadow->setOffset(0, 4);
    m_card->setGraphicsEffect(shadow);
    outer->addWidget(m_card, 0, Qt::AlignCenter);
    setFixedSize(kCardWidth + 2 * kShadowMargin, kCardHeight + 2 * kShadowMargin);

    auto* cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(10, 10, 10, 10);
    cardLayout->setSpacing(6);

    auto* header = new QHBoxLayout();
    header->setSpacing(6);
    m_search = new QLineEdit(m_card);
    m_search->setObjectName(QStringLiteral("EmojiSearchEdit"));
    m_search->setPlaceholderText(QStringLiteral("搜索或直接输入 emoji 回车"));
    m_search->setAccessibleName(QStringLiteral("搜索 emoji"));
    m_search->setClearButtonEnabled(true);
    m_searchAction = m_search->addAction(AppIcons::icon(IconId::Search, ds::ColorToken::TextSecondary),
                                         QLineEdit::LeadingPosition);
    connect(m_search, &QLineEdit::textChanged, this,
            [this](const QString& text) { applyFilter(text.trimmed()); });
    connect(m_search, &QLineEdit::returnPressed, this, &EmojiPickerPopup::confirmFromSearch);
    header->addWidget(m_search, 1);
    m_randomButton = new QPushButton(QStringLiteral("随机"), m_card);
    m_randomButton->setObjectName(QStringLiteral("EmojiRandomButton"));
    m_randomButton->setAutoDefault(false);
    m_randomButton->setToolTip(QStringLiteral("从当前结果中随机选一个图标"));
    m_randomButton->setAccessibleName(QStringLiteral("随机图标"));
    connect(m_randomButton, &QPushButton::clicked, this, &EmojiPickerPopup::pickRandom);
    header->addWidget(m_randomButton);
    m_resetButton = new QPushButton(QStringLiteral("重置"), m_card);
    m_resetButton->setObjectName(QStringLiteral("EmojiResetButton"));
    m_resetButton->setAutoDefault(false);
    m_resetButton->setToolTip(QStringLiteral("清除图标，恢复默认样式"));
    m_resetButton->setAccessibleName(QStringLiteral("重置图标"));
    connect(m_resetButton, &QPushButton::clicked, this, [this]() {
        hide();
        emit emojiSelected(QString());
        accept();
    });
    header->addWidget(m_resetButton);
    cardLayout->addLayout(header);

    m_scroll = new QScrollArea(m_card);
    m_scroll->setObjectName(QStringLiteral("EmojiScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    cardLayout->addWidget(m_scroll, 1);

    m_gridHost = new QWidget(m_scroll);
    m_gridHost->setObjectName(QStringLiteral("EmojiGrid"));
    m_grid = new QGridLayout(m_gridHost);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setHorizontalSpacing(2);
    m_grid->setVerticalSpacing(2);
    m_scroll->setWidget(m_gridHost);

    m_emptyHint = new QLabel(QStringLiteral("没有匹配的 emoji"), m_card);
    m_emptyHint->setObjectName(QStringLiteral("EmojiEmptyHint"));
    m_emptyHint->setProperty("muted", true);
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->hide();
    cardLayout->addWidget(m_emptyHint);

    // 一次性创建分类标题与全部按钮；过滤时只重排/隐藏，不销毁。
    for (const EmojiGroupDef& group : kGroups) {
        const QString title = QString::fromUtf8(group.title);
        auto* label = new QLabel(title, m_gridHost);
        label->setObjectName(QStringLiteral("EmojiCategoryLabel"));
        label->setProperty("muted", true);
        label->setFont(ds::qfont(ds::FontScene::Caption));
        m_categoryLabels.append(label);
        for (int i = 0; i < group.count; ++i) {
            const EmojiDef& def = group.defs[i];
            Cell cell;
            cell.emoji = QString::fromUtf8(def.emoji);
            cell.aliases = QString::fromUtf8(def.aliases);
            cell.categoryTitle = title;
            cell.categoryKeywords = QString::fromUtf8(group.keywords);
            cell.button = new EmojiCellButton(cell.emoji, m_gridHost);
            cell.button->setObjectName(QStringLiteral("EmojiCell"));
            cell.button->setText(cell.emoji);
            cell.button->setFixedSize(kCellSize, kCellSize);
            cell.button->setFocusPolicy(Qt::StrongFocus);
            cell.button->setCursor(Qt::PointingHandCursor);
            const QString name = cell.aliases.section(QLatin1Char(' '), 0, 0);
            const QString accessible = name.isEmpty()
                ? QStringLiteral("%1 emoji").arg(title)
                : QStringLiteral("%1（%2）").arg(name, title);
            cell.button->setToolTip(accessible);
            cell.button->setAccessibleName(accessible);
            const QString emoji = cell.emoji;
            connect(cell.button, &QToolButton::clicked, this,
                    [this, emoji]() { chooseEmoji(emoji); });
            m_cells.append(cell);
        }
    }
    rebuildGrid();
}

void EmojiPickerPopup::rebuildGrid()
{
    // 先把所有条目从网格中摘下来并隐藏（不销毁），再按过滤结果重新排布。
    while (m_grid->count()) {
        QLayoutItem* item = m_grid->takeAt(0);
        delete item;
    }
    for (QLabel* label : m_categoryLabels) label->hide();
    for (Cell& cell : m_cells) cell.button->hide();

    int row = 0, column = 0;
    if (m_filter.isEmpty()) {
        int cellIndex = 0;
        for (int groupIndex = 0; groupIndex < m_categoryLabels.size(); ++groupIndex) {
            QLabel* label = m_categoryLabels.at(groupIndex);
            m_grid->addWidget(label, row, 0, 1, kGridColumns, Qt::AlignLeft);
            label->show();
            ++row;
            const int count = kGroups[groupIndex].count;
            for (int i = 0; i < count; ++i) {
                Cell& cell = m_cells[cellIndex++];
                cell.button->show();
                m_grid->addWidget(cell.button, row, column++);
                if (column == kGridColumns) {
                    column = 0;
                    ++row;
                }
            }
            if (column != 0) { // 分类之间换行
                column = 0;
                ++row;
            }
        }
    } else {
        for (Cell& cell : m_cells) {
            if (!cell.matches(m_filter)) continue;
            cell.button->show();
            m_grid->addWidget(cell.button, row, column++);
            if (column == kGridColumns) {
                column = 0;
                ++row;
            }
        }
    }
    m_gridHost->adjustSize();
    m_scroll->verticalScrollBar()->setValue(0);
}

void EmojiPickerPopup::applyFilter(const QString& query)
{
    if (m_filter == query) return;
    m_filter = query;
    rebuildGrid();
    int visible = 0;
    for (const Cell& cell : m_cells) {
        if (cell.matches(m_filter)) ++visible;
    }
    m_emptyHint->setVisible(visible == 0);
    m_randomButton->setEnabled(visible > 0);
}

void EmojiPickerPopup::chooseEmoji(const QString& emoji)
{
    hide();
    emit emojiSelected(emoji);
    accept();
}

void EmojiPickerPopup::confirmFromSearch()
{
    // 用户直接输入了一个 emoji：回车即选中它。
    const QString typed = firstEmojiIn(m_search->text());
    if (!typed.isEmpty()) {
        chooseEmoji(typed);
        return;
    }
    // 普通过滤词：结果唯一时回车选中，否则不动作（避免误选）。
    const Cell* only = nullptr;
    int visible = 0;
    for (const Cell& cell : m_cells) {
        if (!cell.matches(m_filter)) continue;
        only = &cell;
        ++visible;
    }
    if (visible == 1 && only) chooseEmoji(only->emoji);
}

void EmojiPickerPopup::pickRandom()
{
    QVector<const Cell*> visible;
    for (const Cell& cell : m_cells) {
        if (cell.matches(m_filter)) visible.append(&cell);
    }
    if (visible.isEmpty()) return;
    const Cell* picked = visible.at(QRandomGenerator::global()->bounded(visible.size()));
    chooseEmoji(picked->emoji);
}

void EmojiPickerPopup::applyTheme()
{
    m_card->setStyleSheet(QStringLiteral(
        "QWidget#EmojiPickerCard{background:%1;border:1px solid %2;border-radius:12px;}"
        "QLabel{background:transparent;border:0;color:%3;}"
        "QLabel[muted=\"true\"]{color:%4;}"
        "QLineEdit#EmojiSearchEdit{color:%3;background:%5;border:1px solid %6;"
        "border-radius:6px;padding:5px 8px;selection-background-color:%7;"
        "selection-color:%8;}"
        "QLineEdit#EmojiSearchEdit:focus{border-color:%9;}"
        "QPushButton#EmojiRandomButton,QPushButton#EmojiResetButton{color:%4;"
        "background:transparent;border:1px solid %6;border-radius:6px;padding:4px 8px;}"
        "QPushButton#EmojiRandomButton:hover,QPushButton#EmojiResetButton:hover{background:%10;}"
        "QPushButton#EmojiRandomButton:focus,QPushButton#EmojiResetButton:focus{border-color:%9;}"
        "QPushButton#EmojiRandomButton:disabled,QPushButton#EmojiResetButton:disabled{color:%11;}"
        "QToolButton#EmojiCell{background:transparent;border:1px solid transparent;"
        "border-radius:6px;padding:0;}"
        "QToolButton#EmojiCell:hover{background:%10;}"
        "QToolButton#EmojiCell:pressed{background:%12;}"
        "QToolButton#EmojiCell:focus{border-color:%9;}"
        "QScrollArea#EmojiScroll{background:transparent;border:0;}"
        "QWidget#EmojiGrid{background:transparent;}")
        .arg(token(ds::ColorToken::MenuBg).name())
        .arg(token(ds::ColorToken::Divider).name())
        .arg(token(ds::ColorToken::Text).name())
        .arg(token(ds::ColorToken::TextSecondary).name())
        .arg(token(ds::ColorToken::Field).name())
        .arg(token(ds::ColorToken::Border).name())
        .arg(token(ds::ColorToken::Primary).name())
        .arg(token(ds::ColorToken::OnPrimary).name())
        .arg(token(ds::ColorToken::FocusRing).name())
        .arg(token(ds::ColorToken::Hover).name())
        .arg(token(ds::ColorToken::MenuDisabled).name())
        .arg(token(ds::ColorToken::FieldHover).name()));
    if (auto* effect = qobject_cast<QGraphicsDropShadowEffect*>(m_card->graphicsEffect())) {
        QColor tint = token(ds::ColorToken::Shadow);
        tint.setAlpha(40);
        effect->setColor(tint);
    }
    m_searchAction->setIcon(AppIcons::icon(IconId::Search, ds::ColorToken::TextSecondary));
    QPalette palette = m_search->palette();
    palette.setColor(QPalette::PlaceholderText, token(ds::ColorToken::TextTertiary));
    m_search->setPalette(palette);
    m_card->update();
}

void EmojiPickerPopup::popupAt(QWidget* anchor, bool above)
{
    if (!anchor) return;
    const QPoint top = anchor->mapToGlobal(QPoint(0, 0));
    const QPoint bottom = anchor->mapToGlobal(QPoint(0, anchor->height()));
    QPoint position(top.x() - kShadowMargin,
                    above ? top.y() - height() + kShadowMargin - 4
                          : bottom.y() - kShadowMargin + 4);
    QScreen* screen = QGuiApplication::screenAt(top);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect available = screen->availableGeometry();
        if (above && position.y() < available.top()) position.setY(bottom.y() + 4);
        if (!above && position.y() + height() > available.bottom())
            position.setY(top.y() - height() - 4);
        position.setX(qBound(available.left(), position.x(),
                             qMax(available.left(), available.right() - width() + 1)));
        position.setY(qBound(available.top(), position.y(),
                             qMax(available.top(), available.bottom() - height() + 1)));
    }
    move(position);
    show();
    raise();
    m_search->setFocus();
    m_search->selectAll();
}

} // namespace kl
