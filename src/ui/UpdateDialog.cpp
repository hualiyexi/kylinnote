#include "UpdateDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "../app/Settings.h"
#include "../app/updates/UpdateService.h"

namespace kl {
namespace {
QString sizeText(qint64 bytes)
{
    if (bytes >= 1024 * 1024)
        return QStringLiteral("%1 MB").arg(QString::number(bytes / (1024.0 * 1024.0), 'f', 1));
    if (bytes >= 1024) return QStringLiteral("%1 KB").arg(bytes / 1024);
    return QStringLiteral("%1 字节").arg(bytes);
}
} // namespace

UpdateDialog::UpdateDialog(updates::UpdateService *service, QWidget *parent)
    : QDialog(parent), m_service(service)
{
    setObjectName(QStringLiteral("UpdateDialog"));
    setWindowTitle(QStringLiteral("软件更新"));
    setMinimumWidth(440);
    auto *layout = new QVBoxLayout(this);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("UpdateStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_meta = new QLabel(this);
    m_meta->setObjectName(QStringLiteral("UpdateMeta"));
    m_meta->setWordWrap(true);
    layout->addWidget(m_meta);

    m_notes = new QPlainTextEdit(this);
    m_notes->setObjectName(QStringLiteral("UpdateNotes"));
    m_notes->setReadOnly(true);
    m_notes->setPlaceholderText(QStringLiteral("更新说明"));
    m_notes->setMaximumHeight(140);
    layout->addWidget(m_notes);

    m_progress = new QProgressBar(this);
    m_progress->setObjectName(QStringLiteral("UpdateProgress"));
    layout->addWidget(m_progress);

    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("UpdateError"));
    m_error->setWordWrap(true);
    layout->addWidget(m_error);

    auto *buttons = new QHBoxLayout();
    m_settings = new QPushButton(QStringLiteral("更新服务器设置…"), this);
    m_settings->setObjectName(QStringLiteral("UpdateSettings"));
    connect(m_settings, &QPushButton::clicked, this, &UpdateDialog::settingsRequested);
    buttons->addWidget(m_settings);
    buttons->addStretch(1);
    m_retry = new QPushButton(QStringLiteral("检查更新"), this);
    m_retry->setObjectName(QStringLiteral("UpdateRetryCheck"));
    connect(m_retry, &QPushButton::clicked, this, &UpdateDialog::retryClicked);
    buttons->addWidget(m_retry);
    m_download = new QPushButton(QStringLiteral("下载更新"), this);
    m_download->setObjectName(QStringLiteral("UpdateDownload"));
    connect(m_download, &QPushButton::clicked, this, &UpdateDialog::downloadClicked);
    buttons->addWidget(m_download);
    m_cancel = new QPushButton(QStringLiteral("取消下载"), this);
    m_cancel->setObjectName(QStringLiteral("UpdateCancelDownload"));
    connect(m_cancel, &QPushButton::clicked, this,
            [this]() { if (m_service) m_service->cancelDownload(); });
    buttons->addWidget(m_cancel);
    m_install = new QPushButton(QStringLiteral("安装并退出"), this);
    m_install->setObjectName(QStringLiteral("UpdateInstall"));
    connect(m_install, &QPushButton::clicked, this, &UpdateDialog::installClicked);
    buttons->addWidget(m_install);
    m_postpone = new QPushButton(QStringLiteral("暂缓 7 天"), this);
    m_postpone->setObjectName(QStringLiteral("UpdatePostpone"));
    connect(m_postpone, &QPushButton::clicked, this, &UpdateDialog::postponeClicked);
    buttons->addWidget(m_postpone);
    m_close = new QPushButton(QStringLiteral("关闭"), this);
    m_close->setObjectName(QStringLiteral("UpdateClose"));
    connect(m_close, &QPushButton::clicked, this, [this]() { reject(); });
    buttons->addWidget(m_close);
    layout->addLayout(buttons);

    if (!m_service) return;
    connect(m_service, &updates::UpdateService::stateChanged, this, &UpdateDialog::refresh);
    connect(m_service, &updates::UpdateService::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                if (total > 0) m_progress->setValue(int(qMin<qint64>(received * 100 / total, 100)));
                m_meta->setText(QStringLiteral("已下载 %1 / %2")
                                    .arg(sizeText(received))
                                    .arg(total > 0 ? sizeText(total) : QStringLiteral("未知")));
            });
    refresh();
}

