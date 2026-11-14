#pragma once
#include <QIcon>
#include <QString>
namespace kl {
namespace Branding {

// 品牌定制（白标）：启动时从本机读取一次 branding.json，允许单位覆盖
// 应用显示名与托盘图标。默认品牌为「麒麟清单」。
//
// 约定：
// - 配置目录与素材都在仓库外，本模块只读，不写任何文件；
// - 任何配置损坏都只 qWarning 并回退默认，绝不阻止启动；
// - 所有函数只应在主线程调用（appIcon 涉及 QIcon/Gui）。

// 重读品牌配置。只读取 JSON 文本，不创建 QPixmap，可在 QApplication
// 构造后调用；重复调用安全（每次先恢复默认再套用配置），便于测试。
// 搜索顺序（命中指目录下存在 branding.json 文件）：
//   1. 环境变量 KYLINLIST_BRANDING_DIR
//   2. 数据目录 <dataDirPath>/branding
//   3. 可执行目录 <applicationDirPath>/branding（无 QCoreApplication 时跳过）
//   4. Linux: /etc/kylinlist/branding
void load();

// 当前显示名；未定制或配置无效时返回默认「麒麟清单」。
QString displayName();

// 定制图标；未配置 logo 或加载失败时返回 null QIcon，
// 由调用方（如 TrayController）自绘默认图标。
QIcon appIcon();

// 最近一次 load() 命中的配置目录（绝对路径，不含文件名）；
// 没有命中任何目录时返回空串。
QString configDirectory();

} // namespace Branding
} // namespace kl
