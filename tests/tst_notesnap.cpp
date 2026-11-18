#include "ui/NoteSnap.h"

#include <QRect>
#include <QVector>
#include <QtTest>

using namespace kl;

using RectList = QVector<QRect>;

// 纯几何单测：所有矩形由测试提供，不依赖屏幕、窗口或平台插件。
class NoteSnapTest : public QObject
{
    Q_OBJECT

private slots:
    void snapped_data();
    void snapped();
    void arranged_data();
    void arranged();
};

void NoteSnapTest::snapped_data()
{
    // QFETCH 是宏：模板参数里的逗号会拆散宏参数，故取值一律用 RectList 别名
    // （qData 按 qMetaTypeId<int> 匹配，别名与 QVector<QRect> 同一 id）。
    QTest::addColumn<QRect>("proposed");
    QTest::addColumn<QRect>("screen");
    QTest::addColumn<QVector<QRect>>("peers");
    QTest::addColumn<int>("threshold");
    QTest::addColumn<int>("gap");
    QTest::addColumn<QRect>("expected");

    const QRect work(0, 0, 1920, 1040);
    const QRect leftScreen(-1920, 0, 1920, 1040);
    const RectList none;

    // --- 工作区四边贴平（含双轴同贴与 right()+1 语义） ---
    QTest::newRow("screen left edge")
        << QRect(7, 200, 120, 90) << work << none << 10 << 8 << QRect(0, 200, 120, 90);
    QTest::newRow("screen right edge")
        << QRect(1805, 100, 120, 90) << work << none << 10 << 8 << QRect(1800, 100, 120, 90);
    QTest::newRow("screen top edge")
        << QRect(300, 6, 120, 90) << work << none << 10 << 8 << QRect(300, 0, 120, 90);
    QTest::newRow("screen bottom edge")
        << QRect(300, 952, 120, 90) << work << none << 10 << 8 << QRect(300, 950, 120, 90);
    QTest::newRow("screen corner both axes")
        << QRect(4, 6, 120, 90) << work << none << 10 << 8 << QRect(0, 0, 120, 90);
    QTest::newRow("already flush right no off by one")
        << QRect(1800, 100, 120, 90) << work << none << 10 << 8 << QRect(1800, 100, 120, 90);
    QTest::newRow("one pixel past right edge")
        << QRect(1801, 100, 120, 90) << work << none << 10 << 8 << QRect(1800, 100, 120, 90);
    QTest::newRow("approach from left of screen")
        << QRect(-5, 300, 120, 90) << work << none << 10 << 8 << QRect(0, 300, 120, 90);

    // --- 阈值边界：含等于 threshold，超出 1px 即不吸附 ---
    QTest::newRow("threshold inclusive")
        << QRect(10, 300, 120, 90) << work << none << 10 << 8 << QRect(0, 300, 120, 90);
    QTest::newRow("just beyond threshold")
        << QRect(11, 300, 120, 90) << work << none << 10 << 8 << QRect(11, 300, 120, 90);

    // --- peer 边缘对齐（单轴变化，另一轴保持原位） ---
    const RectList peerAt500{QRect(500, 200, 100, 100)};
    QTest::newRow("peer align left edges x only")
        << QRect(507, 260, 120, 90) << work << peerAt500 << 10 << 8 << QRect(500, 260, 120, 90);
    QTest::newRow("peer align right edges")
        << QRect(488, 210, 120, 90) << work << peerAt500 << 10 << 8 << QRect(480, 210, 120, 90);
    QTest::newRow("peer align top edges y only")
        << QRect(520, 202, 120, 90) << work << peerAt500 << 10 << 8 << QRect(520, 200, 120, 90);
    QTest::newRow("peer align bottom edges")
        << QRect(520, 212, 120, 90) << work << peerAt500 << 10 << 8 << QRect(520, 210, 120, 90);

    // --- 邻接：请求间距与零间距共存，距离相等偏向请求间距，就近者胜 ---
    QTest::newRow("tie favors requested gap")
        << QRect(604, 230, 120, 90) << work << peerAt500 << 10 << 8 << QRect(608, 230, 120, 90);
    QTest::newRow("gap zero gives flush only")
        << QRect(604, 230, 120, 90) << work << peerAt500 << 10 << 0 << QRect(600, 230, 120, 90);
    QTest::newRow("flush wins when closer")
        << QRect(601, 230, 120, 90) << work << peerAt500 << 10 << 8 << QRect(600, 230, 120, 90);
    QTest::newRow("requested gap wins when closer")
        << QRect(612, 230, 120, 90) << work << peerAt500 << 10 << 8 << QRect(608, 230, 120, 90);
    QTest::newRow("flush below peer when closer")
        << QRect(520, 303, 120, 90) << work << peerAt500 << 10 << 8 << QRect(520, 300, 120, 90);
    QTest::newRow("gap below peer when closer")
        << QRect(520, 306, 120, 90) << work << peerAt500 << 10 << 8 << QRect(520, 308, 120, 90);
    QTest::newRow("flush above peer")
        << QRect(520, 112, 120, 90) << work << peerAt500 << 10 << 8 << QRect(520, 110, 120, 90);

    // --- 对角：两轴各自命中，角落贴角落 ---
    QTest::newRow("diagonal corner beside peer")
        << QRect(604, 294, 120, 90) << work << peerAt500 << 10 << 8 << QRect(608, 300, 120, 90);

    // --- 远处便签不吸引（候选距离虽在阈值内，但另一轴不相干） ---
    QTest::newRow("distant peer no adjacency attraction")
        << QRect(500, 100, 120, 90) << work << RectList{QRect(630, 800, 100, 100)} << 10 << 8
        << QRect(500, 100, 120, 90);
    QTest::newRow("distant peer no alignment attraction")
        << QRect(500, 100, 120, 90) << work << RectList{QRect(505, 800, 100, 100)} << 10 << 8
        << QRect(500, 100, 120, 90);
    // 不重叠但间隙 <= threshold 的便签仍算相关（可吸到其上方并留间距）。
    QTest::newRow("near but disjoint peer still attracts")
        << QRect(500, 100, 120, 90) << work << RectList{QRect(505, 195, 100, 100)} << 10 << 8
        << QRect(505, 97, 120, 90);

    // --- 距离相等的决胜：工作区 > peer；peer 之间按输入顺序 ---
    QTest::newRow("screen beats peer on exact tie")
        << QRect(4, 200, 120, 90) << work << RectList{QRect(8, 200, 100, 100)} << 10 << 8
        << QRect(0, 200, 120, 90);
    QTest::newRow("peer order tie first wins")
        << QRect(498, 250, 120, 90) << work
        << RectList{QRect(500, 200, 100, 100), QRect(396, 200, 100, 100)} << 10 << 8
        << QRect(500, 250, 120, 90);
    QTest::newRow("peer order tie reversed input")
        << QRect(498, 250, 120, 90) << work
        << RectList{QRect(396, 200, 100, 100), QRect(500, 200, 100, 100)} << 10 << 8
        << QRect(496, 250, 120, 90);

    // --- 负坐标多显示器 ---
    QTest::newRow("negative screen left edge")
        << QRect(-1915, 300, 120, 90) << leftScreen << none << 10 << 8 << QRect(-1920, 300, 120, 90);
    QTest::newRow("negative screen flush right")
        << QRect(-120, 300, 120, 90) << leftScreen << none << 10 << 8 << QRect(-120, 300, 120, 90);
    QTest::newRow("negative screen one past right edge")
        << QRect(-119, 300, 120, 90) << leftScreen << none << 10 << 8 << QRect(-120, 300, 120, 90);
    QTest::newRow("negative peer gap")
        << QRect(-1810, 250, 120, 90) << leftScreen << RectList{QRect(-1920, 200, 100, 100)}
        << 10 << 8 << QRect(-1812, 250, 120, 90);

    // --- 非法输入 ---
    QTest::newRow("invalid proposed unchanged")
        << QRect(10, 10, 0, 50) << work << none << 10 << 8 << QRect(10, 10, 0, 50);
    QTest::newRow("default proposed unchanged")
        << QRect() << work << none << 10 << 8 << QRect();
    QTest::newRow("invalid peers ignored")
        << QRect(505, 250, 120, 90) << work
        << RectList{QRect(500, 200, 0, 100), QRect(500, 200, 100, 0)} << 10 << 8
        << QRect(505, 250, 120, 90);
    QTest::newRow("invalid screen falls back to peers")
        << QRect(505, 250, 120, 90) << QRect(0, 0, -5, 10) << peerAt500 << 10 << 8
        << QRect(500, 250, 120, 90);

    // --- 参数防御与自由位置 ---
    QTest::newRow("negative threshold disables snap")
        << QRect(7, 200, 120, 90) << work << none << -3 << 8 << QRect(7, 200, 120, 90);
    QTest::newRow("negative gap clamps to flush")
        << QRect(604, 230, 120, 90) << work << peerAt500 << 10 << -5 << QRect(600, 230, 120, 90);
    QTest::newRow("odd size preserved at corner")
        << QRect(5, 7, 137, 83) << work << none << 10 << 8 << QRect(0, 0, 137, 83);
    QTest::newRow("free position stays")
        << QRect(700, 400, 120, 90) << work << RectList{QRect(100, 100, 100, 100)} << 10 << 8
        << QRect(700, 400, 120, 90);
}

