#include "Markdown.h"
#include <md4c-html.h>
#include <QDesktopServices>
#include <QTextDocument>
#include <QUrl>
#include "AppStyle.h"

namespace kn {
QString Markdown::toHtml(const QString& source) {
    const QByteArray input = source.toUtf8();
    QByteArray html;
    const auto append = [](const MD_CHAR* text, MD_SIZE size, void* data) {
        static_cast<QByteArray*>(data)->append(text, static_cast<int>(size));
    };
    const unsigned flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
                           MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_NOHTML;
    if (md_html(input.constData(), static_cast<MD_SIZE>(input.size()), append, &html, flags, 0) != 0)
        return QStringLiteral("<p>%1</p>").arg(source.toHtmlEscaped());
    // QTextDocument 没有浏览器的 checkbox DOM，显示为清晰的静态清单符号。
    QString result = QString::fromUtf8(html);
    result.replace(
        QStringLiteral("<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled checked>"),
        QStringLiteral("&#9745; "));
    result.replace(QStringLiteral("<input type=\"checkbox\" class=\"task-list-item-checkbox\" disabled>"),
                   QStringLiteral("&#9744; "));
    return result;
}
QString Markdown::displayHtml(const Task& task) {
    return task.contentFormat == ContentFormat::Html ? task.description : toHtml(task.description);
}
QString Markdown::plainText(const Task& task) {
    QTextDocument doc;
    doc.setHtml(displayHtml(task));
    return doc.toPlainText();
}
DocumentPreview::DocumentPreview(QWidget* parent) : QTextBrowser(parent) {
    setOpenLinks(false);
    setOpenExternalLinks(false);
    setFrameShape(QFrame::NoFrame);
    document()->setDefaultStyleSheet(AppStyle::previewCss());
    QPalette canvas = palette();
    canvas.setColor(QPalette::Base, Qt::white);
    canvas.setColor(QPalette::Text, QColor("#23324B"));
    canvas.setColor(QPalette::WindowText, QColor("#23324B"));
    setPalette(canvas);
    connect(this, &QTextBrowser::anchorClicked, this, [](const QUrl& url) {
        const QString scheme = url.scheme().toLower();
        if (scheme == QLatin1String("https") || scheme == QLatin1String("http") ||
            scheme == QLatin1String("mailto"))
            QDesktopServices::openUrl(url);
    });
}
QVariant DocumentPreview::loadResource(int, const QUrl&) {
    return QVariant();
}
}  // namespace kn
