#include "BrandingIcons.h"

#include <QPainter>
#include <QPixmap>
#include <QSize>

#include "../app/Branding.h"
#include "AppIcons.h"
#include "design/DesignTokens.h"
#include "design/ThemeManager.h"

namespace kl {
namespace {

// 合成版的逻辑尺寸阶梯：覆盖托盘、标题栏、任务栏与大尺寸场景。
const int kLogicalSizes[] = {16, 24, 32, 48, 64, 128, 256};
// 每个逻辑尺寸同时生成 1x/2x，供启用 AA_UseHighDpiPixmaps 的环境取用。
const qreal kPixelRatios[] = {1.0, 2.0};
// 超大素材先等比缩到该边长再派生各尺寸，避免反复处理照片级原图。
const int kMasterEdgeLimit = 512;

// 默认品牌标设计稿边长：主色圆角方块 + 对勾，几何与原 TrayController
// 默认托盘图标逐像素一致（5,4,54,56 / 圆角 12 / 对勾区 13,13,38,38）。
constexpr qreal kDefaultMarkDesignEdge = 64.0;

struct SynthCache {
    QString key;   // 失效键；空表示尚未构建
    QIcon app;     // 白底软件版
};

// 失效键用 QIcon::cacheKey() 作为“加载代数”：Branding::load() 每次都会
// 重建 QIcon（或重置为空图标，cacheKey 为 0），显式重读配置后绝不复用
// 旧渲染，即使路径、大小、修改时间都没变。
QString synthCacheKey(const QIcon& custom)
{
    auto& theme = ThemeManager::instance();
    return QStringLiteral("%1:%2:%3:%4").arg(custom.cacheKey())
        .arg(theme.color(ds::ColorToken::Primary).rgba())
        .arg(theme.color(ds::ColorToken::OnPrimary).rgba())
        .arg(theme.color(ds::ColorToken::AppIconBg).rgba());
}

// 默认品牌标绘制（全程序唯一实现）。edge 为目标设备像素边长，几何按
// 64 设计稿等比缩放；按目标尺寸原生绘制以保证小尺寸清晰。
void paintDefaultMark(QPainter* painter, int edge)
{
    const qreal s = edge / kDefaultMarkDesignEdge;
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(ThemeManager::instance().color(ds::ColorToken::Primary));
    painter->drawRoundedRect(QRectF(5 * s, 4 * s, 54 * s, 56 * s), 12 * s, 12 * s);
    AppIcons::paint(painter, IconId::Check, QRectF(13 * s, 13 * s, 38 * s, 38 * s),
                    ThemeManager::instance().color(ds::ColorToken::OnPrimary));
}

// 把素材等比放入 edge×edge 方形画布并居中。master 为空时画默认品牌标。
// whiteCanvas 为真时画布填充固定白（AppIconBg 令牌），否则
// 透明。缩放只用 KeepAspectRatio + Smooth，不裁剪、不染色。
QPixmap composeSquare(const QPixmap& master, int edge, qreal dpr, bool whiteCanvas)
{
    QPixmap canvas(edge, edge);
    canvas.fill(whiteCanvas
        ? ThemeManager::instance().color(ds::ColorToken::AppIconBg)
        : Qt::transparent);
    {
        QPainter painter(&canvas);
        if (master.isNull()) {
            paintDefaultMark(&painter, edge);
        } else {
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            QSize scaled = master.size();
            scaled.scale(edge, edge, Qt::KeepAspectRatio);
            const int x = (edge - scaled.width()) / 2;
            const int y = (edge - scaled.height()) / 2;
            painter.drawPixmap(x, y,
                               master.scaled(scaled, Qt::KeepAspectRatio,
                                             Qt::SmoothTransformation));
        }
    }
    canvas.setDevicePixelRatio(dpr);
    return canvas;
}

QIcon makeIcon(const QPixmap& master, bool whiteCanvas)
{
    QIcon icon;
    for (int size : kLogicalSizes) {
        for (qreal dpr : kPixelRatios)
            icon.addPixmap(composeSquare(master, qRound(size * dpr), dpr, whiteCanvas),
                           QIcon::Normal, QIcon::Off);
    }
    return icon;
}

// 把 Branding::appIcon() 的素材渲染成等比“母版”位图：保持原始宽高比与
// 透明度，只在超过 kMasterEdgeLimit 时整体等比缩小。返回空 QPixmap 表示
// 取不到像素（调用方回退默认品牌标）。
QPixmap masterPixmap(const QIcon& source)
{
    QSize native = source.actualSize(QSize(1 << 16, 1 << 16));
    if (native.isEmpty())
        native = QSize(64, 64);  // 拿不到固有尺寸时的保守渲染尺寸
    if (native.width() > kMasterEdgeLimit || native.height() > kMasterEdgeLimit)
        native.scale(kMasterEdgeLimit, kMasterEdgeLimit, Qt::KeepAspectRatio);
    QPixmap master = source.pixmap(native);
    if (master.isNull()) return QPixmap();
    master.setDevicePixelRatio(1.0);  // 统一按设备像素参与后续合成
    return master;
}

// 白底软件版（或无素材时的默认品牌标白底版），按 cacheKey 缓存。
const QIcon& synthesizedAppIcon(const QIcon& custom, const QString& key)
{
    static SynthCache cache;
    if (cache.key != key) {
        // 母版取不到像素的极端情况同样回退默认品牌标。
        const QPixmap master = custom.isNull() ? QPixmap() : masterPixmap(custom);
        cache.app = makeIcon(master, true);
        cache.key = key;
    }
    return cache.app;
}

// 无自定义素材时的默认品牌标（透明底，多尺寸 + 1x/2x）。同一 load 周期
// 内结果确定，直接按 cacheKey 复用同一份缓存（key 与白底版一致）。
const QIcon& defaultLogoIcon(const QString& key)
{
    static SynthCache cache;
    if (cache.key != key) {
        cache.app = makeIcon(QPixmap(), false);
        cache.key = key;
    }
    return cache.app;
}

}  // namespace

QIcon brandLogoIcon()
{
    const QIcon custom = Branding::appIcon();  // 空表示未配置或坏图
    const QString key = synthCacheKey(custom);
    if (!custom.isNull())
        return custom;  // 原始透明 Logo 原样透传：保留矢量与原生尺寸能力，不做重采样
    return defaultLogoIcon(key);
}

QIcon applicationBrandIcon()
{
    const QIcon custom = Branding::appIcon();
    return synthesizedAppIcon(custom, synthCacheKey(custom));
}

}  // namespace kl
