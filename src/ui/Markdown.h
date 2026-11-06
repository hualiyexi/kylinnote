#pragma once
#include <QString>
#include <QTextBrowser>
#include "../domain/Task.h"
namespace kn {
namespace Markdown {
QString toHtml(const QString& source);
QString displayHtml(const Task& task);
QString plainText(const Task& task);
}  // namespace Markdown
// 预览不加载外部/本地图片，也不自动打开链接。
class DocumentPreview : public QTextBrowser {
    Q_OBJECT
public:
    explicit DocumentPreview(QWidget* parent = nullptr);

protected:
    QVariant loadResource(int type, const QUrl& url) override;
};
}  // namespace kn