void UpdateDialog::refresh()
{
    if (!m_service) return;
    const updates::UpdateService::State state = m_service->state();
    const updates::UpdateArtifact *artifact = m_service->targetArtifact();
    const QString current = m_service->currentVersion();
    QString status, meta;
    m_progress->setVisible(false);
    m_progress->setRange(0, 100);
    m_error->clear();
    switch (state) {
    case updates::UpdateService::State::Disabled:
        status = QStringLiteral("未配置更新服务器。");
        if (!m_service->errorText().isEmpty()) status += QStringLiteral("\n") + m_service->errorText();
        break;
    case updates::UpdateService::State::Idle:
        status = QStringLiteral("尚未检查更新。");
        break;
    case updates::UpdateService::State::Checking:
        status = QStringLiteral("正在检查更新…");
        m_progress->setRange(0, 0);
        m_progress->setVisible(true);
        break;
    case updates::UpdateService::State::UpToDate:
        status = QStringLiteral("已是最新版本 %1。").arg(current);
        break;
    case updates::UpdateService::State::UpdateAvailable:
        status = QStringLiteral("发现新版本 %1（当前 %2）。")
                     .arg(m_service->manifest().version, current);
        break;
    case updates::UpdateService::State::Downloading:
        status = QStringLiteral("正在下载 %1…").arg(m_service->manifest().version);
        m_progress->setValue(0);
        m_progress->setVisible(true);
        break;
    case updates::UpdateService::State::Ready:
        status = QStringLiteral("新版本 %1 已下载并校验完成，可以安装。")
                     .arg(m_service->manifest().version);
        break;
    case updates::UpdateService::State::Failed:
        status = QStringLiteral("检查更新失败。");
        m_error->setText(m_service->errorText());
        break;
    }
    if (artifact && (state == updates::UpdateService::State::UpdateAvailable
                     || state == updates::UpdateService::State::Downloading
                     || state == updates::UpdateService::State::Ready)) {
        meta = QStringLiteral("安装包 %1 · %2 · %3")
                   .arg(artifact->path.section(QLatin1Char('/'), -1),
                        sizeText(artifact->size), m_service->manifest().publishedAt);
    }
    m_status->setText(status);
    m_meta->setText(meta);
    if (state == updates::UpdateService::State::UpdateAvailable
        || state == updates::UpdateService::State::Downloading
        || state == updates::UpdateService::State::Ready) {
        const auto history = updates::releaseNotesForUpgrade(m_service->manifest(), current);
        QString notesText;
        for (const auto &release : history) {
            if (!notesText.isEmpty()) notesText += QStringLiteral("\n\n────────────────\n\n");
            notesText += QStringLiteral("v%1").arg(release.version);
            if (!release.publishedAt.isEmpty())
                notesText += QStringLiteral(" · %1").arg(release.publishedAt.left(10));
            notesText += QStringLiteral("\n") + release.notes.trimmed();
        }
        m_notes->setPlainText(notesText);
    } else {
        m_notes->setPlainText(m_service->manifest().notes);
    }
    m_notes->setMaximumHeight(m_automaticPrompt ? 220 : 140);
    m_settings->setVisible(state == updates::UpdateService::State::Disabled);
    m_retry->setVisible(!m_automaticPrompt
                        && state != updates::UpdateService::State::Checking
                        && state != updates::UpdateService::State::Downloading
                        && state != updates::UpdateService::State::Disabled);
    m_retry->setText(state == updates::UpdateService::State::Failed
                         ? QStringLiteral("重试") : QStringLiteral("检查更新"));
    m_download->setText(QStringLiteral("立即更新"));
    m_download->setVisible(state == updates::UpdateService::State::UpdateAvailable);
    m_cancel->setVisible(state == updates::UpdateService::State::Downloading);
    m_install->setVisible(state == updates::UpdateService::State::Ready);
    m_install->setEnabled(state == updates::UpdateService::State::Ready
                          && !m_service->verifiedFilePath().isEmpty());
    m_postpone->setVisible(m_automaticPrompt
                           && state == updates::UpdateService::State::UpdateAvailable);
    m_close->setVisible(!m_automaticPrompt
                        || state != updates::UpdateService::State::UpdateAvailable);
}

