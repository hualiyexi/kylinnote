#pragma once
#include <QObject>
#include <QString>
#include <functional>

namespace kl {

// 开机自启的共享策略层：统一“哪种运行方式允许自启、默认登记只做一次、
// 用户显式选择永远优先于自动初始化”的规则，供主程序启动逻辑与设置入口共用。
// 平台读写仍由 kl::Autostart 完成并被原样保留；本类通过 Backend 注入，
// 测试用替身后端即可覆盖全部策略分支，不触碰真实注册表或 Linux 自启文件。
//
// 一次性标记复用 Settings::autostartInitialized()（不新增首选项键）：
//   - 仅 Installed 在 initializeDefault() 中自动登记一次；已存在或写入成功
//     才落盘标记，写入失败保持 false，下次启动可重试。
//   - 任何成功的显式 setEnabled()（Preview/Portable/Installed）同样落盘标记，
//     之后自动初始化不再覆盖用户的决定——显式关闭不会被复活。
//   - isEnabled() 始终反映真实登记状态，与标记无关。
class AutostartSettings : public QObject {
    Q_OBJECT
public:
    enum class Mode {
        Isolated,     // 纯隔离（自检/无界面）：绝不读写操作系统自启
        Unavailable,  // 当前运行方式不支持自启：行为与 Isolated 相同
        Preview,      // 开发预览（--data-dir + --preview-autostart 显式开启）：
                      // 允许用户手动登记到独立的预览槽位（Windows KylinListPreview
                      // / Linux kylinlist-preview.desktop），绝不自动登记，也绝不
                      // 读写正式安装槽位；isEnabled() 只认与当前数据目录登记命令
                      // 完全一致的预览条目
        Portable,     // 便携版：只响应用户显式开关，不做默认登记
        Installed     // 正式安装：首次运行默认登记一次，之后尊重用户选择
    };

    // 平台后端抽象：isInstalled 查询真实登记；setEnabled 写入并在失败时填充
    // error。两个回调都非空后端才可用（isAvailable() 为 true）。
    struct Backend {
        std::function<bool()> isInstalled;
        std::function<bool(bool, QString*)> setEnabled;
    };

    // 生产构造：委托 kl::Autostart 的平台实现（正式安装槽位）。Isolated/
    // Unavailable 永远置空后端；Preview 在此构造下没有数据目录，同样置空
    // （isAvailable() 为 false，拒绝登记）；Installed/Portable 在检测到
    // KYLINLIST_DATA_DIR（隔离运行标记）时仍置空，防止误传模式写真实
    // 注册表/自启文件。
    explicit AutostartSettings(Mode mode, QObject* parent = nullptr);

    // 生产预览构造：previewDataDir 必须是显式提供的隔离数据目录（main.cpp
    // 仅在 --preview-autostart 与显式 --data-dir 同时出现时到达这里）。
    // 内部捕获其绝对路径并仅在 mode 为 Preview 且目录非空时接真实预览后端：
    // 登记写入独立预览槽位，isEnabled() 按该目录的登记命令精确匹配，关闭时
    // 绝不删除不匹配的预览条目，全程不触碰正式安装槽位。其他模式行为与默认
    // 生产构造一致（目录被忽略）。
    AutostartSettings(Mode mode, const QString& previewDataDir, QObject* parent = nullptr);

    // 测试构造：注入替身后端；不受 KYLINLIST_DATA_DIR 影响，不触碰任何
    // 真实槽位。previewDataDir() 恒为空。
    AutostartSettings(Mode mode, Backend backend, QObject* parent = nullptr);
    ~AutostartSettings() override;

    Mode mode() const;

    // 生产预览构造捕获的隔离数据目录绝对路径；其他构造恒为空。
    QString previewDataDir() const;

    // Preview/Portable/Installed 且后端可用时为 true；Isolated/Unavailable
    // 恒为 false。
    bool isAvailable() const;

    // 当前是否真的登记了自启（实时查询后端），不读取一次性标记。
    // Preview 只认预览槽位中与当前数据目录登记命令完全一致的条目；
    // Isolated/Unavailable 恒为 false 且不触达后端。
    bool isEnabled() const;

    // 用户显式开启/关闭。成功后落盘一次性标记（显式选择优先于自动初始化），
    // 观测状态变化时发出 enabledChanged()。失败返回 false 并填充 error，
    // 若失败前后真实状态发生变化（如半写成功）同样发出 enabledChanged()
    // 供界面重读。Isolated/Unavailable 直接失败，不触达后端与标记。
    // 两个接口的 error 契约一致：返回 true 时 *error 置空，返回 false 时
    // 描述失败原因（含复用旧 QString 的调用方）。
    bool setEnabled(bool enabled, QString* error = nullptr);

    // 默认登记（应用启动时调用一次）：
    //   - Isolated/Unavailable/Preview/Portable：无事可做，视为成功，
    //     不触达后端与标记（Preview 仅允许用户手动开启）。
    //   - Installed：标记已落盘则直接成功；否则已登记则补标记，未登记则尝试
    //     写入，成功才落盘标记；失败返回 false 并填充 error，下次启动重试。
    bool initializeDefault(QString* error = nullptr);

signals:
    // isEnabled() 的观测结果发生变化时发出；两个设置入口共用同一实例时
    // 均可据此刷新开关状态。
    void enabledChanged();

private:
    bool backendOperational() const;
    void markInitialized();

    Mode m_mode;
    Backend m_backend;
    QString m_previewDataDir;
};

}  // namespace kl
