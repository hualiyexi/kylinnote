#ifndef KYLINLIST_EMOJIPICKERPOPUP_H
#define KYLINLIST_EMOJIPICKERPOPUP_H

#include <QDialog>
#include <QString>
#include <QVector>

class QAction;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QToolButton;
class QWidget;

namespace kl {

// 图标 emoji 选择弹层（对齐滴答清单图标选择器）：
//   - 搜索框 + “随机”“重置”按钮；
//   - 人物 / 动物与自然 / 食物 / 活动 / 旅行与地点 / 物体 / 符号 分类网格；
//   - emoji 是文本，直接画在按钮上，不走 SVG 图标表。
//
// 交互约定：
//   - 点击网格按钮 emit emojiSelected(emoji) 并关闭；
//   - “重置” emit emojiSelected(空串) 并关闭（表示清除图标）；
//   - “随机”只从当前可见（搜索过滤后）的按钮里挑一个；
//   - 搜索支持分类名、常见中文别名，也允许直接输入一个 emoji 后回车确认；
//   - Escape / 点击弹层外部关闭（reject，不 emit）。
class EmojiPickerPopup : public QDialog {
    Q_OBJECT
public:
    explicit EmojiPickerPopup(QWidget* parent = nullptr);

    // 在锚点控件上/下方弹出（above=true 时向上），并约束在屏幕可用区域内。
    void popupAt(QWidget* anchor, bool above);

signals:
    void emojiSelected(QString emoji);

private:
    struct Cell {
        QToolButton* button = nullptr;
        QString emoji;
        QString aliases;      // 空格分隔的中文别名
        QString categoryTitle;
        QString categoryKeywords;
        bool matches(const QString& query) const;
    };

    void buildUi();
    void applyTheme();
    void rebuildGrid();
    void applyFilter(const QString& query);
    void chooseEmoji(const QString& emoji);
    void confirmFromSearch();
    void pickRandom();

    QWidget* m_card = nullptr;
    QLineEdit* m_search = nullptr;
    QAction* m_searchAction = nullptr;
    QPushButton* m_randomButton = nullptr;
    QPushButton* m_resetButton = nullptr;
    QWidget* m_gridHost = nullptr;
    QGridLayout* m_grid = nullptr;
    QScrollArea* m_scroll = nullptr;
    QLabel* m_emptyHint = nullptr;
    QVector<Cell> m_cells;
    QVector<QLabel*> m_categoryLabels;
    QString m_filter;
};

} // namespace kl

#endif // KYLINLIST_EMOJIPICKERPOPUP_H