void UpdateDialog::setAutomaticPrompt(bool automatic)
{
    m_automaticPrompt = automatic;
    refresh();
}

void UpdateDialog::reject()
{
    if (m_automaticPrompt && m_service
        && m_service->state() == updates::UpdateService::State::UpdateAvailable) {
        postponeClicked();
        return;
    }
    QDialog::reject();
}

void UpdateDialog::postponeClicked()
{
    if (!m_automaticPrompt) return;
    // 先退出自动提醒语境，避免上层关闭对话框时再次进入 reject()。
    m_automaticPrompt = false;
    emit postponeRequested();
    QDialog::reject();
}

void UpdateDialog::downloadClicked()
{
    if (m_service) m_service->download();
    refresh();
}

void UpdateDialog::installClicked()
{
    if (!m_service || m_service->verifiedFilePath().isEmpty()) return;
    emit installRequested(m_service->verifiedFilePath());
}

void UpdateDialog::retryClicked()
{
    if (!m_service) return;
    if (m_service->state() == updates::UpdateService::State::Disabled)
        emit settingsRequested();
    else
        m_service->checkNow();
    refresh();
}

UpdateSettingsDialog::UpdateSettingsDialog(const QString &baseUrl, bool autoCheckEnabled,
                                           QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("UpdateSettingsDialog"));
    setWindowTitle(QStringLiteral("更新服务器"));
    setMinimumWidth(420);
    auto *layout = new QVBoxLayout(this);
    auto *hint = new QLabel(
        QStringLiteral("可信内网服务器的基础地址。生产默认值为 %1，留空表示停用更新检查与统计上报。")
            .arg(Settings::defaultUpdateBaseUrl()), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *urlRow = new QHBoxLayout();
    m_url = new QLineEdit(baseUrl, this);
    m_url->setObjectName(QStringLiteral("UpdateServerUrl"));
    m_url->setPlaceholderText(QStringLiteral("http://主机:端口"));
    urlRow->addWidget(m_url, 1);
    auto *restore = new QPushButton(QStringLiteral("恢复默认"), this);
    restore->setObjectName(QStringLiteral("UpdateServerRestoreDefault"));
    connect(restore, &QPushButton::clicked, this, [this]() {
        m_url->setText(Settings::defaultUpdateBaseUrl());
    });
    urlRow->addWidget(restore);
    layout->addLayout(urlRow);
    m_auto = new QCheckBox(QStringLiteral("启动后自动检查更新"), this);
    m_auto->setObjectName(QStringLiteral("UpdateAutoCheck"));
    m_auto->setChecked(autoCheckEnabled);
    layout->addWidget(m_auto);
    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("UpdateSettingsError"));
    m_error->setWordWrap(true);
    m_error->hide();
    layout->addWidget(m_error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("UpdateSettingsButtons"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        // 地址非法时留在对话框内提示，避免保存后静默变成“已停用”。
        const QString text = m_url->text().trimmed();
        if (!text.isEmpty()) {
            QUrl url(text, QUrl::TolerantMode);
            if (url.scheme().isEmpty() || url.host().isEmpty())
                url = QUrl(QStringLiteral("http://") + text, QUrl::TolerantMode);
            if (!url.isValid() || url.host().isEmpty()
                || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https"))) {
                m_error->setText(QStringLiteral("地址无效，应形如 http://主机:端口，或留空以停用更新与统计上报。"));
                m_error->show();
                return;
            }
        }
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString UpdateSettingsDialog::baseUrl() const
{
    return m_url->text().trimmed();
}

bool UpdateSettingsDialog::autoCheckEnabled() const
{
    return m_auto->isChecked();
}

} // namespace kl
