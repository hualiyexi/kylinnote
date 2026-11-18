#include "AutostartSettings.h"
#include "Settings.h"
#include "../platform/Autostart.h"

#include <QDir>

namespace kl {
namespace {

// 捕获预览数据目录的规范形式：非空才转绝对路径，与后端判空口径一致。
QString capturedPreviewDataDir(const QString& previewDataDir)
{
    const QString trimmed = previewDataDir.trimmed();
    return trimmed.isEmpty() ? QString() : QDir(trimmed).absolutePath();
}

AutostartSettings::Backend productionBackend(AutostartSettings::Mode mode,
                                             const QString& previewDataDir)
{
    // 纯隔离/不可用模式永远不接真实平台后端。
    if (mode == AutostartSettings::Mode::Isolated || mode == AutostartSettings::Mode::Unavailable)
        return AutostartSettings::Backend();
    // 防御性约束：KYLINLIST_DATA_DIR 是隔离运行的既有判定（main.cpp 的
    // --data-dir/--smoke-test 与 KylinListApp 的 m_isolatedRun 都使用它）。
    // 只有 Preview 在显式提供数据目录时才允许接真实后端（且只写独立预览
    // 槽位）；其余模式在隔离环境下仍置空，即使上层误传 Installed/Portable
    // 也不会读写真实注册表或 Linux 用户自启目录。仅生产构造受此约束，
    // 测试替身不受影响。
    if (mode != AutostartSettings::Mode::Preview
        && qEnvironmentVariableIsSet("KYLINLIST_DATA_DIR"))
        return AutostartSettings::Backend();
    AutostartSettings::Backend backend;
    if (mode == AutostartSettings::Mode::Preview) {
        // 预览只认显式提供了数据目录的运行：无目录（默认生产构造）一律
        // 置空后端，普通 --data-dir 调试与 smoke-test 都不可能登记自启。
        const QString dataDir = capturedPreviewDataDir(previewDataDir);
        if (dataDir.isEmpty())
            return AutostartSettings::Backend();
        backend.isInstalled = [dataDir] {
            // 只认预览槽位中与本数据目录登记命令完全一致的条目；别的数据
            // 目录或手工修改的条目都不应显示为已开启。
            return Autostart::isPreviewRegisteredFor(dataDir);
        };
        backend.setEnabled = [dataDir](bool enabled, QString* error) {
            // 只读写独立预览槽位；关闭时不匹配的预览条目绝不被删除。
            return Autostart::setPreviewEnabled(enabled, dataDir, error);
        };
        return backend;
    }
    backend.isInstalled = [] {
        return Autostart::isInstalled();
    };
    backend.setEnabled = [](bool enabled, QString* error) {
        return Autostart::setEnabled(enabled, error);
    };
    return backend;
}

QString unavailableErrorText()
{
    return QStringLiteral("当前运行方式不支持设置开机自启");
}

}  // namespace

AutostartSettings::AutostartSettings(Mode mode, QObject* parent)
    : AutostartSettings(mode, QString(), parent)
{
}

AutostartSettings::AutostartSettings(Mode mode, const QString& previewDataDir, QObject* parent)
    : QObject(parent)
    , m_mode(mode)
    , m_backend(productionBackend(mode, previewDataDir))
{
    // 仅当预览后端真实接通时暴露目录；其他模式目录被忽略，保持为空。
    if (mode == Mode::Preview && m_backend.isInstalled && m_backend.setEnabled)
        m_previewDataDir = capturedPreviewDataDir(previewDataDir);
}

AutostartSettings::AutostartSettings(Mode mode, Backend backend, QObject* parent)
    : QObject(parent), m_mode(mode), m_backend(std::move(backend))
{
}

AutostartSettings::~AutostartSettings() = default;

AutostartSettings::Mode AutostartSettings::mode() const
{
    return m_mode;
}

QString AutostartSettings::previewDataDir() const
{
    return m_previewDataDir;
}

bool AutostartSettings::backendOperational() const
{
    return (m_mode == Mode::Preview || m_mode == Mode::Portable || m_mode == Mode::Installed)
        && m_backend.isInstalled && m_backend.setEnabled;
}

bool AutostartSettings::isAvailable() const
{
    return backendOperational();
}

bool AutostartSettings::isEnabled() const
{
    if (!backendOperational())
        return false;
    return m_backend.isInstalled();
}

void AutostartSettings::markInitialized()
{
    Settings& settings = Settings::instance();
    if (!settings.autostartInitialized()) {
        settings.setAutostartInitialized(true);
        settings.sync();
    }
}

bool AutostartSettings::setEnabled(bool enabled, QString* error)
{
    if (error)
        error->clear();  // 契约：成功返回时 *error 恒为空，失败时描述原因
    if (!backendOperational()) {
        if (error)
            *error = unavailableErrorText();
        return false;
    }
    const bool before = m_backend.isInstalled();
    QString backendError;
    const bool ok = m_backend.setEnabled(enabled, &backendError);
    if (!ok && error)
        *error = backendError.isEmpty() ? QStringLiteral("写入开机自启设置失败")
                                        : backendError;
    if (ok) {
        // 显式选择优先：成功的显式修改落盘一次性标记，之后的自动初始化
        // 不再覆盖用户的决定（显式关闭的持久性由此保证）。
        markInitialized();
    }
    const bool after = m_backend.isInstalled();
    if (before != after)
        emit enabledChanged();
    return ok;
}

bool AutostartSettings::initializeDefault(QString* error)
{
    if (error)
        error->clear();  // 契约：成功返回时 *error 恒为空，失败时描述原因
    // 隔离/不可用：没有默认可初始化，也不允许任何操作系统读写；
    // 视为成功且不报错，避免开发预览出现无意义的启动告警。
    if (m_mode == Mode::Isolated || m_mode == Mode::Unavailable)
        return true;
    // 便携版只响应显式选择：不做默认登记，也不落盘标记。
    // 开发预览同样只允许手动开启，绝不自动写系统启动项。
    if (m_mode == Mode::Portable || m_mode == Mode::Preview)
        return true;
    // Installed 但后端缺失（生产构造的隔离防御）：按配置错误上抛。
    if (!backendOperational()) {
        if (error)
            *error = unavailableErrorText();
        return false;
    }
    Settings& settings = Settings::instance();
    if (settings.autostartInitialized())
        return true;  // 一次性初始化已完成：尊重既有决定，不触达后端
    const bool before = m_backend.isInstalled();
    if (!before) {
        QString backendError;
        if (!m_backend.setEnabled(true, &backendError)) {
            // 写入失败：标记保持未落盘，下次启动重试。
            if (error)
                *error = backendError.isEmpty() ? QStringLiteral("登记开机自启失败")
                                                : backendError;
            if (m_backend.isInstalled() != before)
                emit enabledChanged();  // 失败但真实状态已变（半写）：通知界面重读
            return false;
        }
    }
    // 已存在（沿用既有登记）或本次写入成功：标记落盘。
    markInitialized();
    if (m_backend.isInstalled() != before)
        emit enabledChanged();
    return true;
}

}  // namespace kl
