#pragma once
#include <QString>

namespace kl {
namespace DesktopBranding {

// 安装阶段的系统桌面品牌覆盖（Linux）。
//
// 运行时品牌由 app/Branding 从 branding.json 读取；本模块在安装/卸载
// 脚本阶段（postinst 调用 `kylinlist --refresh-system-branding`，prerm
// remove 调用 `--clear-system-branding`）把该品牌落到系统菜单：
//   - 生成 <root>/share/applications/KylinList.desktop 覆盖入口，
//     只改写 Name= / Name[zh_CN]=（存在自定义显示名时两者都改为它）
//     与 Icon=（存在可解码自定义图标时改为图标绝对路径），
//     Exec/TryExec 等其余内容保持源文件原样，并写入
//     X-KylinList-Managed-Branding=true 标记表明由本程序管理；
//   - 自定义图标复制到 <root>/share/kylinlist/branding/active.<原扩展名>，
//     desktop 的 Icon 使用该绝对路径。
//
// 输出根默认 /usr/local，可用 KYLINLIST_DESKTOP_BRANDING_ROOT 覆盖；
// 源 desktop 默认 /usr/share/applications/KylinList.desktop，可用
// KYLINLIST_DESKTOP_ENTRY_SOURCE 覆盖（两者均用于测试隔离）。
//
// 约束：
// - 只依赖 QCoreApplication：内部用 QImageReader 真实解码验证 logo，
//   不使用 QPixmap/QIcon/显示服务器；
// - 幂等：重复执行结果一致；不存在有效自定义（自定义名称或可解码
//   自定义图标）时删除自己管理的 override，回退安装包默认入口；
// - 绝不覆盖不带管理标记的 desktop（可能是管理员手工维护的文件）；
// - 所有写入使用 QSaveFile 原子替换；失败返回 false 并给出中文错误。

// 读取 branding.json（内部调用 Branding::load()），存在有效自定义时
// 生成/更新 <root> 下的 desktop 覆盖与图标；无有效自定义时清理自建文件。
bool refreshSystem(QString *error = nullptr);

// 卸载清理：仅删除带 X-KylinList-Managed-Branding 标记的 desktop 与本
// 模块写入的 active.* 图标（含旧扩展名），其余文件一律不动。
bool clearSystem(QString *error = nullptr);

} // namespace DesktopBranding
} // namespace kl
