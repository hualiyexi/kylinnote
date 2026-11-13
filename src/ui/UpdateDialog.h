#ifndef KYLINLIST_UPDATE_DIALOG_H
#define KYLINLIST_UPDATE_DIALOG_H

#include <QDialog>
#include <QPointer>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace kl {
namespace updates {
class UpdateService;
}

// 软件更新对话框：跟随 UpdateService 的状态显示检查、下载与安装入口。
// 只负责展示与用户确认；保存、备份与安装交接由 KylinListApp 协调。
class UpdateDialog : public QDialog {
    Q_OBJECT
public:
    explicit UpdateDialog(updates::UpdateService *service, QWidget *parent = nullptr);
    void refresh();

signals:
    void installRequested(const QString &filePath);
    void settingsRequested();

private slots:
    void downloadClicked();
    void installClicked();
    void retryClicked();

private:
    QPointer<updates::UpdateService> m_service;
    QLabel *m_status = nullptr;
    QLabel *m_meta = nullptr;
    QPlainTextEdit *m_notes = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_error = nullptr;
    QPushButton *m_settings = nullptr, *m_retry = nullptr, *m_download = nullptr,
                *m_cancel = nullptr, *m_install = nullptr, *m_close = nullptr;
};

// 更新服务器设置：基础地址 + 自动检查开关。
class UpdateSettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit UpdateSettingsDialog(const QString &baseUrl, bool autoCheckEnabled,
                                  QWidget *parent = nullptr);
    QString baseUrl() const;
    bool autoCheckEnabled() const;

private:
    QLineEdit *m_url = nullptr;
    QCheckBox *m_auto = nullptr;
};

} // namespace kl

#endif // KYLINLIST_UPDATE_DIALOG_H