void NoteSnapTest::snapped()
{
    QFETCH(QRect, proposed);
    QFETCH(QRect, screen);
    QFETCH(RectList, peers);
    QFETCH(int, threshold);
    QFETCH(int, gap);
    QFETCH(QRect, expected);

    const RectList peersBefore = peers;  // 拖动期间绝不改动其它便签
    const QRect got = NoteSnap::snapped(proposed, screen, peers, threshold, gap);
    QCOMPARE(got.size(), proposed.size());
    QCOMPARE(got, expected);
    QCOMPARE(peers, peersBefore);
    // 纯函数：同样输入必须给出同样结果。
    QCOMPARE(NoteSnap::snapped(proposed, screen, peers, threshold, gap), got);
}

void NoteSnapTest::arranged_data()
{
    QTest::addColumn<QVector<QRect>>("papers");
    QTest::addColumn<QRect>("screen");
    QTest::addColumn<bool>("vertical");
    QTest::addColumn<int>("gap");
    QTest::addColumn<QVector<QRect>>("expected");

    const QRect desk(0, 0, 300, 200);
    const QRect tallDesk(0, 0, 200, 300);

    // --- 水平：向右排满换行，行距 = 当前行最高便签 + gap ---
    QTest::newRow("rows wrap")
        << RectList{QRect(10, 500, 100, 50), QRect(20, 600, 100, 50), QRect(30, 700, 100, 50)}
        << desk << false << 8
        << RectList{QRect(0, 0, 100, 50), QRect(108, 0, 100, 50), QRect(0, 58, 100, 50)};
    QTest::newRow("rows wrap uses tallest")
        << RectList{QRect(0, 0, 100, 50), QRect(0, 0, 100, 90), QRect(0, 0, 100, 50)}
        << desk << false << 8
        << RectList{QRect(0, 0, 100, 50), QRect(108, 0, 100, 90), QRect(0, 98, 100, 50)};
    QTest::newRow("oversize row leader pins left")
        << RectList{QRect(0, 0, 350, 50), QRect(0, 0, 100, 50)}
        << desk << false << 8
        << RectList{QRect(0, 0, 350, 50), QRect(0, 58, 100, 50)};
    QTest::newRow("rows clamp into bottom")
        << RectList{QRect(0, 0, 100, 150), QRect(0, 0, 100, 150), QRect(0, 0, 100, 150)}
        << desk << false << 8
        << RectList{QRect(0, 0, 100, 150), QRect(108, 0, 100, 150), QRect(0, 50, 100, 150)};
    QTest::newRow("exact fit no off by one wrap")
        << RectList{QRect(0, 0, 146, 100), QRect(0, 0, 146, 100)}
        << QRect(0, 0, 300, 100) << false << 8
        << RectList{QRect(0, 0, 146, 100), QRect(154, 0, 146, 100)};

    // --- 垂直：向下排满换列，列距 = 当前列最宽便签 + gap ---
    QTest::newRow("columns wrap")
        << RectList{QRect(0, 0, 50, 100), QRect(0, 0, 50, 100), QRect(0, 0, 50, 100)}
        << tallDesk << true << 8
        << RectList{QRect(0, 0, 50, 100), QRect(0, 108, 50, 100), QRect(58, 0, 50, 100)};
    QTest::newRow("columns wrap uses widest")
        << RectList{QRect(0, 0, 50, 100), QRect(0, 0, 90, 100), QRect(0, 0, 50, 100)}
        << tallDesk << true << 8
        << RectList{QRect(0, 0, 50, 100), QRect(0, 108, 90, 100), QRect(98, 0, 50, 100)};
    QTest::newRow("columns clamp into right")
        << RectList{QRect(0, 0, 90, 100), QRect(0, 0, 90, 100), QRect(0, 0, 90, 100)}
        << QRect(0, 0, 180, 300) << true << 8
        << RectList{QRect(0, 0, 90, 100), QRect(0, 108, 90, 100), QRect(90, 0, 90, 100)};

    // --- 间距与负坐标 ---
    QTest::newRow("rows zero gap")
        << RectList{QRect(9, 9, 100, 50), QRect(9, 9, 100, 50)} << desk << false << 0
        << RectList{QRect(0, 0, 100, 50), QRect(100, 0, 100, 50)};
    QTest::newRow("columns zero gap")
        << RectList{QRect(9, 9, 50, 100), QRect(9, 9, 50, 100)} << tallDesk << true << 0
        << RectList{QRect(0, 0, 50, 100), QRect(0, 100, 50, 100)};
    QTest::newRow("negative origin screen")
        << RectList{QRect(0, 0, 100, 50), QRect(0, 0, 100, 50), QRect(0, 0, 100, 50)}
        << QRect(-1920, 0, 300, 200) << false << 8
        << RectList{QRect(-1920, 0, 100, 50), QRect(-1812, 0, 100, 50), QRect(-1920, 58, 100, 50)};

    // --- 兜底：比工作区大、非法输入 ---
    QTest::newRow("note larger than screen pins top left")
        << RectList{QRect(40, 40, 400, 300)} << desk << false << 8
        << RectList{QRect(0, 0, 400, 300)};
    QTest::newRow("invalid screen returns input")
        << RectList{QRect(37, 41, 100, 50)} << QRect(0, 0, 0, 100) << false << 8
        << RectList{QRect(37, 41, 100, 50)};
    QTest::newRow("invalid papers pass through")
        << RectList{QRect(5, 5, 100, 50), QRect(7, 7, 0, 50), QRect(8, 8, 100, -1),
                    QRect(6, 6, 80, 40)}
        << desk << false << 8
        << RectList{QRect(0, 0, 100, 50), QRect(7, 7, 0, 50), QRect(8, 8, 100, -1),
                    QRect(108, 0, 80, 40)};
    QTest::newRow("empty input")
        << RectList() << desk << false << 8 << RectList();
}

void NoteSnapTest::arranged()
{
    QFETCH(RectList, papers);
    QFETCH(QRect, screen);
    QFETCH(bool, vertical);
    QFETCH(int, gap);
    QFETCH(RectList, expected);

    const RectList got = NoteSnap::arranged(papers, screen, vertical, gap);
    QCOMPARE(got.size(), papers.size());
    for (int i = 0; i < papers.size(); ++i)
        QCOMPARE(got.at(i).size(), papers.at(i).size());  // 任何情况下不改尺寸
    QCOMPARE(got, expected);
    QCOMPARE(NoteSnap::arranged(papers, screen, vertical, gap), got);  // 确定性
}

QTEST_GUILESS_MAIN(NoteSnapTest)
#include "tst_notesnap.moc"
